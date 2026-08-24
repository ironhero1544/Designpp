// Copyright 2026 The Design++ Authors

#include "designpp/application/layout_viewer_service.h"

#include <objbase.h>
#include <windows.h>

#include <fstream>
#include <iterator>
#include <mutex>
#include <utility>

#include "designpp/application/synthesis_fingerprint.h"
#include "designpp/application/wsl_gui_execution_service.h"
#include "designpp/runtime/path_mapper.h"
#include "designpp/runtime/task_scheduler.h"

namespace designpp::application {
namespace {

std::wstring Utf8ToWide(std::string_view text) {
  if (text.empty()) return {};
  const int length =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), nullptr, 0);
  if (length <= 0) return {};
  std::wstring result(static_cast<std::size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                      static_cast<int>(text.size()), result.data(), length);
  return result;
}

std::wstring NewUuid() {
  GUID guid{};
  if (FAILED(CoCreateGuid(&guid))) return L"layout-staging";
  wchar_t value[39]{};
  if (StringFromGUID2(guid, value, static_cast<int>(std::size(value))) <= 0) {
    return L"layout-staging";
  }
  std::wstring result(value);
  if (!result.empty() && result.front() == L'{') result.erase(result.begin());
  if (!result.empty() && result.back() == L'}') result.pop_back();
  return result;
}

core::Status CopyGds(const std::filesystem::path& source,
                     const std::filesystem::path& destination) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(source, error) ||
      std::filesystem::file_size(source, error) == 0) {
    return {core::ErrorCode::kNotFound,
            "A non-empty compatible GDS artifact is required", 0};
  }
  std::filesystem::create_directories(destination.parent_path(), error);
  if (error) {
    return {core::ErrorCode::kIoError,
            "Cannot create the KLayout staging directory",
            static_cast<unsigned long>(error.value())};
  }
  std::filesystem::copy_file(source, destination,
                             std::filesystem::copy_options::overwrite_existing,
                             error);
  return error ? core::Status{core::ErrorCode::kIoError,
                              "Cannot stage the GDS for KLayout",
                              static_cast<unsigned long>(error.value())}
               : core::Status::Success();
}

}  // namespace

struct LayoutViewerService::Implementation final
    : public std::enable_shared_from_this<Implementation> {
  explicit Implementation(runtime::ExecutionProvider* execution_provider)
      : provider(execution_provider), gui_execution(execution_provider) {}

  void QueueCleanup(std::unique_ptr<runtime::ExecutionHandle> completed) {
    if (!completed) return;
    auto shared =
        std::shared_ptr<runtime::ExecutionHandle>(std::move(completed));
    static_cast<void>(
        cleanup.Submit([shared = std::move(shared)](std::stop_token) mutable {
          shared.reset();
        }));
  }

  void Emit(LayoutViewerEvent event) {
    LayoutViewerEventSink current;
    std::unique_ptr<runtime::ExecutionHandle> completed;
    {
      std::scoped_lock lock(mutex);
      if (shutdown || event.generation != request.generation) return;
      if (event.completed) {
        if (terminal) return;
        terminal = true;
        active = false;
        completed = std::move(handle);
      }
      current = sink;
    }
    if (current) current(std::move(event));
    QueueCleanup(std::move(completed));
  }

  void Prepare(std::stop_token stop_token) {
    bool cancelled = false;
    {
      std::scoped_lock lock(mutex);
      cancelled = cancellation_requested;
    }
    if (stop_token.stop_requested() || cancelled) {
      Complete({core::ErrorCode::kCancelled, "KLayout launch cancelled", 0},
               {});
      return;
    }
    staging_directory = std::filesystem::temp_directory_path() /
                        L"DesignPlusPlus" / L"layout" / NewUuid();
    const std::filesystem::path staged = staging_directory / L"layout.gds";
    core::Status copied = CopyGds(request.gds_path, staged);
    if (!copied.Ok()) {
      Complete(std::move(copied), {});
      return;
    }
    auto mapped = mapper.WindowsToWsl(staged);
    if (!mapped.Ok()) {
      Complete(mapped.GetStatus(), {});
      return;
    }
    staged_wsl_path = std::move(mapped).Value();
    Probe();
  }

  void Probe() {
    {
      std::scoped_lock lock(mutex);
      phase = 1;
    }
    runtime::WslCommand command;
    command.program = L"klayout";
    command.arguments = {L"-v"};
    if (!request.profile.wsl_distribution.empty()) {
      command.distribution = Utf8ToWide(request.profile.wsl_distribution);
    }
    const auto self = shared_from_this();
    auto started = provider->Start(
        command,
        [self](std::string output) {
          LayoutViewerEvent event;
          event.generation = self->request.generation;
          event.output = std::move(output);
          self->Emit(std::move(event));
        },
        [self](runtime::ProcessResult result) {
          {
            std::scoped_lock lock(self->mutex);
            if (self->terminal || self->phase != 1) return;
            self->phase = 2;
          }
          if (!result.started || result.cancelled || result.exit_code != 0) {
            self->Complete(
                result.cancelled
                    ? core::Status{core::ErrorCode::kCancelled,
                                   "KLayout launch cancelled", 0}
                    : core::Status{core::ErrorCode::kNotFound,
                                   "KLayout is not installed in the selected "
                                   "WSL toolchain",
                                   0},
                std::move(result));
            return;
          }
          self->Launch();
        });
    if (!started.Ok()) {
      Complete(started.status, {});
      return;
    }
    std::scoped_lock lock(mutex);
    handle = std::move(started.handle);
  }

  void Launch() {
    std::unique_ptr<runtime::ExecutionHandle> probe_handle;
    {
      std::scoped_lock lock(mutex);
      probe_handle = std::move(handle);
    }
    QueueCleanup(std::move(probe_handle));
    runtime::WslCommand command;
    command.program = L"klayout";
    command.arguments = {staged_wsl_path};
    if (!request.profile.wsl_distribution.empty()) {
      command.distribution = Utf8ToWide(request.profile.wsl_distribution);
    }
    const auto self = shared_from_this();
    const core::Status started = gui_execution.Start(
        command,
        [self](std::string output) {
          LayoutViewerEvent event;
          event.generation = self->request.generation;
          event.output = std::move(output);
          self->Emit(std::move(event));
        },
        [self](WslGuiExecutionResult gui_result) {
          runtime::ProcessResult result = std::move(gui_result.process_result);
          const bool succeeded = gui_result.status.Ok() && result.started &&
                                 !result.cancelled && result.exit_code == 0;
          self->Complete(
              succeeded ? core::Status::Success()
              : result.cancelled
                  ? core::Status{core::ErrorCode::kCancelled,
                                 "KLayout launch cancelled", 0}
                  : core::
                        Status{gui_result.status.code,
                               gui_result.status.message.empty()
                                   ? "KLayout could not open through WSLg; the "
                                     "GDS artifact remains valid"
                                   : gui_result.status.message,
                              gui_result.status.native_error},
              std::move(result));
        });
    if (!started.Ok()) {
      Complete(started, {});
      return;
    }
  }

  void Complete(core::Status status, runtime::ProcessResult result) {
    LayoutViewerEvent event;
    event.generation = request.generation;
    event.status = std::move(status);
    event.process_result = std::move(result);
    event.completed = true;
    Emit(std::move(event));
  }

  runtime::ExecutionProvider* provider = nullptr;
  WslGuiExecutionService gui_execution;
  mutable std::mutex mutex;
  runtime::TaskScheduler scheduler{1};
  runtime::TaskScheduler cleanup{1};
  runtime::PathMapper mapper;
  LayoutViewerRequest request;
  LayoutViewerEventSink sink;
  std::unique_ptr<runtime::ExecutionHandle> handle;
  std::filesystem::path staging_directory;
  std::wstring staged_wsl_path;
  bool active = false;
  bool terminal = false;
  bool shutdown = false;
  bool cancellation_requested = false;
  int phase = 0;
};

LayoutViewerService::LayoutViewerService(runtime::ExecutionProvider* provider)
    : implementation_(std::make_shared<Implementation>(provider)) {}

LayoutViewerService::~LayoutViewerService() { Shutdown(); }

core::Status LayoutViewerService::Open(LayoutViewerRequest request,
                                       LayoutViewerEventSink sink) {
  const auto implementation = implementation_;
  if (!implementation || implementation->provider == nullptr || !sink ||
      request.generation == 0 || request.gds_path.empty()) {
    return {core::ErrorCode::kInvalidArgument,
            "KLayout request identity is incomplete", 0};
  }
  {
    std::scoped_lock lock(implementation->mutex);
    if (implementation->shutdown || implementation->active) {
      return {core::ErrorCode::kConflict,
              "A KLayout viewer request is already active", 0};
    }
    implementation->request = std::move(request);
    implementation->sink = std::move(sink);
    implementation->active = true;
    implementation->terminal = false;
    implementation->cancellation_requested = false;
    implementation->phase = 0;
  }
  const bool queued =
      implementation->scheduler.Submit([implementation](std::stop_token token) {
        implementation->Prepare(token);
      });
  if (!queued) {
    std::scoped_lock lock(implementation->mutex);
    implementation->active = false;
    return {core::ErrorCode::kConflict,
            "KLayout preparation queue is unavailable", 0};
  }
  return core::Status::Success();
}

void LayoutViewerService::Cancel() noexcept {
  const auto implementation = implementation_;
  if (!implementation) return;
  bool complete_without_handle = false;
  {
    std::scoped_lock lock(implementation->mutex);
    if (!implementation->active || implementation->terminal) return;
    implementation->cancellation_requested = true;
    if (implementation->phase == 2 &&
        implementation->gui_execution.IsActive()) {
      implementation->gui_execution.Cancel();
    } else if (implementation->handle) {
      implementation->handle->Cancel();
    } else {
      complete_without_handle = true;
    }
  }
  if (complete_without_handle) {
    implementation->Complete(
        {core::ErrorCode::kCancelled, "KLayout launch cancelled", 0}, {});
  }
}

void LayoutViewerService::Shutdown() noexcept {
  const auto implementation = implementation_;
  if (!implementation) return;
  std::unique_ptr<runtime::ExecutionHandle> handle;
  {
    std::scoped_lock lock(implementation->mutex);
    if (implementation->shutdown) return;
    implementation->shutdown = true;
    implementation->active = false;
    implementation->sink = {};
    handle = std::move(implementation->handle);
  }
  if (handle) handle->Cancel();
  handle.reset();
  implementation->gui_execution.Shutdown();
  implementation->scheduler.RequestStop();
  implementation->cleanup.RequestStop();
}

bool LayoutViewerService::IsActive() const {
  const auto implementation = implementation_;
  if (!implementation) return false;
  std::scoped_lock lock(implementation->mutex);
  return implementation->active;
}

}  // namespace designpp::application

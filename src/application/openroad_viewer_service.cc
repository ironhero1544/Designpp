// Copyright 2026 The Design++ Authors

#include "designpp/application/openroad_viewer_service.h"

#include <objbase.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

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
  if (FAILED(CoCreateGuid(&guid))) return L"openroad-staging";
  wchar_t value[39]{};
  if (StringFromGUID2(guid, value, static_cast<int>(std::size(value))) <= 0) {
    return L"openroad-staging";
  }
  std::wstring result(value);
  if (!result.empty() && result.front() == L'{') result.erase(result.begin());
  if (!result.empty() && result.back() == L'}') result.pop_back();
  return result;
}

core::Status CopyCheckpoint(const std::filesystem::path& source,
                            const std::filesystem::path& destination) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(source, error) || error ||
      std::filesystem::file_size(source, error) == 0) {
    return {core::ErrorCode::kNotFound,
            "A non-empty ORFS checkpoint is required", 0};
  }
  std::filesystem::create_directories(destination.parent_path(), error);
  if (error) {
    return {core::ErrorCode::kIoError,
            "Cannot create the OpenROAD staging directory",
            static_cast<unsigned long>(error.value())};
  }
  std::filesystem::copy_file(source, destination,
                             std::filesystem::copy_options::overwrite_existing,
                             error);
  return error ? core::Status{core::ErrorCode::kIoError,
                              "Cannot stage the ORFS checkpoint",
                              static_cast<unsigned long>(error.value())}
               : core::Status::Success();
}

bool IsGuiTarget(std::string_view target) {
  constexpr std::array<std::string_view, 6> kTargets = {
      "gui_synth", "gui_floorplan", "gui_place",
      "gui_cts",   "gui_route",     "gui_final"};
  return std::find(kTargets.begin(), kTargets.end(), target) != kTargets.end();
}

std::string RelativeWorkspace(std::string_view workspace) {
  if (workspace.starts_with("~/")) {
    return "$(HOME)/" + std::string(workspace.substr(2));
  }
  if (workspace.starts_with("./")) workspace.remove_prefix(2);
  if (!workspace.empty() && workspace.front() != '/') {
    return "$(HOME)/" + std::string(workspace);
  }
  return std::string(workspace);
}

}  // namespace

struct OpenRoadViewerService::Implementation final
    : public std::enable_shared_from_this<Implementation> {
  explicit Implementation(runtime::ExecutionProvider* execution_provider)
      : provider(execution_provider), gui_execution(execution_provider) {}

  void Emit(OpenRoadViewerEvent event) {
    OpenRoadViewerEventSink current;
    std::shared_ptr<runtime::ExecutionHandle> completed;
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
    if (completed) {
      QueueHandleCleanup(std::move(completed));
    }
  }

  void QueueHandleCleanup(
      std::shared_ptr<runtime::ExecutionHandle> completed_handle) {
    if (!completed_handle) return;
    auto holder = std::make_shared<std::shared_ptr<runtime::ExecutionHandle>>(
        std::move(completed_handle));
    const bool queued = cleanup.Submit([holder](std::stop_token) mutable {
      auto handle = std::move(*holder);
      holder.reset();
      static_cast<void>(handle->IsRunning());
    });
    if (!queued) {
      std::scoped_lock lock(mutex);
      deferred_handle_cleanup.push_back(std::move(*holder));
    }
  }

  void Complete(core::Status status, runtime::ProcessResult result) {
    OpenRoadViewerEvent event;
    event.generation = request.generation;
    event.status = std::move(status);
    event.process_result = std::move(result);
    event.completed = true;
    Emit(std::move(event));
  }

  void Prepare(std::stop_token stop_token) {
    bool cancelled = false;
    {
      std::scoped_lock lock(mutex);
      cancelled = cancellation_requested;
    }
    if (stop_token.stop_requested() || cancelled) {
      Complete({core::ErrorCode::kCancelled, "OpenROAD launch cancelled", 0},
               {});
      return;
    }
    staging_directory = std::filesystem::temp_directory_path() /
                        L"DesignPlusPlus" / L"openroad" / NewUuid();
    const core::Status copied =
        CopyCheckpoint(request.odb_path, staging_directory / L"checkpoint.odb");
    if (!copied.Ok()) {
      Complete(copied, {});
      return;
    }
    auto mapped = mapper.WindowsToWsl(staging_directory / L"checkpoint.odb");
    if (!mapped.Ok()) {
      Complete(mapped.GetStatus(), {});
      return;
    }
    staged_wsl_path = std::move(mapped).Value();
    if (!request.config_path.empty()) {
      std::error_code config_error;
      if (std::filesystem::is_regular_file(request.config_path, config_error) &&
          !config_error &&
          std::filesystem::file_size(request.config_path, config_error) > 0 &&
          !config_error) {
        const core::Status config_copied = CopyCheckpoint(
            request.config_path, staging_directory / L"config.mk");
        if (!config_copied.Ok()) {
          Complete(config_copied, {});
          return;
        }
        auto config_mapped =
            mapper.WindowsToWsl(staging_directory / L"config.mk");
        if (!config_mapped.Ok()) {
          Complete(config_mapped.GetStatus(), {});
          return;
        }
        staged_config_wsl_path = std::move(config_mapped).Value();
      }
    }
    Probe();
  }

  void Probe() {
    {
      std::scoped_lock lock(mutex);
      phase = 1;
    }
    runtime::WslCommand command;
    command.program = L"openroad";
    command.arguments = {L"-version"};
    if (!request.profile.wsl_distribution.empty()) {
      command.distribution = Utf8ToWide(request.profile.wsl_distribution);
    }
    const auto self = shared_from_this();
    runtime::ExecutionStartResult started = provider->Start(
        command,
        [self](std::string output) {
          OpenRoadViewerEvent event;
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
                                   "OpenROAD launch cancelled", 0}
                    : core::Status{core::ErrorCode::kNotFound,
                                   "OpenROAD is not installed in the selected "
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
    std::shared_ptr<runtime::ExecutionHandle> probe_handle(
        std::move(started.handle));
    bool discard = false;
    {
      std::scoped_lock lock(mutex);
      if (shutdown || terminal || phase != 1) {
        discard = true;
      } else {
        handle = std::move(probe_handle);
      }
    }
    if (discard) {
      if (probe_handle) probe_handle->Cancel();
      QueueHandleCleanup(std::move(probe_handle));
    }
  }

  void Launch() {
    std::shared_ptr<runtime::ExecutionHandle> probe_handle;
    {
      std::scoped_lock lock(mutex);
      probe_handle = std::move(handle);
    }
    if (probe_handle) {
      QueueHandleCleanup(std::move(probe_handle));
    }
    runtime::WslCommand command;
    const bool use_orfs_gui =
        IsGuiTarget(request.gui_target) && !request.orfs_root.empty() &&
        !request.backend_workspace.empty() && !staged_config_wsl_path.empty();
    if (use_orfs_gui) {
      const std::wstring root = Utf8ToWide(request.orfs_root);
      const std::wstring workspace =
          Utf8ToWide(RelativeWorkspace(request.backend_workspace));
      const std::wstring config = staged_config_wsl_path;
      const std::wstring target = Utf8ToWide(request.gui_target);
      const std::wstring variant = Utf8ToWide(request.flow_variant);
      if (!root.empty() && !workspace.empty() && !target.empty() &&
          !variant.empty() && request.orfs_root.front() == '/') {
        command.program = L"/usr/bin/make";
        command.arguments = {L"--no-print-directory",
                             L"-C",
                             root + L"/flow",
                             L"-j",
                             L"1",
                             L"DESIGN_CONFIG=" + config,
                             L"WORK_HOME=" + workspace,
                             L"FLOW_VARIANT=" + variant,
                             target};
      } else if (!root.empty() && !workspace.empty() && !target.empty() &&
                 !variant.empty()) {
        // The supported ~/root shorthand needs HOME expansion.  Keep the
        // script constant and pass all user-controlled values as positional
        // arguments so no path or target is interpolated into shell source.
        command.program = L"/bin/bash";
        command.arguments = {
            L"-lc",
            L"set -eu; root=\"$1\"; workspace=\"$2\"; config=\"$3\"; "
            L"target=\"$4\"; variant=\"$5\"; "
            L"case \"$root\" in '~/'*) root=\"$HOME/${root#\\~/}\";; esac; "
            L"case \"$workspace\" in /*) ;; *) "
            L"workspace=\"$HOME/${workspace#./}\";; esac; "
            L"exec /usr/bin/make --no-print-directory -C \"$root/flow\" -j 1 "
            L"DESIGN_CONFIG=\"$config\" WORK_HOME=\"$workspace\" "
            L"FLOW_VARIANT=\"$variant\" \"$target\"",
            L"designpp-openroad",
            root,
            workspace,
            config,
            target,
            variant};
      } else {
        command.program = L"openroad";
        command.arguments = {L"-gui", staged_wsl_path};
      }
    } else {
      command.program = L"openroad";
      command.arguments = {L"-gui", staged_wsl_path};
    }
    if (!request.profile.wsl_distribution.empty()) {
      command.distribution = Utf8ToWide(request.profile.wsl_distribution);
    }
    const auto self = shared_from_this();
    const core::Status started = gui_execution.Start(
        command,
        [self](std::string output) {
          OpenRoadViewerEvent event;
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
                                 "OpenROAD launch cancelled", 0}
                  : core::Status{gui_result.status.code,
                                 gui_result.status.message.empty()
                                     ? "OpenROAD could not open through WSLg; "
                                       "the checkpoint remains valid"
                                     : gui_result.status.message,
                                 gui_result.status.native_error},
              std::move(result));
        });
    if (!started.Ok()) Complete(started, {});
  }

  runtime::ExecutionProvider* provider = nullptr;
  WslGuiExecutionService gui_execution;
  mutable std::mutex mutex;
  runtime::TaskScheduler scheduler{1};
  runtime::TaskScheduler cleanup{1};
  runtime::PathMapper mapper;
  OpenRoadViewerRequest request;
  OpenRoadViewerEventSink sink;
  std::shared_ptr<runtime::ExecutionHandle> handle;
  std::vector<std::shared_ptr<runtime::ExecutionHandle>>
      deferred_handle_cleanup;
  std::filesystem::path staging_directory;
  std::wstring staged_wsl_path;
  std::wstring staged_config_wsl_path;
  bool active = false;
  bool terminal = false;
  bool shutdown = false;
  bool cancellation_requested = false;
  int phase = 0;
};

OpenRoadViewerService::OpenRoadViewerService(
    runtime::ExecutionProvider* provider)
    : implementation_(std::make_shared<Implementation>(provider)) {}

OpenRoadViewerService::~OpenRoadViewerService() { Shutdown(); }

core::Status OpenRoadViewerService::Open(OpenRoadViewerRequest request,
                                         OpenRoadViewerEventSink sink) {
  const auto implementation = implementation_;
  if (!implementation || !implementation->provider || !sink ||
      request.generation == 0 || request.odb_path.empty()) {
    return {core::ErrorCode::kInvalidArgument,
            "OpenROAD checkpoint request identity is incomplete", 0};
  }
  if (!request.gui_target.empty() && !IsGuiTarget(request.gui_target)) {
    return {core::ErrorCode::kInvalidArgument,
            "OpenROAD GUI target is not supported", 0};
  }
  {
    std::scoped_lock lock(implementation->mutex);
    if (implementation->shutdown || implementation->active) {
      return {core::ErrorCode::kConflict,
              "An OpenROAD viewer request is already active", 0};
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
            "OpenROAD preparation queue is unavailable", 0};
  }
  return core::Status::Success();
}

void OpenRoadViewerService::Cancel() noexcept {
  const auto implementation = implementation_;
  if (!implementation) return;
  bool complete_without_handle = false;
  bool cancel_gui = false;
  std::shared_ptr<runtime::ExecutionHandle> handle;
  {
    std::scoped_lock lock(implementation->mutex);
    if (!implementation->active || implementation->terminal) return;
    implementation->cancellation_requested = true;
    if (implementation->phase == 2 &&
        implementation->gui_execution.IsActive()) {
      cancel_gui = true;
    } else if (implementation->handle) {
      handle = implementation->handle;
    } else {
      complete_without_handle = true;
    }
  }
  if (cancel_gui) implementation->gui_execution.Cancel();
  if (handle) handle->Cancel();
  if (complete_without_handle) {
    implementation->Complete(
        {core::ErrorCode::kCancelled, "OpenROAD launch cancelled", 0}, {});
  }
}

void OpenRoadViewerService::Shutdown() noexcept {
  const auto implementation = implementation_;
  if (!implementation) return;
  std::shared_ptr<runtime::ExecutionHandle> handle;
  std::vector<std::shared_ptr<runtime::ExecutionHandle>> deferred_handles;
  {
    std::scoped_lock lock(implementation->mutex);
    if (implementation->shutdown) return;
    implementation->shutdown = true;
    implementation->active = false;
    implementation->sink = {};
    handle = std::move(implementation->handle);
    deferred_handles.swap(implementation->deferred_handle_cleanup);
  }
  if (handle) handle->Cancel();
  implementation->gui_execution.Shutdown();
  implementation->scheduler.RequestStop();
  implementation->cleanup.RequestStop();
  deferred_handles.clear();
}

bool OpenRoadViewerService::IsActive() const {
  const auto implementation = implementation_;
  if (!implementation) return false;
  std::scoped_lock lock(implementation->mutex);
  return implementation->active;
}

}  // namespace designpp::application

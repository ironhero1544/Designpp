// Copyright 2026 The Design++ Authors

#include "designpp/application/wsl_gui_execution_service.h"

#include <objbase.h>

#include <array>
#include <atomic>
#include <mutex>
#include <utility>

#include "designpp/runtime/task_scheduler.h"

namespace designpp::application {
namespace {

enum class Phase {
  kIdle,
  kProbe,
  kCreateDirectory,
  kMount,
  kVerify,
  kCleanup,
  kLaunch,
};

std::wstring NewProbePath() {
  GUID guid{};
  if (FAILED(CoCreateGuid(&guid))) return {};
  std::array<wchar_t, 39> text{};
  if (StringFromGUID2(guid, text.data(), static_cast<int>(text.size())) <= 0) {
    return {};
  }
  std::wstring id(text.data());
  if (!id.empty() && id.front() == L'{') id.erase(id.begin());
  if (!id.empty() && id.back() == L'}') id.pop_back();
  return L"/mnt/shared_memory/.designpp-wslg-probe-" + id;
}

runtime::WslCommand ChildCommand(const runtime::WslCommand& original,
                                 std::wstring program) {
  runtime::WslCommand command;
  command.distribution = original.distribution;
  command.program = std::move(program);
  return command;
}

core::Status RepairFailureStatus(std::string operation) {
  return {core::ErrorCode::kIoError,
          "WSLg shared-memory repair failed during " + operation +
              "; run 'wsl --shutdown' after closing other WSL work, then "
              "restart the viewer",
          0};
}

}  // namespace

struct WslGuiExecutionService::Implementation final
    : public std::enable_shared_from_this<Implementation> {
  explicit Implementation(runtime::ExecutionProvider* execution_provider)
      : provider(execution_provider) {}

  void QueueCleanup(std::unique_ptr<runtime::ExecutionHandle> completed) {
    if (!completed) return;
    auto shared =
        std::shared_ptr<runtime::ExecutionHandle>(std::move(completed));
    static_cast<void>(
        cleanup.Submit([shared = std::move(shared)](std::stop_token) mutable {
          shared.reset();
        }));
  }

  void StartPhase(Phase next_phase, runtime::WslCommand next_command) {
    std::unique_ptr<runtime::ExecutionHandle> previous;
    bool cancelled = false;
    {
      std::scoped_lock lock(mutex);
      if (shutdown || terminal) return;
      cancelled = cancellation_requested;
      if (!cancelled) {
        phase = next_phase;
        previous = std::move(handle);
      }
    }
    if (cancelled) {
      Finish({core::ErrorCode::kCancelled, "WSLg viewer launch cancelled", 0},
             {}, repair_attempted.load());
      return;
    }
    QueueCleanup(std::move(previous));

    const auto self = shared_from_this();
    auto started = provider->Start(
        next_command,
        [self](std::string output) {
          runtime::ExecutionOutputCallback callback;
          {
            std::scoped_lock lock(self->mutex);
            if (self->shutdown || self->terminal) return;
            callback = self->output_callback;
          }
          if (callback) callback(std::move(output));
        },
        [self, next_phase](runtime::ProcessResult result) {
          self->PhaseCompleted(next_phase, std::move(result));
        });
    if (!started.Ok()) {
      Finish(started.status, {}, repair_attempted.load());
      return;
    }
    std::scoped_lock lock(mutex);
    if (shutdown || terminal || phase != next_phase) {
      started.handle->Cancel();
      QueueCleanup(std::move(started.handle));
      return;
    }
    handle = std::move(started.handle);
  }

  void PhaseCompleted(Phase completed_phase, runtime::ProcessResult result) {
    {
      std::scoped_lock lock(mutex);
      if (shutdown || terminal || phase != completed_phase) return;
    }
    if (result.cancelled) {
      Finish({core::ErrorCode::kCancelled, "WSLg viewer launch cancelled", 0},
             std::move(result), repair_attempted.load());
      return;
    }

    const bool succeeded =
        result.started && result.exit_code == 0 && result.error_message.empty();
    switch (completed_phase) {
      case Phase::kProbe:
        if (succeeded) {
          StartCleanup();
        } else {
          repair_attempted = true;
          runtime::WslCommand create = ChildCommand(command, L"/usr/bin/mkdir");
          create.user = L"root";
          create.arguments = {L"-p", L"/mnt/shared_memory"};
          StartPhase(Phase::kCreateDirectory, std::move(create));
        }
        break;
      case Phase::kCreateDirectory: {
        if (!succeeded) {
          Finish(RepairFailureStatus("directory creation"), std::move(result),
                 true);
          break;
        }
        runtime::WslCommand mount = ChildCommand(command, L"/usr/bin/mount");
        mount.user = L"root";
        mount.arguments = {L"-t",        L"tmpfs", L"-o",
                           L"mode=1777", L"tmpfs", L"/mnt/shared_memory"};
        StartPhase(Phase::kMount, std::move(mount));
        break;
      }
      case Phase::kMount:
        if (!succeeded) {
          Finish(RepairFailureStatus("tmpfs mount"), std::move(result), true);
          break;
        }
        StartTouch(Phase::kVerify);
        break;
      case Phase::kVerify:
        if (!succeeded) {
          Finish(RepairFailureStatus("write verification"), std::move(result),
                 true);
          break;
        }
        StartCleanup();
        break;
      case Phase::kCleanup:
        StartPhase(Phase::kLaunch, command);
        break;
      case Phase::kLaunch:
        Finish(succeeded
                   ? core::Status::Success()
                   : core::Status{core::ErrorCode::kIoError,
                                  "WSL GUI program exited with an error", 0},
               std::move(result), repair_attempted.load());
        break;
      case Phase::kIdle:
        break;
    }
  }

  void StartTouch(Phase touch_phase) {
    runtime::WslCommand touch = ChildCommand(command, L"/usr/bin/touch");
    touch.arguments = {probe_path};
    StartPhase(touch_phase, std::move(touch));
  }

  void StartCleanup() {
    runtime::WslCommand remove = ChildCommand(command, L"/usr/bin/rm");
    remove.arguments = {L"-f", probe_path};
    StartPhase(Phase::kCleanup, std::move(remove));
  }

  void Finish(core::Status status, runtime::ProcessResult result,
              bool repaired) {
    WslGuiCompletionCallback callback;
    std::unique_ptr<runtime::ExecutionHandle> completed;
    {
      std::scoped_lock lock(mutex);
      if (shutdown || terminal) return;
      terminal = true;
      active = false;
      phase = Phase::kIdle;
      completed = std::move(handle);
      callback = completion_callback;
    }
    if (callback) {
      callback({std::move(status), std::move(result), repaired});
    }
    QueueCleanup(std::move(completed));
  }

  runtime::ExecutionProvider* provider = nullptr;
  mutable std::mutex mutex;
  runtime::TaskScheduler cleanup{1};
  runtime::WslCommand command;
  runtime::ExecutionOutputCallback output_callback;
  WslGuiCompletionCallback completion_callback;
  std::unique_ptr<runtime::ExecutionHandle> handle;
  std::wstring probe_path;
  Phase phase = Phase::kIdle;
  bool active = false;
  bool terminal = false;
  bool shutdown = false;
  bool cancellation_requested = false;
  std::atomic_bool repair_attempted = false;
};

WslGuiExecutionService::WslGuiExecutionService(
    runtime::ExecutionProvider* provider)
    : implementation_(std::make_shared<Implementation>(provider)) {}

WslGuiExecutionService::~WslGuiExecutionService() { Shutdown(); }

core::Status WslGuiExecutionService::Start(
    runtime::WslCommand command,
    runtime::ExecutionOutputCallback output_callback,
    WslGuiCompletionCallback completion_callback) {
  const auto implementation = implementation_;
  if (!implementation || implementation->provider == nullptr ||
      command.program.empty() || !completion_callback) {
    return {core::ErrorCode::kInvalidArgument,
            "WSL GUI execution request is incomplete", 0};
  }
  const std::wstring probe_path = NewProbePath();
  if (probe_path.empty()) {
    return {core::ErrorCode::kIoError,
            "Cannot create a unique WSLg health-check identity", 0};
  }
  {
    std::scoped_lock lock(implementation->mutex);
    if (implementation->shutdown || implementation->active) {
      return {core::ErrorCode::kConflict, "A WSL GUI program is already active",
              0};
    }
    implementation->command = std::move(command);
    implementation->output_callback = std::move(output_callback);
    implementation->completion_callback = std::move(completion_callback);
    implementation->probe_path = probe_path;
    implementation->active = true;
    implementation->terminal = false;
    implementation->cancellation_requested = false;
    implementation->repair_attempted = false;
  }
  implementation->StartTouch(Phase::kProbe);
  return core::Status::Success();
}

void WslGuiExecutionService::Cancel() noexcept {
  const auto implementation = implementation_;
  if (!implementation) return;
  bool finish_without_handle = false;
  {
    std::scoped_lock lock(implementation->mutex);
    if (!implementation->active || implementation->terminal) return;
    implementation->cancellation_requested = true;
    if (implementation->handle) {
      implementation->handle->Cancel();
    } else {
      finish_without_handle = true;
    }
  }
  if (finish_without_handle) {
    implementation->Finish(
        {core::ErrorCode::kCancelled, "WSLg viewer launch cancelled", 0}, {},
        implementation->repair_attempted.load());
  }
}

void WslGuiExecutionService::Shutdown() noexcept {
  const auto implementation = implementation_;
  if (!implementation) return;
  std::unique_ptr<runtime::ExecutionHandle> handle;
  {
    std::scoped_lock lock(implementation->mutex);
    if (implementation->shutdown) return;
    implementation->shutdown = true;
    implementation->active = false;
    implementation->output_callback = {};
    implementation->completion_callback = {};
    handle = std::move(implementation->handle);
  }
  if (handle) handle->Cancel();
  handle.reset();
  implementation->cleanup.RequestStop();
}

bool WslGuiExecutionService::IsActive() const {
  const auto implementation = implementation_;
  if (!implementation) return false;
  std::scoped_lock lock(implementation->mutex);
  return implementation->active;
}

}  // namespace designpp::application

// Copyright 2026 The Design++ Authors

#include "designpp/runtime/execution_provider.h"

#include <memory>
#include <utility>

namespace designpp::runtime {
namespace {

class WslExecutionHandle final : public ExecutionHandle {
 public:
  explicit WslExecutionHandle(ProcessSession session)
      : session_(std::move(session)) {}

  void Cancel() noexcept override { session_.Cancel(); }

  [[nodiscard]] bool IsRunning() const noexcept override {
    return session_.IsRunning();
  }

  [[nodiscard]] InputWriteResult WriteInput(std::string bytes) override {
    return session_.WriteInput(std::move(bytes));
  }

 private:
  ProcessSession session_;
};

}  // namespace

ExecutionStartResult WslExecutionProvider::Start(
    const WslCommand& command, ExecutionOutputCallback on_output,
    ExecutionCompletionCallback on_complete) {
  ProcessLaunchResult launch = WslExecutor::RunAsync(
      command, std::move(on_output), std::move(on_complete));
  if (!launch.IsValid()) {
    return {nullptr,
            {core::ErrorCode::kIoError, "Cannot launch WSL command", 0}};
  }
  return {std::make_unique<WslExecutionHandle>(std::move(launch.session)),
          core::Status::Success()};
}

}  // namespace designpp::runtime

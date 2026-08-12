// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_RUNTIME_EXECUTION_PROVIDER_H_
#define DESIGNPP_RUNTIME_EXECUTION_PROVIDER_H_

#include <functional>
#include <memory>
#include <string>

#include "designpp/core/status.h"
#include "designpp/runtime/process_runner.h"
#include "designpp/runtime/wsl_executor.h"

namespace designpp::runtime {

// Owns one external execution without exposing a concrete process type.
class ExecutionHandle {
 public:
  virtual ~ExecutionHandle() = default;

  virtual void Cancel() noexcept = 0;
  [[nodiscard]] virtual bool IsRunning() const noexcept = 0;
  [[nodiscard]] virtual InputWriteResult WriteInput(std::string bytes) = 0;
};

using ExecutionOutputCallback = std::function<void(std::string)>;
using ExecutionCompletionCallback = std::function<void(ProcessResult)>;

struct ExecutionStartResult {
  std::unique_ptr<ExecutionHandle> handle;
  core::Status status;

  [[nodiscard]] bool Ok() const noexcept {
    return status.Ok() && handle != nullptr;
  }
};

// Execution boundary used by application services. Implementations must not
// invoke callbacks while holding their internal ownership locks.
class ExecutionProvider {
 public:
  virtual ~ExecutionProvider() = default;

  [[nodiscard]] virtual ExecutionStartResult Start(
      const WslCommand& command, ExecutionOutputCallback on_output,
      ExecutionCompletionCallback on_complete) = 0;
};

// Starts structured WSL commands through the process runtime.
class WslExecutionProvider final : public ExecutionProvider {
 public:
  [[nodiscard]] ExecutionStartResult Start(
      const WslCommand& command, ExecutionOutputCallback on_output,
      ExecutionCompletionCallback on_complete) override;
};

}  // namespace designpp::runtime

#endif  // DESIGNPP_RUNTIME_EXECUTION_PROVIDER_H_

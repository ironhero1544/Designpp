// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_APPLICATION_WSL_GUI_EXECUTION_SERVICE_H_
#define DESIGNPP_APPLICATION_WSL_GUI_EXECUTION_SERVICE_H_

#include <functional>
#include <memory>
#include <string>

#include "designpp/core/status.h"
#include "designpp/runtime/execution_provider.h"

namespace designpp::application {

struct WslGuiExecutionResult {
  core::Status status;
  runtime::ProcessResult process_result;
  bool repair_attempted = false;
};

using WslGuiCompletionCallback = std::function<void(WslGuiExecutionResult)>;

// Verifies the selected distribution's WSLg shared-memory transport before
// launching a GUI program. A missing or unusable mount is repaired in that
// distribution only; the service never shuts down WSL or changes global state.
class WslGuiExecutionService final {
 public:
  explicit WslGuiExecutionService(runtime::ExecutionProvider* provider);
  WslGuiExecutionService(const WslGuiExecutionService&) = delete;
  WslGuiExecutionService& operator=(const WslGuiExecutionService&) = delete;
  ~WslGuiExecutionService();

  [[nodiscard]] core::Status Start(
      runtime::WslCommand command,
      runtime::ExecutionOutputCallback output_callback,
      WslGuiCompletionCallback completion_callback);
  void Cancel() noexcept;
  void Shutdown() noexcept;
  [[nodiscard]] bool IsActive() const;

 private:
  struct Implementation;
  std::shared_ptr<Implementation> implementation_;
};

}  // namespace designpp::application

#endif  // DESIGNPP_APPLICATION_WSL_GUI_EXECUTION_SERVICE_H_

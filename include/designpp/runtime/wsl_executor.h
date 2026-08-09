#ifndef DESIGNPP_RUNTIME_WSL_EXECUTOR_H_
#define DESIGNPP_RUNTIME_WSL_EXECUTOR_H_

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "designpp/runtime/process_runner.h"

namespace designpp::runtime {

// Describes a Linux program invocation inside WSL2.
struct WslCommand {
  std::optional<std::wstring> distribution;
  std::optional<std::wstring> working_directory;
  std::wstring program;
  std::vector<std::wstring> arguments;
  std::vector<std::pair<std::wstring, std::wstring>> environment;
};

// Converts structured Linux commands into cancellable wsl.exe processes.
class WslExecutor final {
 public:
  // Creates the Windows process request for a Linux command.
  [[nodiscard]] static ProcessRequest BuildRequest(const WslCommand& command);

  // Runs a Linux command asynchronously through WSL2.
  [[nodiscard]] static ProcessLaunchResult RunAsync(
      const WslCommand& command, OutputCallback on_output,
      CompletionCallback on_complete);

  // Runs the lightweight WSL installation/status diagnostic.
  [[nodiscard]] static ProcessLaunchResult ProbeAsync(
      OutputCallback on_output, CompletionCallback on_complete);
};

}  // namespace designpp::runtime

#endif  // DESIGNPP_RUNTIME_WSL_EXECUTOR_H_

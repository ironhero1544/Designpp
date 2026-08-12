#include "designpp/runtime/wsl_executor.h"

namespace designpp::runtime {

ProcessRequest WslExecutor::BuildRequest(const WslCommand& command) {
  if (command.program.empty()) {
    return {};
  }

  ProcessRequest request;
  request.executable = L"wsl.exe";
  request.interactive_input = command.interactive_input;

  if (command.distribution && !command.distribution->empty()) {
    request.arguments.emplace_back(L"--distribution");
    request.arguments.push_back(*command.distribution);
  }
  if (command.working_directory && !command.working_directory->empty()) {
    request.arguments.emplace_back(L"--cd");
    request.arguments.push_back(*command.working_directory);
  } else {
    request.arguments.emplace_back(L"--cd");
    request.arguments.emplace_back(L"~");
  }

  request.arguments.emplace_back(L"--exec");
  if (!command.environment.empty()) {
    request.arguments.emplace_back(L"/usr/bin/env");
    for (const auto& [name, value] : command.environment) {
      request.arguments.push_back(name + L"=" + value);
    }
  }
  request.arguments.push_back(command.program);
  request.arguments.insert(request.arguments.end(), command.arguments.begin(),
                           command.arguments.end());
  return request;
}

ProcessLaunchResult WslExecutor::RunAsync(const WslCommand& command,
                                          OutputCallback on_output,
                                          CompletionCallback on_complete) {
  return ProcessRunner::RunAsync(BuildRequest(command), std::move(on_output),
                                 std::move(on_complete));
}

ProcessLaunchResult WslExecutor::ProbeAsync(OutputCallback on_output,
                                            CompletionCallback on_complete) {
  WslCommand command;
  command.program = L"/usr/bin/uname";
  command.arguments.emplace_back(L"-a");
  return RunAsync(command, std::move(on_output), std::move(on_complete));
}

}  // namespace designpp::runtime

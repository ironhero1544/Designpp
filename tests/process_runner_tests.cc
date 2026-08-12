// Copyright 2026 The Design++ Authors

#include <CppUnitTest.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>

#include "designpp/runtime/process_runner.h"
#include "designpp/runtime/wsl_executor.h"

using Microsoft::VisualStudio::CppUnitTestFramework::Assert;

namespace designpp::tests {

TEST_CLASS(ProcessRunnerTests){
  public : TEST_METHOD(InteractiveInputIsQueuedAndCaptured){
      runtime::ProcessRequest request;
request.executable = L"powershell.exe";
request.arguments = {
    L"-NoProfile", L"-NonInteractive", L"-Command",
    L"$line=[Console]::In.ReadLine(); [Console]::Out.Write('ACK:'+$line)"};
request.interactive_input = true;
std::mutex mutex;
std::condition_variable completed;
bool done = false;
runtime::ProcessResult result;
auto launch = runtime::ProcessRunner::RunAsync(
    std::move(request), [](std::string) {},
    [&](runtime::ProcessResult value) {
      {
        std::scoped_lock lock(mutex);
        result = std::move(value);
        done = true;
      }
      completed.notify_one();
    });
Assert::IsTrue(launch.IsValid());
Assert::IsTrue(launch.session.WriteInput("hello\n") ==
               runtime::InputWriteResult::kAccepted);
std::unique_lock lock(mutex);
Assert::IsTrue(completed.wait_for(lock, std::chrono::seconds(10),
                                  [&] { return done; }));
Assert::IsTrue(result.started);
Assert::AreEqual(static_cast<std::uint32_t>(0), result.exit_code);
Assert::IsTrue(result.output.find("ACK:hello") != std::string::npos);
}  // namespace designpp::tests

TEST_METHOD(OversizedInteractiveInputIsRejected) {
  runtime::ProcessRequest request;
  request.executable = L"powershell.exe";
  request.arguments = {L"-NoProfile", L"-NonInteractive", L"-Command",
                       L"[Console]::In.ReadLine()"};
  request.interactive_input = true;
  auto launch = runtime::ProcessRunner::RunAsync(
      std::move(request), [](std::string) {}, [](runtime::ProcessResult) {});
  Assert::IsTrue(launch.IsValid());
  Assert::IsTrue(launch.session.WriteInput(std::string(64 * 1024 + 1, 'x')) ==
                 runtime::InputWriteResult::kQueueFull);
  launch.session.Cancel();
}

TEST_METHOD(NonInteractiveInputStartsAtEndOfFile) {
  runtime::ProcessRequest request;
  request.executable = L"powershell.exe";
  request.arguments = {L"-NoProfile", L"-NonInteractive", L"-Command",
                       L"$text=[Console]::In.ReadToEnd(); "
                       L"[Console]::Out.Write('EOF:'+$text.Length)"};
  std::mutex mutex;
  std::condition_variable completed;
  bool done = false;
  runtime::ProcessResult result;
  auto launch = runtime::ProcessRunner::RunAsync(
      std::move(request), [](std::string) {},
      [&](runtime::ProcessResult value) {
        {
          std::scoped_lock lock(mutex);
          result = std::move(value);
          done = true;
        }
        completed.notify_one();
      });
  Assert::IsTrue(launch.IsValid());
  std::unique_lock lock(mutex);
  Assert::IsTrue(
      completed.wait_for(lock, std::chrono::seconds(10), [&] { return done; }));
  Assert::IsTrue(result.started);
  Assert::AreEqual(static_cast<std::uint32_t>(0), result.exit_code);
  Assert::IsTrue(result.output.find("EOF:0") != std::string::npos);
}

TEST_METHOD(WslRequestUsesExplicitHomeDirectoryByDefault) {
  runtime::WslCommand command;
  command.program = L"/usr/bin/true";
  const runtime::ProcessRequest request =
      runtime::WslExecutor::BuildRequest(command);

  Assert::IsTrue(request.arguments.size() == 4);
  Assert::AreEqual(L"--cd", request.arguments[0].c_str());
  Assert::AreEqual(L"~", request.arguments[1].c_str());
  Assert::AreEqual(L"--exec", request.arguments[2].c_str());
  Assert::AreEqual(L"/usr/bin/true", request.arguments[3].c_str());
}
}
;

}  // namespace designpp::tests

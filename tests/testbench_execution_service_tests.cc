// Copyright 2026 The Design++ Authors

#include <CppUnitTest.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "designpp/application/testbench_execution_service.h"

using Microsoft::VisualStudio::CppUnitTestFramework::Assert;

namespace designpp::tests {
namespace {

class FakeExecutionHandle final : public runtime::ExecutionHandle {
 public:
  void Cancel() noexcept override { cancelled = true; }
  [[nodiscard]] bool IsRunning() const noexcept override { return !cancelled; }
  [[nodiscard]] runtime::InputWriteResult WriteInput(
      std::string bytes) override {
    input.push_back(std::move(bytes));
    return cancelled ? runtime::InputWriteResult::kClosed
                     : runtime::InputWriteResult::kAccepted;
  }

  bool cancelled = false;
  std::vector<std::string> input;
};

class FakeExecutionProvider final : public runtime::ExecutionProvider {
 public:
  [[nodiscard]] runtime::ExecutionStartResult Start(
      const runtime::WslCommand&, runtime::ExecutionOutputCallback output,
      runtime::ExecutionCompletionCallback complete) override {
    output_ = std::move(output);
    complete_ = std::move(complete);
    handle = std::make_unique<FakeExecutionHandle>();
    return {std::make_unique<FakeExecutionHandle>(), core::Status::Success()};
  }

  void EmitOutput(std::string text) { output_(std::move(text)); }

  void Complete(runtime::ProcessResult result) { complete_(std::move(result)); }

  std::unique_ptr<FakeExecutionHandle> handle;
  runtime::ExecutionOutputCallback output_;
  runtime::ExecutionCompletionCallback complete_;
};

runtime::WslCommand SampleCommand() {
  runtime::WslCommand command;
  command.program = L"iverilog";
  return command;
}

}  // namespace

TEST_CLASS(TestbenchExecutionServiceTests){
  public : TEST_METHOD(ForwardsOutputAndDeliversTerminalEventOnce){
      FakeExecutionProvider provider;
application::TestbenchExecutionService service(&provider);
std::vector<application::TestbenchExecutionEvent> events;
Assert::IsTrue(service
                   .Start(application::TestbenchOperation::kSimulation,
                          application::TestbenchExecutionState::kCompiling, 17,
                          SampleCommand(),
                          [&](application::TestbenchExecutionEvent event) {
                            events.push_back(std::move(event));
                          })
                   .Ok());
provider.EmitOutput("compile output\n");
runtime::ProcessResult result;
result.started = true;
provider.Complete(result);
provider.Complete(result);

Assert::AreEqual(static_cast<std::size_t>(2), events.size());
Assert::IsTrue(events[0].kind ==
               application::TestbenchExecutionEventKind::kOutput);
Assert::IsTrue(events[1].kind ==
               application::TestbenchExecutionEventKind::kCompleted);
Assert::IsFalse(service.Snapshot().active);
Assert::IsTrue(service.Snapshot().state ==
               application::TestbenchExecutionState::kCompleted);
}  // namespace designpp::tests

TEST_METHOD(CancelRejectsLateOutputAndPreservesTerminalCancellation) {
  FakeExecutionProvider provider;
  application::TestbenchExecutionService service(&provider);
  std::vector<application::TestbenchExecutionEvent> events;
  Assert::IsTrue(service
                     .Start(application::TestbenchOperation::kDebug,
                            application::TestbenchExecutionState::kRunning, 18,
                            SampleCommand(),
                            [&](application::TestbenchExecutionEvent event) {
                              events.push_back(std::move(event));
                            })
                     .Ok());
  service.Cancel();
  runtime::ProcessResult result;
  result.started = true;
  result.cancelled = true;
  provider.Complete(result);
  provider.EmitOutput("late output");

  Assert::AreEqual(static_cast<std::size_t>(1), events.size());
  Assert::IsTrue(events[0].result.cancelled);
  Assert::IsTrue(service.Snapshot().state ==
                 application::TestbenchExecutionState::kCancelled);
}

TEST_METHOD(RejectsConcurrentOperationAndWritesInteractiveInput) {
  FakeExecutionProvider provider;
  application::TestbenchExecutionService service(&provider);
  Assert::IsTrue(service
                     .Start(application::TestbenchOperation::kDebug,
                            application::TestbenchExecutionState::kRunning, 19,
                            SampleCommand(),
                            [](application::TestbenchExecutionEvent) {})
                     .Ok());
  Assert::IsFalse(service
                      .Start(application::TestbenchOperation::kSimulation,
                             application::TestbenchExecutionState::kRunning, 20,
                             SampleCommand(),
                             [](application::TestbenchExecutionEvent) {})
                      .Ok());
  Assert::IsTrue(service.WriteInput("step\n") ==
                 runtime::InputWriteResult::kAccepted);
}
}
;

}  // namespace designpp::tests

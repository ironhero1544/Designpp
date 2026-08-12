// Copyright 2026 The Design++ Authors

#include <CppUnitTest.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "designpp/application/synthesis_run_service.h"

using Microsoft::VisualStudio::CppUnitTestFramework::Assert;

namespace designpp::tests {
namespace {

class FakeHandle final : public runtime::ExecutionHandle {
 public:
  void Cancel() noexcept override { cancelled = true; }
  [[nodiscard]] bool IsRunning() const noexcept override { return !cancelled; }
  [[nodiscard]] runtime::InputWriteResult WriteInput(std::string) override {
    return runtime::InputWriteResult::kNotInteractive;
  }

  bool cancelled = false;
};

class FakeProvider final : public runtime::ExecutionProvider {
 public:
  [[nodiscard]] runtime::ExecutionStartResult Start(
      const runtime::WslCommand&, runtime::ExecutionOutputCallback output,
      runtime::ExecutionCompletionCallback complete) override {
    output_ = std::move(output);
    complete_ = std::move(complete);
    return {std::make_unique<FakeHandle>(), core::Status::Success()};
  }

  void Complete(runtime::ProcessResult result) { complete_(std::move(result)); }

  runtime::ExecutionOutputCallback output_;
  runtime::ExecutionCompletionCallback complete_;
};

application::SynthesisRunRequest SampleRequest() {
  application::SynthesisRunRequest request;
  request.project.id = "project";
  request.project.top_module = "top";
  request.library_directory = L"C:\\designpp-test-library";
  request.cell_directory = L"C:\\designpp-test-library\\cells\\cell";
  request.generation = 41;
  return request;
}

}  // namespace

TEST_CLASS(SynthesisRunServiceTests){
  public : TEST_METHOD(ProbeFailureDeliversTerminalEventExactlyOnce){
      FakeProvider provider;
application::SynthesisRunService service(&provider);
std::vector<application::SynthesisRunEvent> events;
Assert::IsTrue(service
                   .Start(SampleRequest(),
                          [&](application::SynthesisRunEvent event) {
                            events.push_back(std::move(event));
                          })
                   .Ok());
runtime::ProcessResult failure;
failure.started = true;
failure.exit_code = 1;
provider.Complete(failure);
provider.Complete(failure);

std::size_t terminal_count = 0;
for (const auto& event : events) {
  if (event.kind == application::SynthesisRunEventKind::kCompleted) {
    ++terminal_count;
  }
}
Assert::AreEqual(static_cast<std::size_t>(1), terminal_count);
Assert::IsFalse(service.IsActive());
Assert::IsTrue(service.State() == application::SynthesisRunState::kFailed);
}  // namespace designpp::tests

TEST_METHOD(CancelledProbeCompletesAsCancelled) {
  FakeProvider provider;
  application::SynthesisRunService service(&provider);
  std::vector<application::SynthesisRunEvent> events;
  Assert::IsTrue(service
                     .Start(SampleRequest(),
                            [&](application::SynthesisRunEvent event) {
                              events.push_back(std::move(event));
                            })
                     .Ok());
  service.Cancel();
  runtime::ProcessResult cancelled;
  cancelled.started = true;
  cancelled.cancelled = true;
  provider.Complete(cancelled);

  Assert::IsFalse(service.IsActive());
  Assert::IsTrue(service.State() == application::SynthesisRunState::kCancelled);
  Assert::IsTrue(events.back().kind ==
                 application::SynthesisRunEventKind::kCompleted);
  Assert::IsTrue(events.back().status.code == core::ErrorCode::kCancelled);
}
}
;

}  // namespace designpp::tests

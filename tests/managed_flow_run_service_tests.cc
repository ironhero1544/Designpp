// Copyright 2026 The Design++ Authors

#include <CppUnitTest.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

#include "designpp/application/managed_flow_run_service.h"
#include "synthesis_test_support.h"

using Microsoft::VisualStudio::CppUnitTestFramework::Assert;

namespace designpp::tests {
namespace {

class ManagedFlowCollector final {
 public:
  void Add(application::ManagedFlowRunEvent event) {
    {
      std::scoped_lock lock(mutex_);
      events_.push_back(std::move(event));
    }
    changed_.notify_all();
  }

  [[nodiscard]] bool WaitForTerminal() {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, std::chrono::seconds(8),
                             [&] { return TerminalCountLocked() != 0; });
  }

  [[nodiscard]] std::size_t TerminalCount() const {
    std::scoped_lock lock(mutex_);
    return TerminalCountLocked();
  }

  [[nodiscard]] application::ManagedFlowRunEvent Terminal() const {
    std::scoped_lock lock(mutex_);
    const auto found =
        std::find_if(events_.begin(), events_.end(), [](const auto& event) {
          return event.kind == application::ManagedFlowRunEventKind::kCompleted;
        });
    return found == events_.end() ? application::ManagedFlowRunEvent{} : *found;
  }

 private:
  [[nodiscard]] std::size_t TerminalCountLocked() const {
    return static_cast<std::size_t>(
        std::count_if(events_.begin(), events_.end(), [](const auto& event) {
          return event.kind == application::ManagedFlowRunEventKind::kCompleted;
        }));
  }

  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::vector<application::ManagedFlowRunEvent> events_;
};

application::ManagedFlowRunRequest MakeRequest(
    const TemporarySynthesisWorkspace& workspace) {
  const application::SynthesisRunRequest synthesis = workspace.Request();
  application::ManagedFlowRunRequest request;
  request.project = synthesis.project;
  request.project.id = "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa";
  request.project.library_id = "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb";
  request.project.cell_id = "cccccccc-cccc-cccc-cccc-cccccccccccc";
  request.project.created_utc = "2026-08-24T00:00:00Z";
  request.project.modified_utc = request.project.created_utc;
  request.project.physical_implementation.clock_ports = {"clk"};
  request.profile.id = "dddddddd-dddd-dddd-dddd-dddddddddddd";
  request.profile.name = "OpenLane";
  request.profile.openlane_root = "/home/test/openlane2";
  request.profile.pdk_root = "/home/test/.volare";
  request.profile.cpu_budget = 1;
  request.sources = synthesis.sources;
  request.library_directory = synthesis.library_directory;
  request.cell_directory = synthesis.cell_directory;
  request.generation = synthesis.generation;
  return request;
}

std::filesystem::path WslToWindows(std::wstring_view path) {
  if (path.size() < 7 || !path.starts_with(L"/mnt/") || path[6] != L'/') {
    return {};
  }
  std::wstring windows{static_cast<wchar_t>(std::towupper(path[5])), L':'};
  windows.append(path.substr(6));
  std::replace(windows.begin(), windows.end(), L'/', L'\\');
  return windows;
}

void WriteCollectedArtifacts(const runtime::WslCommand& copy_command,
                             bool violations = false) {
  const std::filesystem::path root =
      WslToWindows(copy_command.arguments.back());
  std::filesystem::create_directories(root / L"final");
  const auto write = [&root](const wchar_t* path, std::string_view contents) {
    const std::filesystem::path destination = root / path;
    std::filesystem::create_directories(destination.parent_path());
    std::ofstream(destination, std::ios::binary) << contents;
  };
  write(L"resolved.json", "{}\n");
  write(L"state_out.json", "{}\n");
  write(L"metrics.json", violations ? "{\"timing__setup__wns\":-0.1,"
                                      "\"magic__drc_error__count\":0}"
                                    : "{\"timing__setup__wns\":0.1,"
                                      "\"timing__setup__tns\":0.0,"
                                      "\"timing__hold__wns\":0.0,"
                                      "\"timing__hold__tns\":0.0,"
                                      "\"route__antenna_violation__count\":0,"
                                      "\"magic__drc_error__count\":0,"
                                      "\"klayout__xor_error__count\":0,"
                                      "\"design__lvs_error__count\":0}");
  write(L"metrics.csv", "metric,value\n");
  write(L"final/top.gds", "gds");
  write(L"final/top.def", "def");
  write(L"final/top.lef", "lef");
  write(L"final/top.odb", "odb");
  write(L"final/top.v", "module top; endmodule\n");
  write(L"final/top.sdf", "sdf");
  write(L"final/top.spef", "spef");
}

runtime::ProcessResult Success(std::string output = {}) {
  runtime::ProcessResult result;
  result.started = true;
  result.exit_code = 0;
  result.output = std::move(output);
  return result;
}

}  // namespace

TEST_CLASS(ManagedFlowRunServiceTests){
  public : TEST_METHOD(CleanFlowPreservesArtifactsAndCompletesExactlyOnce){
      TemporarySynthesisWorkspace workspace;
ControlledExecutionProvider provider;
application::ManagedFlowRunService service(&provider);
ManagedFlowCollector collector;
Assert::IsTrue(service
                   .Start(MakeRequest(workspace),
                          [&collector](auto event) {
                            collector.Add(std::move(event));
                          })
                   .Ok());
Assert::IsTrue(provider.WaitForStarts(1));
provider.Complete(0, Success("OpenLane v2.3.10\n"));
Assert::IsTrue(provider.WaitForStarts(2));
provider.Complete(1, Success("Starting step Verilator.Lint\n"));
Assert::IsFalse(provider.DestroyedDuringCompletionCallback(1));
Assert::IsTrue(provider.WaitForStarts(3));
provider.Complete(2, Success("Starting step KLayout.StreamOut\n"), 2);
Assert::IsTrue(provider.WaitForStarts(4));
WriteCollectedArtifacts(provider.Command(3));
provider.Complete(3, Success(), 2);
Assert::IsTrue(collector.WaitForTerminal());
Assert::AreEqual(static_cast<std::size_t>(1), collector.TerminalCount());
const auto terminal = collector.Terminal();
Assert::IsTrue(terminal.status.Ok());
Assert::IsNotNull(terminal.run.get());
Assert::IsTrue(terminal.run->outcome.process_succeeded);
Assert::IsTrue(terminal.run->outcome.result_succeeded);
Assert::IsTrue(std::filesystem::is_regular_file(terminal.run->directory /
                                                L"artifacts" / L"final.gds"));
}  // namespace designpp::tests

TEST_METHOD(PhysicalViolationSeparatesProcessAndResultFailure) {
  TemporarySynthesisWorkspace workspace;
  ControlledExecutionProvider provider;
  application::ManagedFlowRunService service(&provider);
  ManagedFlowCollector collector;
  Assert::IsTrue(
      service
          .Start(MakeRequest(workspace),
                 [&collector](auto event) { collector.Add(std::move(event)); })
          .Ok());
  Assert::IsTrue(provider.WaitForStarts(1));
  provider.Complete(0, Success("OpenLane v2.3.10\n"));
  Assert::IsTrue(provider.WaitForStarts(2));
  provider.Complete(1, Success());
  Assert::IsTrue(provider.WaitForStarts(3));
  provider.Complete(2, Success());
  Assert::IsTrue(provider.WaitForStarts(4));
  WriteCollectedArtifacts(provider.Command(3), true);
  provider.Complete(3, Success());
  Assert::IsTrue(collector.WaitForTerminal());
  const auto terminal = collector.Terminal();
  Assert::IsFalse(terminal.status.Ok());
  Assert::IsTrue(terminal.run->outcome.process_succeeded);
  Assert::IsFalse(terminal.run->outcome.result_succeeded);
}

TEST_METHOD(CancelAndDuplicateCallbackDeliverTerminalExactlyOnce) {
  TemporarySynthesisWorkspace workspace;
  ControlledExecutionProvider provider;
  application::ManagedFlowRunService service(&provider);
  ManagedFlowCollector collector;
  Assert::IsTrue(
      service
          .Start(MakeRequest(workspace),
                 [&collector](auto event) { collector.Add(std::move(event)); })
          .Ok());
  Assert::IsTrue(provider.WaitForStarts(1));
  service.Cancel();
  runtime::ProcessResult cancelled;
  cancelled.started = true;
  cancelled.cancelled = true;
  provider.Complete(0, cancelled, 2);
  Assert::IsTrue(collector.WaitForTerminal());
  Assert::AreEqual(static_cast<std::size_t>(1), collector.TerminalCount());
  Assert::IsTrue(collector.Terminal().state ==
                 application::ManagedFlowRunState::kCancelled);
}

TEST_METHOD(ProbeStartFailureCompletesExactlyOnce) {
  TemporarySynthesisWorkspace workspace;
  ControlledExecutionProvider provider;
  provider.FailNextStart();
  application::ManagedFlowRunService service(&provider);
  ManagedFlowCollector collector;
  Assert::IsTrue(
      service
          .Start(MakeRequest(workspace),
                 [&collector](auto event) { collector.Add(std::move(event)); })
          .Ok());
  Assert::IsTrue(collector.WaitForTerminal());
  Assert::AreEqual<std::size_t>(1U, collector.TerminalCount());
  Assert::IsFalse(collector.Terminal().status.Ok());
}

TEST_METHOD(ProbeNonzeroAndDuplicateCompletionAreTerminalOnce) {
  TemporarySynthesisWorkspace workspace;
  ControlledExecutionProvider provider;
  application::ManagedFlowRunService service(&provider);
  ManagedFlowCollector collector;
  Assert::IsTrue(
      service
          .Start(MakeRequest(workspace),
                 [&collector](auto event) { collector.Add(std::move(event)); })
          .Ok());
  Assert::IsTrue(provider.WaitForStarts(1));
  runtime::ProcessResult failure = Success("OpenLane v2.2.0\n");
  failure.exit_code = 3;
  provider.Complete(0, failure, 2);
  Assert::IsTrue(collector.WaitForTerminal());
  Assert::AreEqual<std::size_t>(1U, collector.TerminalCount());
}

TEST_METHOD(MissingFinalArtifactsFailWithoutLosingProcessSuccess) {
  TemporarySynthesisWorkspace workspace;
  ControlledExecutionProvider provider;
  application::ManagedFlowRunService service(&provider);
  ManagedFlowCollector collector;
  Assert::IsTrue(
      service
          .Start(MakeRequest(workspace),
                 [&collector](auto event) { collector.Add(std::move(event)); })
          .Ok());
  Assert::IsTrue(provider.WaitForStarts(1));
  provider.Complete(0, Success("OpenLane v2.3.10\n"));
  Assert::IsTrue(provider.WaitForStarts(2));
  provider.Complete(1, Success());
  Assert::IsTrue(provider.WaitForStarts(3));
  provider.Complete(2, Success());
  Assert::IsTrue(provider.WaitForStarts(4));
  provider.Complete(3, Success());
  Assert::IsTrue(collector.WaitForTerminal());
  const auto terminal = collector.Terminal();
  Assert::IsFalse(terminal.status.Ok());
  Assert::IsTrue(terminal.run->outcome.process_succeeded);
  Assert::IsFalse(terminal.run->outcome.result_succeeded);
}

TEST_METHOD(ChangedFingerprintRejectsResumeBeforeExecution) {
  TemporarySynthesisWorkspace workspace;
  ControlledExecutionProvider provider;
  application::ManagedFlowRunService service(&provider);
  ManagedFlowCollector collector;
  application::ManagedFlowRunRequest request = MakeRequest(workspace);
  request.resume = application::ManagedFlowResumeRequest{
      "parent", "lineage", "OpenROAD.GeneratePDN", "stale-fingerprint",
      std::string(64, 'a')};
  Assert::IsTrue(
      service
          .Start(std::move(request),
                 [&collector](auto event) { collector.Add(std::move(event)); })
          .Ok());
  Assert::IsTrue(provider.WaitForStarts(1));
  provider.Complete(0, Success("OpenLane v2.3.10\n"));
  Assert::IsTrue(collector.WaitForTerminal());
  Assert::IsFalse(collector.Terminal().status.Ok());
  Assert::AreEqual<std::size_t>(1U, collector.TerminalCount());
}
}
;

}  // namespace designpp::tests

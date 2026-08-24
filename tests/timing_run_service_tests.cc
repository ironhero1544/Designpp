// Copyright 2026 The Design++ Authors

#include <CppUnitTest.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

#include "designpp/application/synthesis_fingerprint.h"
#include "designpp/application/timing_run_service.h"
#include "synthesis_test_support.h"

using Microsoft::VisualStudio::CppUnitTestFramework::Assert;

namespace designpp::tests {
namespace {

class TimingEventCollector final {
 public:
  void Add(application::TimingRunEvent event) {
    {
      std::scoped_lock lock(mutex_);
      events_.push_back(std::move(event));
    }
    changed_.notify_all();
  }

  [[nodiscard]] bool WaitForTerminal() {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, std::chrono::seconds(5), [&] {
      return std::any_of(events_.begin(), events_.end(), [](const auto& event) {
        return event.kind == application::TimingRunEventKind::kCompleted;
      });
    });
  }

  [[nodiscard]] application::TimingRunEvent Terminal() const {
    std::scoped_lock lock(mutex_);
    const auto found =
        std::find_if(events_.begin(), events_.end(), [](const auto& event) {
          return event.kind == application::TimingRunEventKind::kCompleted;
        });
    return found == events_.end() ? application::TimingRunEvent{} : *found;
  }

  [[nodiscard]] std::size_t TerminalCount() const {
    std::scoped_lock lock(mutex_);
    return static_cast<std::size_t>(
        std::count_if(events_.begin(), events_.end(), [](const auto& event) {
          return event.kind == application::TimingRunEventKind::kCompleted;
        }));
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::vector<application::TimingRunEvent> events_;
};

application::TimingRunRequest MakeTimingRequest(
    const TemporarySynthesisWorkspace& workspace) {
  const application::SynthesisRunRequest synthesis = workspace.Request();
  application::TimingRunRequest request;
  request.project = synthesis.project;
  request.sources = synthesis.sources;
  request.library_directory = synthesis.library_directory;
  request.cell_directory = synthesis.cell_directory;
  request.generation = synthesis.generation;
  const std::filesystem::path constraints =
      request.library_directory / L"constraints";
  std::filesystem::create_directories(constraints);
  std::ofstream(constraints / L"timing.lib", std::ios::binary)
      << "library(test) {}\n";
  std::ofstream(constraints / L"top.sdc", std::ios::binary)
      << "create_clock -period 10 clk\n";
  application::ResolvedSource liberty;
  liberty.view_kind = core::ViewKind::kConstraints;
  liberty.relative_path = "constraints/timing.lib";
  liberty.windows_path = constraints / L"timing.lib";
  liberty.library_directory = request.library_directory;
  // Timing selection is independent of the RTL Source Set enable state.
  liberty.enabled = false;
  liberty.exists = true;
  request.sources.push_back(liberty);
  application::ResolvedSource sdc = liberty;
  sdc.relative_path = "constraints/top.sdc";
  sdc.windows_path = constraints / L"top.sdc";
  request.sources.push_back(sdc);
  request.project.synthesis.liberty_paths = {liberty.relative_path};
  request.project.timing.liberty_paths = {liberty.relative_path};
  request.project.timing.sdc_path = sdc.relative_path;
  request.project.timing.corner_name = "slow";
  return request;
}

void CreateCompatibleSynthesisRun(
    const application::TimingRunRequest& request) {
  auto fingerprint = application::CalculateSynthesisFingerprint(
      request.project, request.sources, request.library_directory);
  Assert::IsTrue(fingerprint.Ok());
  application::RunStore store;
  auto begun = store.Begin(request.cell_directory, request.project, "Synthesis",
                           "Yosys", "Yosys 0.33");
  Assert::IsTrue(begun.Ok());
  application::RunRecord run = std::move(begun).Value();
  const std::filesystem::path netlist =
      run.directory / L"artifacts" / L"netlist.v";
  const std::filesystem::path summary =
      run.directory / L"reports" / L"synthesis-summary.json";
  std::ofstream(netlist, std::ios::binary)
      << "module top(input a, output y); assign y = a; endmodule\n";
  std::ofstream(summary, std::ios::binary)
      << "{\"schema_version\":2,\"top_module\":\"top\","
         "\"configuration_sha256\":\""
      << fingerprint.Value().configuration_sha256 << "\"}";
  std::vector<application::RunArtifact> artifacts{
      {"netlist", "verilog", "artifacts/netlist.v",
       std::filesystem::file_size(netlist),
       fingerprint.Value().configuration_sha256, false},
      {"summary", "json", "reports/synthesis-summary.json",
       std::filesystem::file_size(summary),
       fingerprint.Value().configuration_sha256, false}};
  application::RunOutcome outcome;
  outcome.process_succeeded = true;
  outcome.result_succeeded = true;
  outcome.summary_relative_path = "reports/synthesis-summary.json";
  Assert::IsTrue(store
                     .Complete(&run, application::RunStatus::kSucceeded, 0, {},
                               std::move(artifacts), std::move(outcome))
                     .Ok());
}

std::string MetricsOutput(double setup_wns, double hold_wns) {
  return "DESIGNPP_SETUP_WNS_BEGIN\n" + std::to_string(setup_wns) +
         "\nDESIGNPP_SETUP_WNS_END\nDESIGNPP_SETUP_TNS_BEGIN\n" +
         std::to_string(setup_wns) +
         "\nDESIGNPP_SETUP_TNS_END\nDESIGNPP_HOLD_WNS_BEGIN\n" +
         std::to_string(hold_wns) +
         "\nDESIGNPP_HOLD_WNS_END\nDESIGNPP_HOLD_TNS_BEGIN\n" +
         std::to_string(hold_wns) +
         "\nDESIGNPP_HOLD_TNS_END\nDESIGNPP_SETUP_PATHS_BEGIN\n1\n"
         "DESIGNPP_SETUP_PATHS_END\nDESIGNPP_HOLD_PATHS_BEGIN\n1\n"
         "DESIGNPP_HOLD_PATHS_END\n";
}

void WriteTimingReport(const runtime::WslCommand& command,
                       bool setup_violation) {
  const std::filesystem::path directory = WindowsWorkingDirectory(command);
  std::ofstream report(directory / L"timing.rpt", std::ios::binary);
  report << "DESIGNPP_SETUP_BEGIN\n";
  if (setup_violation) {
    report << "Startpoint: a\nEndpoint: y\nslack (VIOLATED) -0.5\n";
  }
  report << "DESIGNPP_SETUP_END\nDESIGNPP_HOLD_BEGIN\n"
            "DESIGNPP_HOLD_END\n";
}

}  // namespace

TEST_CLASS(TimingRunServiceTests){
  public : TEST_METHOD(EmptyManagedSdcIsRejectedBeforeRunLookup){
      TemporarySynthesisWorkspace workspace;
auto request = MakeTimingRequest(workspace);
const auto sdc = std::find_if(
    request.sources.begin(), request.sources.end(), [](const auto& source) {
      return source.windows_path.extension() == L".sdc";
    });
Assert::IsTrue(sdc != request.sources.end());
std::ofstream(sdc->windows_path, std::ios::binary | std::ios::trunc);

auto compatible = application::ResolveCompatibleSynthesisRun(request);
Assert::IsFalse(compatible.Ok());
Assert::IsTrue(compatible.GetStatus().code ==
               core::ErrorCode::kInvalidArgument);
Assert::IsTrue(compatible.GetStatus().message.find("missing or empty") !=
               std::string::npos);
}  // namespace designpp::tests

public:
TEST_METHOD(MissingCompatibleSynthesisFailsBeforeExecution) {
  TemporarySynthesisWorkspace workspace;
  auto request = MakeTimingRequest(workspace);
  ControlledExecutionProvider provider;
  application::TimingRunService service(&provider);
  TimingEventCollector collector;
  Assert::IsTrue(service
                     .Start(std::move(request),
                            [&collector](application::TimingRunEvent event) {
                              collector.Add(std::move(event));
                            })
                     .Ok());
  Assert::IsTrue(provider.WaitForStarts(1));
  provider.Complete(0, SuccessfulProcess("OpenSTA 2.6.0\n"));
  Assert::IsTrue(collector.WaitForTerminal());
  Assert::IsTrue(collector.Terminal().status.code ==
                 core::ErrorCode::kNotFound);
  Assert::AreEqual<std::size_t>(1U, collector.TerminalCount());
}  // namespace designpp::tests

TEST_METHOD(CleanSetupAndHoldCompletesSuccessfullyExactlyOnce) {
  TemporarySynthesisWorkspace workspace;
  auto request = MakeTimingRequest(workspace);
  CreateCompatibleSynthesisRun(request);
  ControlledExecutionProvider provider;
  application::TimingRunService service(&provider);
  TimingEventCollector collector;
  Assert::IsTrue(service
                     .Start(std::move(request),
                            [&collector](application::TimingRunEvent event) {
                              collector.Add(std::move(event));
                            })
                     .Ok());
  Assert::IsTrue(provider.WaitForStarts(1));
  provider.Complete(0, SuccessfulProcess("OpenSTA 2.6.0\n"));
  Assert::IsTrue(provider.WaitForStarts(2));
  WriteTimingReport(provider.Command(1), false);
  provider.Complete(1, SuccessfulProcess(MetricsOutput(0.0, 0.0)), 2);
  Assert::IsTrue(collector.WaitForTerminal());
  const auto terminal = collector.Terminal();
  Assert::IsTrue(terminal.status.Ok());
  Assert::IsTrue(terminal.metrics.Passed());
  Assert::IsTrue(terminal.run->status == application::RunStatus::kSucceeded);
  Assert::AreEqual<std::size_t>(1U, collector.TerminalCount());
}

TEST_METHOD(UnicodeLibraryInputsUseAsciiSafeTimingStaging) {
  TemporarySynthesisWorkspace workspace;
  auto request = MakeTimingRequest(workspace);
  CreateCompatibleSynthesisRun(request);
  ControlledExecutionProvider provider;
  application::TimingRunService service(&provider);
  TimingEventCollector collector;
  Assert::IsTrue(service
                     .Start(std::move(request),
                            [&collector](application::TimingRunEvent event) {
                              collector.Add(std::move(event));
                            })
                     .Ok());
  Assert::IsTrue(provider.WaitForStarts(1));
  provider.Complete(0, SuccessfulProcess("OpenSTA 2.6.0\n"));
  Assert::IsTrue(provider.WaitForStarts(2));
  const runtime::WslCommand command = provider.Command(1);
  Assert::IsTrue(command.working_directory.has_value());
  Assert::IsTrue(command.working_directory->find(L"한글") ==
                 std::wstring::npos);
  for (const std::wstring& argument : command.arguments) {
    Assert::IsTrue(argument.find(L"한글") == std::wstring::npos);
  }
  const std::filesystem::path staging = WindowsWorkingDirectory(command);
  Assert::IsTrue(std::filesystem::exists(staging / L"netlist.v"));
  Assert::IsTrue(std::filesystem::exists(staging / L"constraints.sdc"));
  Assert::IsTrue(std::filesystem::exists(staging / L"liberty-0.lib"));
  service.Cancel();
  runtime::ProcessResult cancelled;
  cancelled.started = true;
  cancelled.cancelled = true;
  provider.Complete(1, std::move(cancelled));
  Assert::IsTrue(collector.WaitForTerminal());
  Assert::AreEqual<std::size_t>(1U, collector.TerminalCount());
}

TEST_METHOD(ViolationKeepsProcessSuccessButFailsTimingResult) {
  TemporarySynthesisWorkspace workspace;
  auto request = MakeTimingRequest(workspace);
  CreateCompatibleSynthesisRun(request);
  ControlledExecutionProvider provider;
  application::TimingRunService service(&provider);
  TimingEventCollector collector;
  Assert::IsTrue(service
                     .Start(std::move(request),
                            [&collector](application::TimingRunEvent event) {
                              collector.Add(std::move(event));
                            })
                     .Ok());
  Assert::IsTrue(provider.WaitForStarts(1));
  provider.Complete(0, SuccessfulProcess("OpenSTA 2.6.0\n"));
  Assert::IsTrue(provider.WaitForStarts(2));
  WriteTimingReport(provider.Command(1), true);
  provider.Complete(1, SuccessfulProcess(MetricsOutput(-0.5, 0.0)));
  Assert::IsTrue(collector.WaitForTerminal());
  const auto terminal = collector.Terminal();
  Assert::IsFalse(terminal.status.Ok());
  Assert::IsTrue(terminal.run->outcome.process_succeeded);
  Assert::IsFalse(terminal.run->outcome.result_succeeded);
  Assert::IsTrue(terminal.run->status == application::RunStatus::kFailed);
  Assert::AreEqual<std::size_t>(1U, terminal.metrics.violations.size());
  const std::filesystem::path summary_path =
      terminal.run->directory / L"reports" / L"timing-summary.json";
  std::ifstream summary_stream(summary_path, std::ios::binary);
  const std::string summary((std::istreambuf_iterator<char>(summary_stream)),
                            std::istreambuf_iterator<char>());
  Assert::IsTrue(summary.find("\"schema_version\":2") != std::string::npos);
  Assert::IsTrue(summary.find("\"setup_check_violations\":1") !=
                 std::string::npos);
  Assert::IsTrue(summary.find("\"recovery_violations\":0") !=
                 std::string::npos);
}

TEST_METHOD(RunningCancellationAndDuplicateCallbackCompleteExactlyOnce) {
  TemporarySynthesisWorkspace workspace;
  auto request = MakeTimingRequest(workspace);
  CreateCompatibleSynthesisRun(request);
  ControlledExecutionProvider provider;
  application::TimingRunService service(&provider);
  TimingEventCollector collector;
  Assert::IsTrue(service
                     .Start(std::move(request),
                            [&collector](application::TimingRunEvent event) {
                              collector.Add(std::move(event));
                            })
                     .Ok());
  Assert::IsTrue(provider.WaitForStarts(1));
  provider.Complete(0, SuccessfulProcess("OpenSTA 2.6.0\n"));
  Assert::IsTrue(provider.WaitForStarts(2));
  service.Cancel();
  Assert::IsTrue(provider.Cancelled(1));
  runtime::ProcessResult cancelled = SuccessfulProcess();
  cancelled.cancelled = true;
  provider.Complete(1, cancelled, 2);
  Assert::IsTrue(collector.WaitForTerminal());
  Assert::AreEqual<std::size_t>(1U, collector.TerminalCount());
  Assert::IsTrue(collector.Terminal().status.code ==
                 core::ErrorCode::kCancelled);
}

TEST_METHOD(ProbeStartFailureCompletesExactlyOnce) {
  TemporarySynthesisWorkspace workspace;
  auto request = MakeTimingRequest(workspace);
  ControlledExecutionProvider provider;
  provider.FailNextStart();
  application::TimingRunService service(&provider);
  TimingEventCollector collector;

  Assert::IsTrue(service
                     .Start(std::move(request),
                            [&collector](application::TimingRunEvent event) {
                              collector.Add(std::move(event));
                            })
                     .Ok());

  Assert::IsTrue(collector.WaitForTerminal());
  Assert::IsTrue(collector.Terminal().status.code == core::ErrorCode::kIoError);
  Assert::AreEqual<std::size_t>(1U, collector.TerminalCount());
}

TEST_METHOD(ProbeNonzeroAndDuplicateCallbackCompleteExactlyOnce) {
  TemporarySynthesisWorkspace workspace;
  auto request = MakeTimingRequest(workspace);
  ControlledExecutionProvider provider;
  application::TimingRunService service(&provider);
  TimingEventCollector collector;
  Assert::IsTrue(service
                     .Start(std::move(request),
                            [&collector](application::TimingRunEvent event) {
                              collector.Add(std::move(event));
                            })
                     .Ok());
  Assert::IsTrue(provider.WaitForStarts(1));
  runtime::ProcessResult failure = SuccessfulProcess("probe failed\n");
  failure.exit_code = 127;

  provider.Complete(0, failure, 2);

  Assert::IsTrue(collector.WaitForTerminal());
  Assert::IsTrue(collector.Terminal().status.code ==
                 core::ErrorCode::kNotFound);
  Assert::AreEqual<std::size_t>(1U, collector.TerminalCount());
}

TEST_METHOD(ExecutionStartFailureRecordsFailedRun) {
  TemporarySynthesisWorkspace workspace;
  auto request = MakeTimingRequest(workspace);
  CreateCompatibleSynthesisRun(request);
  ControlledExecutionProvider provider;
  application::TimingRunService service(&provider);
  TimingEventCollector collector;
  Assert::IsTrue(service
                     .Start(std::move(request),
                            [&collector](application::TimingRunEvent event) {
                              collector.Add(std::move(event));
                            })
                     .Ok());
  Assert::IsTrue(provider.WaitForStarts(1));
  provider.FailNextStart();

  provider.Complete(0, SuccessfulProcess("OpenSTA 2.6.0\n"));

  Assert::IsTrue(collector.WaitForTerminal());
  const application::TimingRunEvent terminal = collector.Terminal();
  Assert::IsFalse(terminal.status.Ok());
  Assert::IsNotNull(terminal.run.get());
  Assert::IsTrue(terminal.run->status == application::RunStatus::kFailed);
  Assert::AreEqual<std::size_t>(1U, collector.TerminalCount());
}

TEST_METHOD(NonzeroExecutionPreservesPartialReportAndRawLog) {
  TemporarySynthesisWorkspace workspace;
  auto request = MakeTimingRequest(workspace);
  CreateCompatibleSynthesisRun(request);
  ControlledExecutionProvider provider;
  application::TimingRunService service(&provider);
  TimingEventCollector collector;
  Assert::IsTrue(service
                     .Start(std::move(request),
                            [&collector](application::TimingRunEvent event) {
                              collector.Add(std::move(event));
                            })
                     .Ok());
  Assert::IsTrue(provider.WaitForStarts(1));
  provider.Complete(0, SuccessfulProcess("OpenSTA 2.6.0\n"));
  Assert::IsTrue(provider.WaitForStarts(2));
  WriteTimingReport(provider.Command(1), false);
  runtime::ProcessResult failure = SuccessfulProcess("OpenSTA failed\n");
  failure.exit_code = 2;

  provider.Output(1, "OpenSTA failed\n");
  provider.Complete(1, failure);

  Assert::IsTrue(collector.WaitForTerminal());
  const application::TimingRunEvent terminal = collector.Terminal();
  Assert::IsNotNull(terminal.run.get());
  Assert::IsFalse(terminal.run->outcome.process_succeeded);
  Assert::IsTrue(terminal.run->status == application::RunStatus::kFailed);
  const auto report = std::find_if(
      terminal.run->artifacts.begin(), terminal.run->artifacts.end(),
      [](const application::RunArtifact& artifact) {
        return artifact.kind == "report";
      });
  Assert::IsTrue(report != terminal.run->artifacts.end());
  Assert::IsTrue(report->partial);
  Assert::IsTrue(std::filesystem::file_size(terminal.run->directory / L"logs" /
                                            L"raw.log") > 0);
}

TEST_METHOD(MissingAndEmptyReportFailArtifactValidation) {
  for (const bool create_empty : {false, true}) {
    TemporarySynthesisWorkspace workspace;
    auto request = MakeTimingRequest(workspace);
    CreateCompatibleSynthesisRun(request);
    ControlledExecutionProvider provider;
    application::TimingRunService service(&provider);
    TimingEventCollector collector;
    Assert::IsTrue(service
                       .Start(std::move(request),
                              [&collector](application::TimingRunEvent event) {
                                collector.Add(std::move(event));
                              })
                       .Ok());
    Assert::IsTrue(provider.WaitForStarts(1));
    provider.Complete(0, SuccessfulProcess("OpenSTA 2.6.0\n"));
    Assert::IsTrue(provider.WaitForStarts(2));
    if (create_empty) {
      std::ofstream(
          WindowsWorkingDirectory(provider.Command(1)) / L"timing.rpt",
          std::ios::binary | std::ios::trunc);
    }

    provider.Complete(1, SuccessfulProcess(MetricsOutput(0.0, 0.0)));

    Assert::IsTrue(collector.WaitForTerminal());
    Assert::IsTrue(collector.Terminal().status.code ==
                   core::ErrorCode::kNotFound);
    Assert::IsTrue(collector.Terminal().run->status ==
                   application::RunStatus::kFailed);
  }
}

TEST_METHOD(MalformedMetricsFailWhileRawReportRemainsAvailable) {
  TemporarySynthesisWorkspace workspace;
  auto request = MakeTimingRequest(workspace);
  CreateCompatibleSynthesisRun(request);
  ControlledExecutionProvider provider;
  application::TimingRunService service(&provider);
  TimingEventCollector collector;
  Assert::IsTrue(service
                     .Start(std::move(request),
                            [&collector](application::TimingRunEvent event) {
                              collector.Add(std::move(event));
                            })
                     .Ok());
  Assert::IsTrue(provider.WaitForStarts(1));
  provider.Complete(0, SuccessfulProcess("OpenSTA 2.6.0\n"));
  Assert::IsTrue(provider.WaitForStarts(2));
  WriteTimingReport(provider.Command(1), false);

  provider.Complete(1, SuccessfulProcess("malformed metrics\n"));

  Assert::IsTrue(collector.WaitForTerminal());
  const application::TimingRunEvent terminal = collector.Terminal();
  Assert::IsFalse(terminal.status.Ok());
  Assert::IsNotNull(terminal.run.get());
  Assert::IsTrue(std::filesystem::exists(terminal.run->directory /
                                         L"artifacts" / L"timing.rpt"));
  Assert::IsTrue(terminal.run->status == application::RunStatus::kFailed);
}

TEST_METHOD(StaleGenerationCallbackCannotCompleteNewRun) {
  TemporarySynthesisWorkspace workspace;
  auto first_request = MakeTimingRequest(workspace);
  CreateCompatibleSynthesisRun(first_request);
  ControlledExecutionProvider provider;
  application::TimingRunService service(&provider);
  TimingEventCollector first_collector;
  Assert::IsTrue(
      service
          .Start(first_request,
                 [&first_collector](application::TimingRunEvent event) {
                   first_collector.Add(std::move(event));
                 })
          .Ok());
  Assert::IsTrue(provider.WaitForStarts(1));
  provider.Complete(0, SuccessfulProcess("OpenSTA 2.6.0\n"));
  Assert::IsTrue(provider.WaitForStarts(2));
  WriteTimingReport(provider.Command(1), false);
  provider.Complete(1, SuccessfulProcess(MetricsOutput(0.0, 0.0)));
  Assert::IsTrue(first_collector.WaitForTerminal());

  auto second_request = MakeTimingRequest(workspace);
  second_request.generation = first_request.generation + 1;
  TimingEventCollector second_collector;
  Assert::IsTrue(
      service
          .Start(std::move(second_request),
                 [&second_collector](application::TimingRunEvent event) {
                   second_collector.Add(std::move(event));
                 })
          .Ok());
  Assert::IsTrue(provider.WaitForStarts(3));
  provider.Complete(0, SuccessfulProcess("late old probe\n"), 2);
  Assert::AreEqual<std::size_t>(0U, second_collector.TerminalCount());
  provider.Complete(2, SuccessfulProcess("OpenSTA 2.6.0\n"));
  Assert::IsTrue(provider.WaitForStarts(4));
  WriteTimingReport(provider.Command(3), false);
  provider.Complete(3, SuccessfulProcess(MetricsOutput(0.0, 0.0)));
  Assert::IsTrue(second_collector.WaitForTerminal());
  Assert::AreEqual<std::size_t>(1U, second_collector.TerminalCount());
}

TEST_METHOD(ShutdownSuppressesLateProviderCallbacks) {
  TemporarySynthesisWorkspace workspace;
  auto request = MakeTimingRequest(workspace);
  ControlledExecutionProvider provider;
  application::TimingRunService service(&provider);
  TimingEventCollector collector;
  Assert::IsTrue(service
                     .Start(std::move(request),
                            [&collector](application::TimingRunEvent event) {
                              collector.Add(std::move(event));
                            })
                     .Ok());
  Assert::IsTrue(provider.WaitForStarts(1));

  service.Shutdown();
  provider.Complete(0, SuccessfulProcess("late callback\n"), 2);

  Assert::AreEqual<std::size_t>(0U, collector.TerminalCount());
  Assert::IsFalse(service.IsActive());
}
}
;

}  // namespace designpp::tests

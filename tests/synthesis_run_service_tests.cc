// Copyright 2026 The Design++ Authors

#include <CppUnitTest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "designpp/application/run_store.h"
#include "designpp/application/synthesis_run_service.h"
#include "synthesis_test_support.h"

using Microsoft::VisualStudio::CppUnitTestFramework::Assert;

namespace designpp::tests {
namespace {

class EventCollector final {
 public:
  void Add(application::SynthesisRunEvent event) {
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
        return event.kind == application::SynthesisRunEventKind::kCompleted;
      });
    });
  }

  [[nodiscard]] application::SynthesisRunEvent Terminal() const {
    std::scoped_lock lock(mutex_);
    const auto terminal =
        std::find_if(events_.begin(), events_.end(), [](const auto& event) {
          return event.kind == application::SynthesisRunEventKind::kCompleted;
        });
    return terminal == events_.end() ? application::SynthesisRunEvent{}
                                     : *terminal;
  }

  [[nodiscard]] std::size_t TerminalCount() const {
    std::scoped_lock lock(mutex_);
    return static_cast<std::size_t>(
        std::count_if(events_.begin(), events_.end(), [](const auto& event) {
          return event.kind == application::SynthesisRunEventKind::kCompleted;
        }));
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::vector<application::SynthesisRunEvent> events_;
};

void StartService(application::SynthesisRunService* service,
                  ControlledExecutionProvider* provider,
                  const TemporarySynthesisWorkspace& workspace,
                  EventCollector* collector) {
  Assert::IsTrue(service
                     ->Start(workspace.Request(),
                             [collector](application::SynthesisRunEvent event) {
                               collector->Add(std::move(event));
                             })
                     .Ok());
  Assert::IsTrue(provider->WaitForStarts(1));
}

application::SynthesisRunEvent CompleteSuccessfulRun(
    application::SynthesisRunService* service,
    ControlledExecutionProvider* provider,
    const TemporarySynthesisWorkspace& workspace,
    const ArtifactOptions& options = {}) {
  EventCollector collector;
  StartService(service, provider, workspace, &collector);
  provider->Complete(0, SuccessfulProcess("Yosys 0.33\n"));
  Assert::IsTrue(provider->WaitForStarts(2));
  WriteSynthesisArtifacts(provider->Command(1), options);
  provider->Output(1, "synthesis output\n");
  provider->Complete(1, SuccessfulProcess("synthesis output\n"));
  Assert::IsTrue(collector.WaitForTerminal());
  Assert::AreEqual<std::size_t>(1U, collector.TerminalCount());
  return collector.Terminal();
}

bool ContainsArtifact(const application::RunRecord& run,
                      std::string_view format) {
  return std::any_of(
      run.artifacts.begin(), run.artifacts.end(),
      [format](const auto& artifact) { return artifact.format == format; });
}

}  // namespace

TEST_CLASS(SynthesisRunServiceTests){
  public : TEST_METHOD(ProbeStartFailureDeliversTerminalExactlyOnce){
      TemporarySynthesisWorkspace workspace;
ControlledExecutionProvider provider;
provider.FailNextStart();
application::SynthesisRunService service(&provider);
EventCollector collector;
Assert::IsTrue(service
                   .Start(workspace.Request(),
                          [&collector](application::SynthesisRunEvent event) {
                            collector.Add(std::move(event));
                          })
                   .Ok());
Assert::IsTrue(collector.WaitForTerminal());
Assert::AreEqual<std::size_t>(1U, collector.TerminalCount());
Assert::IsTrue(service.State() == application::SynthesisRunState::kFailed);
}  // namespace designpp::tests

TEST_METHOD(ProbeFailureAndDuplicateCallbackCompleteExactlyOnce) {
  TemporarySynthesisWorkspace workspace;
  ControlledExecutionProvider provider;
  application::SynthesisRunService service(&provider);
  EventCollector collector;
  StartService(&service, &provider, workspace, &collector);
  runtime::ProcessResult failure = SuccessfulProcess("probe failed");
  failure.exit_code = 1;
  provider.Complete(0, failure, 2);
  Assert::IsTrue(collector.WaitForTerminal());
  Assert::AreEqual<std::size_t>(1U, collector.TerminalCount());
  Assert::IsFalse(service.IsActive());
}

TEST_METHOD(ProbeCancellationCancelsHandleAndCompletesOnce) {
  TemporarySynthesisWorkspace workspace;
  ControlledExecutionProvider provider;
  application::SynthesisRunService service(&provider);
  EventCollector collector;
  StartService(&service, &provider, workspace, &collector);
  service.Cancel();
  Assert::IsTrue(provider.Cancelled(0));
  runtime::ProcessResult cancelled = SuccessfulProcess();
  cancelled.cancelled = true;
  provider.Complete(0, cancelled, 2);
  Assert::IsTrue(collector.WaitForTerminal());
  Assert::AreEqual<std::size_t>(1U, collector.TerminalCount());
  Assert::IsTrue(collector.Terminal().status.code ==
                 core::ErrorCode::kCancelled);
}

TEST_METHOD(ExecutionStartFailureRecordsFailedRun) {
  TemporarySynthesisWorkspace workspace;
  ControlledExecutionProvider provider;
  application::SynthesisRunService service(&provider);
  EventCollector collector;
  StartService(&service, &provider, workspace, &collector);
  provider.FailNextStart();
  provider.Complete(0, SuccessfulProcess("Yosys 0.33\n"));
  Assert::IsTrue(collector.WaitForTerminal());
  const auto terminal = collector.Terminal();
  Assert::IsNotNull(terminal.run.get());
  Assert::IsTrue(terminal.run->status == application::RunStatus::kFailed);
  Assert::AreEqual<std::size_t>(1U, collector.TerminalCount());
}

TEST_METHOD(SuccessPreservesArtifactsSummaryMetricsAndReload) {
  TemporarySynthesisWorkspace workspace;
  ControlledExecutionProvider provider;
  application::SynthesisRunService service(&provider);
  const application::SynthesisRunEvent terminal =
      CompleteSuccessfulRun(&service, &provider, workspace);

  Assert::IsTrue(terminal.status.Ok());
  Assert::IsNotNull(terminal.run.get());
  Assert::IsTrue(terminal.run->status == application::RunStatus::kSucceeded);
  Assert::AreEqual<std::uint64_t>(1U, terminal.metrics.cell_count);
  Assert::IsTrue(terminal.metrics.has_area);
  Assert::IsTrue(ContainsArtifact(*terminal.run, "yosys"));
  Assert::IsTrue(ContainsArtifact(*terminal.run, "json"));
  Assert::IsTrue(ContainsArtifact(*terminal.run, "verilog"));
  Assert::IsTrue(ContainsArtifact(*terminal.run, "text"));
  Assert::IsTrue(ContainsArtifact(*terminal.run, "yosys-structural-json"));
  Assert::IsTrue(terminal.run->outcome.process_succeeded);
  Assert::IsTrue(terminal.run->outcome.result_succeeded);
  Assert::AreEqual(std::string("reports/synthesis-summary.json"),
                   terminal.run->outcome.summary_relative_path);
  Assert::IsTrue(std::filesystem::file_size(terminal.run->directory / L"logs" /
                                            L"raw.log") > 0);

  application::RunStore reloaded_store;
  auto reloaded = reloaded_store.List(workspace.cell());
  Assert::IsTrue(reloaded.Ok());
  Assert::AreEqual<std::size_t>(1U, reloaded.Value().size());
  Assert::IsTrue(reloaded.Value().front().status ==
                 application::RunStatus::kSucceeded);
  Assert::IsTrue(reloaded.Value().front().outcome.result_succeeded);
}

TEST_METHOD(EveryMissingRequiredArtifactFailsButPreservesRun) {
  constexpr std::array<const wchar_t*, 4> kRequired{
      L"netlist.json", L"netlist.v", L"statistics.json", L"synthesis.rpt"};
  for (const wchar_t* missing : kRequired) {
    TemporarySynthesisWorkspace workspace;
    ControlledExecutionProvider provider;
    application::SynthesisRunService service(&provider);
    ArtifactOptions options;
    options.omitted_file = missing;
    const auto terminal =
        CompleteSuccessfulRun(&service, &provider, workspace, options);
    Assert::IsFalse(terminal.status.Ok());
    Assert::IsNotNull(terminal.run.get());
    Assert::IsTrue(terminal.run->status == application::RunStatus::kFailed);
    Assert::IsTrue(std::filesystem::exists(terminal.run->directory /
                                           L"artifacts" / L"synthesis.ys"));
  }
}

TEST_METHOD(MalformedStatisticsFailsAndPreservesRawReport) {
  TemporarySynthesisWorkspace workspace;
  ControlledExecutionProvider provider;
  application::SynthesisRunService service(&provider);
  ArtifactOptions options;
  options.malformed_statistics = true;
  const auto terminal =
      CompleteSuccessfulRun(&service, &provider, workspace, options);
  Assert::IsFalse(terminal.status.Ok());
  Assert::IsTrue(std::filesystem::exists(terminal.run->directory /
                                         L"artifacts" / L"synthesis.rpt"));
  Assert::IsTrue(ContainsArtifact(*terminal.run, "text"));
}

TEST_METHOD(EmptyRequiredArtifactFailsValidation) {
  TemporarySynthesisWorkspace workspace;
  ControlledExecutionProvider provider;
  application::SynthesisRunService service(&provider);
  ArtifactOptions options;
  options.empty_file = L"netlist.v";
  const auto terminal =
      CompleteSuccessfulRun(&service, &provider, workspace, options);
  Assert::IsFalse(terminal.status.Ok());
  Assert::IsTrue(terminal.run->status == application::RunStatus::kFailed);
}

TEST_METHOD(MalformedStructuralArtifactIsWarningOnly) {
  TemporarySynthesisWorkspace workspace;
  ControlledExecutionProvider provider;
  application::SynthesisRunService service(&provider);
  ArtifactOptions options;
  options.malformed_structural = true;
  const auto terminal =
      CompleteSuccessfulRun(&service, &provider, workspace, options);
  Assert::IsTrue(terminal.status.Ok());
  Assert::IsFalse(terminal.readable_schematic_status.Ok());
  Assert::IsTrue(std::any_of(terminal.diagnostics.begin(),
                             terminal.diagnostics.end(),
                             [](const auto& diagnostic) {
                               return diagnostic.code == "SCHEMATIC-READABLE";
                             }));
}

TEST_METHOD(MissingStructuralArtifactIsWarningOnly) {
  TemporarySynthesisWorkspace workspace;
  ControlledExecutionProvider provider;
  application::SynthesisRunService service(&provider);
  ArtifactOptions options;
  options.omitted_file = L"schematic-structural.json";
  const auto terminal =
      CompleteSuccessfulRun(&service, &provider, workspace, options);
  Assert::IsTrue(terminal.status.Ok());
  Assert::IsFalse(terminal.readable_schematic_status.Ok());
  Assert::IsFalse(ContainsArtifact(*terminal.run, "yosys-structural-json"));
}

TEST_METHOD(RunningCancellationCancelsHandleAndCompletesOnce) {
  TemporarySynthesisWorkspace workspace;
  ControlledExecutionProvider provider;
  application::SynthesisRunService service(&provider);
  EventCollector collector;
  StartService(&service, &provider, workspace, &collector);
  provider.Complete(0, SuccessfulProcess("Yosys 0.33\n"));
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

TEST_METHOD(NonzeroExecutionPreservesPartialArtifactsAndLog) {
  TemporarySynthesisWorkspace workspace;
  ControlledExecutionProvider provider;
  application::SynthesisRunService service(&provider);
  EventCollector collector;
  StartService(&service, &provider, workspace, &collector);
  provider.Complete(0, SuccessfulProcess("Yosys 0.33\n"));
  Assert::IsTrue(provider.WaitForStarts(2));
  WriteSynthesisArtifacts(provider.Command(1));
  provider.Output(1, "ERROR: synthesis failed\n");
  runtime::ProcessResult failure = SuccessfulProcess("ERROR");
  failure.exit_code = 1;
  provider.Complete(1, failure);
  Assert::IsTrue(collector.WaitForTerminal());
  const auto terminal = collector.Terminal();
  Assert::IsFalse(terminal.status.Ok());
  Assert::IsTrue(std::all_of(
      terminal.run->artifacts.begin(), terminal.run->artifacts.end(),
      [](const auto& artifact) { return artifact.partial; }));
  Assert::IsTrue(std::filesystem::file_size(terminal.run->directory / L"logs" /
                                            L"raw.log") > 0);
}

TEST_METHOD(StagingCopyFailureFailsRunAndKeepsRecoverableArtifacts) {
  TemporarySynthesisWorkspace workspace;
  ControlledExecutionProvider provider;
  application::SynthesisRunService service(&provider);
  EventCollector collector;
  StartService(&service, &provider, workspace, &collector);
  provider.Complete(0, SuccessfulProcess("Yosys 0.33\n"));
  Assert::IsTrue(provider.WaitForStarts(2));

  const runtime::WslCommand synthesis_command = provider.Command(1);
  const std::filesystem::path staging_directory =
      WindowsWorkingDirectory(synthesis_command);
  WriteSynthesisArtifacts(synthesis_command);

  const std::filesystem::path runs_directory =
      workspace.cell() / L".designpp" / L"runs";
  std::filesystem::path run_directory;
  for (const auto& entry :
       std::filesystem::directory_iterator(runs_directory)) {
    if (entry.is_directory()) {
      run_directory = entry.path();
      break;
    }
  }
  Assert::IsFalse(run_directory.empty());
  const std::filesystem::path artifact_target = run_directory / L"artifacts";
  std::filesystem::remove_all(artifact_target);
  {
    std::ofstream blocking_file(artifact_target, std::ios::binary);
    blocking_file << "prevents artifact directory creation";
  }

  provider.Complete(1, SuccessfulProcess("synthesis output\n"));
  Assert::IsTrue(collector.WaitForTerminal());
  const auto terminal = collector.Terminal();
  Assert::IsFalse(terminal.status.Ok());
  Assert::IsNotNull(terminal.run.get());
  Assert::IsTrue(terminal.run->status == application::RunStatus::kFailed);
  Assert::IsTrue(std::filesystem::exists(staging_directory / L"netlist.json"));

  service.Shutdown();
  std::error_code cleanup_error;
  std::filesystem::remove_all(staging_directory, cleanup_error);
}

TEST_METHOD(StaleGenerationCallbackCannotCompleteNewRun) {
  TemporarySynthesisWorkspace workspace;
  ControlledExecutionProvider provider;
  application::SynthesisRunService service(&provider);
  EventCollector first;
  StartService(&service, &provider, workspace, &first);
  runtime::ProcessResult failure = SuccessfulProcess();
  failure.exit_code = 1;
  provider.Complete(0, failure);
  Assert::IsTrue(first.WaitForTerminal());

  EventCollector second;
  Assert::IsTrue(service
                     .Start(workspace.Request(42),
                            [&second](application::SynthesisRunEvent event) {
                              second.Add(std::move(event));
                            })
                     .Ok());
  Assert::IsTrue(provider.WaitForStarts(2));
  provider.Complete(0, SuccessfulProcess("late old callback"));
  provider.Complete(1, failure, 2);
  Assert::IsTrue(second.WaitForTerminal());
  Assert::AreEqual<std::size_t>(1U, second.TerminalCount());
  Assert::AreEqual<std::uint64_t>(42U, second.Terminal().generation);
}

TEST_METHOD(ShutdownSuppressesLateProviderCallbacks) {
  TemporarySynthesisWorkspace workspace;
  ControlledExecutionProvider provider;
  application::SynthesisRunService service(&provider);
  EventCollector collector;
  StartService(&service, &provider, workspace, &collector);
  service.Shutdown();
  Assert::IsTrue(provider.Cancelled(0));
  provider.Complete(0, SuccessfulProcess("late"), 2);
  Assert::AreEqual<std::size_t>(0U, collector.TerminalCount());
}
}
;

}  // namespace designpp::tests

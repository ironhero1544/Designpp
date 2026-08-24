// Copyright 2026 The Design++ Authors

#include <CppUnitTest.h>

#include <filesystem>
#include <fstream>

#include "designpp/application/timing_run_history.h"
#include "synthesis_test_support.h"

using Microsoft::VisualStudio::CppUnitTestFramework::Assert;

namespace designpp::tests {

TEST_CLASS(TimingRunHistoryTests){
  public : TEST_METHOD(RestoresMetricsCornerScriptAndDiagnosticsAfterRestart){
      TemporarySynthesisWorkspace workspace;
const auto project = workspace.Request().project;
application::RunStore store;
auto begun = store.Begin(workspace.cell(), project, "StaticTimingAnalysis",
                         "OpenSTA", "2.6.0");
Assert::IsTrue(begun.Ok());
application::RunRecord run = std::move(begun).Value();
const std::filesystem::path report =
    run.directory / L"artifacts" / L"timing.rpt";
const std::filesystem::path script =
    run.directory / L"artifacts" / L"timing.tcl";
const std::filesystem::path summary =
    run.directory / L"reports" / L"timing-summary.json";
std::ofstream(report, std::ios::binary)
    << "DESIGNPP_SETUP_BEGIN\nDESIGNPP_SETUP_END\n"
       "DESIGNPP_HOLD_BEGIN\nDESIGNPP_HOLD_END\n";
std::ofstream(script, std::ios::binary) << "read_liberty timing.lib\n";
std::ofstream(summary, std::ios::binary)
    << "{\"schema_version\":2,\"corner\":\"slow\","
       "\"setup_wns\":0.25,\"setup_tns\":0.0,"
       "\"hold_wns\":0.10,\"hold_tns\":0.0}";
Assert::IsTrue(store.AppendLog(run, "%Warning: test warning\n").Ok());
std::vector<application::RunArtifact> artifacts{
    {"report",
     "text",
     "artifacts/timing.rpt",
     std::filesystem::file_size(report),
     {},
     false},
    {"script",
     "tcl",
     "artifacts/timing.tcl",
     std::filesystem::file_size(script),
     {},
     false},
    {"summary",
     "json",
     "reports/timing-summary.json",
     std::filesystem::file_size(summary),
     {},
     false}};
application::RunOutcome outcome;
outcome.process_succeeded = true;
outcome.result_succeeded = true;
outcome.summary_relative_path = "reports/timing-summary.json";
Assert::IsTrue(store
                   .Complete(&run, application::RunStatus::kSucceeded, 0, {},
                             std::move(artifacts), std::move(outcome))
                   .Ok());

auto history = application::LoadTimingRunHistory(workspace.cell(), "typical");

Assert::IsTrue(history.Ok());
Assert::AreEqual<std::size_t>(1, history.Value().size());
const application::TimingRunSnapshot& restored = history.Value().front();
Assert::AreEqual("slow", restored.corner.c_str());
Assert::AreEqual(0.25, restored.metrics.setup.wns, 0.000001);
Assert::AreEqual(0.10, restored.metrics.hold.wns, 0.000001);
Assert::IsTrue(restored.metrics.setup.has_paths);
Assert::IsTrue(restored.metrics.hold.has_paths);
Assert::IsTrue(restored.script_text.find("read_liberty") != std::string::npos);
}  // namespace designpp::tests

TEST_METHOD(IgnoresOtherStagesAndKeepsFailedTimingRunVisible) {
  TemporarySynthesisWorkspace workspace;
  const auto project = workspace.Request().project;
  application::RunStore store;
  auto synthesis =
      store.Begin(workspace.cell(), project, "Synthesis", "Yosys", "0.33");
  Assert::IsTrue(synthesis.Ok());
  application::RunRecord synthesis_run = std::move(synthesis).Value();
  Assert::IsTrue(
      store.Complete(&synthesis_run, application::RunStatus::kFailed, 1, {})
          .Ok());
  auto timing = store.Begin(workspace.cell(), project, "StaticTimingAnalysis",
                            "OpenSTA", "2.6.0");
  Assert::IsTrue(timing.Ok());
  application::RunRecord timing_run = std::move(timing).Value();
  Assert::IsTrue(
      store.Complete(&timing_run, application::RunStatus::kFailed, 1, {}).Ok());

  auto history = application::LoadTimingRunHistory(workspace.cell(), "typical");

  Assert::IsTrue(history.Ok());
  Assert::AreEqual<std::size_t>(1, history.Value().size());
  Assert::IsTrue(history.Value().front().run.status ==
                 application::RunStatus::kFailed);
  Assert::IsFalse(history.Value().front().metrics.setup.has_paths);
}
}
;

}  // namespace designpp::tests

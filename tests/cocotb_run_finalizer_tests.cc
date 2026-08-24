// Copyright 2026 The Design++ Authors

#include <CppUnitTest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

#include "designpp/application/cocotb_run_finalizer.h"
#include "synthesis_test_support.h"

using Microsoft::VisualStudio::CppUnitTestFramework::Assert;

namespace designpp::tests {
namespace {

struct CocotbRunFixture {
  TemporarySynthesisWorkspace workspace;
  application::RunStore store;
  application::RunRecord run;
  adapters::CocotbPlan plan;

  CocotbRunFixture() {
    auto begun = store.Begin(workspace.cell(), workspace.Request().project,
                             "Simulation", "cocotb", "1.9");
    Assert::IsTrue(begun.Ok());
    run = std::move(begun).Value();
    plan.results_path = run.directory / L"artifacts" / L"results.xml";
    plan.waveform_path = run.directory / L"artifacts" / L"waves.fst";
    plan.input_hash = "input-hash";
  }

  void WriteResults(std::string_view xml) const {
    std::ofstream(plan.results_path, std::ios::binary) << xml;
  }

  void WriteWaveform() const {
    std::ofstream(plan.waveform_path, std::ios::binary) << "FST";
  }
};

runtime::ProcessResult SuccessfulCocotbProcess() {
  runtime::ProcessResult process;
  process.started = true;
  process.exit_code = 0;
  return process;
}

}  // namespace

TEST_CLASS(CocotbRunFinalizerTests){
  public : TEST_METHOD(PreservesPassingResultsSummaryCasesAndWaveform){
      CocotbRunFixture fixture;
fixture.WriteResults(
    "<testsuite><testcase classname=\"counter\" name=\"increments\" "
    "time=\"0.25\"/></testsuite>");
fixture.WriteWaveform();

const application::CocotbRunFinalization finalized =
    application::FinalizeCocotbRun(&fixture.run, fixture.plan,
                                   SuccessfulCocotbProcess(), fixture.store);

Assert::IsTrue(finalized.status.Ok());
Assert::IsTrue(finalized.summary.has_value());
Assert::IsTrue(fixture.run.status == application::RunStatus::kSucceeded);
Assert::IsTrue(fixture.run.outcome.process_succeeded);
Assert::IsTrue(fixture.run.outcome.result_succeeded);
Assert::AreEqual("reports/simulation-summary.json",
                 fixture.run.outcome.summary_relative_path.c_str());
Assert::AreEqual<std::size_t>(3, fixture.run.artifacts.size());
std::ifstream summary(fixture.run.directory /
                      L"reports/simulation-summary.json");
const std::string json((std::istreambuf_iterator<char>(summary)),
                       std::istreambuf_iterator<char>());
Assert::IsTrue(json.find("\"schema_version\":2") != std::string::npos);
Assert::IsTrue(json.find("increments") != std::string::npos);
}  // namespace designpp::tests

TEST_METHOD(ProcessSuccessWithFailedCaseIsDomainFailure) {
  CocotbRunFixture fixture;
  fixture.WriteResults(
      "<testsuite><testcase name=\"fails\"><failure>expected 1</failure>"
      "</testcase></testsuite>");
  fixture.WriteWaveform();

  const application::CocotbRunFinalization finalized =
      application::FinalizeCocotbRun(&fixture.run, fixture.plan,
                                     SuccessfulCocotbProcess(), fixture.store);

  Assert::IsFalse(finalized.status.Ok());
  Assert::IsTrue(fixture.run.status == application::RunStatus::kFailed);
  Assert::IsTrue(fixture.run.outcome.process_succeeded);
  Assert::IsFalse(fixture.run.outcome.result_succeeded);
  Assert::AreEqual(static_cast<std::uint32_t>(1), finalized.summary->failed);
}

TEST_METHOD(MalformedXmlRemainsAnAuthoritativeArtifact) {
  CocotbRunFixture fixture;
  fixture.WriteResults("<testsuite><testcase name=\"broken\">");
  fixture.WriteWaveform();

  const application::CocotbRunFinalization finalized =
      application::FinalizeCocotbRun(&fixture.run, fixture.plan,
                                     SuccessfulCocotbProcess(), fixture.store);

  Assert::IsFalse(finalized.status.Ok());
  Assert::IsFalse(finalized.summary.has_value());
  Assert::IsTrue(fixture.run.status == application::RunStatus::kFailed);
  Assert::IsTrue(fixture.run.outcome.process_succeeded);
  const auto results_artifact = std::find_if(
      fixture.run.artifacts.begin(), fixture.run.artifacts.end(),
      [](const application::RunArtifact& artifact) {
        return artifact.kind == "test-results" && artifact.format == "xunit";
      });
  Assert::IsTrue(
      results_artifact != fixture.run.artifacts.end(),
      (L"artifact count=" + std::to_wstring(fixture.run.artifacts.size()))
          .c_str());
  Assert::AreEqual(
      "COCOTB_RESULTS", finalized.diagnostics.front().code.c_str(),
      (L"diagnostic count=" + std::to_wstring(finalized.diagnostics.size()))
          .c_str());
}

TEST_METHOD(NonzeroProcessPreservesPartialOutputsAndOutcome) {
  CocotbRunFixture fixture;
  fixture.WriteResults("<testsuite><testcase name=\"partial\"/></testsuite>");
  fixture.WriteWaveform();
  runtime::ProcessResult process;
  process.started = true;
  process.exit_code = 7;

  const application::CocotbRunFinalization finalized =
      application::FinalizeCocotbRun(&fixture.run, fixture.plan, process,
                                     fixture.store);

  Assert::IsFalse(finalized.status.Ok());
  Assert::IsFalse(fixture.run.outcome.process_succeeded);
  Assert::IsFalse(fixture.run.outcome.result_succeeded);
  Assert::IsTrue(fixture.run.status == application::RunStatus::kFailed);
  for (const application::RunArtifact& artifact : fixture.run.artifacts) {
    Assert::IsTrue(artifact.partial);
  }
}

TEST_METHOD(MissingWaveformFailsAndRestartRestoresOutcome) {
  CocotbRunFixture fixture;
  fixture.WriteResults("<testsuite><testcase name=\"pass\"/></testsuite>");

  const application::CocotbRunFinalization finalized =
      application::FinalizeCocotbRun(&fixture.run, fixture.plan,
                                     SuccessfulCocotbProcess(), fixture.store);
  auto restored = application::RunStore().List(fixture.workspace.cell());

  Assert::IsFalse(finalized.status.Ok());
  Assert::IsTrue(restored.Ok());
  Assert::AreEqual<std::size_t>(1, restored.Value().size());
  Assert::IsTrue(restored.Value().front().outcome.process_succeeded);
  Assert::IsFalse(restored.Value().front().outcome.result_succeeded);
  Assert::AreEqual(
      "reports/simulation-summary.json",
      restored.Value().front().outcome.summary_relative_path.c_str());
}

TEST_METHOD(CancellationCompletesExactlyAsCancelledWithoutSpuriousDiagnostics) {
  CocotbRunFixture fixture;
  runtime::ProcessResult process;
  process.started = true;
  process.cancelled = true;

  const application::CocotbRunFinalization finalized =
      application::FinalizeCocotbRun(&fixture.run, fixture.plan, process,
                                     fixture.store);

  Assert::IsTrue(finalized.run_status == application::RunStatus::kCancelled);
  Assert::IsTrue(fixture.run.status == application::RunStatus::kCancelled);
  Assert::IsTrue(finalized.diagnostics.empty());
  Assert::IsTrue(finalized.status.code == core::ErrorCode::kCancelled);
}
}
;

}  // namespace designpp::tests

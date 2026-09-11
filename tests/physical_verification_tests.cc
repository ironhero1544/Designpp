// Copyright 2026 The Design++ Authors

#include <CppUnitTest.h>

#include <memory>
#include <string>

#include "designpp/adapters/physical_verification_adapter.h"
#include "designpp/application/physical_verification_service.h"
#include "synthesis_test_support.h"

using Microsoft::VisualStudio::CppUnitTestFramework::Assert;

namespace designpp::tests {

TEST_CLASS(PhysicalVerificationAdapterTests){
  public : TEST_METHOD(KLayoutDrcUsesBatchModeAndPreservesMarkerCoordinates){
      auto adapter = adapters::CreatePhysicalVerificationAdapter("klayout_drc");
Assert::IsNotNull(adapter.get());

adapters::VerificationCommandInput input;
input.recipe.root = "/opt/orfs/flow/platforms/sky130hd";
input.recipe.entrypoint = "drc/sky130hd.lydrc";
input.gds_path = L"/mnt/c/stage/layout.gds";
input.report_path = L"/mnt/c/stage/drc.lyrdb";
input.distribution = L"Ubuntu";
auto command = adapter->BuildCommand(input);
Assert::IsTrue(command.Ok());
Assert::AreEqual(std::wstring(L"klayout"), command.Value().program);
Assert::IsTrue(command.Value().arguments[0] == L"-b");
Assert::IsTrue(
    command.Value().arguments.back().ends_with(L"drc/sky130hd.lydrc"));

const std::string report =
    "<report-database><items><item><category>m1.spacing</category>"
    "<cell>counter</cell><description>minimum spacing</description>"
    "<values><box>1.2,3.4;5.6,7.8</box></values></item></items>"
    "</report-database>";
auto parsed = adapter->ParseReport(report);
Assert::IsTrue(parsed.Ok());
Assert::IsTrue(parsed.Value().parsed);
Assert::IsFalse(parsed.Value().passed);
Assert::AreEqual<std::uint64_t>(1, parsed.Value().violation_count);
Assert::AreEqual(std::string("1.2,3.4;5.6,7.8"),
                 parsed.Value().markers.front().geometry);
}  // namespace designpp::tests

TEST_METHOD(EmptyAndMalformedReportsNeverPass) {
  auto drc = adapters::CreatePhysicalVerificationAdapter("klayout_drc");
  Assert::IsFalse(drc->ParseReport("").Ok());
  Assert::IsFalse(drc->ParseReport("no marker database").Ok());

  auto lvs = adapters::CreatePhysicalVerificationAdapter("netgen_lvs");
  Assert::IsFalse(lvs->ParseReport("").Ok());
  Assert::IsFalse(lvs->ParseReport("netgen exited").Ok());
}

TEST_METHOD(NetgenSeparatesProcessOutputFromLvsResult) {
  auto adapter = adapters::CreatePhysicalVerificationAdapter("netgen_lvs");
  adapters::VerificationCommandInput input;
  input.recipe.root = "/opt/rules";
  input.recipe.setup_file = "sky130_setup.tcl";
  input.extracted_path = L"/mnt/c/stage/extracted.spice";
  input.schematic_path = L"/mnt/c/stage/schematic.spice";
  input.report_path = L"/mnt/c/stage/lvs.json";
  input.top_cell = L"counter";
  auto command = adapter->BuildCommand(input);
  Assert::IsTrue(command.Ok());
  Assert::AreEqual(std::wstring(L"netgen"), command.Value().program);

  auto mismatch = adapter->ParseReport("Circuits do not match");
  Assert::IsTrue(mismatch.Ok());
  Assert::IsFalse(mismatch.Value().passed);
  auto match = adapter->ParseReport("Circuits match uniquely.");
  Assert::IsTrue(match.Ok());
  Assert::IsTrue(match.Value().passed);
}

TEST_METHOD(CompletedProcessHandleIsNotDestroyedInsideItsCallback) {
  TemporarySynthesisWorkspace workspace;
  const std::filesystem::path gds = workspace.cell() / L"layout.gds";
  std::ofstream(gds, std::ios::binary | std::ios::trunc) << "GDS";

  ControlledExecutionProvider provider;
  application::PhysicalVerificationService service(&provider);
  application::PhysicalVerificationRequest request;
  request.project = workspace.Request().project;
  request.profile.wsl_distribution = "Ubuntu";
  request.recipe.id = "test-klayout-drc";
  request.recipe.engine = "klayout_drc";
  request.recipe.root = "/rules";
  request.recipe.entrypoint = "test.lydrc";
  request.recipe.output_format = "lyrdb";
  request.recipe.content_hash = "recipe-hash";
  request.recipe.trusted = true;
  request.check = adapters::VerificationCheck::kDrc;
  request.source_run.id = "source-run";
  request.cell_directory = workspace.cell();
  request.gds_path = gds;
  request.generation = 91;

  std::mutex mutex;
  std::condition_variable changed;
  bool completed = false;
  Assert::IsTrue(service
                     .Start(std::move(request),
                            [&](application::PhysicalVerificationEvent event) {
                              if (!event.completed) return;
                              {
                                std::scoped_lock lock(mutex);
                                completed = true;
                              }
                              changed.notify_one();
                            })
                     .Ok());
  Assert::IsTrue(provider.WaitForStarts(1));

  runtime::ProcessResult probe;
  probe.started = true;
  provider.Complete(0, probe);
  Assert::IsFalse(provider.DestroyedDuringCompletionCallback(0));
  Assert::IsTrue(provider.WaitForStarts(2));

  runtime::ProcessResult execution;
  execution.started = true;
  execution.exit_code = 1;
  provider.Complete(1, execution);
  Assert::IsFalse(provider.DestroyedDuringCompletionCallback(1));
  std::unique_lock lock(mutex);
  Assert::IsTrue(changed.wait_for(lock, std::chrono::seconds(5),
                                  [&] { return completed; }));
  lock.unlock();
  service.Shutdown();
}
}
;

}  // namespace designpp::tests

// Copyright 2026 The Design++ Authors

#include <CppUnitTest.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
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

TEST_METHOD(KLayoutLvsBuildsCdlPreparationCommands) {
  auto adapter = adapters::CreatePhysicalVerificationAdapter("klayout_lvs");
  Assert::IsNotNull(adapter.get());
  adapters::VerificationCommandInput input;
  input.recipe.engine = "klayout_lvs";
  input.recipe.root = "/home/test/orfs/flow/platforms/sky130hd";
  input.recipe.reference_netlist = "cdl/sky130hd.cdl";
  input.odb_path = L"/mnt/c/stage/final.odb";
  input.model_path =
      L"/home/test/orfs/flow/platforms/sky130hd/cdl/sky130hd.cdl";
  input.staged_model_path = L"/mnt/c/stage/cell-model.cdl";
  input.design_cdl_path = L"/mnt/c/stage/design.cdl";
  input.combined_cdl_path = L"/mnt/c/stage/combined.cdl";
  input.preparation_script_path = L"/mnt/c/stage/cdl-export.tcl";
  input.toolchain_root = L"~/.designpp/toolchains/orfs";
  input.check = adapters::VerificationCheck::kLvs;

  const auto script = adapter->BuildPreparationScript(input);
  Assert::IsTrue(script.Ok());
  Assert::IsTrue(script.Value().find("read_db") != std::string::npos);
  Assert::IsTrue(script.Value().find("write_cdl -masters") !=
                 std::string::npos);
  const auto commands = adapter->BuildPreparationCommands(input);
  Assert::IsTrue(commands.Ok());
  Assert::AreEqual<std::size_t>(3, commands.Value().size());
  Assert::AreEqual(std::wstring(L"/bin/bash"), commands.Value()[0].program);
  Assert::AreEqual(std::wstring(L"/bin/bash"), commands.Value()[1].program);
  Assert::AreEqual(std::wstring(L"/bin/bash"), commands.Value()[2].program);
  Assert::IsTrue(commands.Value()[1].arguments[1].find(L"openroad") !=
                 std::wstring::npos);
  const auto probe = adapter->BuildProbeCommand(input);
  for (const auto& command : {probe, commands.Value()[1]}) {
    for (const auto* required :
         {L"--override-input yosys", L"--override-input openroad",
          L"--override-input eqy-src", L"--offline", L"--max-jobs 0"}) {
      Assert::IsTrue(command.arguments[1].find(required) != std::wstring::npos);
    }
  }
}

TEST_METHOD(EmptyAndMalformedReportsNeverPass) {
  auto drc = adapters::CreatePhysicalVerificationAdapter("klayout_drc");
  Assert::IsFalse(drc->ParseReport("").Ok());
  Assert::IsFalse(drc->ParseReport("no marker database").Ok());

  auto lvs = adapters::CreatePhysicalVerificationAdapter("netgen_lvs");
  Assert::IsFalse(lvs->ParseReport("").Ok());
  Assert::IsFalse(lvs->ParseReport("netgen exited").Ok());
}

TEST_METHOD(KLayoutLvsSeparatesMismatchFromMissingComparisonEvidence) {
  auto adapter = adapters::CreatePhysicalVerificationAdapter("klayout_lvs");
  Assert::IsNotNull(adapter.get());
  adapters::VerificationResultArtifacts mismatch;
  mismatch.process_output = "ERROR : Netlists don't match\n";
  Assert::IsFalse(adapter->ParseResult(mismatch).Ok());
  mismatch.report_exists = true;
  mismatch.report = "#%lvsdb-klayout\n";
  mismatch.comparison_summary = "DESIGNPP_LVS_XREF_V1 34 5 1\n";
  const auto parsed_mismatch = adapter->ParseResult(mismatch);
  Assert::IsTrue(parsed_mismatch.Ok());
  Assert::IsTrue(parsed_mismatch.Value().parsed);
  Assert::IsFalse(parsed_mismatch.Value().passed);
  Assert::IsFalse(parsed_mismatch.Value().comparison_completed);
  Assert::AreEqual<std::size_t>(5, parsed_mismatch.Value().violation_count);
  mismatch.comparison_summary = "DESIGNPP_LVS_XREF_V1 4 0 0\n";
  Assert::IsTrue(adapter->ParseResult(mismatch).Value().passed);
  mismatch.comparison_summary = "DESIGNPP_LVS_XREF_V1 0 0 0\n";
  Assert::IsFalse(adapter->ParseResult(mismatch).Ok());
  mismatch.comparison_summary = "DESIGNPP_LVS_XREF_V1 1 2 0\n";
  Assert::IsFalse(adapter->ParseResult(mismatch).Ok());

  adapters::VerificationResultArtifacts missing;
  missing.report_exists = true;
  missing.report = "<report-database/>";
  const auto missing_result = adapter->ParseResult(missing);
  Assert::IsFalse(missing_result.Ok());
  Assert::IsTrue(missing_result.GetStatus().message.starts_with(
      "LVS-COMPARISON-MISSING:"));
}

TEST_METHOD(ManagedRecipeUsesTheSourceRunPlatform) {
  core::ToolchainProfile profile;
  profile.orfs_root = "/home/test/orfs";
  const auto sky130 = application::ResolveManagedVerificationRecipes(
      profile, "orfs", "sky130hd");
  Assert::AreEqual<std::size_t>(2, sky130.size());
  Assert::IsTrue(std::all_of(
      sky130.begin(), sky130.end(), [](const core::VerificationRecipe& recipe) {
        return recipe.platform == "sky130hd" && recipe.managed;
      }));
  const auto asap7 =
      application::ResolveManagedVerificationRecipes(profile, "orfs", "asap7");
  Assert::AreEqual<std::size_t>(1, asap7.size());
  Assert::AreEqual(std::string("klayout_drc"), asap7.front().engine);
  const auto nangate45 = application::ResolveManagedVerificationRecipes(
      profile, "orfs", "nangate45");
  Assert::IsTrue(nangate45.empty());
}

TEST_METHOD(VerificationCapabilityRequiresFilesAndRegisteredRecipe) {
  core::ToolchainProfile profile;
  profile.orfs_root = "/opt/orfs";

  const auto sky130 = application::ResolveVerificationCapability(
      profile, "orfs", "sky130hd", true, true, true);
  Assert::IsTrue(sky130.drc_ready);
  Assert::IsTrue(sky130.lvs_ready);

  const auto asap7 = application::ResolveVerificationCapability(
      profile, "orfs", "asap7", true, true, true);
  Assert::IsTrue(asap7.drc_ready);
  Assert::IsFalse(asap7.lvs_ready);
  Assert::AreEqual(std::string("No managed recipe"), asap7.lvs_status);

  const auto missing_rule = application::ResolveVerificationCapability(
      profile, "orfs", "sky130hd", true, false, true);
  Assert::IsFalse(missing_rule.drc_ready);
  Assert::AreEqual(std::string("Required rule files missing"),
                   missing_rule.drc_status);

  const auto nangate = application::ResolveVerificationCapability(
      profile, "orfs", "nangate45", true, true, true);
  Assert::IsFalse(nangate.drc_ready);
  Assert::IsFalse(nangate.lvs_ready);

  const auto openlane = application::ResolveVerificationCapability(
      profile, "openlane2", "sky130A", true, true, true);
  Assert::IsFalse(openlane.drc_ready);
  Assert::AreEqual(std::string("Flow results only"), openlane.drc_status);
}

TEST_METHOD(Sky130ContractIsExplicitAndDoesNotApplyToCustomRules) {
  auto adapter = adapters::CreatePhysicalVerificationAdapter("klayout_lvs");
  adapters::VerificationCommandInput input;
  input.recipe = application::ResolveManagedVerificationRecipes(
                     [] {
                       core::ToolchainProfile p;
                       p.orfs_root = "/tools/orfs";
                       return p;
                     }(),
                     "orfs", "sky130hd")
                     .back();
  input.gds_path = L"/stage/layout.gds";
  input.schematic_path = L"/stage/source.cdl";
  input.report_path = L"/stage/lvs.report";
  input.verification_script_path = L"/stage/driver.rb";
  auto command = adapter->BuildCommand(input);
  Assert::IsTrue(command.Ok());
  Assert::IsTrue(std::find(command.Value().arguments.begin(),
                           command.Value().arguments.end(),
                           L"recipe_contract=sky130hd-bulk-v1") !=
                 command.Value().arguments.end());
  input.recipe.managed = false;
  command = adapter->BuildCommand(input);
  Assert::IsTrue(command.Ok());
  Assert::IsTrue(std::find(command.Value().arguments.begin(),
                           command.Value().arguments.end(),
                           L"recipe_contract=custom") !=
                 command.Value().arguments.end());
}

TEST_METHOD(OptInSky130LayoutRunThroughProductionService) {
  wchar_t cell[4096] = {};
  char source_id[128] = {};
  char top[256] = {};
  if (!GetEnvironmentVariableW(L"DESIGNPP_LVS_TEST_CELL", cell, 4096) ||
      !GetEnvironmentVariableA("DESIGNPP_LVS_TEST_SOURCE", source_id, 128) ||
      !GetEnvironmentVariableA("DESIGNPP_LVS_TEST_TOP", top, 256)) {
    Microsoft::VisualStudio::CppUnitTestFramework::Logger::WriteMessage(
        "Integration not selected: set DESIGNPP_LVS_TEST_CELL/SOURCE/TOP");
    return;
  }
  application::RunStore store;
  auto runs = store.List(cell);
  Assert::IsTrue(runs.Ok());
  const auto source = std::find_if(
      runs.Value().begin(), runs.Value().end(),
      [source_id](const auto& run) { return run.id == source_id; });
  Assert::IsTrue(source != runs.Value().end());
  application::PhysicalVerificationRequest request;
  request.cell_directory = cell;
  request.source_run = *source;
  request.project.id = source->project_id;
  request.project.top_module = top;
  request.project.cpu_budget = 2;
  request.profile.orfs_root = "~/.designpp/toolchains/orfs";
  request.recipe = application::ResolveManagedVerificationRecipes(
                       request.profile, "orfs", "sky130hd")
                       .back();
  request.check = adapters::VerificationCheck::kLvs;
  request.environment_id = source->environment_id;
  request.environment_fingerprint = source->environment_fingerprint;
  for (const auto& artifact : source->artifacts) {
    if (artifact.partial) continue;
    const auto path = source->directory / artifact.relative_path;
    if (artifact.kind == "final.gds") request.gds_path = path;
    if (artifact.kind == "final.odb") request.odb_path = path;
    if (artifact.kind == "netlist.v") request.source_netlist_path = path;
  }
  request.generation = 1;
  runtime::WslExecutionProvider provider;
  application::PhysicalVerificationService service(&provider);
  std::mutex mutex;
  std::condition_variable changed;
  bool done = false;
  application::PhysicalVerificationEvent final;
  Assert::IsTrue(service
                     .Start(std::move(request),
                            [&](auto event) {
                              if (!event.completed) return;
                              {
                                std::scoped_lock lock(mutex);
                                final = std::move(event);
                                done = true;
                              }
                              changed.notify_one();
                            })
                     .Ok());
  {
    std::unique_lock lock(mutex);
    Assert::IsTrue(
        changed.wait_for(lock, std::chrono::minutes(5), [&] { return done; }));
  }
  service.Shutdown();
  if (final.run)
    Microsoft::VisualStudio::CppUnitTestFramework::Logger::WriteMessage(
        final.run->directory.c_str());
  Microsoft::VisualStudio::CppUnitTestFramework::Logger::WriteMessage(
      final.status.message.c_str());
  Assert::IsTrue(final.status.Ok());
  Assert::IsTrue(final.result.parsed);
  Assert::IsTrue(final.result.comparison_completed);
  Assert::IsTrue(final.result.passed);
}

TEST_METHOD(CdlSeparatorIsNotAnExtraPin) {
  const auto tie = adapters::NormalizeCdlForSpice(
      "rI12 VGND LO short\nrI11 HI VPWR SHORT\nR3 A B 100\n");
  Assert::IsTrue(tie.Ok());
  Assert::AreEqual(
      std::string("rI12 VGND LO 0    \nrI11 HI VPWR 0    \nR3 A B 100\n"),
      tie.Value());
  const std::string cdl =
      "* retain / comments\n.SUBCKT cell A B\n"
      "XI1 VGND VNB VPB VPWR net59 LO / sky130_fd_sc_hd__conb_1\n"
      "x2 net/a\n+ net/b / cell\n"
      "X3 net/a net/b cell\n.ENDS\n";
  const auto normalized = adapters::NormalizeCdlForSpice(cdl);
  Assert::IsTrue(normalized.Ok());
  Assert::AreEqual(
      std::string("* retain / comments\n.SUBCKT cell A B\n"
                  "XI1 VGND VNB VPB VPWR net59 LO   sky130_fd_sc_hd__conb_1\n"
                  "x2 net/a\n+ net/b   cell\n"
                  "X3 net/a net/b cell\n.ENDS\n"),
      normalized.Value());
  Assert::IsFalse(adapters::NormalizeCdlForSpice("X1 a / b cell\n").Ok());
  Assert::IsFalse(adapters::NormalizeCdlForSpice("X1 a b /\n").Ok());
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

TEST_METHOD(SynchronousProbeCompletionDoesNotReplaceExecutionHandle) {
  TemporarySynthesisWorkspace workspace;
  const std::filesystem::path gds = workspace.cell() / L"layout.gds";
  std::ofstream(gds, std::ios::binary | std::ios::trunc) << "GDS";

  ControlledExecutionProvider provider;
  runtime::ProcessResult probe;
  probe.started = true;
  provider.CompleteNextStartSynchronously(probe);

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
  request.generation = 92;

  std::mutex mutex;
  std::condition_variable changed;
  int completions = 0;
  Assert::IsTrue(service
                     .Start(std::move(request),
                            [&](auto event) {
                              if (!event.completed) return;
                              {
                                std::scoped_lock lock(mutex);
                                ++completions;
                              }
                              changed.notify_one();
                            })
                     .Ok());
  Assert::IsTrue(provider.WaitForStarts(2));
  Assert::IsTrue(provider.WaitForHandleReturned(0));
  Assert::IsTrue(provider.CallbackExited(0));
  Assert::IsFalse(provider.DestroyedDuringCompletionCallback(0));

  runtime::ProcessResult execution;
  execution.started = true;
  execution.exit_code = 1;
  provider.Complete(1, execution, 2);
  {
    std::unique_lock lock(mutex);
    Assert::IsTrue(changed.wait_for(lock, std::chrono::seconds(5),
                                    [&] { return completions == 1; }));
  }
  Assert::AreEqual(1, completions);
  Assert::IsFalse(provider.DestroyedDuringCompletionCallback(1));
  service.Shutdown();
  service.Shutdown();
}
}
;

}  // namespace designpp::tests

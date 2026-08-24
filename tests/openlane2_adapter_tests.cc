// Copyright 2026 The Design++ Authors

#include <CppUnitTest.h>

#include <string>

#include "designpp/adapters/openlane2_adapter.h"

using Microsoft::VisualStudio::CppUnitTestFramework::Assert;

namespace designpp::tests {
namespace {

adapters::OpenLaneRequest SampleRequest() {
  adapters::OpenLaneRequest request;
  request.profile.openlane_root = "/home/test/openlane2";
  request.configuration.pdk = "sky130A";
  request.configuration.standard_cell_library = "sky130_fd_sc_hd";
  request.configuration.clock_ports = {"clk", "scan_clk"};
  request.configuration.clock_period_ns = "10.5";
  request.configuration.core_utilization_percent = 40;
  request.configuration.placement_density_percent = "55.0";
  request.configuration.die_area = {"0", "0", "120", "140"};
  request.top_module = "counter";
  request.sources.push_back({"rtl/counter.sv", "source_0.sv"});
  request.include_directories.push_back("rtl/include");
  request.defines.push_back("SYNTHESIS=1");
  request.parameters.push_back("WIDTH=8");
  request.pnr_sdc = "pnr.sdc";
  request.signoff_sdc = "signoff.sdc";
  request.backend_workspace = ".designpp/runs/openlane/project/lineage";
  request.staging_workspace = "/mnt/c/staging";
  request.cpu_threads = 4;
  return request;
}

}  // namespace

TEST_CLASS(OpenLane2AdapterTests){
  public : TEST_METHOD(ConfigMapsManagedInputsAndFloorplan){
      adapters::OpenLane2Adapter adapter;
auto plan = adapter.BuildPlan(SampleRequest());
Assert::IsTrue(plan.Ok());
const std::string& json = plan.Value().config_json;
Assert::IsTrue(json.find("\"DESIGN_NAME\": \"counter\"") != std::string::npos);
Assert::IsTrue(json.find("dir::src/source_0.sv") != std::string::npos);
Assert::IsTrue(json.find("\"VERILOG_DEFINES\": [\"SYNTHESIS=1\"]") !=
               std::string::npos);
Assert::IsTrue(json.find("\"SYNTH_PARAMETERS\": [\"WIDTH=8\"]") !=
               std::string::npos);
Assert::IsTrue(json.find("\"CLOCK_PERIOD\": 10.5") != std::string::npos);
Assert::IsTrue(json.find("\"DIE_AREA\": [0, 0, 120, 140]") !=
               std::string::npos);
Assert::IsTrue(json.find("dir::constraints/pnr.sdc") != std::string::npos);
Assert::IsTrue(plan.Value().execute.arguments.size() >= 8);
Assert::IsTrue(plan.Value().execute.arguments[1].find(
                   L"workspace=\"$HOME/$workspace\"") != std::wstring::npos);
Assert::IsTrue(plan.Value().validate.arguments[1].find(
                   L"workspace=\"$HOME/$workspace\"") != std::wstring::npos);
}  // namespace designpp::tests

TEST_METHOD(AdvancedOverridesRejectReservedNestedAndPathValues) {
  adapters::OpenLane2Adapter adapter;
  Assert::IsFalse(
      adapter.ValidateAdvancedOverrides("{\"PDK\":\"gf180mcuC\"}").Ok());
  Assert::IsFalse(
      adapter.ValidateAdvancedOverrides("{\"EXTRA\":{\"A\":1}}").Ok());
  Assert::IsFalse(
      adapter.ValidateAdvancedOverrides("{\"EXTRA\":\"/home/user/file\"}")
          .Ok());
  Assert::IsFalse(
      adapter.ValidateAdvancedOverrides("{\"RUN_HEURISTIC_DIODE_INSERTION\":1}")
          .Ok());
  Assert::IsFalse(
      adapter.ValidateAdvancedOverrides("{\"FP_SIZING\":\"relative\"}").Ok());
  Assert::IsTrue(
      adapter.ValidateAdvancedOverrides("{\"DIODE_ON_PORTS\":\"in\"}").Ok());
}

TEST_METHOD(ProbeUsesSelectedProfileAndResumeVerifiesCheckpointHash) {
  adapters::OpenLane2Adapter adapter;
  core::ToolchainProfile profile;
  profile.openlane_root = "~/custom/openlane2";
  profile.wsl_distribution = "Ubuntu-24.04";
  const runtime::WslCommand probe = adapter.BuildProbeCommand(profile);
  Assert::IsTrue(probe.distribution.has_value());
  Assert::AreEqual(std::wstring(L"Ubuntu-24.04"), *probe.distribution);
  Assert::IsTrue(probe.arguments.back().find(L"~/custom/openlane2") !=
                 std::wstring::npos);

  adapters::OpenLaneRequest request = SampleRequest();
  request.resume_step = "OpenROAD.GeneratePDN";
  Assert::IsFalse(adapter.BuildPlan(request).Ok());
  request.checkpoint_hash = std::string(64, 'a');
  auto plan = adapter.BuildPlan(request);
  Assert::IsTrue(plan.Ok());
  Assert::IsTrue(plan.Value().execute.arguments.back() ==
                 std::wstring(64, L'a'));
  Assert::IsTrue(plan.Value().execute.arguments[1].find(L"sha256sum") !=
                 std::wstring::npos);
}

TEST_METHOD(NixEnvironmentWarningsAreNotOpenLaneDiagnostics) {
  adapters::OpenLane2Adapter adapter;
  const auto diagnostics = adapter.ParseDiagnostics(
      "warning: In a derivation named 'neovim-unwrapped-0.9.5'\n"
      "warning: Using 'builtins.derivation' for devshell-env.bash\n"
      "ERROR OpenROAD routing failed\n");
  Assert::AreEqual<std::size_t>(1U, diagnostics.size());
  Assert::AreEqual(std::string("OPENLANE-ERROR"), diagnostics[0].code);
}

TEST_METHOD(ClearCheckerMessagesAreNotReportedAsErrors) {
  adapters::OpenLane2Adapter adapter;
  const auto diagnostics = adapter.ParseDiagnostics(
      "──────────────── Error Checker ────────────────\n"
      "[15:34:32] INFO Check for Magic DRC errors clear.\n"
      "No errors found.\n"
      "[INFO] Saving mag view with DRC errors\n"
      "ABC: Error: The network is combinational.\n");
  Assert::AreEqual<std::size_t>(1U, diagnostics.size());
  Assert::IsTrue(diagnostics[0].severity == core::DiagnosticSeverity::kWarning);
}

TEST_METHOD(ProgressAndStagesAreNormalizedWithoutLosingStepId) {
  adapters::OpenLane2Adapter adapter;
  auto progress = adapter.ParseProgress("[INFO] Starting step OpenROAD.CTS");
  Assert::IsTrue(progress.has_value());
  Assert::AreEqual(std::string("OpenROAD.CTS"), progress->step_id);
  Assert::IsTrue(progress->stage == core::StageId::kClockTreeSynthesis);
  progress = adapter.ParseProgress(
      "[12:00:00] VERBOSE Running 'OpenROAD.GeneratePDN' at step.py:1");
  Assert::IsTrue(progress.has_value());
  Assert::AreEqual(std::string("OpenROAD.GeneratePDN"), progress->step_id);
  Assert::IsTrue(adapter.ClassifyStep("Future.UnknownStep") ==
                 core::StageId::kFinalOutputs);
}

TEST_METHOD(Metrics21KnownAndUnknownValuesArePreserved) {
  adapters::OpenLane2Adapter adapter;
  auto metrics = adapter.ParseMetrics(
      "{\"design__core__area\":123.5,"
      "\"design__instance__count\":42,"
      "\"timing__setup__wns\":0.1,"
      "\"timing__hold__wns\":-0.2,"
      "\"magic__drc_error__count\":0,"
      "\"future__metric\":7}");
  Assert::IsTrue(metrics.Ok());
  Assert::AreEqual(123.5, *metrics.Value().core_area, 0.0001);
  Assert::AreEqual(-0.2, *metrics.Value().hold_wns, 0.0001);
  Assert::IsTrue(metrics.Value().raw.contains("future__metric"));
  Assert::IsFalse(metrics.Value().Passed());
}

TEST_METHOD(CurrentXorMetricAllowsCleanFlowToPass) {
  adapters::OpenLane2Adapter adapter;
  auto metrics = adapter.ParseMetrics(
      "{\"timing__setup__wns\":0,\"timing__setup__tns\":0,"
      "\"timing__hold__wns\":0,\"timing__hold__tns\":0,"
      "\"route__antenna_violation__count\":0,"
      "\"magic__drc_error__count\":0,"
      "\"design__xor_difference__count\":0,"
      "\"design__lvs_error__count\":0}");
  Assert::IsTrue(metrics.Ok());
  Assert::IsTrue(metrics.Value().xor_violations.has_value());
  Assert::AreEqual(0.0, *metrics.Value().xor_violations, 0.0001);
  Assert::IsTrue(metrics.Value().Passed());
}
}
;

}  // namespace designpp::tests

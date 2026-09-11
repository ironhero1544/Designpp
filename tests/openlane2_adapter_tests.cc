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
  request.configuration.core_area = {"10", "10", "110", "130"};
  request.configuration.tap_cell_distance_um = "14.0";
  request.configuration.power_distribution.multilayer = false;
  request.configuration.power_distribution.vertical_width_um = "1.6";
  request.configuration.power_distribution.horizontal_pitch_um = "20.0";
  request.configuration.power_distribution.vertical_offset_um = "2.0";
  auto& io = request.configuration.io_placement;
  io.algorithm = "annealing";
  io.minimum_distance_um = "1.5";
  io.vertical_length_um = "2.0";
  io.horizontal_length_um = "2.5";
  io.vertical_thickness_multiplier = "3.0";
  io.horizontal_extension_um = "0.8";
  io.vertical_layer = "met3";
  io.unmatched_policy = "both";
  io.north.minimum_distance_um = "2.0";
  io.north.bit_major = true;
  io.north.entries = {"data\\[\\d+\\]", "$2"};
  io.east.entries = {"clk"};
  request.top_module = "counter";
  request.sources.push_back({"rtl/counter.sv", "source_0.sv"});
  request.include_directories.push_back("rtl/include");
  request.defines.push_back("SYNTHESIS=1");
  request.parameters.push_back("WIDTH=8");
  request.pnr_sdc = "pnr.sdc";
  request.signoff_sdc = "signoff.sdc";
  request.pin_order_cfg = "pin_order.cfg";
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
Assert::IsTrue(json.find("\"CORE_AREA\": [10, 10, 110, 130]") !=
               std::string::npos);
Assert::IsTrue(json.find("\"FP_TAPCELL_DIST\": 14.0") != std::string::npos);
Assert::IsTrue(json.find("\"FP_PDN_MULTILAYER\": false") != std::string::npos);
Assert::IsTrue(json.find("\"FP_PDN_VWIDTH\": 1.6") != std::string::npos);
Assert::IsTrue(json.find("\"FP_PDN_HPITCH\": 20.0") != std::string::npos);
Assert::IsTrue(json.find("\"FP_PDN_VOFFSET\": 2.0") != std::string::npos);
Assert::IsTrue(json.find("\"FP_PPL_MODE\": \"annealing\"") !=
               std::string::npos);
Assert::IsTrue(json.find("\"FP_IO_MIN_DISTANCE\": 1.5") != std::string::npos);
Assert::IsTrue(json.find("\"FP_IO_VLAYER\": \"met3\"") != std::string::npos);
Assert::IsTrue(json.find("dir::constraints/pin_order.cfg") !=
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
  Assert::IsFalse(
      adapter.ValidateAdvancedOverrides("{\"CORE_AREA\":[1,1,9,9]}").Ok());
  Assert::IsFalse(
      adapter.ValidateAdvancedOverrides("{\"FP_TAPCELL_DIST\":14}").Ok());
  Assert::IsFalse(
      adapter.ValidateAdvancedOverrides("{\"FP_PDN_VPITCH\":20}").Ok());
  Assert::IsFalse(
      adapter.ValidateAdvancedOverrides("{\"FP_IO_VLENGTH\":2}").Ok());
  Assert::IsFalse(adapter.ValidateAdvancedOverrides("{\"A\":1,\"A\":2}").Ok());
  Assert::IsFalse(adapter.ValidateAdvancedOverrides("{\"A\":}").Ok());
  Assert::IsFalse(
      adapter.ValidateAdvancedOverrides("{\"CUSTOM\":\"DIR::secret\"}").Ok());
  Assert::IsTrue(
      adapter.ValidateAdvancedOverrides("{\"DIODE_ON_PORTS\":\"in\"}").Ok());
}

TEST_METHOD(CombinationalConfigurationOmitsClockPeriod) {
  adapters::OpenLane2Adapter adapter;
  adapters::OpenLaneRequest request = SampleRequest();
  request.configuration.clock_ports.clear();
  request.configuration.clock_period_ns.clear();
  const auto plan = adapter.BuildPlan(request);
  Assert::IsTrue(plan.Ok());
  Assert::IsTrue(plan.Value().config_json.find("\"CLOCK_PERIOD\"") ==
                 std::string::npos);
  auto editable = adapter.EncodeEditableConfiguration(request.configuration);
  Assert::IsTrue(editable.Ok());
  Assert::IsTrue(editable.Value().find("\"CLOCK_PERIOD\"") ==
                 std::string::npos);
}

TEST_METHOD(PinOrderAndEditableJsonAreDeterministicAndSynchronized) {
  adapters::OpenLane2Adapter adapter;
  auto request = SampleRequest();
  auto pin_order = adapter.BuildPinOrderConfiguration(request.configuration);
  Assert::IsTrue(pin_order.Ok());
  Assert::AreEqual(std::string("#N\n@min_distance=2.0\n@bit_major\n"
                               "data\\[\\d+\\]\n$2\n\n#E\n@bus_major\nclk\n\n"),
                   pin_order.Value());

  request.configuration.advanced_overrides_json = "{\"GRT_ANTENNA_ITERS\": 5}";
  auto encoded = adapter.EncodeEditableConfiguration(request.configuration);
  Assert::IsTrue(encoded.Ok());
  Assert::IsTrue(encoded.Value().find("\"FP_CORE_UTIL\": 40") !=
                 std::string::npos);
  Assert::IsTrue(encoded.Value().find("\"GRT_ANTENNA_ITERS\": 5") !=
                 std::string::npos);
  std::string edited = encoded.Value();
  const std::size_t mode = edited.find("\"FP_PPL_MODE\": \"annealing\"");
  Assert::IsTrue(mode != std::string::npos);
  edited.replace(mode, std::string("\"FP_PPL_MODE\": \"annealing\"").size(),
                 "\"FP_PPL_MODE\": \"matching\"");
  auto applied =
      adapter.ApplyEditableConfiguration(edited, request.configuration);
  const std::wstring apply_message(applied.GetStatus().message.begin(),
                                   applied.GetStatus().message.end());
  Assert::IsTrue(applied.Ok(), apply_message.c_str());
  Assert::AreEqual(std::string("matching"),
                   applied.Value().io_placement.algorithm);
  Assert::IsTrue(applied.Value().advanced_overrides_json.find(
                     "GRT_ANTENNA_ITERS") != std::string::npos);
  Assert::IsTrue(applied.Value().advanced_overrides_json.find("FP_PPL_MODE") ==
                 std::string::npos);
  edited = encoded.Value();
  const std::size_t die_area =
      edited.find("\"DIE_AREA\": [\"0\", \"0\", \"120\", \"140\"]");
  Assert::IsTrue(die_area != std::string::npos);
  edited.replace(
      die_area,
      std::string("\"DIE_AREA\": [\"0\", \"0\", \"120\", \"140\"]").size(),
      "\"DIE_AREA\": [0, 0, 16, 16]");
  const std::size_t core_area =
      edited.find("\"CORE_AREA\": [\"10\", \"10\", \"110\", \"130\"]");
  Assert::IsTrue(core_area != std::string::npos);
  edited.replace(
      core_area,
      std::string("\"CORE_AREA\": [\"10\", \"10\", \"110\", \"130\"]").size(),
      "\"CORE_AREA\": [1.84, 2.72, 14.72, 13.60]");
  applied = adapter.ApplyEditableConfiguration(edited, request.configuration);
  Assert::IsTrue(applied.Ok());
  Assert::AreEqual(std::string("16"), applied.Value().die_area[2]);
  Assert::AreEqual(std::string("1.84"), applied.Value().core_area[0]);
  std::string resolved_snapshot = encoded.Value();
  resolved_snapshot.insert(resolved_snapshot.rfind('}'),
                           ",\n  \"FP_PIN_ORDER_CFG\": "
                           "\"/tmp/pin_order.cfg\"\n");
  const auto managed_path = adapter.ApplyEditableConfiguration(
      resolved_snapshot, request.configuration);
  Assert::IsTrue(managed_path.Ok());
  Assert::IsTrue(managed_path.Value().advanced_overrides_json.find(
                     "FP_PIN_ORDER_CFG") == std::string::npos);
  Assert::IsFalse(adapter
                      .ApplyEditableConfiguration(
                          "{\"DESIGN_NAME\":\"unsafe\"}", request.configuration)
                      .Ok());
}

TEST_METHOD(EditableJsonIgnoresResolvedPinOrderPathAndAppliesOtherValues) {
  adapters::OpenLane2Adapter adapter;
  auto request = SampleRequest();
  const std::string json = R"({
    "CLOCK_PERIOD": 10.0,
    "CLOCK_PORT": [],
    "CORE_AREA": [1.84, 2.72, 14.72, 13.60],
    "DIE_AREA": [0, 0, 16, 16],
    "FP_CORE_UTIL": 40,
    "FP_TAPCELL_DIST": 5.52,
    "FP_PPL_MODE": "matching",
    "ERRORS_ON_UNMATCHED_IO": "both",
    "FP_IO_VLENGTH": 0.5,
    "FP_IO_HLENGTH": 0.5,
    "FP_IO_VTHICKNESS_MULT": 1,
    "FP_IO_HTHICKNESS_MULT": 1,
    "FP_PIN_ORDER_CFG": "/path/to/generated/pin_order.cfg",
    "FP_PDN_CORE_RING": false,
    "FP_PDN_ENABLE_RAILS": true,
    "FP_PDN_MULTILAYER": false,
    "FP_PDN_VOFFSET": 1.5,
    "FP_PDN_VPITCH": 10,
    "FP_PDN_VSPACING": 0.5,
    "FP_PDN_VWIDTH": 0.5,
    "PDK": "sky130A",
    "STD_CELL_LIBRARY": "sky130_fd_sc_hd"
  })";
  auto applied =
      adapter.ApplyEditableConfiguration(json, request.configuration);
  const std::wstring message(applied.GetStatus().message.begin(),
                             applied.GetStatus().message.end());
  Assert::IsTrue(applied.Ok(), message.c_str());
  Assert::AreEqual(std::string("10.0"), applied.Value().clock_period_ns);
  Assert::IsTrue(applied.Value().clock_ports.empty());
  Assert::AreEqual(std::string("1.84"), applied.Value().core_area[0]);
  Assert::AreEqual(std::string("5.52"), *applied.Value().tap_cell_distance_um);
  Assert::AreEqual(std::string("0.5"),
                   *applied.Value().power_distribution.vertical_width_um);
  Assert::IsTrue(applied.Value().advanced_overrides_json.find(
                     "FP_PIN_ORDER_CFG") == std::string::npos);
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

TEST_METHOD(SmallCorePdnFailureProvidesAnActionableLayoutHint) {
  adapters::OpenLane2Adapter adapter;
  const auto diagnostics = adapter.ParseDiagnostics(
      "Error: [PDN-0185] Insufficient width (7.36 um) to add straps on "
      "layer met4.\n");
  Assert::AreEqual<std::size_t>(1U, diagnostics.size());
  Assert::AreEqual(std::string("OPENLANE-PDN-TOO-SMALL"), diagnostics[0].code);
  Assert::IsTrue(diagnostics[0].message.find("0,0,100,100") !=
                 std::string::npos);
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

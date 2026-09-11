// Copyright 2026 The Design++ Authors

#include <CppUnitTest.h>

#include <algorithm>
#include <string>

#include "designpp/adapters/orfs_adapter.h"

using Microsoft::VisualStudio::CppUnitTestFramework::Assert;

namespace designpp::tests {
namespace {

adapters::OrfsRequest SampleRequest() {
  adapters::OrfsRequest request;
  request.profile.orfs_root = "/home/test/orfs";
  request.configuration.backend_id = "orfs";
  request.configuration.orfs.platform = "sky130hd";
  request.configuration.orfs.flow_variant = "base";
  request.configuration.clock_ports = {"clk"};
  request.configuration.clock_period_ns = "10.0";
  request.configuration.placement_density_percent = "65.0";
  request.configuration.die_area = {"0", "0", "100", "100"};
  request.configuration.core_area = {"10", "10", "90", "90"};
  request.top_module = "counter";
  request.sources.push_back({"rtl/counter.sv", "source_0.sv"});
  request.include_directories.push_back("rtl/include");
  request.defines.push_back("SYNTHESIS=1");
  request.parameters.push_back("WIDTH=8");
  request.sdc_path = "constraints.sdc";
  request.effective_clock_period_ns = "10.0";
  request.backend_workspace = ".designpp/runs/orfs/project/lineage";
  request.staging_workspace = "/mnt/c/staging";
  request.cpu_threads = 4;
  request.tool_mode = "direct";
  return request;
}

}  // namespace

TEST_CLASS(OrfsAdapterTests){
  public : TEST_METHOD(ProbeUsesManagedCheckoutAndDistribution){
      adapters::OrfsAdapter adapter;
core::ToolchainProfile profile;
profile.orfs_root = "~/custom/orfs";
profile.openlane_root = "~/custom/openlane2";
profile.wsl_distribution = "Ubuntu-24.04";
const runtime::WslCommand command = adapter.BuildProbeCommand(profile);
Assert::AreEqual(std::wstring(L"/bin/bash"), command.program);
Assert::IsTrue(command.distribution.has_value());
Assert::AreEqual(std::wstring(L"Ubuntu-24.04"), *command.distribution);
Assert::IsTrue(command.arguments[1].find(L"flow/Makefile") !=
               std::wstring::npos);
Assert::IsTrue(command.arguments[1].find(L"OPENROAD_EXE") !=
               std::wstring::npos);
Assert::IsTrue(command.arguments[1].find(L"nix-command flakes") !=
               std::wstring::npos);
Assert::IsTrue(command.arguments[1].find(L"-unit_delay") != std::wstring::npos);
Assert::IsTrue(command.arguments[1].find(L"-hierarchy") != std::wstring::npos);
Assert::IsTrue(command.arguments[1].find(L"read_lib -h") != std::wstring::npos);
Assert::IsTrue(command.arguments[1].find(L"help repair_timing") !=
               std::wstring::npos);
Assert::IsTrue(command.arguments[1].find(L"grep -Fq -- -sequence") !=
               std::wstring::npos);
Assert::IsTrue(command.arguments[1].find(L"--override-input openroad") !=
               std::wstring::npos);
Assert::IsTrue(command.arguments[1].find(L"--override-input yosys") !=
               std::wstring::npos);
Assert::IsTrue(command.arguments[1].find(L"tools/yosys?submodules=1") !=
               std::wstring::npos);
Assert::AreEqual(std::size_t(0), command.environment.size());
Assert::AreEqual(std::wstring(L"~/custom/orfs"), command.arguments.back());
}  // namespace designpp::tests

TEST_METHOD(PlatformDiscoveryFiltersAndClassifiesContracts) {
  adapters::OrfsAdapter adapter;
  core::ToolchainProfile profile;
  profile.orfs_root = "~/orfs";
  const runtime::WslCommand command =
      adapter.BuildPlatformDiscoveryCommand(profile);
  Assert::IsTrue(command.arguments[1].find(L"flow/platforms") !=
                 std::wstring::npos);
  const auto parsed = adapter.ParsePlatformDiscovery(
      "common|ready|\nsky130hd|ready|\nnangate45|unavailable|missing "
      "config.mk\n"
      ".hidden|ready|\n");
  Assert::IsTrue(parsed.Ok());
  Assert::AreEqual(std::size_t(2), parsed.Value().size());
  Assert::AreEqual(std::string("nangate45"), parsed.Value()[0].name);
  Assert::IsFalse(parsed.Value()[0].runnable);
  Assert::AreEqual(std::string("sky130hd"), parsed.Value()[1].name);
  Assert::IsTrue(parsed.Value()[1].runnable);
}

TEST_METHOD(OrfsFlakeExecutionDoesNotReuseOpenLaneShellOrDryRunMake) {
  adapters::OrfsAdapter adapter;
  adapters::OrfsRequest request = SampleRequest();
  request.tool_mode = "orfs-flake";
  request.profile.openlane_root = "/home/test/openlane2";
  const auto plan = adapter.BuildPlan(request);
  Assert::IsTrue(plan.Ok());
  const std::wstring cache_only =
      L"--offline --max-jobs 0 --builders '' --option fallback false";
  Assert::IsTrue(plan.Value().execute.arguments[1].find(cache_only) !=
                 std::wstring::npos);
  const auto probe = adapter.BuildProbeCommand(request.profile);
  Assert::IsTrue(probe.arguments[1].find(cache_only) != std::wstring::npos);
  Assert::AreEqual(std::wstring(L"/bin/bash"), plan.Value().execute.program);
  Assert::IsTrue(plan.Value().execute.arguments[1].find(
                     L"nix-command flakes") != std::wstring::npos);
  Assert::IsTrue(plan.Value().execute.arguments[1].find(
                     L"--no-write-lock-file --override-input yosys") !=
                 std::wstring::npos);
  Assert::IsTrue(plan.Value().execute.arguments[1].find(
                     L"git+file://$root/tools/yosys?submodules=1") !=
                 std::wstring::npos);
  Assert::IsTrue(plan.Value().execute.arguments[1].find(
                     L"git+file://$root/tools/eqy") != std::wstring::npos);
  Assert::IsTrue(plan.Value().execute.arguments[1].find(L"openlane_root") ==
                 std::wstring::npos);
  Assert::IsTrue(plan.Value().execute.arguments[1].find(
                     L"-j 1 DESIGN_CONFIG=") != std::wstring::npos);
  Assert::IsTrue(plan.Value().execute.arguments[1].find(
                     L"NUM_CORES=\"$jobs\"") != std::wstring::npos);
  Assert::IsTrue(plan.Value().execute.arguments[1].find(L"-j \"$jobs\"") ==
                 std::wstring::npos);
  Assert::IsTrue(plan.Value().validate.arguments[1].find(L"make -n") ==
                 std::wstring::npos);
  Assert::IsTrue(plan.Value().validate.arguments[1].find(L"cp -R") ==
                 std::wstring::npos);
  Assert::IsTrue(plan.Value().validate.arguments[1].find(L"-delete") ==
                 std::wstring::npos);
}

TEST_METHOD(ConfigMapsSourcesAndStageTargets) {
  adapters::OrfsAdapter adapter;
  const auto plan = adapter.BuildPlan(SampleRequest());
  Assert::IsTrue(plan.Ok());
  const std::string& config = plan.Value().config_makefile;
  Assert::IsTrue(config.find("export VERILOG_FILES :=") != std::string::npos);
  Assert::IsTrue(config.find("export DESIGN_NAME := counter") !=
                 std::string::npos);
  Assert::IsTrue(config.find("PLATFORM := sky130hd") != std::string::npos);
  Assert::IsTrue(config.find("DESIGN_NAME := counter") != std::string::npos);
  Assert::IsTrue(config.find("/mnt/c/staging/src/source_0.sv") !=
                 std::string::npos);
  Assert::IsTrue(config.find("VERILOG_INCLUDE_DIRS :=") != std::string::npos);
  Assert::IsTrue(config.find("VERILOG_DEFINES := SYNTHESIS\\=1") !=
                 std::string::npos);
  Assert::IsTrue(config.find("SYNTH_PARAMETERS := WIDTH\\=8") !=
                 std::string::npos);
  Assert::IsTrue(
      config.find("SDC_FILE := /mnt/c/staging/constraints/constraints.sdc") !=
      std::string::npos);
  Assert::IsTrue(config.find("ABC_CLOCK_PERIOD_IN_PS := 10000") !=
                 std::string::npos);
  Assert::IsTrue(
      config.find(
          "OPENROAD_EXE := $(WORK_HOME)/designpp-openroad-wrapper.sh") !=
      std::string::npos);
  Assert::IsTrue(config.find("DESIGNPP_OPENROAD_INIT := "
                             "$(WORK_HOME)/designpp-openroad-init.tcl") !=
                 std::string::npos);
  Assert::IsTrue(plan.Value().openroad_init.find("set_cmd_units -time ns") !=
                 std::string::npos);
  Assert::IsTrue(plan.Value().openroad_init.find(
                     "rename read_sdc designpp_original_read_sdc") !=
                 std::string::npos);
  Assert::IsTrue(plan.Value().openroad_wrapper.find(
                     "source {%s}\\nsource {%s}") != std::string::npos);
  Assert::IsTrue(plan.Value().openroad_wrapper.find(
                     "candidate=${args[$index]}") != std::string::npos);
  Assert::IsTrue(plan.Value().openroad_wrapper.find(
                     "args[$script_index]=$combined") != std::string::npos);
  Assert::AreEqual(std::string("synth"),
                   adapter.TargetForStage(core::StageId::kSynthesis));
  Assert::AreEqual(std::string("gui_route"),
                   adapter.GuiTargetForStage(core::StageId::kRouting));
  Assert::AreEqual(
      std::string("5_route.odb"),
      adapter.CheckpointForStage(core::StageId::kRouting).generic_string());
  Assert::AreEqual(std::wstring(L"/bin/bash"), plan.Value().execute.program);
  Assert::IsTrue(
      plan.Value().execute.arguments[1].find(
          L"chmod 700 \"$workspace/designpp-openroad-wrapper.sh\"") !=
      std::wstring::npos);
  Assert::IsTrue(plan.Value().execute.arguments[1].find(
                     L"designpp-openroad-home/.openroad") ==
                 std::wstring::npos);
}

TEST_METHOD(CombinationalConfigurationOmitsClockPeriod) {
  adapters::OrfsAdapter adapter;
  adapters::OrfsRequest request = SampleRequest();
  request.configuration.clock_ports.clear();
  request.configuration.clock_period_ns.clear();
  request.effective_clock_period_ns.reset();
  const auto plan = adapter.BuildPlan(request);
  Assert::IsTrue(plan.Ok());
  Assert::IsTrue(plan.Value().config_makefile.find("CLOCK_PERIOD :=") ==
                 std::string::npos);
}

TEST_METHOD(OpenRoadInitPreservesExplicitSdcCommandUnit) {
  adapters::OrfsAdapter adapter;
  adapters::OrfsRequest request = SampleRequest();
  request.sdc_time_unit = "ps";
  const auto plan = adapter.BuildPlan(request);
  Assert::IsTrue(plan.Ok());
  Assert::IsTrue(plan.Value().openroad_init.find("set_cmd_units -time ps") !=
                 std::string::npos);
  request.sdc_time_unit = "fortnight";
  Assert::IsFalse(adapter.BuildPlan(request).Ok());
}

TEST_METHOD(FloorplanInitializationMethodsAreExclusive) {
  adapters::OrfsAdapter adapter;
  auto request = SampleRequest();
  auto absolute = adapter.BuildPlan(request);
  Assert::IsTrue(absolute.Ok());
  Assert::IsTrue(absolute.Value().config_makefile.find("CORE_UTILIZATION") ==
                 std::string::npos);
  Assert::IsTrue(absolute.Value().config_makefile.find("export CORE_AREA :=") !=
                 std::string::npos);
  request.configuration.die_area.clear();
  request.configuration.core_area.clear();
  auto automatic = adapter.BuildPlan(request);
  Assert::IsTrue(automatic.Ok());
  Assert::IsTrue(automatic.Value().config_makefile.find(
                     "export CORE_UTILIZATION :=") != std::string::npos);
  Assert::IsTrue(automatic.Value().config_makefile.find(
                     "export CORE_AREA :=") == std::string::npos);
}

TEST_METHOD(Utf8StagingValidationDoesNotUseWindowsCodePage) {
  adapters::OrfsAdapter adapter;
  adapters::OrfsRequest request = SampleRequest();
  request.staging_workspace = "/mnt/c/한글 staging";
  const auto plan = adapter.BuildPlan(request);
  Assert::IsTrue(plan.Ok());
  Assert::IsTrue(plan.Value().config_makefile.find("/mnt/c/한글\\ staging") !=
                 std::string::npos);
}

TEST_METHOD(Asap7MapsTheCornerDffLibertyForYosys) {
  adapters::OrfsAdapter adapter;
  adapters::OrfsRequest request = SampleRequest();
  request.configuration.orfs.platform = "asap7";
  const auto plan = adapter.BuildPlan(request);

  Assert::IsTrue(plan.Ok());
  Assert::IsTrue(plan.Value().config_makefile.find(
                     "export DFF_LIB_FILE = $(strip $($(CORNER)_$(LIB_MODEL)_"
                     "DFF_LIB_FILE))") != std::string::npos);
  Assert::IsTrue(plan.Value().config_makefile.find("ABC_AREA") ==
                 std::string::npos);
  Assert::IsTrue(plan.Value().config_makefile.find("DESIGNPP_ABC_LIB") ==
                 std::string::npos);
  Assert::IsTrue(plan.Value().config_makefile.find("SYNTH_SCRIPT") ==
                 std::string::npos);
}

TEST_METHOD(NonAsap7DoesNotPrepareACompatibilityLiberty) {
  adapters::OrfsAdapter adapter;
  const auto plan = adapter.BuildPlan(SampleRequest());

  Assert::IsTrue(plan.Ok());
  Assert::IsTrue(plan.Value().config_makefile.find("DESIGNPP_ABC_LIB") ==
                 std::string::npos);
}

TEST_METHOD(Asap7AllowsAnExplicitAbcStrategyOverride) {
  adapters::OrfsAdapter adapter;
  adapters::OrfsRequest request = SampleRequest();
  request.configuration.orfs.platform = "asap7";
  request.configuration.orfs.advanced_variables_json = R"({"ABC_AREA":0})";

  const auto plan = adapter.BuildPlan(request);

  Assert::IsTrue(plan.Ok());
  const std::size_t explicit_value =
      plan.Value().config_makefile.find("export ABC_AREA := 0");
  Assert::IsTrue(explicit_value != std::string::npos);
  Assert::IsTrue(plan.Value().config_makefile.find("ABC_AREA ?=") ==
                 std::string::npos);
}

TEST_METHOD(FullLayoutUsesAllTargetAndEscapesStagingSpaces) {
  adapters::OrfsAdapter adapter;
  adapters::OrfsRequest request = SampleRequest();
  request.target_stage = core::StageId::kFinalOutputs;
  request.full_flow = true;
  request.staging_workspace = "/mnt/c/staging area";
  const auto plan = adapter.BuildPlan(request);
  Assert::IsTrue(plan.Ok());
  Assert::AreEqual(std::string("all"), plan.Value().target);
  Assert::IsTrue(plan.Value().config_makefile.find(
                     "/mnt/c/staging\\ area/src/source_0.sv") !=
                 std::string::npos);
  Assert::IsTrue(std::find(plan.Value().execute.arguments.begin(),
                           plan.Value().execute.arguments.end(),
                           L"all") != plan.Value().execute.arguments.end());
}

TEST_METHOD(AdvancedVariablesRejectInjectionAndPreserveMetrics) {
  adapters::OrfsAdapter adapter;
  Assert::IsTrue(adapter
                     .ValidateAdvancedVariables(
                         R"({"CTS_DISTANCE":12,"FASTROUTE_TCL":"none"})")
                     .Ok());
  Assert::IsFalse(
      adapter.ValidateAdvancedVariables(R"({"DESIGN_CONFIG":"bad"})").Ok());
  Assert::IsFalse(adapter.ValidateAdvancedVariables(R"({"X":{"Y":1}})").Ok());
  Assert::IsFalse(
      adapter.ValidateAdvancedVariables(R"JSON({"X":"$(shell whoami)"})JSON")
          .Ok());
  Assert::IsFalse(
      adapter.ValidateAdvancedVariables(R"JSON({"X":"relative/path"})JSON")
          .Ok());
  Assert::IsFalse(
      adapter.ValidateAdvancedVariables(R"JSON({"MY_FILE":"value"})JSON").Ok());
  Assert::IsFalse(adapter.ValidateAdvancedVariables(R"JSON({"X":1)JSON").Ok());
  const auto canonical =
      adapter.CanonicalizeAdvancedVariables(R"JSON({"Z":1,"A":["x","y"]})JSON");
  Assert::IsTrue(canonical.Ok());
  Assert::IsTrue(canonical.Value().find("\"A\": [\"x\",\"y\"]") <
                 canonical.Value().find("\"Z\": 1"));
  const auto metrics = adapter.ParseMetrics(
      R"({"timing__setup__wns":1.0,"timing__setup__wns":-0.2,"future":7,"label":"route","enabled":true})");
  Assert::IsTrue(metrics.Ok());
  Assert::AreEqual(std::size_t(5), metrics.Value().raw_entries.size());
  Assert::AreEqual(-0.2, *metrics.Value().setup_wns, 0.0001);
  Assert::IsTrue(metrics.Value().raw.contains("future"));
  Assert::IsTrue(metrics.Value().raw.contains("label"));
  Assert::IsTrue(metrics.Value().raw.contains("enabled"));
  Assert::IsFalse(adapter.ParseMetrics(R"({"future":})").Ok());
  Assert::IsFalse(adapter.ParseMetrics(R"({"future":1)").Ok());
}

TEST_METHOD(ParsesStagePrefixedOrfs26Q2Metrics) {
  adapters::OrfsAdapter adapter;
  const auto metrics = adapter.ParseMetrics(
      R"({"floorplan__design__die__area":95.199,"finish__design__die__area":95.199,"finish__design__core__area":58.3783,"finish__design__instance__utilization":0.539211,"finish__timing__setup__ws":-186.723,"finish__timing__setup__tns":-4814.77,"finish__timing__hold__ws":-18.5083,"finish__timing__hold__tns":-18.5083,"finish__clock__skew__setup":4.68795,"finish__clock__skew__hold":7.8643,"detailedroute__route__drc_errors":0,"detailedroute__route__wirelength":455})");
  Assert::IsTrue(metrics.Ok());
  Assert::AreEqual(95.199, *metrics.Value().die_area, 0.0001);
  Assert::AreEqual(58.3783, *metrics.Value().core_area, 0.0001);
  Assert::AreEqual(0.539211, *metrics.Value().utilization, 0.000001);
  Assert::AreEqual(-186.723, *metrics.Value().setup_wns, 0.0001);
  Assert::AreEqual(-18.5083, *metrics.Value().hold_wns, 0.0001);
  Assert::AreEqual(4.68795, *metrics.Value().worst_setup_skew, 0.00001);
  Assert::AreEqual(0.0, *metrics.Value().routing_violations, 0.0001);
  Assert::AreEqual(455.0, *metrics.Value().wire_length, 0.0001);
}
}
;

}  // namespace designpp::tests

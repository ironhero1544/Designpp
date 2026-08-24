// Copyright 2026 The Design++ Authors

#include <CppUnitTest.h>
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "designpp/adapters/cocotb_runner_adapter.h"
#include "designpp/adapters/icarus_debug_adapter.h"
#include "designpp/adapters/icarus_simulation_adapter.h"
#include "designpp/adapters/opensta_adapter.h"
#include "designpp/adapters/verilator_adapter.h"
#include "designpp/adapters/verilator_simulation_adapter.h"
#include "designpp/adapters/yosys_adapter.h"
#include "designpp/application/debug_session_service.h"
#include "designpp/application/project_service.h"
#include "designpp/core/project.h"
#include "designpp/runtime/path_mapper.h"
#include "designpp/runtime/resource_coordinator.h"

using Microsoft::VisualStudio::CppUnitTestFramework::Assert;

namespace designpp::tests {

TEST_CLASS(PathMapperTests){
  public :
      TEST_METHOD(WindowsDrivePathMapsToWslMount){runtime::PathMapper mapper;
auto mapped = mapper.WindowsToWsl(L"C:\\Design++\\rtl\\top.sv");
Assert::IsTrue(mapped.Ok());
Assert::AreEqual(std::wstring(L"/mnt/c/Design++/rtl/top.sv"), mapped.Value());
}  // namespace designpp::tests

TEST_METHOD(RelativeAndUncPathsAreRejected) {
  runtime::PathMapper mapper;
  Assert::IsFalse(mapper.WindowsToWsl(L"rtl\\top.sv").Ok());
  Assert::IsFalse(mapper.WindowsToWsl(L"\\\\server\\share\\top.sv").Ok());
}
}
;

TEST_CLASS(VerilatorAdapterTests){
  public : TEST_METHOD(BuildsStructuredLintCommand){core::Project project;
project.top_module = "counter";
project.cpu_budget = 2;
project.defines.push_back("SIM=1");
application::ResolvedSource source;
source.view_kind = core::ViewKind::kVerilog;
source.windows_path = L"C:\\Design\\top.sv";
source.library_directory = L"C:\\Design";
source.relative_path = "top.sv";
source.enabled = true;
source.exists = true;
adapters::VerilatorAdapter adapter;
runtime::PathMapper mapper;
auto command =
    adapter.BuildCommand(project, {source}, mapper,
                         adapters::ToolCapabilities{"Verilator", "5.0", true});
Assert::IsTrue(command.Ok());
Assert::AreEqual(std::wstring(L"verilator"), command.Value().program);
Assert::IsTrue(std::find(command.Value().arguments.begin(),
                         command.Value().arguments.end(),
                         L"--Wno-fatal") != command.Value().arguments.end());
Assert::IsTrue(std::find(command.Value().arguments.begin(),
                         command.Value().arguments.end(),
                         L"-Wno-DECLFILENAME") !=
               command.Value().arguments.end());
Assert::IsTrue(std::find(command.Value().arguments.begin(),
                         command.Value().arguments.end(),
                         L"/mnt/c/Design/top.sv") !=
               command.Value().arguments.end());
}

TEST_METHOD(ParsesVerilatorDiagnostics) {
  adapters::VerilatorAdapter adapter;
  const auto diagnostics = adapter.ParseDiagnostics(
      "%Error-WIDTH: /mnt/c/top.sv:12:7: Width mismatch\n"
      "%Warning-UNUSED: /mnt/c/top.sv:20:3: Signal unused\n");
  Assert::AreEqual(static_cast<std::size_t>(2), diagnostics.size());
  Assert::AreEqual(std::string("WIDTH"), diagnostics[0].code);
  Assert::AreEqual(static_cast<std::uint32_t>(12), diagnostics[0].line);
  Assert::AreEqual(static_cast<std::uint32_t>(7), diagnostics[0].column);
}
}
;

TEST_CLASS(IcarusSimulationAdapterTests){
  public : TEST_METHOD(BuildsTwoStepPlanForSelectedTestbenchView){
      adapters::SimulationRequest request;
request.project.top_module = "dut";
request.project.defines.push_back("SIMULATION=1");
request.testbench_view_id = "11111111-1111-1111-1111-111111111111";
request.testbench_relative_path = "cells/c/views/tb/files/dut_tb.sv";
request.testbench_top = "dut_tb";
request.waveform_enabled = true;
request.artifact_directory = L"C:\\Design\\run\\artifacts";
application::ResolvedSource rtl;
rtl.view_kind = core::ViewKind::kVerilog;
rtl.windows_path = L"C:\\Design\\rtl\\dut.sv";
rtl.library_directory = L"C:\\Design";
rtl.relative_path = "cells/c/views/rtl/files/dut.sv";
rtl.enabled = true;
rtl.exists = true;
application::ResolvedSource testbench = rtl;
testbench.view_kind = core::ViewKind::kTestbench;
testbench.view_id = request.testbench_view_id;
testbench.windows_path = L"C:\\Design\\tb\\dut_tb.sv";
testbench.relative_path = request.testbench_relative_path;
application::ResolvedSource other = testbench;
other.view_id = "22222222-2222-2222-2222-222222222222";
other.windows_path = L"C:\\Design\\tb\\other_tb.sv";
other.relative_path = "cells/c/views/other/files/other_tb.sv";
request.sources = {rtl, testbench, other};

adapters::IcarusSimulationAdapter adapter;
runtime::PathMapper mapper;
auto plan = adapter.BuildPlan(request, mapper);
Assert::IsTrue(plan.Ok());
Assert::AreEqual(std::wstring(L"iverilog"), plan.Value().compile.program);
Assert::AreEqual(std::wstring(L"vvp"), plan.Value().execute.program);
Assert::IsTrue(std::find(plan.Value().execute.arguments.begin(),
                         plan.Value().execute.arguments.end(),
                         L"-N") != plan.Value().execute.arguments.end());
Assert::IsTrue(plan.Value().wrapper_text.find("dut_tb designpp_testbench") !=
               std::string::npos);
Assert::IsTrue(std::find(plan.Value().compile.arguments.begin(),
                         plan.Value().compile.arguments.end(),
                         L"/mnt/c/Design/tb/dut_tb.sv") !=
               plan.Value().compile.arguments.end());
Assert::IsTrue(std::find(plan.Value().compile.arguments.begin(),
                         plan.Value().compile.arguments.end(),
                         L"/mnt/c/Design/tb/other_tb.sv") ==
               plan.Value().compile.arguments.end());
}

TEST_METHOD(ParsesIcarusDiagnostics) {
  adapters::IcarusSimulationAdapter adapter;
  const auto diagnostics = adapter.ParseDiagnostics(
      "/mnt/c/dut_tb.sv:12: syntax error\n"
      "/mnt/c/dut.sv:8: warning: implicit wire\n");
  Assert::AreEqual(static_cast<std::size_t>(2), diagnostics.size());
  Assert::AreEqual(static_cast<std::uint32_t>(12), diagnostics[0].line);
  Assert::IsTrue(diagnostics[0].severity == core::DiagnosticSeverity::kError);
}

TEST_METHOD(BuildsInteractiveDebugPlanAndValidatesCommands) {
  adapters::SimulationRequest request;
  request.project.top_module = "dut";
  request.testbench_view_id = "11111111-1111-1111-1111-111111111111";
  request.testbench_relative_path = "cells/c/views/tb/files/dut_tb.sv";
  request.testbench_top = "dut_tb";
  request.waveform_enabled = true;
  request.artifact_directory = L"C:\\Design\\run\\artifacts";
  application::ResolvedSource rtl;
  rtl.view_kind = core::ViewKind::kVerilog;
  rtl.windows_path = L"C:\\Design\\rtl\\dut.sv";
  rtl.library_directory = L"C:\\Design";
  rtl.relative_path = "cells/c/views/rtl/files/dut.sv";
  rtl.enabled = true;
  rtl.exists = true;
  application::ResolvedSource testbench = rtl;
  testbench.view_kind = core::ViewKind::kTestbench;
  testbench.view_id = request.testbench_view_id;
  testbench.windows_path = L"C:\\Design\\tb\\dut_tb.sv";
  testbench.relative_path = request.testbench_relative_path;
  request.sources = {rtl, testbench};
  adapters::IcarusDebugAdapter adapter;
  runtime::PathMapper mapper;
  auto plan = adapter.BuildDebugPlan(request, mapper);
  Assert::IsTrue(plan.Ok());
  Assert::IsTrue(plan.Value().simulation.execute.interactive_input);
  Assert::AreEqual(std::wstring(L"-i"),
                   plan.Value().simulation.execute.arguments[0]);
  Assert::AreEqual(std::wstring(L"-s"),
                   plan.Value().simulation.execute.arguments[1]);
  Assert::IsTrue(adapter.ValidateConsoleCommand("$display signal").Ok());
  Assert::IsFalse(adapter.ValidateConsoleCommand("load unsafe").Ok());
  Assert::IsFalse(adapter.ValidateConsoleCommand("$system shell").Ok());
}

TEST_METHOD(ParsesFragmentedVvpPromptAndScopeData) {
  adapters::IcarusDebugProtocolParser parser;
  auto first = parser.Feed("** VVP Stop(0) **\n** Current simulation time ");
  Assert::IsTrue(first.empty());
  auto stopped = parser.Feed("is 0 ticks.\n> ");
  Assert::IsTrue(
      std::any_of(stopped.begin(), stopped.end(), [](const auto& event) {
        return event.kind == adapters::DebugProtocolEventKind::kStopped;
      }));
  parser.SetPendingCommand("list");
  auto items = parser.Feed(
      "list\n2 items in this scope:\nreg : count[3:0]\nmodule : dut\n> ");
  const auto scope =
      std::find_if(items.begin(), items.end(), [](const auto& event) {
        return event.kind == adapters::DebugProtocolEventKind::kScopeItems;
      });
  Assert::IsTrue(scope != items.end());
  Assert::AreEqual(static_cast<std::size_t>(2), scope->scope_items.size());
  Assert::AreEqual(std::string("count[3:0]"), scope->scope_items[0].name);
}

TEST_METHOD(DebugSessionSerializesCommandsAtPrompts) {
  application::DebugSessionService service;
  service.BeginProbe();
  service.MarkStarting();
  service.Apply({{adapters::DebugProtocolEventKind::kStopped, {}, "0"},
                 {adapters::DebugProtocolEventKind::kPromptReady}});
  Assert::IsTrue(service.Snapshot().state == application::DebugState::kPaused);
  auto step = service.PrepareCommand(application::DebugCommandKind::kStep);
  Assert::IsTrue(step.Ok());
  Assert::AreEqual(std::string("step"), step.Value());
  Assert::IsFalse(
      service.PrepareCommand(application::DebugCommandKind::kContinue).Ok());
  service.Apply({{adapters::DebugProtocolEventKind::kStopped, {}, "1"},
                 {adapters::DebugProtocolEventKind::kPromptReady}});
  auto unsafe = service.PrepareCommand(application::DebugCommandKind::kConsole,
                                       "load unsafe");
  Assert::IsFalse(unsafe.Ok());
  Assert::IsTrue(service.Snapshot().prompt_ready);
}
}
;

TEST_CLASS(VerilatorSimulationAdapterTests){
  public :
      TEST_METHOD(BuildsFstSimulationPlan){adapters::SimulationRequest request;
request.project.top_module = "dut";
request.project.cpu_budget = 2;
request.testbench_view_id = "11111111-1111-1111-1111-111111111111";
request.testbench_relative_path = "cells/c/views/tb/files/dut_tb.sv";
request.testbench_top = "dut_tb";
request.waveform_enabled = true;
request.waveform_format = "fst";
request.artifact_directory = L"C:\\Design\\run\\artifacts";
request.build_directory = L"C:\\Temp\\DesignPlusPlus\\verilator\\run";
application::ResolvedSource rtl;
rtl.view_kind = core::ViewKind::kVerilog;
rtl.windows_path = L"C:\\Design\\rtl\\dut.sv";
rtl.library_directory = L"C:\\Design";
rtl.relative_path = "cells/c/views/rtl/files/dut.sv";
rtl.enabled = true;
rtl.exists = true;
application::ResolvedSource testbench = rtl;
testbench.view_kind = core::ViewKind::kTestbench;
testbench.view_id = request.testbench_view_id;
testbench.windows_path = L"C:\\Design\\tb\\dut_tb.sv";
testbench.relative_path = request.testbench_relative_path;
request.sources = {rtl, testbench};

adapters::VerilatorSimulationAdapter adapter;
runtime::PathMapper mapper;
auto plan = adapter.BuildPlan(request, mapper);
Assert::IsTrue(plan.Ok());
Assert::AreEqual(std::wstring(L"verilator"), plan.Value().compile.program);
Assert::IsTrue(std::find(plan.Value().compile.arguments.begin(),
                         plan.Value().compile.arguments.end(),
                         L"--trace-fst") !=
               plan.Value().compile.arguments.end());
Assert::IsTrue(std::find(plan.Value().compile.arguments.begin(),
                         plan.Value().compile.arguments.end(),
                         L"--Wno-fatal") !=
               plan.Value().compile.arguments.end());
Assert::IsTrue(std::find(plan.Value().compile.arguments.begin(),
                         plan.Value().compile.arguments.end(),
                         L"-Wno-DECLFILENAME") !=
               plan.Value().compile.arguments.end());
Assert::AreEqual(
    std::wstring(L"/mnt/c/Temp/DesignPlusPlus/verilator/run/simulation"),
    plan.Value().execute.program);
Assert::AreEqual(std::wstring(L"waveform.fst"),
                 plan.Value().waveform_path.filename().wstring());
}

TEST_METHOD(BuildsCompatibleVcdTracePlan) {
  adapters::SimulationRequest request;
  request.project.top_module = "dut";
  request.testbench_view_id = "11111111-1111-1111-1111-111111111111";
  request.testbench_relative_path = "cells/c/views/tb/files/dut_tb.sv";
  request.testbench_top = "dut_tb";
  request.waveform_enabled = true;
  request.waveform_format = "vcd";
  request.artifact_directory = L"C:\\Design\\run\\artifacts";
  application::ResolvedSource rtl;
  rtl.view_kind = core::ViewKind::kVerilog;
  rtl.windows_path = L"C:\\Design\\rtl\\dut.sv";
  rtl.library_directory = L"C:\\Design";
  rtl.relative_path = "cells/c/views/rtl/files/dut.sv";
  rtl.enabled = true;
  rtl.exists = true;
  application::ResolvedSource testbench = rtl;
  testbench.view_kind = core::ViewKind::kTestbench;
  testbench.view_id = request.testbench_view_id;
  testbench.windows_path = L"C:\\Design\\tb\\dut_tb.sv";
  testbench.relative_path = request.testbench_relative_path;
  request.sources = {rtl, testbench};

  adapters::VerilatorSimulationAdapter adapter;
  runtime::PathMapper mapper;
  auto plan = adapter.BuildPlan(request, mapper);
  Assert::IsTrue(plan.Ok());
  Assert::IsTrue(std::find(plan.Value().compile.arguments.begin(),
                           plan.Value().compile.arguments.end(),
                           L"--trace") != plan.Value().compile.arguments.end());
  Assert::IsTrue(std::find(plan.Value().compile.arguments.begin(),
                           plan.Value().compile.arguments.end(),
                           L"--trace-vcd") ==
                 plan.Value().compile.arguments.end());
}

TEST_METHOD(IcarusFstPlanSelectsFstRuntime) {
  adapters::SimulationRequest request;
  request.project.top_module = "dut";
  request.testbench_view_id = "11111111-1111-1111-1111-111111111111";
  request.testbench_relative_path = "cells/c/views/tb/files/dut_tb.sv";
  request.testbench_top = "dut_tb";
  request.waveform_enabled = true;
  request.waveform_format = "fst";
  request.artifact_directory = L"C:\\Design\\run\\artifacts";
  application::ResolvedSource rtl;
  rtl.view_kind = core::ViewKind::kVerilog;
  rtl.windows_path = L"C:\\Design\\rtl\\dut.sv";
  rtl.library_directory = L"C:\\Design";
  rtl.relative_path = "cells/c/views/rtl/files/dut.sv";
  rtl.enabled = true;
  rtl.exists = true;
  application::ResolvedSource testbench = rtl;
  testbench.view_kind = core::ViewKind::kTestbench;
  testbench.view_id = request.testbench_view_id;
  testbench.windows_path = L"C:\\Design\\tb\\dut_tb.sv";
  testbench.relative_path = request.testbench_relative_path;
  request.sources = {rtl, testbench};

  adapters::IcarusSimulationAdapter adapter;
  runtime::PathMapper mapper;
  auto plan = adapter.BuildPlan(request, mapper);
  Assert::IsTrue(plan.Ok());
  Assert::AreEqual(std::wstring(L"-fst"), plan.Value().execute.arguments[0]);
  Assert::AreEqual(std::wstring(L"waveform.fst"),
                   plan.Value().waveform_path.filename().wstring());
}
}
;

TEST_CLASS(YosysAdapterTests){
  public : TEST_METHOD(BuildsSynthesisScriptAndParsesMetrics){
      const std::filesystem::path source_path =
          std::filesystem::temp_directory_path() / L"designpp-yosys-top.sv";
std::ofstream(source_path, std::ios::binary)
    << "module top(input logic a, output logic y); assign y = a; "
       "endmodule\n";
adapters::SynthesisRequest request;
request.project.top_module = "top";
request.project.defines.push_back("FEATURE=1; unsafe");
request.artifact_directory = L"C:\\Design\\run\\artifacts";
application::ResolvedSource source;
source.view_kind = core::ViewKind::kVerilog;
source.windows_path = source_path;
source.library_directory = source_path.parent_path();
source.relative_path = "top.sv";
source.enabled = true;
source.exists = true;
request.sources = {source};
const std::filesystem::path liberty_path =
    std::filesystem::temp_directory_path() / L"designpp-yosys-top.lib";
std::ofstream(liberty_path, std::ios::binary) << "library(test) {}\n";
request.liberty_files = {liberty_path};

adapters::YosysAdapter adapter;
runtime::PathMapper mapper;
auto plan = adapter.BuildPlan(request, mapper);
Assert::IsTrue(plan.Ok());
Assert::AreEqual(std::wstring(L"yosys"), plan.Value().execute.program);
Assert::IsTrue(plan.Value().script_text.find("hierarchy -check -top top") !=
               std::string::npos);
Assert::IsTrue(plan.Value().script_text.find("synth -top top -noabc") !=
               std::string::npos);
Assert::IsTrue(plan.Value().script_text.find("abc -liberty") !=
               std::string::npos);
Assert::IsTrue(plan.Value().script_text.find("abc -g") == std::string::npos);
Assert::IsTrue(plan.Value().script_text.find("write_json") !=
               std::string::npos);
Assert::IsTrue(plan.Value().script_text.find("\"-DFEATURE=1; unsafe\"") !=
               std::string::npos);
Assert::IsTrue(plan.Value().script_text.find("stat -json -top top -liberty") !=
               std::string::npos);
Assert::IsTrue(plan.Value().script_text.find("write_json netlist.json") !=
               std::string::npos);
Assert::IsTrue(
    plan.Value().script_text.find("write_json schematic-structural.json") !=
    std::string::npos);
Assert::IsTrue(plan.Value().script_text.find("tee -o synthesis.rpt") !=
               std::string::npos);
Assert::IsTrue(plan.Value().script_text.find("/mnt/c/Design/run/artifacts") ==
               std::string::npos);
Assert::AreEqual(std::wstring(L"synthesis.ys"),
                 plan.Value().execute.arguments.back());
request.liberty_files.clear();
auto generic_plan = adapter.BuildPlan(request, mapper);
Assert::IsTrue(generic_plan.Ok());
Assert::IsTrue(generic_plan.Value().script_text.find(
                   "abc -g AND,OR,XOR,XNOR,NAND,NOR") != std::string::npos);
Assert::IsTrue(generic_plan.Value().script_text.find("abc -liberty") ==
               std::string::npos);
const auto metrics = adapter.ParseStatistics(
    "{\"modules\":{\"top\":{\"num_cells\":12,\"area\":4.5}}}");
Assert::IsTrue(metrics.Ok());
Assert::AreEqual(static_cast<std::uint64_t>(12), metrics.Value().cell_count);
Assert::IsTrue(metrics.Value().has_area);
std::error_code error;
std::filesystem::remove(source_path, error);
std::filesystem::remove(liberty_path, error);
}

TEST_METHOD(RejectsTopMissingFromEnabledRtl) {
  const std::filesystem::path source_path =
      std::filesystem::temp_directory_path() / L"designpp-yosys-other.sv";
  std::ofstream(source_path, std::ios::binary) << "module other; endmodule\n";
  adapters::SynthesisRequest request;
  request.project.top_module = "top";
  request.artifact_directory = L"C:\\Design\\run\\artifacts";
  application::ResolvedSource source;
  source.view_kind = core::ViewKind::kVerilog;
  source.windows_path = source_path;
  source.relative_path = "other.sv";
  source.enabled = true;
  source.exists = true;
  request.sources = {source};
  Assert::IsFalse(adapters::YosysAdapter().Validate(request).Ok());
  std::error_code error;
  std::filesystem::remove(source_path, error);
}

TEST_METHOD(UsesOnlyEnabledRtlAndHonorsFlatten) {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / L"designpp-yosys-enabled";
  std::filesystem::create_directories(root);
  const std::filesystem::path enabled_path = root / L"top enabled.sv";
  std::ofstream(enabled_path, std::ios::binary)
      << "module top(input a, output y); assign y = a; endmodule\n";
  adapters::SynthesisRequest request;
  request.project.top_module = "top";
  request.artifact_directory = root / L"artifacts";
  request.flatten = true;
  application::ResolvedSource enabled;
  enabled.view_kind = core::ViewKind::kVerilog;
  enabled.windows_path = enabled_path;
  enabled.relative_path = "top enabled.sv";
  enabled.enabled = true;
  enabled.exists = true;
  application::ResolvedSource disabled = enabled;
  disabled.windows_path = root / L"disabled.sv";
  disabled.relative_path = "disabled.sv";
  disabled.enabled = false;
  request.sources = {enabled, disabled};

  auto plan =
      adapters::YosysAdapter().BuildPlan(request, runtime::PathMapper());
  Assert::IsTrue(plan.Ok());
  Assert::IsTrue(plan.Value().script_text.find("top enabled.sv") !=
                 std::string::npos);
  Assert::IsTrue(plan.Value().script_text.find("disabled.sv") ==
                 std::string::npos);
  Assert::IsTrue(plan.Value().script_text.find("flatten") != std::string::npos);
  std::error_code error;
  std::filesystem::remove_all(root, error);
}

TEST_METHOD(PreservesSignalsMetadataParametersAndSequentialPolarity) {
  constexpr std::string_view json = R"json({
    "modules": {"top": {
      "ports": {
        "clock": {"direction":"input", "bits":[2]},
        "reset_n": {"direction":"input", "bits":[3]},
        "enable": {"direction":"input", "bits":[4]},
        "q": {"direction":"output", "bits":[7,8]}
      },
      "netnames": {
        "q": {"hide_name":0, "bits":[7,8], "offset":4, "upto":1}
      },
      "cells": {"state": {
        "type":"$adffe",
        "parameters": {
          "CLK_POLARITY":"0", "ARST_POLARITY":"0",
          "EN_POLARITY":"1", "ARST_VALUE":"00"
        },
        "attributes":{"src":"state.sv:12.3-18.6"},
        "port_directions": {
          "ARST":"input", "CLK":"input", "D":"input",
          "EN":"input", "Q":"output"
        },
        "connections": {
          "ARST":[3], "CLK":[2], "D":[5,6], "EN":[4], "Q":[7,8]
        }
      }}
    }}
  })json";
  auto parsed = adapters::YosysAdapter().ParseNetlist(json, "top");
  Assert::IsTrue(parsed.Ok());
  Assert::AreEqual<std::size_t>(1U, parsed.Value().signals.size());
  Assert::AreEqual(4, parsed.Value().signals.front().offset);
  Assert::IsTrue(parsed.Value().signals.front().upto);
  const core::SchematicNode& node = parsed.Value().nodes.front();
  Assert::IsFalse(node.clock_positive);
  Assert::IsFalse(node.reset_positive);
  Assert::IsTrue(node.enable_positive);
  Assert::AreEqual(std::string("state.sv:12.3-18.6"),
                   node.attributes.at("src"));
  Assert::IsTrue(
      std::any_of(node.pins.begin(), node.pins.end(), [](const auto& pin) {
        return pin.role == core::SchematicPinRole::kEnable;
      }));
}

TEST_METHOD(UnknownCellRemainsGenericAndConnected) {
  constexpr std::string_view json = R"json({
    "modules":{"top":{"ports":{},"cells":{"mystery":{
      "type":"vendor_unknown", "parameters":{"WIDTH":"10"},
      "attributes":{"src":"unknown.sv:1"},
      "port_directions":{"A":"input","Y":"output"},
      "connections":{"A":[2,3],"Y":[4,5]}
    }}}}
  })json";
  auto parsed = adapters::YosysAdapter().ParseNetlist(json, "top");
  Assert::IsTrue(parsed.Ok());
  Assert::AreEqual<std::size_t>(1U, parsed.Value().nodes.size());
  const core::SchematicNode& node = parsed.Value().nodes.front();
  Assert::IsTrue(node.kind == core::SchematicNodeKind::kGeneric);
  Assert::AreEqual(std::string("vendor_unknown"), node.label);
  Assert::AreEqual<std::size_t>(2U, node.pins.size());
  Assert::AreEqual<std::size_t>(2U, node.pins.front().bits.size());
}

TEST_METHOD(ParsesGateSchematicFromYosysNetlist) {
  constexpr std::string_view json = R"json({
    "modules": {
      "half_adder": {
        "ports": {
          "a": {"direction": "input", "bits": [2]},
          "sum": {"direction": "output", "bits": [4]}
        },
        "cells": {
          "$xor": {
            "type": "$_XOR_",
            "port_directions": {"A": "input", "Y": "output"},
            "connections": {"A": [2], "Y": [4]}
          }
        }
      }
    }
  })json";
  auto schematic = adapters::YosysAdapter().ParseNetlist(json, "half_adder");
  Assert::IsTrue(schematic.Ok());
  Assert::AreEqual(std::string("half_adder"), schematic.Value().top_module);
  Assert::AreEqual(static_cast<std::size_t>(2), schematic.Value().ports.size());
  Assert::AreEqual(static_cast<std::size_t>(1), schematic.Value().nodes.size());
  Assert::AreEqual(std::string("$_XOR_"), schematic.Value().nodes.front().type);
  Assert::AreEqual(std::string("4"),
                   schematic.Value().nodes.front().pins.back().bits.front());
}

TEST_METHOD(ClassifiesLowercaseStructuralCellsForReadableView) {
  constexpr std::string_view json = R"json({
      "modules": {
        "top": {
          "ports": {},
          "cells": {
            "$counter": {
              "type": "$adffe",
              "parameters": {},
              "port_directions": {
                "ARST": "input", "CLK": "input", "D": "input",
                "EN": "input", "Q": "output"
              },
              "connections": {
                "ARST": [2], "CLK": [3], "D": [4], "EN": [5], "Q": [6]
              }
            },
            "$active": {
              "type": "$reduce_bool",
              "parameters": {},
              "port_directions": {"A": "input", "Y": "output"},
              "connections": {"A": [6, 7], "Y": [8]}
            }
          }
        }
      }
    })json";
  auto schematic = adapters::YosysAdapter().ParseNetlist(json, "top");

  Assert::IsTrue(schematic.Ok());
  Assert::AreEqual<std::size_t>(2U, schematic.Value().nodes.size());
  const auto counter = std::find_if(
      schematic.Value().nodes.begin(), schematic.Value().nodes.end(),
      [](const auto& node) { return node.type == "$adffe"; });
  const auto active = std::find_if(
      schematic.Value().nodes.begin(), schematic.Value().nodes.end(),
      [](const auto& node) { return node.type == "$reduce_bool"; });
  Assert::IsTrue(counter != schematic.Value().nodes.end());
  Assert::IsTrue(active != schematic.Value().nodes.end());
  Assert::IsTrue(counter->kind == core::SchematicNodeKind::kRegister);
  Assert::AreEqual(std::string("DFFE"), counter->label);
  Assert::IsTrue(active->kind == core::SchematicNodeKind::kComparator);
  Assert::AreEqual(std::string("ANY"), active->label);
}

TEST_METHOD(ParsesChildModuleForHierarchicalSchematicNavigation) {
  constexpr std::string_view json = R"json({
      "modules": {
        "top": {
          "ports": {},
          "cells": {
            "core": {
              "type": "vm80a_core",
              "port_directions": {"A": "input", "Y": "output"},
              "connections": {"A": [2], "Y": [3]}
            }
          }
        },
        "vm80a_core": {
          "ports": {
            "A": {"direction": "input", "bits": [2]},
            "Y": {"direction": "output", "bits": [3]}
          },
          "cells": {}
        }
      }
    })json";
  auto top = adapters::YosysAdapter().ParseNetlist(json, "top");
  auto child = adapters::YosysAdapter().ParseNetlist(json, "vm80a_core");

  Assert::IsTrue(top.Ok());
  Assert::IsTrue(child.Ok());
  Assert::AreEqual<std::size_t>(1U, top.Value().nodes.size());
  Assert::IsTrue(top.Value().nodes.front().kind ==
                 core::SchematicNodeKind::kModule);
  Assert::AreEqual(std::string("vm80a_core"), top.Value().nodes.front().type);
  Assert::AreEqual(std::string("vm80a_core"), top.Value().nodes.front().label);
  Assert::AreEqual(std::string("vm80a_core"), child.Value().top_module);
  Assert::AreEqual<std::size_t>(2U, child.Value().ports.size());
}
}
;

TEST_CLASS(CocotbRunnerAdapterTests){
  public : TEST_METHOD(ProbeUsesManagedDesignPlusPlusEnvironment){
      adapters::CocotbRunnerAdapter adapter;
const runtime::WslCommand version = adapter.BuildProbeCommand();
const runtime::WslCommand makefiles = adapter.BuildMakefilesProbeCommand();
Assert::AreEqual(std::wstring(L"/bin/bash"), version.program);
Assert::AreEqual(std::wstring(L"/bin/bash"), makefiles.program);
Assert::IsTrue(
    version.arguments.at(1).find(L"$HOME/.designpp/venv/bin/cocotb-config") !=
    std::wstring::npos);
Assert::IsTrue(makefiles.arguments.at(1).find(L"--makefiles") !=
               std::wstring::npos);
}

public:
TEST_METHOD(BuildsStructuredCocotbInvocation) {
  adapters::CocotbRequest request;
  request.module = "tests.counter";
  request.makefiles_directory = L"/usr/share/cocotb/makefiles";
  request.backend = "verilator";
  request.simulation.project.cpu_budget = 2;
  request.simulation.testbench_top = "top";
  request.simulation.waveform_enabled = true;
  request.simulation.waveform_format = "fst";
  request.simulation.artifact_directory = L"C:\\Design\\run\\artifacts";
  application::ResolvedSource source;
  source.view_kind = core::ViewKind::kVerilog;
  source.windows_path = L"C:\\Design\\rtl\\top.sv";
  source.relative_path = "top.sv";
  source.enabled = true;
  source.exists = true;
  request.simulation.sources = {source};
  auto plan =
      adapters::CocotbRunnerAdapter().BuildPlan(request, runtime::PathMapper());
  Assert::IsTrue(plan.Ok());
  Assert::AreEqual(std::wstring(L"/bin/bash"), plan.Value().execute.program);
  Assert::IsTrue(plan.Value().execute.arguments.at(1).find(
                     L"$HOME/.designpp/venv/bin") != std::wstring::npos);
  Assert::AreEqual(std::wstring(L"dump.fst"),
                   plan.Value().waveform_path.filename().wstring());
  const auto modules = std::find_if(
      plan.Value().execute.environment.begin(),
      plan.Value().execute.environment.end(),
      [](const auto& value) { return value.first == L"COCOTB_TEST_MODULES"; });
  Assert::IsTrue(modules != plan.Value().execute.environment.end());
  Assert::AreEqual(std::wstring(L"tests.counter"), modules->second);
}
}
;

TEST_CLASS(OpenStaAdapterTests){
  public : TEST_METHOD(BuildsTimingScriptAndParsesViolations){
      const std::filesystem::path directory =
          std::filesystem::temp_directory_path() / L"designpp-opensta-adapter";
std::filesystem::create_directories(directory);
const auto netlist = directory / L"netlist.v";
const auto liberty = directory / L"slow.lib";
const auto sdc = directory / L"top.sdc";
std::ofstream(netlist) << "module top; endmodule\n";
std::ofstream(liberty) << "library(test) {}\n";
std::ofstream(sdc) << "create_clock -period 10 clk\n";
adapters::TimingRequest request;
request.top_module = "top";
request.corner_name = "slow";
request.netlist_path = netlist;
request.liberty_files = {liberty};
request.sdc_path = sdc;
request.artifact_directory = directory / L"artifacts";
adapters::OpenStaAdapter adapter;
runtime::PathMapper mapper;
auto plan = adapter.BuildPlan(request, mapper);
Assert::IsTrue(plan.Ok());
Assert::IsTrue(plan.Value().script_text.find("link_design top") !=
               std::string::npos);
Assert::AreEqual(std::wstring(L"/bin/bash"), plan.Value().execute.program);
Assert::IsTrue(plan.Value().script_text.find("DESIGNPP_SETUP_WNS_BEGIN") !=
               std::string::npos);
Assert::IsTrue(plan.Value().script_text.find("DESIGNPP_HOLD_WNS_BEGIN") !=
               std::string::npos);
auto metrics = adapter.ParseReport(
    "DESIGNPP_SETUP_WNS_BEGIN\n-0.25\nDESIGNPP_SETUP_WNS_END\n"
    "DESIGNPP_SETUP_TNS_BEGIN\n-1.5\nDESIGNPP_SETUP_TNS_END\n"
    "DESIGNPP_HOLD_WNS_BEGIN\n-0.10\nDESIGNPP_HOLD_WNS_END\n"
    "DESIGNPP_HOLD_TNS_BEGIN\n-0.2\nDESIGNPP_HOLD_TNS_END\n"
    "DESIGNPP_SETUP_PATHS_BEGIN\n1\nDESIGNPP_SETUP_PATHS_END\n"
    "DESIGNPP_HOLD_PATHS_BEGIN\n1\nDESIGNPP_HOLD_PATHS_END\n",
    "DESIGNPP_SETUP_BEGIN\nStartpoint: r1/Q\nEndpoint: r2/D\n"
    "slack (VIOLATED) -0.25\nDESIGNPP_SETUP_END\n"
    "DESIGNPP_HOLD_BEGIN\nStartpoint: r2/Q\nEndpoint: r3/D\n"
    "slack (VIOLATED) -0.10\nDESIGNPP_HOLD_END\n",
    "slow");
Assert::IsTrue(metrics.Ok());
Assert::AreEqual(-0.25, metrics.Value().setup.wns, 0.0001);
Assert::AreEqual(-1.5, metrics.Value().setup.tns, 0.0001);
Assert::AreEqual(-0.10, metrics.Value().hold.wns, 0.0001);
Assert::AreEqual(static_cast<std::size_t>(2),
                 metrics.Value().violations.size());
Assert::IsFalse(metrics.Value().Passed());
std::error_code error;
std::filesystem::remove_all(directory, error);
}

TEST_METHOD(RejectsUnsupportedCollectionRemovalBeforeExecution) {
  adapters::OpenStaAdapter adapter;
  const core::Status status = adapter.ValidateSdcText(
      "create_clock -period 20 [get_ports pin_clk]\n"
      "set_input_delay 2 -clock pin_clk \\\n"
      "  [remove_from_collection [all_inputs] [get_ports pin_clk]]\n");
  Assert::IsFalse(status.Ok());
  Assert::IsTrue(status.message.find("SDC line 3") != std::string::npos);
  Assert::IsTrue(status.message.find("all_inputs -no_clocks") !=
                 std::string::npos);
}

TEST_METHOD(AsyncRemovalViolationsContributeToHoldMetrics) {
  adapters::OpenStaAdapter adapter;
  auto metrics = adapter.ParseReport(
      "DESIGNPP_SETUP_WNS_BEGIN\n0\nDESIGNPP_SETUP_WNS_END\n"
      "DESIGNPP_SETUP_TNS_BEGIN\n0\nDESIGNPP_SETUP_TNS_END\n"
      "DESIGNPP_HOLD_WNS_BEGIN\n-8.98822e-11\nDESIGNPP_HOLD_WNS_END\n"
      "DESIGNPP_HOLD_TNS_BEGIN\n-1.52767e-10\nDESIGNPP_HOLD_TNS_END\n"
      "DESIGNPP_SETUP_PATHS_BEGIN\n1\nDESIGNPP_SETUP_PATHS_END\n"
      "DESIGNPP_HOLD_PATHS_BEGIN\n1\nDESIGNPP_HOLD_PATHS_END\n",
      "DESIGNPP_SETUP_BEGIN\nDESIGNPP_SETUP_END\n"
      "DESIGNPP_HOLD_BEGIN\n"
      "Startpoint: core/_4103_\n"
      "Endpoint: core/_3983_ (removal check against pin_clk)\n"
      "slack (VIOLATED) -0.090000\n"
      "Startpoint: core/_4103_\n"
      "Endpoint: core/_3982_ (removal check against pin_clk)\n"
      "slack (VIOLATED) -0.060000\n"
      "DESIGNPP_HOLD_END\n",
      "typical");
  Assert::IsTrue(metrics.Ok());
  Assert::AreEqual(-0.09, metrics.Value().hold.wns, 0.000001);
  Assert::AreEqual(-0.15, metrics.Value().hold.tns, 0.000001);
  Assert::AreEqual(static_cast<std::size_t>(2),
                   metrics.Value().hold.violation_count);
  Assert::AreEqual(static_cast<std::size_t>(2),
                   metrics.Value().violation_counts.removal);
  Assert::AreEqual(static_cast<std::size_t>(0),
                   metrics.Value().violation_counts.hold);
  Assert::AreEqual(std::string("removal"),
                   metrics.Value().violations[0].check_type);

  adapters::TimingMetrics stale_metrics = metrics.Value();
  stale_metrics.hold.wns = -8.98822e-11;
  stale_metrics.hold.tns = -1.52767e-10;
  stale_metrics.hold.violation_count = 0;
  for (auto& violation : stale_metrics.violations) {
    violation.check_type = "hold";
  }
  const adapters::TimingMetrics normalized =
      adapter.NormalizeMetrics(std::move(stale_metrics));
  Assert::AreEqual(-0.09, normalized.hold.wns, 0.000001);
  Assert::AreEqual(-0.15, normalized.hold.tns, 0.000001);
  Assert::AreEqual(std::string("removal"), normalized.violations[0].check_type);
  Assert::AreEqual(static_cast<std::size_t>(2),
                   normalized.violation_counts.removal);
}
}
;

TEST_CLASS(ResourceCoordinatorTests){
  public : TEST_METHOD(CrossInstanceQuotaPreventsOversubscription){
      const std::wstring name = L"Local\\DesignPlusPlus.TestQuota." +
                                std::to_wstring(GetCurrentProcessId());
runtime::ResourceCoordinator first(name, 2);
runtime::ResourceCoordinator second(name, 2);
auto lease = first.TryAcquireCpu(2);
Assert::IsTrue(lease.Ok());
auto blocked = second.TryAcquireCpu(1);
Assert::IsFalse(blocked.Ok());
Assert::IsTrue(blocked.GetStatus().code == core::ErrorCode::kConflict);
}
}
;

}  // namespace designpp::tests

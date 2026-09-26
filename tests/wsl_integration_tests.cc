// Copyright 2026 The Design++ Authors

#include <CppUnitTest.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include "designpp/adapters/cocotb_runner_adapter.h"
#include "designpp/adapters/openlane2_adapter.h"
#include "designpp/adapters/opensta_adapter.h"
#include "designpp/adapters/orfs_adapter.h"
#include "designpp/adapters/orfs_flake_inputs.h"
#include "designpp/adapters/simulation_result_parser.h"
#include "designpp/adapters/verilator_simulation_adapter.h"
#include "designpp/adapters/yosys_adapter.h"
#include "designpp/application/synthesis_fingerprint.h"
#include "designpp/gui/schematic_scene_builder.h"
#include "designpp/runtime/execution_provider.h"
#include "designpp/runtime/path_mapper.h"
#include "designpp/runtime/wsl_executor.h"

using Microsoft::VisualStudio::CppUnitTestFramework::Assert;
using Microsoft::VisualStudio::CppUnitTestFramework::Logger;

namespace designpp::tests {
namespace {

std::string WideToUtf8(std::wstring_view text) {
  if (text.empty()) return {};
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                       static_cast<int>(text.size()), nullptr,
                                       0, nullptr, nullptr);
  std::string result(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                      result.data(), size, nullptr, nullptr);
  return result;
}

class ScopedDirectory final {
 public:
  ScopedDirectory() {
    wchar_t temporary[MAX_PATH]{};
    GetTempPathW(static_cast<DWORD>(std::size(temporary)), temporary);
    path_ = std::filesystem::path(temporary) /
            (L"DesignPlusPlus.WslIntegration." +
             std::to_wstring(GetCurrentProcessId()) + L"." +
             std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(path_);
  }

  ~ScopedDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

runtime::ProcessResult Run(
    const runtime::WslCommand& command,
    std::chrono::seconds timeout = std::chrono::seconds(30)) {
  std::mutex mutex;
  std::condition_variable completed;
  bool done = false;
  runtime::ProcessResult result;
  runtime::WslExecutionProvider provider;
  auto launch = provider.Start(
      command, [](std::string) {},
      [&](runtime::ProcessResult value) {
        {
          std::scoped_lock lock(mutex);
          result = std::move(value);
          done = true;
        }
        completed.notify_one();
      });
  Assert::IsTrue(launch.Ok());
  std::unique_lock lock(mutex);
  Assert::IsTrue(completed.wait_for(lock, timeout, [&] { return done; }));
  return result;
}

adapters::OpenLaneRequest WriteOpenLaneFixture(
    const std::filesystem::path& directory) {
  const std::filesystem::path staging = directory / L"한글 OpenLane staging";
  const std::filesystem::path backend = directory / L"backend";
  std::filesystem::create_directories(staging / L"src");
  std::filesystem::create_directories(backend);
  std::ofstream(staging / L"src" / L"source_0.sv")
      << "module tiny_counter(input logic clk, input logic rst_n, "
         "output logic [3:0] count);\n"
      << "always_ff @(posedge clk or negedge rst_n) begin\n"
      << "  if (!rst_n) count <= '0; else count <= count + 1'b1;\n"
      << "end\nendmodule\n";
  runtime::PathMapper mapper;
  auto staging_wsl = mapper.WindowsToWsl(staging);
  auto backend_wsl = mapper.WindowsToWsl(backend);
  Assert::IsTrue(staging_wsl.Ok());
  Assert::IsTrue(backend_wsl.Ok());
  adapters::OpenLaneRequest request;
  request.profile.openlane_root = "~/.designpp/toolchains/openlane2";
  request.profile.pdk_root = "~/.volare";
  request.configuration.pdk = "sky130A";
  request.configuration.standard_cell_library = "sky130_fd_sc_hd";
  request.configuration.clock_ports = {"clk"};
  request.configuration.clock_period_ns = "20.0";
  request.configuration.core_utilization_percent = 40;
  request.configuration.die_area = {"0", "0", "100", "100"};
  request.top_module = "tiny_counter";
  request.sources.push_back({"tiny_counter.sv", "source_0.sv"});
  request.backend_workspace = WideToUtf8(backend_wsl.Value());
  request.staging_workspace = WideToUtf8(staging_wsl.Value());
  request.cpu_threads = 2;
  return request;
}

adapters::OrfsRequest WriteOrfsFixture(const std::filesystem::path& directory,
                                       std::string platform,
                                       std::string tool_mode) {
  const std::filesystem::path staging =
      directory /
      (L"ORFS 한글 staging " + std::wstring(platform.begin(), platform.end()));
  const std::filesystem::path backend = directory / L"backend";
  std::filesystem::create_directories(staging / L"src");
  std::filesystem::create_directories(staging / L"constraints");
  std::filesystem::create_directories(backend);
  std::ofstream(staging / L"src" / L"source_0.sv")
      << "module tiny_counter(input logic clk, input logic rst_n, "
         "output logic [3:0] count);\n"
      << "always_ff @(posedge clk or negedge rst_n) begin\n"
      << "  if (!rst_n) count <= '0; else count <= count + 1'b1;\n"
      << "end\nendmodule\n";
  std::ofstream(staging / L"constraints" / L"constraints.sdc")
      << "set_cmd_units -time ns\n"
      << "create_clock -name clk -period 20.0 [get_ports clk]\n";
  runtime::PathMapper mapper;
  auto staging_wsl = mapper.WindowsToWsl(staging);
  auto backend_wsl = mapper.WindowsToWsl(backend);
  Assert::IsTrue(staging_wsl.Ok());
  Assert::IsTrue(backend_wsl.Ok());
  adapters::OrfsRequest request;
  request.profile.orfs_root = "~/.designpp/toolchains/orfs";
  request.configuration.backend_id = "orfs";
  request.configuration.orfs.platform = std::move(platform);
  request.configuration.orfs.flow_variant = "base";
  request.configuration.clock_ports = {"clk"};
  request.configuration.clock_period_ns = "20.0";
  request.configuration.core_utilization_percent = 40;
  request.configuration.die_area = {"0", "0", "100", "100"};
  request.configuration.core_area = {"10", "10", "90", "90"};
  request.top_module = "tiny_counter";
  request.sources.push_back({"tiny_counter.sv", "source_0.sv"});
  request.sdc_path = "constraints.sdc";
  request.effective_clock_period_ns = "20.0";
  request.backend_workspace = WideToUtf8(backend_wsl.Value());
  request.staging_workspace = WideToUtf8(staging_wsl.Value());
  request.cpu_threads = 2;
  request.target_stage = core::StageId::kFloorplan;
  request.tool_mode = std::move(tool_mode);
  return request;
}

void WriteOrfsPlanFiles(const std::filesystem::path& staging,
                        const adapters::OrfsPlan& plan) {
  std::ofstream(staging / L"config.mk", std::ios::binary)
      << plan.config_makefile;
  std::ofstream(staging / L"designpp-openroad-wrapper.sh", std::ios::binary)
      << plan.openroad_wrapper;
  std::ofstream(staging / L"designpp-openroad-init.tcl", std::ios::binary)
      << plan.openroad_init;
}

void WriteFixture(const std::filesystem::path& directory) {
  std::ofstream(directory / L"dut.sv")
      << "module dut(input a, input b, output y);\n"
      << "  assign y = a ^ b;\nendmodule\n";
  std::ofstream(directory / L"dut_tb.sv")
      << "module dut_tb;\n"
      << "  reg a = 0; reg b = 0; wire y;\n"
      << "  dut uut(.a(a), .b(b), .y(y));\n"
      << "  initial begin\n"
      << "    $dumpfile(\"waveform.vcd\"); $dumpvars(0, dut_tb);\n"
      << "    #1 a = 1; #1 b = 1; #1 $finish;\n"
      << "  end\nendmodule\n";
}

std::string TrimAscii(std::string value) {
  while (!value.empty() && (value.back() == '\r' || value.back() == '\n' ||
                            value.back() == ' ' || value.back() == '\t')) {
    value.pop_back();
  }
  const std::size_t begin = value.find_first_not_of(" \t\r\n");
  return begin == std::string::npos ? std::string() : value.substr(begin);
}

adapters::SimulationRequest MakeSimulationRequest(
    const std::filesystem::path& directory, std::string waveform_format,
    bool python_testbench) {
  adapters::SimulationRequest request;
  request.project.top_module = "dut";
  request.project.cpu_budget = 1;
  request.testbench_view_id = "testbench";
  request.testbench_top = python_testbench ? "dut" : "dut_tb";
  request.waveform_enabled = true;
  request.waveform_format = std::move(waveform_format);
  request.artifact_directory = directory / L"artifacts";
  request.build_directory = directory / L"build";
  std::filesystem::create_directories(request.artifact_directory);
  std::filesystem::create_directories(request.build_directory);

  application::ResolvedSource rtl;
  rtl.view_kind = core::ViewKind::kVerilog;
  rtl.view_id = "rtl";
  rtl.relative_path = "dut.sv";
  rtl.windows_path = directory / L"dut.sv";
  rtl.library_directory = directory;
  rtl.enabled = true;
  rtl.exists = true;
  request.sources.push_back(rtl);

  application::ResolvedSource testbench = rtl;
  testbench.view_kind = core::ViewKind::kTestbench;
  testbench.view_id = request.testbench_view_id;
  testbench.relative_path = python_testbench ? "test_dut.py" : "dut_tb.sv";
  testbench.windows_path =
      directory / (python_testbench ? L"test_dut.py" : L"dut_tb.sv");
  request.testbench_relative_path = testbench.relative_path;
  request.sources.push_back(std::move(testbench));
  return request;
}

void WriteVerilatorSimulationFixture(const std::filesystem::path& directory) {
  std::ofstream(directory / L"dut.sv")
      << "module dut(input logic a, input logic b, output logic y);\n"
      << "  assign y = a ^ b;\nendmodule\n";
  std::ofstream(directory / L"dut_tb.sv")
      << "module dut_tb; logic a = 0; logic b = 0; logic y;\n"
      << "  dut u_dut(.a(a), .b(b), .y(y));\n"
      << "  initial begin #1 a = 1; #1 b = 1; #1 $finish; end\n"
      << "endmodule\n";
}

void WriteCocotbFixture(const std::filesystem::path& directory) {
  std::ofstream(directory / L"dut.sv")
      << "module dut(input logic a, input logic b, output logic y);\n"
      << "  assign y = a ^ b;\nendmodule\n";
  std::ofstream(directory / L"test_dut.py") << R"py(import cocotb
from cocotb.triggers import Timer

@cocotb.test()
async def xor_truth_table(dut):
    for a, b, expected in ((0, 0, 0), (0, 1, 1), (1, 0, 1), (1, 1, 0)):
        dut.a.value = a
        dut.b.value = b
        await Timer(1, units="ns")
        assert int(dut.y.value) == expected
)py";
}

core::Result<adapters::CocotbPlan> BuildCocotbIntegrationPlan(
    const std::filesystem::path& directory, std::string backend,
    std::string waveform_format) {
  runtime::WslCommand makefiles_probe;
  makefiles_probe =
      adapters::CocotbRunnerAdapter().BuildMakefilesProbeCommand();
  const runtime::ProcessResult probe = Run(makefiles_probe);
  if (!probe.started || probe.exit_code != 0) {
    return core::Status{core::ErrorCode::kNotFound,
                        "cocotb Makefiles are unavailable", 0};
  }
  adapters::CocotbRequest request;
  request.simulation =
      MakeSimulationRequest(directory, std::move(waveform_format), true);
  request.simulation.build_directory.clear();
  request.module = "test_dut";
  request.makefiles_directory =
      std::filesystem::path(TrimAscii(probe.output)).wstring();
  request.backend = std::move(backend);
  return adapters::CocotbRunnerAdapter().BuildPlan(request,
                                                   runtime::PathMapper());
}

bool CocotbIsAvailable() {
  const runtime::WslCommand probe =
      adapters::CocotbRunnerAdapter().BuildProbeCommand();
  const runtime::ProcessResult result = Run(probe);
  return result.started && result.exit_code == 0;
}

std::size_t CountOccurrences(std::string_view text, std::string_view token) {
  std::size_t count = 0;
  std::size_t position = 0;
  while ((position = text.find(token, position)) != std::string_view::npos) {
    ++count;
    position += token.size();
  }
  return count;
}

void WriteOpenStaFixture(const std::filesystem::path& directory) {
  std::ofstream(directory / L"netlist.v")
      << "module top(input a, output y); BUF_X1 u1(.A(a), .Y(y)); endmodule\n";
  std::ofstream(directory / L"timing.sdc")
      << "create_clock -name vclk -period 10\n"
      << "set_input_delay -clock vclk 1 [get_ports a]\n"
      << "set_output_delay -clock vclk 1 [get_ports y]\n";
  std::ofstream(directory / L"timing.lib") << R"lib(library(test) {
  time_unit : "1ns";
  voltage_unit : "1V";
  current_unit : "1mA";
  capacitive_load_unit(1,pf);
  leakage_power_unit : "1nW";
  delay_model : table_lookup;
  input_threshold_pct_rise : 50;
  output_threshold_pct_rise : 50;
  slew_lower_threshold_pct_rise : 20;
  slew_upper_threshold_pct_rise : 80;
  input_threshold_pct_fall : 50;
  output_threshold_pct_fall : 50;
  slew_lower_threshold_pct_fall : 20;
  slew_upper_threshold_pct_fall : 80;
  lu_table_template(t1) {
    variable_1 : input_net_transition;
    variable_2 : total_output_net_capacitance;
    index_1("0.01"); index_2("0.01");
  }
  cell(BUF_X1) {
    area : 1;
    pin(A) { direction : input; capacitance : 0.01; }
    pin(Y) {
      direction : output; function : "A";
      timing() {
        related_pin : "A"; timing_sense : positive_unate;
        cell_rise(t1) { values("0.10"); }
        cell_fall(t1) { values("0.10"); }
        rise_transition(t1) { values("0.01"); }
        fall_transition(t1) { values("0.01"); }
      }
    }
  }
})lib";
}

core::Result<adapters::TimingMetrics> RunOpenStaFixture(
    const std::filesystem::path& directory) {
  adapters::TimingRequest request;
  request.top_module = "top";
  request.corner_name = "typical";
  request.netlist_path = directory / L"netlist.v";
  request.liberty_files = {directory / L"timing.lib"};
  request.sdc_path = directory / L"timing.sdc";
  request.artifact_directory = directory;
  request.cpu_threads = 1;
  adapters::OpenStaAdapter adapter;
  runtime::PathMapper mapper;
  auto plan = adapter.BuildPlan(request, mapper);
  if (!plan.Ok()) return plan.GetStatus();
  std::ofstream(plan.Value().script_path, std::ios::binary)
      << plan.Value().script_text;
  const runtime::ProcessResult result = Run(plan.Value().execute);
  if (!result.started || result.exit_code != 0) {
    return core::Status{core::ErrorCode::kIoError,
                        "Managed OpenSTA integration execution failed", 0};
  }
  std::ifstream report_input(plan.Value().report_path, std::ios::binary);
  const std::string report((std::istreambuf_iterator<char>(report_input)),
                           std::istreambuf_iterator<char>());
  return adapter.ParseReport(result.output, report, "typical");
}

}  // namespace

TEST_CLASS(WslIcarusIntegrationTests){
  public : TEST_METHOD(ProbeAndRunProducesVcd){runtime::WslCommand probe;
probe.program = L"iverilog";
probe.arguments = {L"-V"};
const runtime::ProcessResult probe_result = Run(probe);
Assert::IsTrue(probe_result.started);
Assert::AreEqual(static_cast<std::uint32_t>(0), probe_result.exit_code);
Assert::IsTrue(probe_result.output.find("Icarus Verilog") != std::string::npos);

ScopedDirectory directory;
WriteFixture(directory.path());
runtime::PathMapper mapper;
const auto mapped = mapper.WindowsToWsl(directory.path());
Assert::IsTrue(mapped.Ok());
runtime::WslCommand compile;
compile.program = L"iverilog";
compile.working_directory = mapped.Value();
compile.arguments = {L"-g2012", L"-o", L"simulation.vvp", L"dut.sv",
                     L"dut_tb.sv"};
const runtime::ProcessResult compile_result = Run(compile);
Assert::IsTrue(compile_result.started);
Assert::AreEqual(static_cast<std::uint32_t>(0), compile_result.exit_code);

runtime::WslCommand execute;
execute.program = L"vvp";
execute.working_directory = mapped.Value();
execute.arguments = {L"-N", L"simulation.vvp"};
const runtime::ProcessResult execute_result = Run(execute);
Assert::IsTrue(execute_result.started);
Assert::AreEqual(static_cast<std::uint32_t>(0), execute_result.exit_code);
Assert::IsTrue(std::filesystem::exists(directory.path() / L"waveform.vcd"));
Assert::IsTrue(std::filesystem::file_size(directory.path() / L"waveform.vcd") >
               0);
}  // namespace designpp::tests

TEST_METHOD(WslgEnvironmentAndGtkWaveAreAvailable) {
  runtime::WslCommand environment;
  environment.program = L"/usr/bin/env";
  const runtime::ProcessResult environment_result = Run(environment);
  Assert::IsTrue(environment_result.started);
  Assert::AreEqual(static_cast<std::uint32_t>(0), environment_result.exit_code);
  Assert::IsTrue(environment_result.output.find("DISPLAY=") !=
                 std::string::npos);
  Assert::IsTrue(environment_result.output.find("WAYLAND_DISPLAY=") !=
                 std::string::npos);
  Assert::IsTrue(environment_result.output.find("XDG_RUNTIME_DIR=") !=
                 std::string::npos);

  runtime::WslCommand gtkwave;
  gtkwave.program = L"gtkwave";
  gtkwave.arguments = {L"--version"};
  const runtime::ProcessResult gtkwave_result = Run(gtkwave);
  Assert::IsTrue(gtkwave_result.started);
  Assert::AreEqual(static_cast<std::uint32_t>(0), gtkwave_result.exit_code);
  Assert::IsTrue(gtkwave_result.output.find("GTKWave") != std::string::npos);
}

TEST_METHOD(VerilatorAdapterProducesExecutableAndFstWaveform) {
  ScopedDirectory directory;
  WriteVerilatorSimulationFixture(directory.path());
  adapters::SimulationRequest request =
      MakeSimulationRequest(directory.path(), "fst", false);
  adapters::VerilatorSimulationAdapter adapter;
  auto plan = adapter.BuildPlan(request, runtime::PathMapper());
  Assert::IsTrue(plan.Ok());
  std::ofstream(plan.Value().wrapper_path, std::ios::binary)
      << plan.Value().wrapper_text;

  const runtime::ProcessResult compiled = Run(plan.Value().compile);
  if (compiled.exit_code != 0) Logger::WriteMessage(compiled.output.c_str());
  Assert::IsTrue(compiled.started);
  Assert::AreEqual(static_cast<std::uint32_t>(0), compiled.exit_code);
  Assert::IsTrue(std::filesystem::exists(plan.Value().binary_path));

  const runtime::ProcessResult executed = Run(plan.Value().execute);
  if (executed.exit_code != 0) Logger::WriteMessage(executed.output.c_str());
  Assert::IsTrue(executed.started);
  Assert::AreEqual(static_cast<std::uint32_t>(0), executed.exit_code);
  Assert::IsTrue(std::filesystem::exists(plan.Value().waveform_path));
  Assert::IsTrue(std::filesystem::file_size(plan.Value().waveform_path) > 0);
}

TEST_METHOD(CocotbIcarusProducesPassingXunitAndVcd) {
  if (!CocotbIsAvailable()) {
    Logger::WriteMessage("Managed cocotb environment is unavailable\n");
    return;
  }
  ScopedDirectory directory;
  WriteCocotbFixture(directory.path());
  auto plan = BuildCocotbIntegrationPlan(directory.path(), "icarus", "vcd");
  if (!plan.Ok()) Logger::WriteMessage(plan.GetStatus().message.c_str());
  Assert::IsTrue(plan.Ok());
  const runtime::ProcessResult result = Run(plan.Value().execute);
  if (result.exit_code != 0) Logger::WriteMessage(result.output.c_str());
  Assert::IsTrue(result.started);
  Assert::AreEqual(static_cast<std::uint32_t>(0), result.exit_code);
  std::ifstream results(plan.Value().results_path, std::ios::binary);
  const std::string xml((std::istreambuf_iterator<char>(results)),
                        std::istreambuf_iterator<char>());
  auto summary = adapters::ParseCocotbResults(xml);
  Assert::IsTrue(summary.Ok());
  Assert::IsTrue(summary.Value().Passed());
  Assert::IsTrue(std::filesystem::exists(plan.Value().waveform_path));
  Assert::IsTrue(std::filesystem::file_size(plan.Value().waveform_path) > 0);
}

TEST_METHOD(CocotbVerilatorProducesPassingXunitAndFst) {
  if (!CocotbIsAvailable()) {
    Logger::WriteMessage("Managed cocotb environment is unavailable\n");
    return;
  }
  ScopedDirectory directory;
  WriteCocotbFixture(directory.path());
  auto plan = BuildCocotbIntegrationPlan(directory.path(), "verilator", "fst");
  if (!plan.Ok()) Logger::WriteMessage(plan.GetStatus().message.c_str());
  Assert::IsTrue(plan.Ok());
  const runtime::ProcessResult result = Run(plan.Value().execute);
  if (result.exit_code != 0) Logger::WriteMessage(result.output.c_str());
  Assert::IsTrue(result.started);
  Assert::AreEqual(static_cast<std::uint32_t>(0), result.exit_code);
  std::ifstream results(plan.Value().results_path, std::ios::binary);
  const std::string xml((std::istreambuf_iterator<char>(results)),
                        std::istreambuf_iterator<char>());
  auto summary = adapters::ParseCocotbResults(xml);
  Assert::IsTrue(summary.Ok());
  Assert::IsTrue(summary.Value().Passed());
  Assert::IsTrue(std::filesystem::exists(plan.Value().waveform_path));
  Assert::IsTrue(std::filesystem::file_size(plan.Value().waveform_path) > 0);
}

TEST_METHOD(YosysProducesNetlistAndStatisticsArtifacts) {
  runtime::WslCommand probe;
  probe.program = L"yosys";
  probe.arguments = {L"-V"};
  const runtime::ProcessResult probe_result = Run(probe);
  Assert::IsTrue(probe_result.started);
  Assert::AreEqual(static_cast<std::uint32_t>(0), probe_result.exit_code);

  ScopedDirectory directory;
  std::ofstream(directory.path() / L"dut.sv")
      << "module dut(input a, input b, output y);\n"
      << "  assign y = a ^ b;\nendmodule\n";
  std::ofstream(directory.path() / L"synthesis.ys")
      << "read_verilog -sv dut.sv\n"
      << "hierarchy -check -top dut\n"
      << "design -save designpp_rtl\n"
      << "synth -top dut\n"
      << "write_json netlist.json\n"
      << "write_verilog netlist.v\n"
      << "tee -o synthesis.rpt stat\n"
      << "tee -o statistics.json stat -json\n"
      << "design -load designpp_rtl\n"
      << "proc\n"
      << "opt\n"
      << "write_json schematic-structural.json\n";
  runtime::PathMapper mapper;
  const auto mapped = mapper.WindowsToWsl(directory.path());
  Assert::IsTrue(mapped.Ok());
  runtime::WslCommand synthesis;
  synthesis.program = L"yosys";
  synthesis.arguments = {L"-s", L"synthesis.ys"};
  synthesis.working_directory = mapped.Value();
  const runtime::ProcessResult result = Run(synthesis);
  Assert::IsTrue(result.started);
  Assert::AreEqual(static_cast<std::uint32_t>(0), result.exit_code);
  for (const wchar_t* name :
       {L"schematic-structural.json", L"netlist.json", L"netlist.v",
        L"statistics.json", L"synthesis.rpt"}) {
    const std::filesystem::path artifact = directory.path() / name;
    Assert::IsTrue(std::filesystem::exists(artifact));
    Assert::IsTrue(std::filesystem::file_size(artifact) > 0);
  }
}

TEST_METHOD(YosysAsciiStagingCopiesArtifactsToUnicodeDirectory) {
  ScopedDirectory root;
  const std::filesystem::path unicode_directory = root.path() / L"한글 결과";
  const std::filesystem::path staging_directory = root.path() / L"yosys-stage";
  std::filesystem::create_directories(unicode_directory);
  std::filesystem::create_directories(staging_directory);
  const std::filesystem::path source = unicode_directory / L"dut.sv";
  std::ofstream(source) << "module dut(input a, output y);\n"
                           "  assign y = ~a;\nendmodule\n";
  runtime::PathMapper mapper;
  const auto mapped_source = mapper.WindowsToWsl(source);
  const auto mapped_staging = mapper.WindowsToWsl(staging_directory);
  Assert::IsTrue(mapped_source.Ok());
  Assert::IsTrue(mapped_staging.Ok());
  std::ofstream(staging_directory / L"synthesis.ys")
      << "read_verilog -sv \"" << WideToUtf8(mapped_source.Value()) << "\"\n"
      << "hierarchy -check -top dut\n"
      << "design -save designpp_rtl\n"
      << "synth -top dut\n"
      << "write_json netlist.json\n"
      << "write_verilog netlist.v\n"
      << "tee -o synthesis.rpt stat\n"
      << "tee -o statistics.json stat -json\n"
      << "design -load designpp_rtl\n"
      << "proc\n"
      << "opt\n"
      << "write_json schematic-structural.json\n";
  runtime::WslCommand synthesis;
  synthesis.program = L"yosys";
  synthesis.arguments = {L"-s", L"synthesis.ys"};
  synthesis.working_directory = mapped_staging.Value();
  const runtime::ProcessResult result = Run(synthesis);
  Assert::IsTrue(result.started);
  Assert::AreEqual(static_cast<std::uint32_t>(0), result.exit_code);
  for (const wchar_t* name :
       {L"schematic-structural.json", L"netlist.json", L"netlist.v",
        L"statistics.json", L"synthesis.rpt"}) {
    const std::filesystem::path staged = staging_directory / name;
    const std::filesystem::path preserved = unicode_directory / name;
    std::filesystem::copy_file(staged, preserved);
    Assert::IsTrue(std::filesystem::exists(preserved));
    Assert::IsTrue(std::filesystem::file_size(preserved) > 0);
  }
}

TEST_METHOD(YosysTimerPreservesStructuralAndGateSequentialViews) {
  ScopedDirectory directory;
  const std::filesystem::path fixture =
      std::filesystem::path(__FILE__).parent_path() / L"fixtures" /
      L"timer_1s.sv";
  std::filesystem::copy_file(fixture, directory.path() / L"timer_1s.sv");
  std::ofstream(directory.path() / L"synthesis.ys")
      << "read_verilog -sv timer_1s.sv\n"
      << "hierarchy -check -top timer_1s\n"
      << "design -save designpp_rtl\n"
      << "synth -top timer_1s -noabc\n"
      << "abc -g AND,OR,XOR,XNOR,NAND,NOR\n"
      << "write_json netlist.json\n"
      << "design -load designpp_rtl\n"
      << "proc\n"
      << "opt\n"
      << "write_json schematic-structural.json\n";
  runtime::PathMapper mapper;
  const auto mapped = mapper.WindowsToWsl(directory.path());
  Assert::IsTrue(mapped.Ok());
  runtime::WslCommand synthesis;
  synthesis.program = L"yosys";
  synthesis.arguments = {L"-s", L"synthesis.ys"};
  synthesis.working_directory = mapped.Value();
  const runtime::ProcessResult result = Run(synthesis);
  Assert::IsTrue(result.started);
  Assert::AreEqual(static_cast<std::uint32_t>(0), result.exit_code);

  std::ifstream structural_file(directory.path() /
                                L"schematic-structural.json");
  const std::string structural(
      (std::istreambuf_iterator<char>(structural_file)),
      std::istreambuf_iterator<char>());
  std::ifstream gate_file(directory.path() / L"netlist.json");
  const std::string gate((std::istreambuf_iterator<char>(gate_file)),
                         std::istreambuf_iterator<char>());
  Assert::IsTrue(structural.find("\"type\": \"$adff\"") != std::string::npos);
  Assert::AreEqual<std::size_t>(28U,
                                CountOccurrences(gate, "\"type\": \"$_DFF"));
  Assert::AreEqual<std::size_t>(136U, CountOccurrences(gate, "\"type\":"));

  adapters::YosysAdapter adapter;
  auto gate_model = adapter.ParseNetlist(gate, "timer_1s");
  auto readable_model = adapter.ParseNetlist(structural, "timer_1s");
  Assert::IsTrue(gate_model.Ok());
  Assert::IsTrue(readable_model.Ok());
  Assert::AreEqual<std::size_t>(136U, gate_model.Value().nodes.size());
  auto readable_scene = gui::SchematicSceneBuilder().Build(
      {std::move(readable_model).Value(), gui::SchematicViewMode::kReadable});
  Assert::IsTrue(readable_scene.Ok());
  Assert::AreEqual<std::size_t>(
      1U, readable_scene.Value().summary.register_bank_count);
  Assert::IsTrue(std::any_of(
      readable_scene.Value().nodes.begin(), readable_scene.Value().nodes.end(),
      [](const auto& node) {
        return node.kind == core::SchematicNodeKind::kRegisterBank &&
               node.label == "cnt[25:0]";
      }));
}

TEST_METHOD(YosysHierarchyRemainsNavigableWithoutFlattening) {
  ScopedDirectory directory;
  const std::filesystem::path fixture =
      std::filesystem::path(__FILE__).parent_path() / L"fixtures" /
      L"hierarchy.sv";
  std::filesystem::copy_file(fixture, directory.path() / L"hierarchy.sv");
  std::ofstream(directory.path() / L"synthesis.ys")
      << "read_verilog -sv hierarchy.sv\n"
      << "hierarchy -check -top hierarchy\n"
      << "proc\nopt\n"
      << "write_json schematic-structural.json\n"
      << "synth -top hierarchy -noabc\n"
      << "abc -g AND,OR,XOR,XNOR,NAND,NOR\n"
      << "write_json netlist.json\n"
      << "write_verilog netlist.v\n"
      << "tee -o synthesis.rpt stat\n"
      << "tee -o statistics.json stat -json\n";
  runtime::PathMapper mapper;
  const auto mapped = mapper.WindowsToWsl(directory.path());
  Assert::IsTrue(mapped.Ok());
  runtime::WslCommand synthesis;
  synthesis.program = L"yosys";
  synthesis.arguments = {L"-s", L"synthesis.ys"};
  synthesis.working_directory = mapped.Value();
  const runtime::ProcessResult result = Run(synthesis);
  Assert::IsTrue(result.started);
  Assert::AreEqual(static_cast<std::uint32_t>(0), result.exit_code);

  std::ifstream input(directory.path() / L"schematic-structural.json");
  const std::string json((std::istreambuf_iterator<char>(input)),
                         std::istreambuf_iterator<char>());
  adapters::YosysAdapter adapter;
  auto top = adapter.ParseNetlist(json, "hierarchy");
  auto child = adapter.ParseNetlist(json, "hierarchy_child");
  Assert::IsTrue(top.Ok());
  Assert::IsTrue(child.Ok());
  Assert::IsTrue(std::any_of(
      top.Value().nodes.begin(), top.Value().nodes.end(), [](const auto& node) {
        return node.kind == core::SchematicNodeKind::kModule &&
               node.type == "hierarchy_child";
      }));
}

TEST_METHOD(YosysFixtureMatrixProducesParseableReadableAndGateModels) {
  struct Fixture {
    const wchar_t* file;
    const char* top;
  };
  constexpr std::array<Fixture, 7> kFixtures{{
      {L"combinational.sv", "combinational"},
      {L"mux4.sv", "mux4"},
      {L"fsm.sv", "fsm"},
      {L"register_controls.sv", "register_controls"},
      {L"multi_clock.sv", "multi_clock"},
      {L"signed_vector.sv", "signed_vector"},
      {L"blackbox.sv", "blackbox_top"},
  }};
  for (const Fixture& fixture : kFixtures) {
    ScopedDirectory directory;
    const std::filesystem::path fixture_path =
        std::filesystem::path(__FILE__).parent_path() / L"fixtures" /
        fixture.file;
    std::filesystem::copy_file(fixture_path, directory.path() / fixture.file);
    std::ofstream script(directory.path() / L"synthesis.ys");
    script << "read_verilog -sv " << WideToUtf8(fixture.file) << "\n"
           << "hierarchy -check -top " << fixture.top << "\n"
           << "design -save designpp_rtl\n"
           << "synth -top " << fixture.top << " -noabc\n"
           << "abc -g AND,OR,XOR,XNOR,NAND,NOR\n"
           << "write_json netlist.json\n"
           << "write_verilog netlist.v\n"
           << "tee -o synthesis.rpt stat\n"
           << "tee -o statistics.json stat -json\n"
           << "design -load designpp_rtl\n"
           << "proc\nopt\nwrite_json schematic-structural.json\n";
    script.close();
    runtime::PathMapper mapper;
    const auto mapped = mapper.WindowsToWsl(directory.path());
    Assert::IsTrue(mapped.Ok());
    runtime::WslCommand synthesis;
    synthesis.program = L"yosys";
    synthesis.arguments = {L"-s", L"synthesis.ys"};
    synthesis.working_directory = mapped.Value();
    const runtime::ProcessResult result = Run(synthesis);
    Assert::AreEqual(static_cast<std::uint32_t>(0), result.exit_code);

    std::ifstream gate_input(directory.path() / L"netlist.json");
    const std::string gate((std::istreambuf_iterator<char>(gate_input)),
                           std::istreambuf_iterator<char>());
    std::ifstream readable_input(directory.path() /
                                 L"schematic-structural.json");
    const std::string readable((std::istreambuf_iterator<char>(readable_input)),
                               std::istreambuf_iterator<char>());
    adapters::YosysAdapter adapter;
    auto gate_model = adapter.ParseNetlist(gate, fixture.top);
    auto readable_model = adapter.ParseNetlist(readable, fixture.top);
    Assert::IsTrue(gate_model.Ok());
    Assert::IsTrue(readable_model.Ok());
    auto scene = gui::SchematicSceneBuilder().Build(
        {std::move(readable_model).Value(), gui::SchematicViewMode::kReadable});
    Assert::IsTrue(scene.Ok());
  }
}

TEST_METHOD(InteractiveDebugAcceptsTimeAndFinish) {
  ScopedDirectory directory;
  WriteFixture(directory.path());
  runtime::PathMapper mapper;
  const auto mapped = mapper.WindowsToWsl(directory.path());
  Assert::IsTrue(mapped.Ok());
  runtime::WslCommand compile;
  compile.program = L"iverilog";
  compile.working_directory = mapped.Value();
  compile.arguments = {L"-g2012", L"-o", L"debug.vvp", L"dut.sv", L"dut_tb.sv"};
  Assert::AreEqual(static_cast<std::uint32_t>(0), Run(compile).exit_code);

  runtime::WslCommand execute;
  execute.program = L"vvp";
  execute.working_directory = mapped.Value();
  execute.arguments = {L"-i", L"-s", L"debug.vvp"};
  execute.interactive_input = true;
  std::mutex mutex;
  std::condition_variable signal;
  std::string output;
  std::size_t prompt_count = 0;
  bool completed = false;
  runtime::ProcessResult result;
  auto launch = runtime::WslExecutor::RunAsync(
      execute,
      [&](std::string bytes) {
        {
          std::scoped_lock lock(mutex);
          output += bytes;
          std::size_t position = 0;
          prompt_count = 0;
          while ((position = output.find("\n> ", position)) !=
                 std::string::npos) {
            ++prompt_count;
            position += 3;
          }
          if (output.starts_with("> ")) ++prompt_count;
        }
        signal.notify_all();
      },
      [&](runtime::ProcessResult value) {
        {
          std::scoped_lock lock(mutex);
          result = std::move(value);
          completed = true;
        }
        signal.notify_all();
      });
  Assert::IsTrue(launch.IsValid());
  {
    std::unique_lock lock(mutex);
    Assert::IsTrue(signal.wait_for(lock, std::chrono::seconds(10),
                                   [&] { return prompt_count >= 1; }));
  }
  Assert::IsTrue(launch.session.WriteInput("time\n") ==
                 runtime::InputWriteResult::kAccepted);
  {
    std::unique_lock lock(mutex);
    Assert::IsTrue(signal.wait_for(lock, std::chrono::seconds(10), [&] {
      return output.find("ticks") != std::string::npos && prompt_count >= 2;
    }));
  }
  Assert::IsTrue(launch.session.WriteInput("finish\n") ==
                 runtime::InputWriteResult::kAccepted);
  {
    std::unique_lock lock(mutex);
    Assert::IsTrue(signal.wait_for(lock, std::chrono::seconds(10),
                                   [&] { return completed; }));
  }
  Assert::IsTrue(result.started);
  Assert::AreEqual(static_cast<std::uint32_t>(0), result.exit_code);
}

TEST_METHOD(InstalledManagedOrfsEnvironmentPassesCompatibilityProbe) {
  core::ToolchainProfile profile;
  profile.orfs_root = "~/.designpp/toolchains/environments/orfs-26Q2";
  const auto result = Run(adapters::OrfsAdapter().BuildProbeCommand(profile),
                          std::chrono::seconds(120));
  Logger::WriteMessage(result.output.c_str());
  Assert::AreEqual<std::uint32_t>(0, result.exit_code);
  Assert::IsTrue(result.output.find("DESIGNPP_COMPAT_FINGERPRINT=") !=
                 std::string::npos);
}

TEST_METHOD(CancelTerminatesLongRunningWslCommand) {
  runtime::WslCommand command;
  command.program = L"/usr/bin/sleep";
  command.arguments = {L"30"};
  std::mutex mutex;
  std::condition_variable completed;
  bool done = false;
  runtime::ProcessResult result;
  auto launch = runtime::WslExecutor::RunAsync(
      command, [](std::string) {},
      [&](runtime::ProcessResult value) {
        {
          std::scoped_lock lock(mutex);
          result = std::move(value);
          done = true;
        }
        completed.notify_one();
      });
  Assert::IsTrue(launch.IsValid());
  launch.session.Cancel();
  std::unique_lock lock(mutex);
  Assert::IsTrue(
      completed.wait_for(lock, std::chrono::seconds(10), [&] { return done; }));
  Assert::IsTrue(result.cancelled);
}

TEST_METHOD(OpenStaManagedEnvironmentProducesSetupAndHoldMetrics) {
  ScopedDirectory directory;
  WriteOpenStaFixture(directory.path());
  auto metrics = RunOpenStaFixture(directory.path());
  Assert::IsTrue(metrics.Ok());
  Assert::IsTrue(metrics.Value().setup.has_paths);
  Assert::IsTrue(metrics.Value().hold.has_paths);
}

TEST_METHOD(OpenStaReportsIntentionalSetupViolation) {
  ScopedDirectory directory;
  WriteOpenStaFixture(directory.path());
  std::ofstream(directory.path() / L"timing.sdc", std::ios::trunc)
      << "create_clock -name vclk -period 10\n"
      << "set_input_delay -clock vclk 1 [get_ports a]\n"
      << "set_output_delay -clock vclk 12 [get_ports y]\n";
  auto metrics = RunOpenStaFixture(directory.path());
  Assert::IsTrue(metrics.Ok());
  Assert::IsTrue(metrics.Value().setup.wns < 0.0);
  Assert::IsTrue(metrics.Value().setup.violation_count > 0);
}

TEST_METHOD(OpenStaReportsIntentionalHoldViolation) {
  ScopedDirectory directory;
  WriteOpenStaFixture(directory.path());
  std::ofstream(directory.path() / L"timing.sdc", std::ios::trunc)
      << "create_clock -name vclk -period 10\n"
      << "set_input_delay -max -clock vclk 1 [get_ports a]\n"
      << "set_input_delay -min -clock vclk -5 [get_ports a]\n"
      << "set_output_delay -max -clock vclk 1 [get_ports y]\n"
      << "set_output_delay -min -clock vclk 1 [get_ports y]\n";
  auto metrics = RunOpenStaFixture(directory.path());
  Assert::IsTrue(metrics.Ok());
  Assert::IsTrue(metrics.Value().hold.wns < 0.0);
  Assert::IsTrue(metrics.Value().hold.violation_count > 0);
}

TEST_METHOD(OrfsCommandUnitsKeepTwentyNanosecondClockAcrossLibertyUnits) {
  adapters::OrfsAdapter adapter;
  core::ToolchainProfile profile;
  profile.orfs_root = "~/.designpp/toolchains/orfs";
  const runtime::ProcessResult probe =
      Run(adapter.BuildProbeCommand(profile), std::chrono::minutes(2));
  if (probe.exit_code != 0) Logger::WriteMessage(probe.output.c_str());
  Assert::IsTrue(probe.started);
  Assert::AreEqual(static_cast<std::uint32_t>(0), probe.exit_code);

  ScopedDirectory directory;
  const std::filesystem::path verilog = directory.path() / L"clock_top.v";
  std::ofstream(verilog, std::ios::binary)
      << "module clock_top(input clk, input d, output q);\n"
         "  assign q = d;\n"
         "endmodule\n";
  runtime::PathMapper mapper;
  const auto verilog_wsl = mapper.WindowsToWsl(verilog);
  Assert::IsTrue(verilog_wsl.Ok());
  const std::array<std::string_view, 2> liberty_files = {
      ".designpp/toolchains/orfs/flow/platforms/asap7/lib/NLDM/"
      "asap7sc7p5t_SEQ_RVT_FF_nldm_220123.lib",
      ".designpp/toolchains/orfs/flow/platforms/nangate45/lib/"
      "NangateOpenCellLibrary_typical.lib"};
  const std::array<std::string_view, 2> technology_lefs = {
      ".designpp/toolchains/orfs/flow/platforms/asap7/lef/"
      "asap7_tech_1x_201209.lef",
      ".designpp/toolchains/orfs/flow/platforms/nangate45/lef/"
      "NangateOpenCellLibrary.tech.lef"};
  const std::array<std::string_view, 2> cell_lefs = {
      ".designpp/toolchains/orfs/flow/platforms/asap7/lef/"
      "asap7sc7p5t_28_R_1x_220121a.lef",
      ".designpp/toolchains/orfs/flow/platforms/nangate45/lef/"
      "NangateOpenCellLibrary.macro.lef"};
  for (std::size_t index = 0; index < liberty_files.size(); ++index) {
    const std::filesystem::path unit_home =
        directory.path() / (L"openroad_home_" + std::to_wstring(index));
    std::filesystem::create_directories(unit_home);
    const std::filesystem::path init = unit_home / L"designpp-init.tcl";
    std::ofstream(init, std::ios::binary)
        << "rename read_sdc designpp_original_read_sdc\n"
        << "proc read_sdc {args} {\n"
        << "  set_cmd_units -time ns\n"
        << "  uplevel 1 [list designpp_original_read_sdc {*}$args]\n"
        << "}\n";
    const std::filesystem::path writer =
        directory.path() /
        (L"clock_writer_" + std::to_wstring(index) + L".tcl");
    const std::filesystem::path reader =
        directory.path() /
        (L"clock_reader_" + std::to_wstring(index) + L".tcl");
    const std::filesystem::path serialized =
        directory.path() / (L"serialized_" + std::to_wstring(index) + L".sdc");
    const std::filesystem::path input_sdc =
        directory.path() / (L"input_" + std::to_wstring(index) + L".sdc");
    const std::filesystem::path writer_entry =
        directory.path() /
        (L"clock_writer_entry_" + std::to_wstring(index) + L".tcl");
    const std::filesystem::path reader_entry =
        directory.path() /
        (L"clock_reader_entry_" + std::to_wstring(index) + L".tcl");
    const auto writer_wsl = mapper.WindowsToWsl(writer);
    const auto reader_wsl = mapper.WindowsToWsl(reader);
    const auto serialized_wsl = mapper.WindowsToWsl(serialized);
    const auto input_sdc_wsl = mapper.WindowsToWsl(input_sdc);
    const auto init_wsl = mapper.WindowsToWsl(init);
    const auto writer_entry_wsl = mapper.WindowsToWsl(writer_entry);
    const auto reader_entry_wsl = mapper.WindowsToWsl(reader_entry);
    Assert::IsTrue(writer_wsl.Ok());
    Assert::IsTrue(reader_wsl.Ok());
    Assert::IsTrue(serialized_wsl.Ok());
    Assert::IsTrue(input_sdc_wsl.Ok());
    Assert::IsTrue(init_wsl.Ok());
    Assert::IsTrue(writer_entry_wsl.Ok());
    Assert::IsTrue(reader_entry_wsl.Ok());
    const auto write_design_setup = [&](std::ostream& output) -> std::ostream& {
      return output
             << "read_lef [file normalize \"$::env(DESIGNPP_ORIGINAL_HOME)/"
             << technology_lefs[index] << "\"]\n"
             << "read_lef [file normalize \"$::env(DESIGNPP_ORIGINAL_HOME)/"
             << cell_lefs[index] << "\"]\n"
             << "set liberty [file normalize \"$::env(DESIGNPP_ORIGINAL_HOME)/"
             << liberty_files[index] << "\"]\n"
             << "read_liberty $liberty\n"
             << "read_verilog {" << WideToUtf8(verilog_wsl.Value()) << "}\n"
             << "link_design clock_top\n";
    };
    {
      std::ofstream(input_sdc, std::ios::binary)
          << "create_clock -name clk -period 20.0 [get_ports clk]\n"
          << "set_clock_uncertainty -setup 0.5 [get_clocks clk]\n";
      std::ofstream output(writer, std::ios::binary);
      write_design_setup(output)
          << "read_sdc {" << WideToUtf8(input_sdc_wsl.Value()) << "}\n"
          << "write_sdc -no_timestamp {" << WideToUtf8(serialized_wsl.Value())
          << "}\n"
          << "exit\n";
    }
    {
      std::ofstream output(reader, std::ios::binary);
      write_design_setup(output)
          << "read_sdc {" << WideToUtf8(serialized_wsl.Value()) << "}\n"
          << "set_cmd_units -time ns\n"
          << "puts \"DESIGNPP_CLOCK_PERIOD=[get_property [get_clocks clk] "
             "period]\"\n"
          << "exit\n";
    }
    std::ofstream(writer_entry, std::ios::binary)
        << "source {" << WideToUtf8(init_wsl.Value()) << "}\nsource {"
        << WideToUtf8(writer_wsl.Value()) << "}\n";
    std::ofstream(reader_entry, std::ios::binary)
        << "source {" << WideToUtf8(init_wsl.Value()) << "}\nsource {"
        << WideToUtf8(reader_wsl.Value()) << "}\n";
    runtime::WslCommand command;
    command.program = L"/bin/bash";
    command.arguments = {
        L"-lc",
        L"root=\"$1\"; script=\"$2\"; "
        L"original_home=\"$HOME\"; case \"$root\" in '~/'*) "
        L"root=\"$original_home/${root#\\~/}\";; esac; " +
            std::wstring(adapters::kOrfsFlakeInputSelectionScript) +
            L"exec nix "
            L"--extra-experimental-features 'nix-command flakes' develop "
            L"\"$root\" --offline --no-write-lock-file --override-input yosys "
            L"\"$yosys_input\" --override-input "
            L"openroad \"$openroad_input\" "
            L"--override-input eqy-src \"$eqy_input\" "
            L"--max-jobs 0 --builders '' --option fallback false --command "
            L"/usr/bin/env DESIGNPP_ORIGINAL_HOME=\"$original_home\" "
            L"openroad -exit "
            L"\"$script\"",
        L"designpp-orfs-clock-units", L"~/.designpp/toolchains/orfs",
        writer_entry_wsl.Value()};
    const runtime::ProcessResult write_result =
        Run(command, std::chrono::minutes(2));
    if (write_result.exit_code != 0) {
      Logger::WriteMessage(write_result.output.c_str());
    }
    Assert::IsTrue(write_result.started);
    Assert::AreEqual(static_cast<std::uint32_t>(0), write_result.exit_code);
    command.arguments[4] = reader_entry_wsl.Value();
    const runtime::ProcessResult result = Run(command, std::chrono::minutes(2));
    if (result.exit_code != 0) Logger::WriteMessage(result.output.c_str());
    Assert::IsTrue(result.started);
    Assert::AreEqual(static_cast<std::uint32_t>(0), result.exit_code);
    Assert::IsTrue(result.output.find("DESIGNPP_CLOCK_PERIOD=20") !=
                   std::string::npos);
  }
}

TEST_METHOD(OrfsAsap7SynthesisHandlesCompressedMultiLiberty) {
  adapters::OrfsAdapter adapter;
  core::ToolchainProfile profile;
  profile.orfs_root = "~/.designpp/toolchains/environments/orfs-26Q2";
  const runtime::ProcessResult probe =
      Run(adapter.BuildProbeCommand(profile), std::chrono::minutes(2));
  if (probe.exit_code != 0) Logger::WriteMessage(probe.output.c_str());
  Assert::IsTrue(probe.started);
  Assert::AreEqual(static_cast<std::uint32_t>(0), probe.exit_code);
  constexpr std::string_view kMarker = "DESIGNPP_ORFS_TOOL_MODE=";
  const std::size_t marker = probe.output.find(kMarker);
  Assert::IsTrue(marker != std::string::npos);
  const std::size_t begin = marker + kMarker.size();
  const std::size_t end = probe.output.find_first_of("\r\n", begin);
  ScopedDirectory directory;
  adapters::OrfsRequest request = WriteOrfsFixture(
      directory.path(), "asap7", probe.output.substr(begin, end - begin));
  request.profile = profile;
  // Mirror the service boundary: user paths may contain Unicode, but EDA
  // tools receive an ASCII-safe staging copy rather than the original path.
  const std::filesystem::path staging = directory.path() / L"staging";
  std::filesystem::copy(directory.path() / L"ORFS 한글 staging asap7", staging,
                        std::filesystem::copy_options::recursive);
  runtime::PathMapper mapper;
  const auto staging_wsl = mapper.WindowsToWsl(staging);
  Assert::IsTrue(staging_wsl.Ok());
  request.staging_workspace = WideToUtf8(staging_wsl.Value());
  request.backend_workspace =
      ".designpp/diagnostics/asap7-" + directory.path().filename().string();
  request.target_stage = core::StageId::kSynthesis;
  const auto plan = adapter.BuildPlan(request);
  Assert::IsTrue(plan.Ok());
  WriteOrfsPlanFiles(staging, plan.Value());
  const runtime::ProcessResult flow =
      Run(plan.Value().execute, std::chrono::minutes(3));
  if (flow.exit_code != 0) Logger::WriteMessage(flow.output.c_str());
  Assert::IsTrue(flow.started);
  Assert::AreEqual(static_cast<std::uint32_t>(0), flow.exit_code);
  runtime::WslCommand artifacts;
  artifacts.program = L"/bin/bash";
  artifacts.arguments = {
      L"-lc",
      L"test -s \"$HOME/$1/results/asap7/tiny_counter/base/1_2_yosys.v\" && "
      L"test -s \"$HOME/$1/results/asap7/tiny_counter/base/1_synth.odb\" && "
      L"grep -R -q 'DESIGNPP_OPENROAD_SDC_TIME_UNIT=ns' "
      L"\"$HOME/$1/logs\"",
      L"designpp-asap7-artifacts",
      std::wstring(request.backend_workspace.begin(),
                   request.backend_workspace.end())};
  Assert::AreEqual(static_cast<std::uint32_t>(0), Run(artifacts).exit_code);
  Assert::IsTrue(flow.output.find("Re-integrating ABC results") !=
                 std::string::npos);
  Assert::IsTrue(flow.output.find("asap7sc7p5t_AO_RVT_FF_nldm_211120.lib.gz") !=
                 std::string::npos);
}

TEST_METHOD(OrfsReadyPlatformsReachFloorplanAndLoadCheckpointHeadless) {
  adapters::OrfsAdapter adapter;
  core::ToolchainProfile profile;
  profile.orfs_root = "~/.designpp/toolchains/orfs";
  const runtime::ProcessResult probe =
      Run(adapter.BuildProbeCommand(profile), std::chrono::minutes(2));
  if (!probe.started || probe.exit_code != 0 ||
      probe.output.find("DESIGNPP_ORFS_READY") == std::string::npos) {
    Logger::WriteMessage(
        "ORFS checkout or its OpenROAD executable is unavailable; skipping "
        "the ORFS physical integration fixture.\n");
    return;
  }
  const runtime::ProcessResult discovery =
      Run(adapter.BuildPlatformDiscoveryCommand(profile));
  Assert::IsTrue(discovery.started);
  Assert::AreEqual(static_cast<std::uint32_t>(0), discovery.exit_code);
  const auto candidates = adapter.ParsePlatformDiscovery(discovery.output);
  Assert::IsTrue(candidates.Ok());
  ScopedDirectory root;
  constexpr std::string_view kToolModeMarker = "DESIGNPP_ORFS_TOOL_MODE=";
  const std::size_t tool_mode_begin = probe.output.find(kToolModeMarker);
  Assert::IsTrue(tool_mode_begin != std::string::npos);
  const std::size_t tool_mode_value = tool_mode_begin + kToolModeMarker.size();
  const std::size_t tool_mode_end =
      probe.output.find_first_of("\r\n", tool_mode_value);
  const std::string tool_mode =
      probe.output.substr(tool_mode_value, tool_mode_end - tool_mode_value);
  std::size_t exercised = 0;
  for (const char* platform : {"sky130hd", "nangate45"}) {
    const bool runnable = std::any_of(
        candidates.Value().begin(), candidates.Value().end(),
        [platform](const adapters::OrfsPlatformCandidate& candidate) {
          return candidate.name == platform && candidate.runnable;
        });
    if (!runnable) continue;
    const std::filesystem::path directory = root.path() / platform;
    std::filesystem::create_directories(directory);
    adapters::OrfsRequest request =
        WriteOrfsFixture(directory, platform, tool_mode);
    const auto staging = directory / L"staging";
    std::filesystem::copy(
        directory / (L"ORFS 한글 staging " +
                     std::wstring(platform, platform + std::strlen(platform))),
        staging, std::filesystem::copy_options::recursive);
    runtime::PathMapper mapper;
    const auto staged_path = mapper.WindowsToWsl(staging);
    Assert::IsTrue(staged_path.Ok());
    request.staging_workspace = WideToUtf8(staged_path.Value());
    request.backend_workspace = ".designpp/diagnostics/floorplan-" +
                                root.path().filename().string() + "-" +
                                platform;
    auto plan = adapter.BuildPlan(request);
    if (!plan.Ok()) Logger::WriteMessage(plan.GetStatus().message.c_str());
    Assert::IsTrue(plan.Ok());
    WriteOrfsPlanFiles(staging, plan.Value());
    const runtime::ProcessResult validation =
        Run(plan.Value().validate, std::chrono::minutes(2));
    if (validation.exit_code != 0) {
      Logger::WriteMessage(validation.output.c_str());
    }
    Assert::IsTrue(validation.started);
    Assert::AreEqual(static_cast<std::uint32_t>(0), validation.exit_code);
    const runtime::ProcessResult flow =
        Run(plan.Value().execute, std::chrono::minutes(20));
    if (flow.exit_code != 0) Logger::WriteMessage(flow.output.c_str());
    Assert::IsTrue(flow.started);
    Assert::AreEqual(static_cast<std::uint32_t>(0), flow.exit_code);
    const auto destination = mapper.WindowsToWsl(directory / L"backend");
    Assert::IsTrue(destination.Ok());
    runtime::WslCommand copy;
    copy.program = L"/bin/bash";
    copy.arguments = {L"-lc",
                      L"grep -R -q 'DESIGNPP_OPENROAD_SDC_TIME_UNIT=ns' "
                      L"\"$HOME/$1/logs\" && cp -R \"$HOME/$1\"/. \"$2\"/",
                      L"designpp-fixture-export",
                      std::wstring(request.backend_workspace.begin(),
                                   request.backend_workspace.end()),
                      destination.Value()};
    Assert::AreEqual(std::uint32_t(0), Run(copy).exit_code);
    const adapters::ManagedFlowArtifactSet artifacts =
        adapter.DiscoverAvailableArtifacts(directory / L"backend");
    Assert::IsTrue(
        adapter.ValidateStageArtifacts(artifacts, core::StageId::kFloorplan)
            .Ok());
    Assert::IsFalse(artifacts.metrics_json.empty());
    std::ifstream metrics_input(artifacts.metrics_json, std::ios::binary);
    const std::string metrics((std::istreambuf_iterator<char>(metrics_input)),
                              std::istreambuf_iterator<char>());
    Assert::IsTrue(adapter.ParseMetrics(metrics).Ok());

    const auto checkpoint = mapper.WindowsToWsl(artifacts.odb);
    const std::filesystem::path script_path = directory / L"load_odb.tcl";
    const auto script = mapper.WindowsToWsl(script_path);
    Assert::IsTrue(checkpoint.Ok());
    Assert::IsTrue(script.Ok());
    std::ofstream(script_path, std::ios::binary)
        << "read_db {" << WideToUtf8(checkpoint.Value()) << "}\n"
        << "puts DESIGNPP_ORFS_ODB_LOADED\nexit\n";
    runtime::WslCommand load;
    load.program = L"/bin/bash";
    load.arguments = {
        L"-lc",
        L"root=\"$1\"; case \"$root\" in '~/'*) "
        L"root=\"$HOME/${root#\\~/}\";; esac; " +
            std::wstring(adapters::kOrfsFlakeInputSelectionScript) +
            L"if [ -f \"$root/flake.nix\" ]; then "
            L"exec nix --extra-experimental-features 'nix-command flakes' "
            L"develop \"$root\" --no-write-lock-file --override-input yosys "
            L"\"$yosys_input\" "
            L"--override-input openroad "
            L"\"$openroad_input\" "
            L"--command openroad -exit \"$2\"; fi; "
            L"exe=\"${OPENROAD_EXE:-}\"; "
            L"if [ -z \"$exe\" ]; then "
            L"exe=\"$(command -v openroad 2>/dev/null || true)\"; fi; "
            L"if [ -z \"$exe\" ]; then "
            L"exe=\"$root/tools/install/OpenROAD/bin/openroad\"; fi; "
            L"exec \"$exe\" -exit \"$2\"",
        L"designpp-orfs-headless", L"~/.designpp/toolchains/orfs",
        script.Value()};
    const runtime::ProcessResult loaded = Run(load, std::chrono::minutes(2));
    if (loaded.exit_code != 0) Logger::WriteMessage(loaded.output.c_str());
    Assert::IsTrue(loaded.started);
    Assert::AreEqual(static_cast<std::uint32_t>(0), loaded.exit_code);
    Assert::IsTrue(loaded.output.find("DESIGNPP_ORFS_ODB_LOADED") !=
                   std::string::npos);
    ++exercised;
  }
  Assert::IsTrue(exercised > 0);
}

TEST_METHOD(OpenLaneClassicReachesFloorplanWithManagedConfig) {
  ScopedDirectory directory;
  adapters::OpenLane2Adapter adapter;
  adapters::OpenLaneRequest request = WriteOpenLaneFixture(directory.path());
  request.configuration.io_placement.north.entries = {"clk"};
  request.configuration.io_placement.south.entries = {"rst_n"};
  request.configuration.io_placement.east.entries = {"count.*"};
  auto pin_order = adapter.BuildPinOrderConfiguration(request.configuration);
  Assert::IsTrue(pin_order.Ok());
  const std::filesystem::path constraints =
      directory.path() / L"한글 OpenLane staging" / L"constraints";
  std::filesystem::create_directories(constraints);
  std::ofstream(constraints / L"pin_order.cfg", std::ios::binary)
      << pin_order.Value();
  request.pin_order_cfg = "pin_order.cfg";
  auto plan = adapter.BuildPlan(request);
  Assert::IsTrue(plan.Ok());
  std::ofstream(directory.path() / L"한글 OpenLane staging" / L"config.json",
                std::ios::binary)
      << plan.Value().config_json;
  runtime::WslCommand floorplan = plan.Value().validate;
  std::wstring& script = floorplan.arguments[1];
  const std::wstring lint = L"Verilator.Lint";
  const std::size_t position = script.find(lint);
  Assert::IsTrue(position != std::wstring::npos);
  script.replace(position, lint.size(), L"OpenROAD.Floorplan");
  const runtime::ProcessResult result =
      Run(floorplan, std::chrono::minutes(15));
  if (result.exit_code != 0) Logger::WriteMessage(result.output.c_str());
  Assert::IsTrue(result.started);
  Assert::AreEqual(static_cast<std::uint32_t>(0), result.exit_code);
  const adapters::ManagedFlowArtifactSet artifacts =
      adapter.DiscoverAvailableArtifacts(directory.path() / L"backend");
  Assert::IsFalse(artifacts.final_state.empty());
}

TEST_METHOD(OpenLaneClassicTinyFixtureProducesFinalPhysicalArtifacts) {
  ScopedDirectory directory;
  adapters::OpenLane2Adapter adapter;
  adapters::OpenLaneRequest request = WriteOpenLaneFixture(directory.path());
  const runtime::ProcessResult probe =
      Run(adapter.BuildProbeCommand(request.profile), std::chrono::minutes(2));
  if (probe.exit_code != 0) Logger::WriteMessage(probe.output.c_str());
  Assert::IsTrue(probe.started);
  Assert::AreEqual(static_cast<std::uint32_t>(0), probe.exit_code);
  Assert::IsTrue(probe.output.find("DESIGNPP_COMPAT_CONTRACT=") !=
                 std::string::npos);
  auto plan = adapter.BuildPlan(request);
  Assert::IsTrue(plan.Ok());
  std::ofstream(directory.path() / L"한글 OpenLane staging" / L"config.json",
                std::ios::binary)
      << plan.Value().config_json;
  runtime::WslCommand floorplan = plan.Value().validate;
  std::wstring& floorplan_script = floorplan.arguments[1];
  const std::wstring lint = L"Verilator.Lint";
  const std::size_t lint_position = floorplan_script.find(lint);
  Assert::IsTrue(lint_position != std::wstring::npos);
  floorplan_script.replace(lint_position, lint.size(), L"OpenROAD.Floorplan");
  const runtime::ProcessResult checkpoint_attempt =
      Run(floorplan, std::chrono::minutes(15));
  Assert::AreEqual(static_cast<std::uint32_t>(0), checkpoint_attempt.exit_code);
  const adapters::ManagedFlowArtifactSet checkpoint =
      adapter.DiscoverAvailableArtifacts(directory.path() / L"backend");
  Assert::IsFalse(checkpoint.final_state.empty());
  auto checkpoint_hash =
      application::CalculateFileSha256(checkpoint.final_state);
  Assert::IsTrue(checkpoint_hash.Ok());

  request.resume_step = "Odb.CheckMacroAntennaProperties";
  request.checkpoint_hash = std::move(checkpoint_hash).Value();
  plan = adapter.BuildPlan(request);
  Assert::IsTrue(plan.Ok());
  const runtime::ProcessResult result =
      Run(plan.Value().execute, std::chrono::minutes(40));
  if (result.exit_code != 0) Logger::WriteMessage(result.output.c_str());
  Assert::IsTrue(result.started);
  Assert::AreEqual(static_cast<std::uint32_t>(0), result.exit_code);
  auto artifacts = adapter.DiscoverArtifacts(directory.path() / L"backend" /
                                             L"runs" / L"designpp");
  Assert::IsTrue(artifacts.Ok());
  Assert::IsFalse(artifacts.Value().gds.empty());
  runtime::PathMapper mapper;
  auto mapped_gds = mapper.WindowsToWsl(artifacts.Value().gds);
  Assert::IsTrue(mapped_gds.Ok());
  runtime::WslCommand layout_check;
  layout_check.program = L"klayout";
  layout_check.arguments = {L"-b", mapped_gds.Value()};
  const runtime::ProcessResult layout_result =
      Run(layout_check, std::chrono::minutes(2));
  if (layout_result.exit_code != 0) {
    Logger::WriteMessage(layout_result.output.c_str());
  }
  Assert::IsTrue(layout_result.started);
  Assert::AreEqual(static_cast<std::uint32_t>(0), layout_result.exit_code);
  std::ifstream metrics_input(artifacts.Value().metrics_json, std::ios::binary);
  const std::string metrics((std::istreambuf_iterator<char>(metrics_input)),
                            std::istreambuf_iterator<char>());
  auto parsed = adapter.ParseMetrics(metrics);
  Assert::IsTrue(parsed.Ok());
  Assert::IsTrue(parsed.Value().drc_violations.has_value());
  Assert::IsTrue(parsed.Value().lvs_errors.has_value());
  Assert::IsTrue(parsed.Value().Passed());
}
}
;

}  // namespace designpp::tests

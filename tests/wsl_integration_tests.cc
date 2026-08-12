// Copyright 2026 The Design++ Authors

#include <CppUnitTest.h>
#include <windows.h>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <utility>

#include "designpp/runtime/path_mapper.h"
#include "designpp/runtime/wsl_executor.h"

using Microsoft::VisualStudio::CppUnitTestFramework::Assert;

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

runtime::ProcessResult Run(const runtime::WslCommand& command) {
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
  std::unique_lock lock(mutex);
  Assert::IsTrue(
      completed.wait_for(lock, std::chrono::seconds(30), [&] { return done; }));
  return result;
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
      << "synth -top dut\n"
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
  for (const wchar_t* name :
       {L"netlist.json", L"netlist.v", L"statistics.json", L"synthesis.rpt"}) {
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
      << "synth -top dut\n"
      << "write_json netlist.json\n"
      << "write_verilog netlist.v\n"
      << "tee -o synthesis.rpt stat\n"
      << "tee -o statistics.json stat -json\n";
  runtime::WslCommand synthesis;
  synthesis.program = L"yosys";
  synthesis.arguments = {L"-s", L"synthesis.ys"};
  synthesis.working_directory = mapped_staging.Value();
  const runtime::ProcessResult result = Run(synthesis);
  Assert::IsTrue(result.started);
  Assert::AreEqual(static_cast<std::uint32_t>(0), result.exit_code);
  for (const wchar_t* name :
       {L"netlist.json", L"netlist.v", L"statistics.json", L"synthesis.rpt"}) {
    const std::filesystem::path staged = staging_directory / name;
    const std::filesystem::path preserved = unicode_directory / name;
    std::filesystem::copy_file(staged, preserved);
    Assert::IsTrue(std::filesystem::exists(preserved));
    Assert::IsTrue(std::filesystem::file_size(preserved) > 0);
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
}
;

}  // namespace designpp::tests

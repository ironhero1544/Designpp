// Copyright 2026 The Design++ Authors

#include "designpp/adapters/icarus_simulation_adapter.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <sstream>

namespace designpp::adapters {
namespace {

std::wstring Utf8ToWide(std::string_view text) {
  if (text.empty()) return {};
  const int length =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), nullptr, 0);
  if (length <= 0) return {};
  std::wstring result(static_cast<std::size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                      static_cast<int>(text.size()), result.data(), length);
  return result;
}

std::string WideToUtf8(std::wstring_view text) {
  if (text.empty()) return {};
  const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr,
                                         0, nullptr, nullptr);
  if (length <= 0) return {};
  std::string result(static_cast<std::size_t>(length), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                      result.data(), length, nullptr, nullptr);
  return result;
}

bool IsIdentifier(std::string_view value) {
  if (value.empty() ||
      !(std::isalpha(static_cast<unsigned char>(value.front())) ||
        value.front() == '_' || value.front() == '$')) {
    return false;
  }
  return std::all_of(value.begin() + 1, value.end(), [](char character) {
    return std::isalnum(static_cast<unsigned char>(character)) ||
           character == '_' || character == '$';
  });
}

std::uint32_t ParseNumber(std::string_view value) {
  std::uint32_t result = 0;
  const auto parsed =
      std::from_chars(value.data(), value.data() + value.size(), result);
  return parsed.ec == std::errc() ? result : 0;
}

}  // namespace

runtime::WslCommand IcarusSimulationAdapter::BuildProbeCommand() const {
  runtime::WslCommand command;
  command.program = L"iverilog";
  command.arguments.emplace_back(L"-V");
  return command;
}

core::Status IcarusSimulationAdapter::Validate(
    const SimulationRequest& request) const {
  if (request.waveform_format != "none" && request.waveform_format != "vcd" &&
      request.waveform_format != "fst") {
    return {core::ErrorCode::kInvalidArgument, "Waveform format is invalid", 0};
  }
  if (request.waveform_enabled != (request.waveform_format != "none")) {
    return {core::ErrorCode::kInvalidArgument,
            "Waveform settings are inconsistent", 0};
  }
  if (!IsIdentifier(request.testbench_top)) {
    return {core::ErrorCode::kInvalidArgument,
            "Testbench top module is invalid", 0};
  }
  if (request.testbench_view_id.empty() ||
      request.testbench_relative_path.empty() ||
      request.artifact_directory.empty()) {
    return {core::ErrorCode::kInvalidArgument,
            "Testbench execution context is incomplete", 0};
  }
  bool has_rtl = false;
  bool has_testbench = false;
  for (const application::ResolvedSource& source : request.sources) {
    if (!source.enabled) continue;
    const bool selected = source.view_kind == core::ViewKind::kVerilog ||
                          (source.view_kind == core::ViewKind::kTestbench &&
                           source.view_id == request.testbench_view_id);
    if (!selected) continue;
    if (!source.exists) {
      return {core::ErrorCode::kNotFound,
              "Enabled simulation source is missing: " + source.relative_path,
              0};
    }
    has_rtl |= source.view_kind == core::ViewKind::kVerilog;
    has_testbench |= source.relative_path == request.testbench_relative_path;
  }
  if (!has_rtl) {
    return {core::ErrorCode::kInvalidArgument,
            "At least one enabled RTL source is required", 0};
  }
  return has_testbench
             ? core::Status::Success()
             : core::Status{core::ErrorCode::kInvalidArgument,
                            "The active Testbench source is not enabled", 0};
}

core::Result<SimulationPlan> IcarusSimulationAdapter::BuildPlan(
    const SimulationRequest& request,
    const runtime::PathMapper& path_mapper) const {
  const core::Status validation = Validate(request);
  if (!validation.Ok()) return validation;

  SimulationPlan plan;
  plan.binary_path = request.artifact_directory / L"simulation.vvp";
  plan.waveform_path =
      request.artifact_directory /
      (request.waveform_format == "fst" ? L"waveform.fst" : L"waveform.vcd");
  auto working_directory = path_mapper.WindowsToWsl(request.artifact_directory);
  auto binary = path_mapper.WindowsToWsl(plan.binary_path);
  if (!working_directory.Ok()) return working_directory.GetStatus();
  if (!binary.Ok()) return binary.GetStatus();

  std::string selected_top = request.testbench_top;
  if (request.waveform_enabled) {
    std::string suffix = WideToUtf8(
        request.artifact_directory.parent_path().filename().wstring());
    suffix.erase(std::remove_if(suffix.begin(), suffix.end(),
                                [](char value) {
                                  return !std::isalnum(
                                      static_cast<unsigned char>(value));
                                }),
                 suffix.end());
    const std::string wrapper = "designpp_waveform_" + suffix;
    plan.wrapper_path = request.artifact_directory / L"waveform_wrapper.sv";
    const std::string waveform_name =
        request.waveform_format == "fst" ? "waveform.fst" : "waveform.vcd";
    plan.wrapper_text = "module " + wrapper + ";\n  " + request.testbench_top +
                        " designpp_testbench();\n  initial begin\n    "
                        "$dumpfile(\"" +
                        waveform_name +
                        "\");\n    "
                        "$dumpvars(0, designpp_testbench);\n  end\nendmodule\n";
    selected_top = wrapper;
  }

  plan.compile.program = L"iverilog";
  plan.compile.working_directory = working_directory.Value();
  plan.compile.arguments = {L"-g2012", L"-s", Utf8ToWide(selected_top), L"-o",
                            binary.Value()};
  for (const std::string& define : request.project.defines) {
    plan.compile.arguments.push_back(L"-D" + Utf8ToWide(define));
  }
  for (const std::string& directory : request.project.include_directories) {
    if (request.sources.empty()) {
      return core::Status{core::ErrorCode::kInvalidArgument,
                          "Cannot map include path without a source", 0};
    }
    auto mapped = path_mapper.WindowsToWsl(
        request.sources.front().library_directory / Utf8ToWide(directory));
    if (!mapped.Ok()) return mapped.GetStatus();
    plan.compile.arguments.push_back(L"-I" + mapped.Value());
  }
  for (const application::ResolvedSource& source : request.sources) {
    if (!source.enabled || (source.view_kind != core::ViewKind::kVerilog &&
                            !(source.view_kind == core::ViewKind::kTestbench &&
                              source.view_id == request.testbench_view_id))) {
      continue;
    }
    auto mapped = path_mapper.WindowsToWsl(source.windows_path);
    if (!mapped.Ok()) return mapped.GetStatus();
    plan.compile.arguments.push_back(mapped.Value());
  }
  if (request.waveform_enabled) {
    auto wrapper = path_mapper.WindowsToWsl(plan.wrapper_path);
    if (!wrapper.Ok()) return wrapper.GetStatus();
    plan.compile.arguments.push_back(wrapper.Value());
  }

  plan.execute.program = L"vvp";
  plan.execute.working_directory = working_directory.Value();
  if (request.waveform_format == "fst") {
    plan.execute.arguments.push_back(L"-fst");
  }
  plan.execute.arguments.push_back(L"-N");
  plan.execute.arguments.push_back(binary.Value());
  return plan;
}

std::vector<core::Diagnostic> IcarusSimulationAdapter::ParseDiagnostics(
    std::string_view raw_output) const {
  std::vector<core::Diagnostic> diagnostics;
  std::istringstream input{std::string(raw_output)};
  std::string line;
  while (std::getline(input, line)) {
    const std::size_t first = line.find(':');
    const std::size_t second = first == std::string::npos
                                   ? std::string::npos
                                   : line.find(':', first + 1);
    if (first == std::string::npos || second == std::string::npos) continue;
    const std::string_view remainder(line.data() + second + 1,
                                     line.size() - second - 1);
    const bool error = remainder.find("error") != std::string_view::npos ||
                       remainder.find("syntax") != std::string_view::npos ||
                       remainder.find("sorry") != std::string_view::npos;
    const bool warning = remainder.find("warning") != std::string_view::npos;
    if (!error && !warning) continue;
    core::Diagnostic diagnostic;
    diagnostic.severity = error ? core::DiagnosticSeverity::kError
                                : core::DiagnosticSeverity::kWarning;
    diagnostic.code = error ? "IVERILOG" : "IVERILOG-WARNING";
    diagnostic.file = line.substr(0, first);
    diagnostic.line = ParseNumber(
        std::string_view(line).substr(first + 1, second - first - 1));
    diagnostic.message = line.substr(second + 1);
    while (!diagnostic.message.empty() && diagnostic.message.front() == ' ') {
      diagnostic.message.erase(diagnostic.message.begin());
    }
    diagnostics.push_back(std::move(diagnostic));
  }
  return diagnostics;
}

}  // namespace designpp::adapters

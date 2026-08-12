// Copyright 2026 The Design++ Authors

#include "designpp/adapters/verilator_simulation_adapter.h"

#include <windows.h>

#include <algorithm>
#include <cctype>

#include "designpp/adapters/verilator_adapter.h"

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

}  // namespace

runtime::WslCommand VerilatorSimulationAdapter::BuildProbeCommand() const {
  return VerilatorAdapter().BuildProbeCommand();
}

core::Status VerilatorSimulationAdapter::Validate(
    const SimulationRequest& request) const {
  if (!IsIdentifier(request.testbench_top)) {
    return {core::ErrorCode::kInvalidArgument,
            "Testbench top module is invalid", 0};
  }
  if (request.waveform_format != "none" && request.waveform_format != "vcd" &&
      request.waveform_format != "fst") {
    return {core::ErrorCode::kInvalidArgument, "Waveform format is invalid", 0};
  }
  if (request.waveform_enabled != (request.waveform_format != "none")) {
    return {core::ErrorCode::kInvalidArgument,
            "Waveform settings are inconsistent", 0};
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

core::Result<SimulationPlan> VerilatorSimulationAdapter::BuildPlan(
    const SimulationRequest& request,
    const runtime::PathMapper& path_mapper) const {
  const core::Status validation = Validate(request);
  if (!validation.Ok()) return validation;

  const std::filesystem::path build_directory = request.build_directory.empty()
                                                    ? request.artifact_directory
                                                    : request.build_directory;
  SimulationPlan plan;
  plan.binary_path = build_directory / L"simulation";
  plan.waveform_path =
      build_directory /
      (request.waveform_format == "fst" ? L"waveform.fst" : L"waveform.vcd");
  plan.wrapper_path = build_directory / L"waveform_wrapper.sv";
  const std::filesystem::path object_directory =
      build_directory / L"verilator_obj";
  auto working_directory = path_mapper.WindowsToWsl(build_directory);
  auto binary = path_mapper.WindowsToWsl(plan.binary_path);
  auto object = path_mapper.WindowsToWsl(object_directory);
  if (!working_directory.Ok()) return working_directory.GetStatus();
  if (!binary.Ok()) return binary.GetStatus();
  if (!object.Ok()) return object.GetStatus();

  std::string selected_top = request.testbench_top;
  if (request.waveform_enabled) {
    auto waveform = path_mapper.WindowsToWsl(plan.waveform_path);
    if (!waveform.Ok()) return waveform.GetStatus();
    std::string suffix =
        WideToUtf8(build_directory.parent_path().filename().wstring());
    suffix.erase(std::remove_if(suffix.begin(), suffix.end(),
                                [](char value) {
                                  return !std::isalnum(
                                      static_cast<unsigned char>(value));
                                }),
                 suffix.end());
    selected_top = "designpp_waveform_" + suffix;
    plan.wrapper_text = "module " + selected_top + ";\n  " +
                        request.testbench_top +
                        " designpp_testbench();\n  initial begin\n    "
                        "$dumpfile(\"" +
                        WideToUtf8(waveform.Value()) +
                        "\");\n    $dumpvars(0, designpp_testbench);\n  "
                        "end\nendmodule\n";
  } else {
    plan.wrapper_path.clear();
  }

  plan.compile.program = L"verilator";
  plan.compile.working_directory = working_directory.Value();
  plan.compile.arguments = {
      L"--binary", L"--timing",    L"--top-module", Utf8ToWide(selected_top),
      L"--Mdir",   object.Value(), L"-o",           binary.Value()};
  if (request.waveform_format == "fst") {
    plan.compile.arguments.push_back(L"--trace-fst");
  } else if (request.waveform_format == "vcd") {
    // Verilator 5.020, supported by the WSL runtime, does not recognize the
    // newer --trace-vcd spelling. --trace is the compatible VCD request and
    // remains accepted by current versions.
    plan.compile.arguments.push_back(L"--trace");
  }
  plan.compile.arguments.push_back(L"-j");
  plan.compile.arguments.push_back(std::to_wstring(request.project.cpu_budget));
  for (const std::string& define : request.project.defines) {
    plan.compile.arguments.push_back(L"-D" + Utf8ToWide(define));
  }
  for (const std::string& parameter : request.project.parameters) {
    plan.compile.arguments.push_back(L"-G" + Utf8ToWide(parameter));
  }
  for (const std::string& directory : request.project.include_directories) {
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

  plan.execute.program = binary.Value();
  plan.execute.working_directory = working_directory.Value();
  return plan;
}

std::vector<core::Diagnostic> VerilatorSimulationAdapter::ParseDiagnostics(
    std::string_view raw_output) const {
  return VerilatorAdapter().ParseDiagnostics(raw_output);
}

}  // namespace designpp::adapters

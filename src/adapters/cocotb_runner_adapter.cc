// Copyright 2026 The Design++ Authors

#include "designpp/adapters/cocotb_runner_adapter.h"

#include <windows.h>

#include <cctype>

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

bool IsPythonModule(std::string_view value) {
  if (value.empty()) return false;
  bool component_start = true;
  for (const char character : value) {
    if (component_start) {
      if (!(std::isalpha(static_cast<unsigned char>(character)) ||
            character == '_')) {
        return false;
      }
      component_start = false;
    } else if (character == '.') {
      component_start = true;
    } else if (!(std::isalnum(static_cast<unsigned char>(character)) ||
                 character == '_')) {
      return false;
    }
  }
  return !component_start;
}

std::wstring EscapeMakeListValue(std::wstring_view value) {
  std::wstring result;
  for (const wchar_t character : value) {
    if (character == L' ' || character == L'\t' || character == L'#' ||
        character == L'$' || character == L'\\') {
      result.push_back(L'\\');
    }
    result.push_back(character);
  }
  return result;
}

}  // namespace

runtime::WslCommand CocotbRunnerAdapter::BuildProbeCommand() const {
  runtime::WslCommand command;
  command.program = L"cocotb-config";
  command.arguments.push_back(L"--version");
  return command;
}

runtime::WslCommand CocotbRunnerAdapter::BuildMakefilesProbeCommand() const {
  runtime::WslCommand command;
  command.program = L"cocotb-config";
  command.arguments.push_back(L"--makefiles");
  return command;
}

core::Status CocotbRunnerAdapter::Validate(const CocotbRequest& request) const {
  if (!IsPythonModule(request.module)) {
    return {core::ErrorCode::kInvalidArgument, "cocotb test module is invalid",
            0};
  }
  if (request.backend != "icarus" && request.backend != "verilator") {
    return {core::ErrorCode::kInvalidArgument,
            "cocotb simulator backend is invalid", 0};
  }
  if (request.makefiles_directory.empty()) {
    return {core::ErrorCode::kInvalidArgument,
            "cocotb Makefile directory is missing", 0};
  }
  if (request.simulation.artifact_directory.empty() ||
      request.simulation.testbench_top.empty()) {
    return {core::ErrorCode::kInvalidArgument,
            "cocotb execution context is incomplete", 0};
  }
  bool has_rtl = false;
  for (const application::ResolvedSource& source : request.simulation.sources) {
    if (!source.enabled || source.view_kind != core::ViewKind::kVerilog) {
      continue;
    }
    if (!source.exists) {
      return {core::ErrorCode::kNotFound,
              "Enabled RTL source is missing: " + source.relative_path, 0};
    }
    has_rtl = true;
  }
  return has_rtl
             ? core::Status::Success()
             : core::Status{core::ErrorCode::kInvalidArgument,
                            "At least one enabled RTL source is required", 0};
}

core::Result<CocotbPlan> CocotbRunnerAdapter::BuildPlan(
    const CocotbRequest& request,
    const runtime::PathMapper& path_mapper) const {
  const core::Status validation = Validate(request);
  if (!validation.Ok()) return validation;
  CocotbPlan plan;
  plan.results_path = request.simulation.artifact_directory / L"results.xml";
  if (request.simulation.waveform_enabled) {
    const std::wstring extension =
        request.simulation.waveform_format == "vcd" ? L".vcd" : L".fst";
    const std::wstring name =
        request.backend == "verilator"
            ? L"dump"
            : Utf8ToWide(request.simulation.testbench_top);
    plan.waveform_path = request.simulation.artifact_directory /
                         std::filesystem::path(name + extension);
  }
  auto working =
      path_mapper.WindowsToWsl(request.simulation.artifact_directory);
  auto results = path_mapper.WindowsToWsl(plan.results_path);
  if (!working.Ok()) return working.GetStatus();
  if (!results.Ok()) return results.GetStatus();
  std::wstring sources;
  for (const application::ResolvedSource& source : request.simulation.sources) {
    if (!source.enabled || source.view_kind != core::ViewKind::kVerilog) {
      continue;
    }
    auto mapped = path_mapper.WindowsToWsl(source.windows_path);
    if (!mapped.Ok()) return mapped.GetStatus();
    if (!sources.empty()) sources.push_back(L' ');
    sources += EscapeMakeListValue(mapped.Value());
  }
  plan.execute.program = L"make";
  plan.execute.working_directory = working.Value();
  plan.execute.arguments = {
      L"-f", request.makefiles_directory + L"/Makefile.sim", L"-j",
      std::to_wstring(request.simulation.project.cpu_budget), L"sim"};
  plan.execute.environment = {
      {L"SIM", Utf8ToWide(request.backend)},
      {L"TOPLEVEL_LANG", L"verilog"},
      {L"TOPLEVEL", Utf8ToWide(request.simulation.testbench_top)},
      {L"COCOTB_TEST_MODULES", Utf8ToWide(request.module)},
      {L"COCOTB_RESULTS_FILE", results.Value()},
      {L"VERILOG_SOURCES", std::move(sources)},
      {L"WAVES", request.simulation.waveform_enabled ? L"1" : L"0"}};
  if (!request.testcase_filter.empty()) {
    plan.execute.environment.emplace_back(L"COCOTB_TEST_FILTER",
                                          Utf8ToWide(request.testcase_filter));
  }
  if (request.backend == "verilator" && request.simulation.waveform_enabled) {
    plan.execute.environment.emplace_back(
        L"EXTRA_ARGS", request.simulation.waveform_format == "fst"
                           ? L"--trace-fst --trace-structs"
                           : L"--trace-vcd --trace-structs");
  }
  return plan;
}

}  // namespace designpp::adapters

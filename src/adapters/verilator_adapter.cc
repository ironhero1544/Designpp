// Copyright 2026 The Design++ Authors

#include "designpp/adapters/verilator_adapter.h"

#include <windows.h>

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

std::uint32_t ParseNumber(std::string_view value) {
  std::uint32_t number = 0;
  const auto parsed =
      std::from_chars(value.data(), value.data() + value.size(), number);
  return parsed.ec == std::errc() ? number : 0;
}

}  // namespace

runtime::WslCommand VerilatorAdapter::BuildProbeCommand() const {
  runtime::WslCommand command;
  command.program = L"verilator";
  command.arguments.emplace_back(L"--version");
  return command;
}

core::Status VerilatorAdapter::Validate(
    const core::Project& project,
    const std::vector<application::ResolvedSource>& sources) const {
  if (project.top_module.empty()) {
    return {core::ErrorCode::kInvalidArgument, "Top module is not configured",
            0};
  }
  bool has_rtl = false;
  for (const application::ResolvedSource& source : sources) {
    if (!source.enabled) continue;
    if (!source.exists) {
      return {core::ErrorCode::kNotFound,
              "Enabled source file is missing: " + source.relative_path, 0};
    }
    has_rtl |= source.view_kind == core::ViewKind::kVerilog;
  }
  return has_rtl ? core::Status::Success()
                 : core::Status{core::ErrorCode::kInvalidArgument,
                                "At least one RTL source is required", 0};
}

core::Result<runtime::WslCommand> VerilatorAdapter::BuildCommand(
    const core::Project& project,
    const std::vector<application::ResolvedSource>& sources,
    const runtime::PathMapper& path_mapper,
    const ToolCapabilities& capabilities) const {
  core::Status validation = Validate(project, sources);
  if (!validation.Ok()) return validation;
  runtime::WslCommand command;
  command.program = L"verilator";
  command.arguments = {L"--lint-only", L"--Wall", L"--top-module",
                       Utf8ToWide(project.top_module)};
  if (capabilities.supports_parallel_jobs && project.cpu_budget > 1) {
    command.arguments.emplace_back(L"-j");
    command.arguments.push_back(std::to_wstring(project.cpu_budget));
  }
  for (const std::string& define : project.defines) {
    command.arguments.push_back(L"-D" + Utf8ToWide(define));
  }
  for (const std::string& parameter : project.parameters) {
    command.arguments.push_back(L"-G" + Utf8ToWide(parameter));
  }
  for (const std::string& directory : project.include_directories) {
    if (sources.empty()) {
      return core::Status{core::ErrorCode::kInvalidArgument,
                          "Cannot map include path without a Library source",
                          0};
    }
    auto mapped =
        path_mapper.WindowsToWsl(sources.front().library_directory /
                                 std::filesystem::path(Utf8ToWide(directory)));
    if (!mapped.Ok()) return mapped.GetStatus();
    command.arguments.push_back(L"-I" + mapped.Value());
  }
  for (const application::ResolvedSource& source : sources) {
    if (!source.enabled || source.view_kind == core::ViewKind::kConstraints) {
      continue;
    }
    auto mapped = path_mapper.WindowsToWsl(source.windows_path);
    if (!mapped.Ok()) return mapped.GetStatus();
    command.arguments.push_back(mapped.Value());
  }
  return command;
}

std::vector<core::Diagnostic> VerilatorAdapter::ParseDiagnostics(
    std::string_view raw_output) const {
  std::vector<core::Diagnostic> diagnostics;
  std::istringstream input{std::string(raw_output)};
  std::string line;
  while (std::getline(input, line)) {
    if (!line.starts_with("%Error") && !line.starts_with("%Warning")) {
      continue;
    }
    core::Diagnostic diagnostic;
    diagnostic.severity = line.starts_with("%Error")
                              ? core::DiagnosticSeverity::kError
                              : core::DiagnosticSeverity::kWarning;
    const std::size_t label_end = line.find(':');
    if (label_end == std::string::npos) continue;
    const std::size_t dash = line.find('-');
    if (dash != std::string::npos && dash < label_end) {
      diagnostic.code = line.substr(dash + 1, label_end - dash - 1);
    }
    std::size_t file_start = label_end + 1;
    while (file_start < line.size() && line[file_start] == ' ') ++file_start;
    const std::size_t line_separator = line.find(':', file_start);
    const std::size_t column_separator =
        line_separator == std::string::npos
            ? std::string::npos
            : line.find(':', line_separator + 1);
    const std::size_t message_separator =
        column_separator == std::string::npos
            ? std::string::npos
            : line.find(':', column_separator + 1);
    if (line_separator == std::string::npos ||
        column_separator == std::string::npos) {
      diagnostic.message = line.substr(file_start);
    } else {
      diagnostic.file = line.substr(file_start, line_separator - file_start);
      diagnostic.line = ParseNumber(std::string_view(line).substr(
          line_separator + 1, column_separator - line_separator - 1));
      if (message_separator != std::string::npos) {
        diagnostic.column = ParseNumber(std::string_view(line).substr(
            column_separator + 1, message_separator - column_separator - 1));
        diagnostic.message = line.substr(message_separator + 1);
      } else {
        diagnostic.message = line.substr(column_separator + 1);
      }
    }
    while (!diagnostic.message.empty() && diagnostic.message.front() == ' ') {
      diagnostic.message.erase(diagnostic.message.begin());
    }
    diagnostics.push_back(std::move(diagnostic));
  }
  return diagnostics;
}

}  // namespace designpp::adapters

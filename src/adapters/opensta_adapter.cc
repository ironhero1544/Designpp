// Copyright 2026 The Design++ Authors

#include "designpp/adapters/opensta_adapter.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <sstream>

namespace designpp::adapters {
namespace {

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
        value.front() == '_')) {
    return false;
  }
  return std::all_of(value.begin() + 1, value.end(), [](char character) {
    return std::isalnum(static_cast<unsigned char>(character)) ||
           character == '_';
  });
}

std::string TclQuote(std::wstring_view value) {
  std::string result = "\"";
  for (const char character : WideToUtf8(value)) {
    if (character == '\\' || character == '$' || character == '[' ||
        character == ']' || character == '"') {
      result.push_back('\\');
    }
    result.push_back(character);
  }
  result.push_back('"');
  return result;
}

std::string_view Trim(std::string_view value) {
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.front()))) {
    value.remove_prefix(1);
  }
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.back()))) {
    value.remove_suffix(1);
  }
  return value;
}

core::Result<double> FirstNumber(std::string_view section) {
  for (std::size_t position = 0; position < section.size(); ++position) {
    if (!std::isdigit(static_cast<unsigned char>(section[position])) &&
        section[position] != '-' && section[position] != '+') {
      continue;
    }
    double value = 0.0;
    const auto parsed = std::from_chars(section.data() + position,
                                        section.data() + section.size(), value);
    if (parsed.ec == std::errc()) return value;
  }
  return core::Status{core::ErrorCode::kCorruptData,
                      "Timing report metric is missing", 0};
}

core::Result<double> MarkerValue(std::string_view output,
                                 std::string_view begin_marker,
                                 std::string_view end_marker) {
  const std::size_t begin = output.find(begin_marker);
  if (begin == std::string_view::npos) {
    return core::Status{core::ErrorCode::kCorruptData,
                        "Timing report marker is missing", 0};
  }
  const std::size_t content = begin + begin_marker.size();
  const std::size_t end = output.find(end_marker, content);
  if (end == std::string_view::npos) {
    return core::Status{core::ErrorCode::kCorruptData,
                        "Timing report marker is unterminated", 0};
  }
  return FirstNumber(output.substr(content, end - content));
}

}  // namespace

runtime::WslCommand OpenStaAdapter::BuildProbeCommand() const {
  runtime::WslCommand command;
  command.program = L"sta";
  command.arguments.push_back(L"-version");
  return command;
}

core::Status OpenStaAdapter::Validate(const TimingRequest& request) const {
  if (!IsIdentifier(request.top_module)) {
    return {core::ErrorCode::kInvalidArgument, "Top module is invalid", 0};
  }
  if (request.corner_name.empty() || request.artifact_directory.empty()) {
    return {core::ErrorCode::kInvalidArgument,
            "Timing corner or artifact directory is missing", 0};
  }
  if (request.liberty_files.empty()) {
    return {core::ErrorCode::kInvalidArgument,
            "At least one Liberty file is required", 0};
  }
  std::vector<std::filesystem::path> required = {request.netlist_path,
                                                 request.sdc_path};
  required.insert(required.end(), request.liberty_files.begin(),
                  request.liberty_files.end());
  for (const std::filesystem::path& path : required) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error) {
      return {core::ErrorCode::kNotFound,
              "Timing analysis input file is missing", 0};
    }
  }
  return core::Status::Success();
}

core::Result<TimingPlan> OpenStaAdapter::BuildPlan(
    const TimingRequest& request,
    const runtime::PathMapper& path_mapper) const {
  const core::Status validation = Validate(request);
  if (!validation.Ok()) return validation;
  TimingPlan plan;
  plan.script_path = request.artifact_directory / L"timing.tcl";
  plan.report_path = request.artifact_directory / L"timing.rpt";
  auto working = path_mapper.WindowsToWsl(request.artifact_directory);
  auto script = path_mapper.WindowsToWsl(plan.script_path);
  auto netlist = path_mapper.WindowsToWsl(request.netlist_path);
  auto sdc = path_mapper.WindowsToWsl(request.sdc_path);
  if (!working.Ok()) return working.GetStatus();
  if (!script.Ok()) return script.GetStatus();
  if (!netlist.Ok()) return netlist.GetStatus();
  if (!sdc.Ok()) return sdc.GetStatus();
  for (const std::filesystem::path& liberty_path : request.liberty_files) {
    auto liberty = path_mapper.WindowsToWsl(liberty_path);
    if (!liberty.Ok()) return liberty.GetStatus();
    plan.script_text += "read_liberty " + TclQuote(liberty.Value()) + "\n";
  }
  plan.script_text += "read_verilog " + TclQuote(netlist.Value()) + "\n";
  plan.script_text += "link_design " + request.top_module + "\n";
  plan.script_text += "read_sdc " + TclQuote(sdc.Value()) + "\n";
  plan.script_text += "check_setup\n";
  plan.script_text += "puts \"DESIGNPP_CHECKS_BEGIN\"\n";
  plan.script_text += "report_checks -path_delay max -slack_max 0.0\n";
  plan.script_text += "puts \"DESIGNPP_CHECKS_END\"\n";
  plan.script_text += "puts \"DESIGNPP_WNS_BEGIN\"\nreport_wns\n";
  plan.script_text += "puts \"DESIGNPP_WNS_END\"\n";
  plan.script_text += "puts \"DESIGNPP_TNS_BEGIN\"\nreport_tns\n";
  plan.script_text += "puts \"DESIGNPP_TNS_END\"\n";
  plan.execute.program = L"sta";
  plan.execute.working_directory = working.Value();
  plan.execute.arguments = {L"-no_init", L"-exit", script.Value()};
  return plan;
}

core::Result<TimingMetrics> OpenStaAdapter::ParseReport(
    std::string_view raw_output, std::string_view corner_name) const {
  if (raw_output.empty() || raw_output.size() > 64 * 1024 * 1024) {
    return core::Status{raw_output.empty() ? core::ErrorCode::kCorruptData
                                           : core::ErrorCode::kFileTooLarge,
                        "Timing report size is invalid", 0};
  }
  auto wns = MarkerValue(raw_output, "DESIGNPP_WNS_BEGIN", "DESIGNPP_WNS_END");
  auto tns = MarkerValue(raw_output, "DESIGNPP_TNS_BEGIN", "DESIGNPP_TNS_END");
  if (!wns.Ok()) return wns.GetStatus();
  if (!tns.Ok()) return tns.GetStatus();
  TimingMetrics metrics;
  metrics.wns = wns.Value();
  metrics.tns = tns.Value();

  std::istringstream lines{std::string(raw_output)};
  std::string line;
  std::string startpoint;
  std::string endpoint;
  while (std::getline(lines, line)) {
    if (line.starts_with("Startpoint:")) {
      startpoint = std::string(Trim(std::string_view(line).substr(11)));
    } else if (line.starts_with("Endpoint:")) {
      endpoint = std::string(Trim(std::string_view(line).substr(9)));
    } else if (line.find("slack") != std::string::npos &&
               line.find("VIOLATED") != std::string::npos) {
      auto slack = FirstNumber(line);
      if (!slack.Ok()) continue;
      metrics.violations.push_back({std::string(corner_name), startpoint,
                                    endpoint, "setup", slack.Value()});
      startpoint.clear();
      endpoint.clear();
    }
  }
  return metrics;
}

std::vector<core::Diagnostic> OpenStaAdapter::ParseDiagnostics(
    std::string_view raw_output) const {
  std::vector<core::Diagnostic> diagnostics;
  std::istringstream lines{std::string(raw_output)};
  std::string line;
  while (std::getline(lines, line)) {
    const bool error = line.starts_with("Error:") || line.starts_with("ERROR:");
    const bool warning =
        line.starts_with("Warning:") || line.starts_with("WARNING:");
    if (!error && !warning) continue;
    core::Diagnostic diagnostic;
    diagnostic.severity = error ? core::DiagnosticSeverity::kError
                                : core::DiagnosticSeverity::kWarning;
    diagnostic.code = error ? "OPENSTA" : "OPENSTA-WARNING";
    diagnostic.message = line.substr(line.find(':') + 1);
    while (!diagnostic.message.empty() && diagnostic.message.front() == ' ') {
      diagnostic.message.erase(diagnostic.message.begin());
    }
    diagnostics.push_back(std::move(diagnostic));
  }
  return diagnostics;
}

}  // namespace designpp::adapters

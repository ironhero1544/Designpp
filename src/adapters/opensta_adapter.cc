// Copyright 2026 The Design++ Authors

#include "designpp/adapters/opensta_adapter.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
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

double NormalizeNumericalNoise(double value) {
  constexpr double kTimingEpsilon = 1.0e-9;
  return std::abs(value) < kTimingEpsilon ? 0.0 : value;
}

bool IsSetupCheck(std::string_view check_type) {
  return check_type == "setup" || check_type == "recovery";
}

bool IsHoldCheck(std::string_view check_type) {
  return check_type == "hold" || check_type == "removal";
}

}  // namespace

runtime::WslCommand OpenStaAdapter::BuildProbeCommand() const {
  runtime::WslCommand command;
  command.program = L"/bin/bash";
  command.arguments = {
      L"-lc",
      L". /nix/var/nix/profiles/default/etc/profile.d/nix-daemon.sh "
      L"2>/dev/null || true; nix-shell "
      L"\"$HOME/.designpp/toolchains/openlane2/shell.nix\" "
      L"--run 'sta -version'"};
  return command;
}

core::Status OpenStaAdapter::Validate(const TimingRequest& request) const {
  if (!IsIdentifier(request.top_module)) {
    return {core::ErrorCode::kInvalidArgument, "Top module is invalid", 0};
  }
  if (!IsIdentifier(request.corner_name) ||
      request.artifact_directory.empty() || request.cpu_threads == 0) {
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

core::Status OpenStaAdapter::ValidateSdcText(std::string_view text) const {
  if (Trim(text).empty()) {
    return {core::ErrorCode::kInvalidArgument, "Timing SDC is empty", 0};
  }
  std::istringstream lines{std::string(text)};
  std::string line;
  std::size_t line_number = 0;
  while (std::getline(lines, line)) {
    ++line_number;
    const std::size_t comment = line.find('#');
    const std::string_view command =
        comment == std::string::npos
            ? std::string_view(line)
            : std::string_view(line).substr(0, comment);
    if (command.find("remove_from_collection") != std::string_view::npos) {
      return {core::ErrorCode::kInvalidArgument,
              "SDC line " + std::to_string(line_number) +
                  " uses unsupported OpenSTA command "
                  "'remove_from_collection'; use [all_inputs -no_clocks] "
                  "for non-clock inputs",
              0};
    }
  }
  return core::Status::Success();
}

TimingMetrics OpenStaAdapter::NormalizeMetrics(TimingMetrics metrics) const {
  metrics.setup.wns = std::min(0.0, NormalizeNumericalNoise(metrics.setup.wns));
  metrics.setup.tns = std::min(0.0, NormalizeNumericalNoise(metrics.setup.tns));
  metrics.hold.wns = std::min(0.0, NormalizeNumericalNoise(metrics.hold.wns));
  metrics.hold.tns = std::min(0.0, NormalizeNumericalNoise(metrics.hold.tns));
  metrics.setup.violation_count = 0;
  metrics.hold.violation_count = 0;
  metrics.violation_counts = {};
  double setup_violation_wns = 0.0;
  double setup_violation_tns = 0.0;
  double hold_violation_wns = 0.0;
  double hold_violation_tns = 0.0;
  for (TimingViolation& violation : metrics.violations) {
    if (violation.endpoint.find("removal check") != std::string::npos) {
      violation.check_type = "removal";
    } else if (violation.endpoint.find("recovery check") != std::string::npos) {
      violation.check_type = "recovery";
    }
    if (IsSetupCheck(violation.check_type)) {
      ++metrics.setup.violation_count;
      if (violation.check_type == "recovery") {
        ++metrics.violation_counts.recovery;
      } else {
        ++metrics.violation_counts.setup;
      }
      setup_violation_wns = std::min(setup_violation_wns, violation.slack);
      setup_violation_tns += std::min(0.0, violation.slack);
    } else if (IsHoldCheck(violation.check_type)) {
      ++metrics.hold.violation_count;
      if (violation.check_type == "removal") {
        ++metrics.violation_counts.removal;
      } else {
        ++metrics.violation_counts.hold;
      }
      hold_violation_wns = std::min(hold_violation_wns, violation.slack);
      hold_violation_tns += std::min(0.0, violation.slack);
    } else {
      ++metrics.violation_counts.unknown;
    }
  }
  metrics.setup.wns = std::min(metrics.setup.wns, setup_violation_wns);
  metrics.setup.tns = std::min(metrics.setup.tns, setup_violation_tns);
  metrics.hold.wns = std::min(metrics.hold.wns, hold_violation_wns);
  metrics.hold.tns = std::min(metrics.hold.tns, hold_violation_tns);
  return metrics;
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
  auto report = path_mapper.WindowsToWsl(plan.report_path);
  if (!working.Ok()) return working.GetStatus();
  if (!script.Ok()) return script.GetStatus();
  if (!netlist.Ok()) return netlist.GetStatus();
  if (!sdc.Ok()) return sdc.GetStatus();
  if (!report.Ok()) return report.GetStatus();
  for (const std::filesystem::path& liberty_path : request.liberty_files) {
    auto liberty = path_mapper.WindowsToWsl(liberty_path);
    if (!liberty.Ok()) return liberty.GetStatus();
    plan.script_text += "read_liberty -corner " + request.corner_name + " " +
                        TclQuote(liberty.Value()) + "\n";
  }
  plan.script_text += "read_verilog " + TclQuote(netlist.Value()) + "\n";
  plan.script_text =
      "define_corners " + request.corner_name + "\n" + plan.script_text;
  plan.script_text += "link_design " + request.top_module + "\n";
  plan.script_text += "read_sdc " + TclQuote(sdc.Value()) + "\n";
  plan.script_text += "check_setup\n";
  plan.script_text +=
      "set designpp_report [open " + TclQuote(report.Value()) + " w]\n";
  plan.script_text +=
      "puts $designpp_report \"DESIGNPP_SETUP_BEGIN\"\nclose "
      "$designpp_report\n";
  plan.script_text +=
      "report_checks -path_delay max -slack_max 0.0 -group_count 1000 "
      "-endpoint_count 1 -format full_clock_expanded -digits 6 "
      "-no_line_splits >> " +
      TclQuote(report.Value()) + "\n";
  plan.script_text +=
      "set designpp_report [open " + TclQuote(report.Value()) + " a]\n";
  plan.script_text +=
      "puts $designpp_report \"DESIGNPP_SETUP_END\"\n"
      "puts $designpp_report \"DESIGNPP_HOLD_BEGIN\"\n"
      "close $designpp_report\n";
  plan.script_text +=
      "report_checks -path_delay min -slack_max 0.0 -group_count 1000 "
      "-endpoint_count 1 -format full_clock_expanded -digits 6 "
      "-no_line_splits >> " +
      TclQuote(report.Value()) + "\n";
  plan.script_text +=
      "set designpp_report [open " + TclQuote(report.Value()) + " a]\n";
  plan.script_text +=
      "puts $designpp_report \"DESIGNPP_HOLD_END\"\nclose "
      "$designpp_report\n";
  plan.script_text +=
      "puts \"DESIGNPP_SETUP_WNS_BEGIN\"\nputs [sta::worst_slack_cmd max]\n"
      "puts \"DESIGNPP_SETUP_WNS_END\"\n"
      "puts \"DESIGNPP_SETUP_TNS_BEGIN\"\n"
      "puts [sta::total_negative_slack_cmd max]\n"
      "puts \"DESIGNPP_SETUP_TNS_END\"\n"
      "puts \"DESIGNPP_HOLD_WNS_BEGIN\"\nputs [sta::worst_slack_cmd min]\n"
      "puts \"DESIGNPP_HOLD_WNS_END\"\n"
      "puts \"DESIGNPP_HOLD_TNS_BEGIN\"\n"
      "puts [sta::total_negative_slack_cmd min]\n"
      "puts \"DESIGNPP_HOLD_TNS_END\"\n"
      "puts \"DESIGNPP_SETUP_PATHS_BEGIN\"\n"
      "puts [llength [find_timing_paths -path_delay max -group_count 1]]\n"
      "puts \"DESIGNPP_SETUP_PATHS_END\"\n"
      "puts \"DESIGNPP_HOLD_PATHS_BEGIN\"\n"
      "puts [llength [find_timing_paths -path_delay min -group_count 1]]\n"
      "puts \"DESIGNPP_HOLD_PATHS_END\"\n";
  plan.execute.program = L"/bin/bash";
  plan.execute.working_directory = working.Value();
  plan.execute.arguments = {
      L"-lc",
      L". /nix/var/nix/profiles/default/etc/profile.d/nix-daemon.sh "
      L"2>/dev/null || true; nix-shell "
      L"\"$HOME/.designpp/toolchains/openlane2/shell.nix\" --run "
      L"'sta -threads " +
          std::to_wstring(request.cpu_threads) + L" -no_init -exit \"" +
          script.Value() + L"\"'"};
  return plan;
}

core::Result<TimingMetrics> OpenStaAdapter::ParseReport(
    std::string_view raw_output, std::string_view report,
    std::string_view corner_name) const {
  if (raw_output.empty() || report.empty() ||
      raw_output.size() + report.size() > 64 * 1024 * 1024) {
    return core::Status{raw_output.empty() ? core::ErrorCode::kCorruptData
                                           : core::ErrorCode::kFileTooLarge,
                        "Timing report size is invalid", 0};
  }
  auto setup_wns = MarkerValue(raw_output, "DESIGNPP_SETUP_WNS_BEGIN",
                               "DESIGNPP_SETUP_WNS_END");
  auto setup_tns = MarkerValue(raw_output, "DESIGNPP_SETUP_TNS_BEGIN",
                               "DESIGNPP_SETUP_TNS_END");
  auto hold_wns = MarkerValue(raw_output, "DESIGNPP_HOLD_WNS_BEGIN",
                              "DESIGNPP_HOLD_WNS_END");
  auto hold_tns = MarkerValue(raw_output, "DESIGNPP_HOLD_TNS_BEGIN",
                              "DESIGNPP_HOLD_TNS_END");
  auto setup_paths = MarkerValue(raw_output, "DESIGNPP_SETUP_PATHS_BEGIN",
                                 "DESIGNPP_SETUP_PATHS_END");
  auto hold_paths = MarkerValue(raw_output, "DESIGNPP_HOLD_PATHS_BEGIN",
                                "DESIGNPP_HOLD_PATHS_END");
  if (!setup_wns.Ok()) return setup_wns.GetStatus();
  if (!setup_tns.Ok()) return setup_tns.GetStatus();
  if (!hold_wns.Ok()) return hold_wns.GetStatus();
  if (!hold_tns.Ok()) return hold_tns.GetStatus();
  if (!setup_paths.Ok()) return setup_paths.GetStatus();
  if (!hold_paths.Ok()) return hold_paths.GetStatus();
  TimingMetrics metrics;
  metrics.setup.wns = std::min(0.0, NormalizeNumericalNoise(setup_wns.Value()));
  metrics.setup.tns = std::min(0.0, NormalizeNumericalNoise(setup_tns.Value()));
  metrics.setup.has_paths = setup_paths.Value() > 0.0;
  metrics.hold.wns = std::min(0.0, NormalizeNumericalNoise(hold_wns.Value()));
  metrics.hold.tns = std::min(0.0, NormalizeNumericalNoise(hold_tns.Value()));
  metrics.hold.has_paths = hold_paths.Value() > 0.0;

  std::istringstream lines{std::string(report)};
  std::string line;
  std::string startpoint;
  std::string endpoint;
  std::string check_type;
  std::string section_check_type;
  while (std::getline(lines, line)) {
    const std::string_view trimmed = Trim(line);
    if (trimmed == "DESIGNPP_SETUP_BEGIN") {
      section_check_type = "setup";
      check_type = section_check_type;
    } else if (trimmed == "DESIGNPP_HOLD_BEGIN") {
      section_check_type = "hold";
      check_type = section_check_type;
    } else if (trimmed.starts_with("Startpoint:")) {
      startpoint = std::string(Trim(trimmed.substr(11)));
    } else if (trimmed.starts_with("Endpoint:")) {
      endpoint = std::string(Trim(trimmed.substr(9)));
      check_type = section_check_type;
      if (trimmed.find("removal check") != std::string_view::npos) {
        check_type = "removal";
      } else if (trimmed.find("recovery check") != std::string_view::npos) {
        check_type = "recovery";
      }
    } else if (trimmed.find("slack") != std::string::npos &&
               trimmed.find("VIOLATED") != std::string::npos) {
      auto slack = FirstNumber(trimmed);
      if (!slack.Ok()) continue;
      metrics.violations.push_back({std::string(corner_name), startpoint,
                                    endpoint, check_type, slack.Value()});
      startpoint.clear();
      endpoint.clear();
    }
  }
  metrics = NormalizeMetrics(std::move(metrics));
  metrics.violations_truncated = metrics.setup.violation_count >= 1000 ||
                                 metrics.hold.violation_count >= 1000;
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

// Copyright 2026 The Design++ Authors

#include "designpp/adapters/openlane2_adapter.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <map>
#include <set>
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

std::string EscapeJson(std::string_view value) {
  std::string result;
  for (char character : value) {
    switch (character) {
      case '\\':
        result += "\\\\";
        break;
      case '"':
        result += "\\\"";
        break;
      case '\n':
        result += "\\n";
        break;
      case '\r':
        result += "\\r";
        break;
      case '\t':
        result += "\\t";
        break;
      default:
        result.push_back(character);
        break;
    }
  }
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

bool IsStepId(std::string_view value) {
  return !value.empty() &&
         std::all_of(value.begin(), value.end(), [](char character) {
           return std::isalnum(static_cast<unsigned char>(character)) ||
                  character == '_' || character == '.' || character == '-';
         });
}

std::optional<double> JsonNumber(std::string_view json, std::string_view key) {
  const std::string token = "\"" + std::string(key) + "\"";
  const std::size_t key_position = json.find(token);
  if (key_position == std::string_view::npos) return std::nullopt;
  const std::size_t colon = json.find(':', key_position + token.size());
  if (colon == std::string_view::npos) return std::nullopt;
  std::size_t begin = colon + 1;
  while (begin < json.size() &&
         std::isspace(static_cast<unsigned char>(json[begin]))) {
    ++begin;
  }
  double number = 0.0;
  const auto parsed =
      std::from_chars(json.data() + begin, json.data() + json.size(), number);
  if (parsed.ec != std::errc() || !std::isfinite(number)) return std::nullopt;
  return number;
}

std::map<std::string, std::string> RawNumbers(std::string_view json) {
  std::map<std::string, std::string> result;
  std::size_t position = 0;
  while ((position = json.find('"', position)) != std::string_view::npos) {
    const std::size_t end = json.find('"', position + 1);
    if (end == std::string_view::npos) break;
    const std::size_t colon = json.find(':', end + 1);
    if (colon == std::string_view::npos) break;
    std::size_t begin = colon + 1;
    while (begin < json.size() &&
           std::isspace(static_cast<unsigned char>(json[begin]))) {
      ++begin;
    }
    std::size_t value_end = begin;
    while (value_end < json.size() && json[value_end] != ',' &&
           json[value_end] != '}' &&
           !std::isspace(static_cast<unsigned char>(json[value_end]))) {
      ++value_end;
    }
    if (value_end > begin) {
      result.emplace(std::string(json.substr(position + 1, end - position - 1)),
                     std::string(json.substr(begin, value_end - begin)));
    }
    position = end + 1;
  }
  return result;
}

std::filesystem::path FindNewest(const std::filesystem::path& root,
                                 const std::set<std::string>& extensions,
                                 const std::set<std::string>& names = {}) {
  std::error_code error;
  std::filesystem::path newest;
  std::filesystem::file_time_type newest_time{};
  for (std::filesystem::recursive_directory_iterator iterator(
           root, std::filesystem::directory_options::skip_permission_denied,
           error),
       end;
       !error && iterator != end; iterator.increment(error)) {
    if (!iterator->is_regular_file(error) || error) continue;
    const std::filesystem::path& path = iterator->path();
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](char character) {
                     return static_cast<char>(
                         std::tolower(static_cast<unsigned char>(character)));
                   });
    if ((!extensions.empty() && !extensions.contains(extension)) ||
        (!names.empty() && !names.contains(path.filename().string()))) {
      continue;
    }
    const auto modified = iterator->last_write_time(error);
    if (!error && (newest.empty() || modified > newest_time)) {
      newest = path;
      newest_time = modified;
    }
  }
  return newest;
}

void AppendJsonString(std::ostringstream* output, std::string_view key,
                      std::string_view value, bool* first) {
  if (!*first) *output << ',';
  *first = false;
  *output << "\n  \"" << key << "\": \"" << EscapeJson(value) << '"';
}

}  // namespace

bool ManagedFlowMetrics::Passed() const noexcept {
  auto positive = [](const std::optional<double>& value) {
    return !value || *value <= 0.0;
  };
  if (!setup_wns || !setup_tns || !hold_wns || !hold_tns ||
      !antenna_violations || !drc_violations || !xor_violations ||
      !lvs_errors) {
    return false;
  }
  return positive(antenna_violations) && positive(slew_violations) &&
         positive(capacitance_violations) && positive(fanout_violations) &&
         positive(drc_violations) && positive(xor_violations) &&
         positive(lvs_errors) && (!setup_wns || *setup_wns >= 0.0) &&
         (!setup_tns || *setup_tns >= 0.0) && (!hold_wns || *hold_wns >= 0.0) &&
         (!hold_tns || *hold_tns >= 0.0);
}

std::string_view OpenLane2Adapter::Name() const noexcept {
  return "OpenLane 2 Classic";
}

runtime::WslCommand OpenLane2Adapter::BuildProbeCommand() const {
  core::ToolchainProfile profile;
  profile.openlane_root = "~/.designpp/toolchains/openlane2";
  return BuildProbeCommand(profile);
}

runtime::WslCommand OpenLane2Adapter::BuildProbeCommand(
    const core::ToolchainProfile& profile) const {
  runtime::WslCommand command;
  command.program = L"/bin/bash";
  command.arguments = {
      L"-lc",
      L"root=\"$1\"; case \"$root\" in '~/'*) "
      L"root=\"$HOME/${root#\\~/}\";; esac; "
      L". /nix/var/nix/profiles/default/etc/profile.d/nix-daemon.sh "
      L"2>/dev/null || true; cd \"$root\"; "
      L"nix-shell shell.nix --run 'openlane --version'",
      L"designpp-openlane-probe", Utf8ToWide(profile.openlane_root)};
  if (!profile.wsl_distribution.empty()) {
    command.distribution = Utf8ToWide(profile.wsl_distribution);
  }
  return command;
}

core::Status OpenLane2Adapter::ValidateAdvancedOverrides(
    std::string_view json) const {
  if (json.size() > 64 * 1024) {
    return {core::ErrorCode::kInvalidArgument,
            "OpenLane advanced overrides exceed 64 KiB", 0};
  }
  json = Trim(json);
  if (json.size() < 2 || json.front() != '{' || json.back() != '}') {
    return {core::ErrorCode::kInvalidArgument,
            "OpenLane advanced overrides must be a JSON object", 0};
  }
  static const std::array<std::string_view, 17> kReserved = {
      "DESIGN_NAME",
      "VERILOG_FILES",
      "VERILOG_INCLUDE_DIRS",
      "VERILOG_DEFINES",
      "SYNTH_PARAMETERS",
      "PDK",
      "STD_CELL_LIBRARY",
      "CLOCK_PORT",
      "CLOCK_PERIOD",
      "PNR_SDC_FILE",
      "SIGNOFF_SDC_FILE",
      "FALLBACK_SDC_FILE",
      "FP_CORE_UTIL",
      "PL_TARGET_DENSITY_PCT",
      "FP_SIZING",
      "DIE_AREA",
      "RUN_TAG"};
  bool in_string = false;
  bool escaped = false;
  int array_depth = 0;
  for (std::size_t index = 1; index + 1 < json.size(); ++index) {
    const char character = json[index];
    if (in_string) {
      if (escaped) {
        escaped = false;
      } else if (character == '\\') {
        escaped = true;
      } else if (character == '"') {
        in_string = false;
      }
      continue;
    }
    if (character == '"') {
      in_string = true;
    } else if (character == '{' || character == '}') {
      return {core::ErrorCode::kInvalidArgument,
              "Nested OpenLane override objects are not allowed", 0};
    } else if (character == '[') {
      ++array_depth;
    } else if (character == ']') {
      --array_depth;
      if (array_depth < 0) {
        return {core::ErrorCode::kInvalidArgument,
                "OpenLane advanced overrides are invalid JSON", 0};
      }
    }
  }
  if (in_string || array_depth != 0) {
    return {core::ErrorCode::kInvalidArgument,
            "OpenLane advanced overrides are invalid JSON", 0};
  }
  for (std::string_view key : kReserved) {
    if (json.find("\"" + std::string(key) + "\"") != std::string_view::npos) {
      return {core::ErrorCode::kInvalidArgument,
              "OpenLane advanced overrides contain a reserved key", 0};
    }
  }
  if (json.find("\"RUN_") != std::string_view::npos) {
    return {core::ErrorCode::kInvalidArgument,
            "OpenLane RUN_* variables are managed by Design++", 0};
  }
  const std::string lower(json);
  if (lower.find("dir::") != std::string::npos ||
      lower.find("/home/") != std::string::npos ||
      lower.find("/mnt/") != std::string::npos ||
      lower.find(":\\") != std::string::npos ||
      lower.find("../") != std::string::npos) {
    return {core::ErrorCode::kInvalidArgument,
            "Path-like OpenLane override values are not allowed", 0};
  }
  return core::Status::Success();
}

core::Status OpenLane2Adapter::Validate(const OpenLaneRequest& request) const {
  if (!IsIdentifier(request.top_module) || request.sources.empty() ||
      request.cpu_threads == 0 || request.backend_workspace.empty() ||
      request.staging_workspace.empty()) {
    return {core::ErrorCode::kInvalidArgument,
            "OpenLane top, sources, workspace, or CPU budget is invalid", 0};
  }
  if (!IsIdentifier(request.configuration.pdk) ||
      !IsIdentifier(request.configuration.standard_cell_library)) {
    return {core::ErrorCode::kInvalidArgument,
            "OpenLane PDK or standard-cell library is invalid", 0};
  }
  if (!request.resume_step.empty() && !IsStepId(request.resume_step)) {
    return {core::ErrorCode::kInvalidArgument,
            "OpenLane resume step is invalid", 0};
  }
  if (!request.resume_step.empty() &&
      (request.checkpoint_hash.size() != 64 ||
       !std::all_of(request.checkpoint_hash.begin(),
                    request.checkpoint_hash.end(), [](char character) {
                      return std::isxdigit(
                          static_cast<unsigned char>(character));
                    }))) {
    return {core::ErrorCode::kInvalidArgument,
            "OpenLane resume checkpoint hash is invalid", 0};
  }
  return ValidateAdvancedOverrides(
      request.configuration.advanced_overrides_json);
}

core::Result<OpenLanePlan> OpenLane2Adapter::BuildPlan(
    const OpenLaneRequest& request) const {
  core::Status status = Validate(request);
  if (!status.Ok()) return status;

  std::ostringstream config;
  config << '{';
  bool first = true;
  AppendJsonString(&config, "DESIGN_NAME", request.top_module, &first);
  AppendJsonString(&config, "PDK", request.configuration.pdk, &first);
  AppendJsonString(&config, "STD_CELL_LIBRARY",
                   request.configuration.standard_cell_library, &first);
  config << ",\n  \"VERILOG_FILES\": [";
  for (std::size_t index = 0; index < request.sources.size(); ++index) {
    if (index != 0) config << ", ";
    config
        << "\"dir::src/"
        << EscapeJson(
               request.sources[index].staged_path.filename().generic_string())
        << "\"";
  }
  config << ']';
  if (!request.include_directories.empty()) {
    config << ",\n  \"VERILOG_INCLUDE_DIRS\": [";
    for (std::size_t index = 0; index < request.include_directories.size();
         ++index) {
      if (index != 0) config << ", ";
      config << "\"dir::include/include_" << index << "\"";
    }
    config << ']';
  }
  if (!request.defines.empty()) {
    config << ",\n  \"VERILOG_DEFINES\": [";
    for (std::size_t index = 0; index < request.defines.size(); ++index) {
      if (index != 0) config << ", ";
      config << '"' << EscapeJson(request.defines[index]) << '"';
    }
    config << ']';
  }
  if (!request.parameters.empty()) {
    config << ",\n  \"SYNTH_PARAMETERS\": [";
    for (std::size_t index = 0; index < request.parameters.size(); ++index) {
      if (index != 0) config << ", ";
      config << '"' << EscapeJson(request.parameters[index]) << '"';
    }
    config << ']';
  }
  if (!request.configuration.clock_ports.empty()) {
    config << ",\n  \"CLOCK_PORT\": [";
    for (std::size_t index = 0;
         index < request.configuration.clock_ports.size(); ++index) {
      if (index != 0) config << ", ";
      config << '"' << EscapeJson(request.configuration.clock_ports[index])
             << '"';
    }
    config << ']';
  }
  config << ",\n  \"CLOCK_PERIOD\": " << request.configuration.clock_period_ns
         << ",\n  \"FP_CORE_UTIL\": "
         << request.configuration.core_utilization_percent;
  if (request.configuration.placement_density_percent) {
    config << ",\n  \"PL_TARGET_DENSITY_PCT\": "
           << *request.configuration.placement_density_percent;
  }
  if (request.configuration.die_area.size() == 4) {
    config << ",\n  \"FP_SIZING\": \"absolute\""
           << ",\n  \"DIE_AREA\": [";
    for (std::size_t index = 0; index < 4; ++index) {
      if (index != 0) config << ", ";
      config << request.configuration.die_area[index];
    }
    config << ']';
  }
  if (!request.pnr_sdc.empty()) {
    AppendJsonString(
        &config, "PNR_SDC_FILE",
        "dir::constraints/" + request.pnr_sdc.filename().generic_string(),
        &first);
  }
  if (!request.signoff_sdc.empty()) {
    AppendJsonString(
        &config, "SIGNOFF_SDC_FILE",
        "dir::constraints/" + request.signoff_sdc.filename().generic_string(),
        &first);
  }
  std::string_view advanced =
      Trim(request.configuration.advanced_overrides_json);
  advanced.remove_prefix(1);
  advanced.remove_suffix(1);
  advanced = Trim(advanced);
  if (!advanced.empty()) config << ",\n  " << advanced;
  config << "\n}\n";

  const std::wstring root = Utf8ToWide(request.profile.openlane_root);
  const std::wstring workspace = Utf8ToWide(request.backend_workspace);
  const std::wstring jobs = std::to_wstring(request.cpu_threads);
  const std::wstring resume = Utf8ToWide(request.resume_step);
  const std::wstring staging = Utf8ToWide(request.staging_workspace);
  const std::wstring checkpoint_hash = Utf8ToWide(request.checkpoint_hash);
  const std::wstring script =
      L"root=\"$1\"; workspace=\"$2\"; jobs=\"$3\"; resume=\"$4\"; "
      L"staging=\"$5\"; expected_hash=\"$6\"; "
      L"case \"$workspace\" in /*) ;; *) workspace=\"$HOME/$workspace\";; "
      L"esac; "
      L"mkdir -p \"$workspace\"; "
      L"cp -R \"$staging\"/. \"$workspace\"/; "
      L"case \"$root\" in '~/'*) root=\"$HOME/${root#\\~/}\";; esac; "
      L". /nix/var/nix/profiles/default/etc/profile.d/nix-daemon.sh "
      L"2>/dev/null || true; cd \"$root\"; "
      L"if [ -n \"$resume\" ]; then "
      L"checkpoint=$(find \"$workspace/runs/designpp\" -type f "
      L"-name state_out.json -printf '%T@ %p\\n' 2>/dev/null | "
      L"sort -nr | head -n1 | cut -d' ' -f2-); "
      L"test -n \"$checkpoint\" || exit 44; "
      L"actual_hash=$(sha256sum \"$checkpoint\" | cut -d' ' -f1); "
      L"test \"$actual_hash\" = \"$expected_hash\" || exit 45; "
      L"selector=\"--last-run --from $resume\"; "
      L"else selector=\"--run-tag designpp\"; fi; "
      L"nix-shell shell.nix --run \"openlane --flow Classic -j $jobs "
      L"$selector $workspace/config.json\"";
  OpenLanePlan plan;
  plan.config_json = config.str();
  plan.execute.program = L"/bin/bash";
  plan.execute.arguments = {L"-lc", script,    L"designpp-openlane",
                            root,   workspace, jobs,
                            resume, staging,   checkpoint_hash};
  if (!request.profile.wsl_distribution.empty()) {
    plan.execute.distribution = Utf8ToWide(request.profile.wsl_distribution);
  }
  plan.validate = plan.execute;
  plan.validate.arguments[1] =
      L"root=\"$1\"; workspace=\"$2\"; staging=\"$5\"; "
      L"case \"$workspace\" in /*) ;; *) workspace=\"$HOME/$workspace\";; "
      L"esac; "
      L"mkdir -p \"$workspace\"; cp -R \"$staging\"/. \"$workspace\"/; "
      L"case \"$root\" in '~/'*) root=\"$HOME/${root#\\~/}\";; esac; "
      L". /nix/var/nix/profiles/default/"
      L"etc/profile.d/nix-daemon.sh 2>/dev/null || true; cd \"$root\"; "
      L"nix-shell shell.nix --run \"openlane --flow Classic --run-tag "
      L"designpp --to "
      L"Verilator.Lint $workspace/config.json\"";
  return plan;
}

core::StageId OpenLane2Adapter::ClassifyStep(std::string_view step_id) const {
  if (step_id.find("Yosys.") != std::string_view::npos) {
    return core::StageId::kSynthesis;
  }
  if (step_id.find("Floorplan") != std::string_view::npos ||
      step_id.find("PDN") != std::string_view::npos) {
    return core::StageId::kFloorplan;
  }
  if (step_id.find("Placement") != std::string_view::npos) {
    return core::StageId::kPlacement;
  }
  if (step_id.find("CTS") != std::string_view::npos) {
    return core::StageId::kClockTreeSynthesis;
  }
  if (step_id.find("Routing") != std::string_view::npos ||
      step_id.find("RCX") != std::string_view::npos ||
      step_id.find("STAPostPNR") != std::string_view::npos) {
    return core::StageId::kRouting;
  }
  if (step_id.find("DRC") != std::string_view::npos ||
      step_id.find("LVS") != std::string_view::npos ||
      step_id.find("StreamOut") != std::string_view::npos ||
      step_id.find("XOR") != std::string_view::npos ||
      step_id.find("EQY") != std::string_view::npos) {
    return core::StageId::kDrcLvs;
  }
  if (step_id.find("Verilator.") != std::string_view::npos ||
      step_id.find("Checker.") != std::string_view::npos) {
    return core::StageId::kRtlInput;
  }
  return core::StageId::kFinalOutputs;
}

std::optional<ManagedFlowProgress> OpenLane2Adapter::ParseProgress(
    std::string_view line) const {
  const std::size_t marker = line.find("Starting step ");
  const std::size_t quoted = line.find("Running '");
  const std::size_t alternative = line.find("Running ");
  std::size_t begin =
      marker == std::string_view::npos
          ? (quoted == std::string_view::npos ? alternative : quoted)
          : marker + std::string_view("Starting step ").size();
  if (begin == std::string_view::npos) return std::nullopt;
  if (marker == std::string_view::npos) {
    begin += quoted == std::string_view::npos
                 ? std::string_view("Running ").size()
                 : std::string_view("Running '").size();
  }
  std::size_t end = begin;
  while (end < line.size() &&
         (std::isalnum(static_cast<unsigned char>(line[end])) ||
          line[end] == '.' || line[end] == '_' || line[end] == '-')) {
    ++end;
  }
  if (end == begin) return std::nullopt;
  ManagedFlowProgress progress;
  progress.step_id = std::string(line.substr(begin, end - begin));
  progress.message = std::string(Trim(line));
  progress.stage = ClassifyStep(progress.step_id);
  return progress;
}

std::vector<core::Diagnostic> OpenLane2Adapter::ParseDiagnostics(
    std::string_view output) const {
  std::vector<core::Diagnostic> diagnostics;
  std::istringstream lines{std::string(output)};
  std::string line;
  while (std::getline(lines, line)) {
    const std::string lower = [&line] {
      std::string value = line;
      std::transform(value.begin(), value.end(), value.begin(), [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      });
      return value;
    }();
    if (lower.find("structuredattrs") != std::string::npos ||
        lower.find("builtins.derivation") != std::string::npos ||
        lower.find("derivation named 'neovim-unwrapped") != std::string::npos) {
      continue;
    }
    const std::string_view trimmed = Trim(lower);
    const bool abc_combinational =
        lower.find("abc: error: the network is combinational") !=
        std::string::npos;
    const bool explicit_error = trimmed.starts_with("error") ||
                                trimmed.starts_with("critical") ||
                                lower.find("] error") != std::string::npos ||
                                lower.find("] critical") != std::string::npos ||
                                lower.find(" error:") != std::string::npos;
    const bool explicit_warning = trimmed.starts_with("warning") ||
                                  lower.find("] warning") != std::string::npos;
    core::DiagnosticSeverity severity;
    if (abc_combinational) {
      severity = core::DiagnosticSeverity::kWarning;
    } else if (explicit_error) {
      severity = core::DiagnosticSeverity::kError;
    } else if (explicit_warning) {
      severity = core::DiagnosticSeverity::kWarning;
    } else {
      continue;
    }
    core::Diagnostic diagnostic;
    diagnostic.severity = severity;
    diagnostic.code = severity == core::DiagnosticSeverity::kError
                          ? "OPENLANE-ERROR"
                          : "OPENLANE-WARNING";
    diagnostic.message = line;
    diagnostics.push_back(std::move(diagnostic));
  }
  return diagnostics;
}

core::Result<ManagedFlowMetrics> OpenLane2Adapter::ParseMetrics(
    std::string_view metrics_json) const {
  if (Trim(metrics_json).empty() ||
      metrics_json.find('{') == std::string_view::npos) {
    return core::Status{core::ErrorCode::kCorruptData,
                        "OpenLane metrics JSON is malformed", 0};
  }
  ManagedFlowMetrics metrics;
  metrics.raw = RawNumbers(metrics_json);
  metrics.core_area = JsonNumber(metrics_json, "design__core__area");
  metrics.die_area = JsonNumber(metrics_json, "design__die__area");
  metrics.utilization =
      JsonNumber(metrics_json, "design__instance__utilization");
  metrics.instance_count = JsonNumber(metrics_json, "design__instance__count");
  metrics.setup_wns = JsonNumber(metrics_json, "timing__setup__wns");
  metrics.setup_tns = JsonNumber(metrics_json, "timing__setup__tns");
  metrics.hold_wns = JsonNumber(metrics_json, "timing__hold__wns");
  metrics.hold_tns = JsonNumber(metrics_json, "timing__hold__tns");
  metrics.worst_setup_skew =
      JsonNumber(metrics_json, "clock__skew__worst_setup");
  metrics.worst_hold_skew = JsonNumber(metrics_json, "clock__skew__worst_hold");
  metrics.wire_length = JsonNumber(metrics_json, "route__wirelength");
  metrics.antenna_violations =
      JsonNumber(metrics_json, "route__antenna_violation__count");
  metrics.slew_violations =
      JsonNumber(metrics_json, "design__max_slew_violation__count");
  metrics.capacitance_violations =
      JsonNumber(metrics_json, "design__max_cap_violation__count");
  metrics.fanout_violations =
      JsonNumber(metrics_json, "design__max_fanout_violation__count");
  metrics.drc_violations = JsonNumber(metrics_json, "magic__drc_error__count");
  metrics.xor_violations =
      JsonNumber(metrics_json, "klayout__xor_error__count");
  if (!metrics.xor_violations) {
    metrics.xor_violations =
        JsonNumber(metrics_json, "design__xor_difference__count");
  }
  metrics.lvs_errors = JsonNumber(metrics_json, "design__lvs_error__count");
  metrics.ir_drop_worst = JsonNumber(metrics_json, "ir__drop__worst");
  return metrics;
}

ManagedFlowArtifactSet OpenLane2Adapter::DiscoverAvailableArtifacts(
    const std::filesystem::path& root) const {
  ManagedFlowArtifactSet artifacts;
  artifacts.resolved_config =
      FindNewest(root, {}, {"resolved.json", "resolved_config.json"});
  artifacts.final_state = FindNewest(root, {}, {"state_out.json"});
  artifacts.metrics_json = FindNewest(root, {}, {"metrics.json"});
  artifacts.metrics_csv = FindNewest(root, {}, {"metrics.csv"});
  artifacts.gds = FindNewest(root, {".gds"});
  artifacts.def = FindNewest(root, {".def"});
  artifacts.lef = FindNewest(root, {".lef"});
  artifacts.odb = FindNewest(root, {".odb"});
  artifacts.gate_netlist = FindNewest(root, {".v"});
  artifacts.power_netlist = FindNewest(root, {}, {"power.v", "pnl.v"});
  artifacts.sdf = FindNewest(root, {".sdf"});
  artifacts.spef = FindNewest(root, {".spef"});
  return artifacts;
}

core::Status OpenLane2Adapter::ValidateFinalArtifacts(
    const ManagedFlowArtifactSet& artifacts) const {
  const std::array<std::filesystem::path, 7> required = {
      artifacts.resolved_config,
      artifacts.final_state,
      artifacts.metrics_json,
      artifacts.gds,
      artifacts.def,
      artifacts.lef,
      artifacts.gate_netlist};
  for (const std::filesystem::path& path : required) {
    std::error_code error;
    if (path.empty() || !std::filesystem::is_regular_file(path, error) ||
        error || std::filesystem::file_size(path, error) == 0 || error) {
      return core::Status{core::ErrorCode::kNotFound,
                          "A required OpenLane final artifact is missing", 0};
    }
  }
  return core::Status::Success();
}

core::Result<ManagedFlowArtifactSet> OpenLane2Adapter::DiscoverArtifacts(
    const std::filesystem::path& root) const {
  ManagedFlowArtifactSet artifacts = DiscoverAvailableArtifacts(root);
  core::Status status = ValidateFinalArtifacts(artifacts);
  if (!status.Ok()) return status;
  return artifacts;
}

}  // namespace designpp::adapters

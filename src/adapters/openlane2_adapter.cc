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

#include "designpp/adapters/toolchain_compatibility_probe.h"

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

struct FlatJsonObject {
  std::map<std::string, std::string> values;
};

class FlatJsonParser final {
 public:
  explicit FlatJsonParser(std::string_view input) : input_(input) {}

  core::Result<FlatJsonObject> Parse() {
    SkipSpace();
    if (!Take('{')) return Error("expected an object");
    FlatJsonObject result;
    SkipSpace();
    if (Take('}')) return result;
    while (position_ < input_.size()) {
      auto key = ParseString();
      if (!key) return Error("expected a string key");
      SkipSpace();
      if (!Take(':')) return Error("expected ':' after key");
      SkipSpace();
      const std::size_t value_begin = position_;
      if (!SkipValue()) return Error("invalid value for key " + *key);
      const std::string value(
          Trim(input_.substr(value_begin, position_ - value_begin)));
      if (!result.values.emplace(*key, value).second) {
        return Error("duplicate key " + *key);
      }
      SkipSpace();
      if (Take('}')) {
        SkipSpace();
        if (position_ != input_.size()) return Error("trailing characters");
        return result;
      }
      if (!Take(',')) return Error("expected ',' between values");
      SkipSpace();
    }
    return Error("unterminated object");
  }

 private:
  core::Status Error(std::string detail) const {
    std::size_t line = 1;
    std::size_t column = 1;
    for (std::size_t index = 0; index < position_ && index < input_.size();
         ++index) {
      if (input_[index] == '\n') {
        ++line;
        column = 1;
      } else {
        ++column;
      }
    }
    return {core::ErrorCode::kInvalidArgument,
            "OpenLane JSON line " + std::to_string(line) + ", column " +
                std::to_string(column) + ": " + detail,
            0};
  }

  void SkipSpace() {
    while (position_ < input_.size() &&
           std::isspace(static_cast<unsigned char>(input_[position_]))) {
      ++position_;
    }
  }

  bool Take(char expected) {
    if (position_ >= input_.size() || input_[position_] != expected) {
      return false;
    }
    ++position_;
    return true;
  }

  std::optional<std::string> ParseString() {
    SkipSpace();
    if (!Take('"')) return std::nullopt;
    std::string value;
    while (position_ < input_.size()) {
      char character = input_[position_++];
      if (character == '"') return value;
      if (static_cast<unsigned char>(character) < 0x20) return std::nullopt;
      if (character != '\\') {
        value.push_back(character);
        continue;
      }
      if (position_ >= input_.size()) return std::nullopt;
      const char escaped = input_[position_++];
      switch (escaped) {
        case '"':
        case '\\':
        case '/':
          value.push_back(escaped);
          break;
        case 'b':
          value.push_back('\b');
          break;
        case 'f':
          value.push_back('\f');
          break;
        case 'n':
          value.push_back('\n');
          break;
        case 'r':
          value.push_back('\r');
          break;
        case 't':
          value.push_back('\t');
          break;
        default:
          return std::nullopt;
      }
    }
    return std::nullopt;
  }

  bool SkipString() { return ParseString().has_value(); }

  bool SkipPrimitive() {
    const std::size_t begin = position_;
    while (position_ < input_.size() && input_[position_] != ',' &&
           input_[position_] != ']' && input_[position_] != '}' &&
           !std::isspace(static_cast<unsigned char>(input_[position_]))) {
      ++position_;
    }
    const std::string_view value = input_.substr(begin, position_ - begin);
    if (value == "true" || value == "false" || value == "null") return true;
    double number = 0.0;
    const auto parsed =
        std::from_chars(value.data(), value.data() + value.size(), number);
    return parsed.ec == std::errc() &&
           parsed.ptr == value.data() + value.size() && std::isfinite(number);
  }

  bool SkipArray() {
    if (!Take('[')) return false;
    SkipSpace();
    if (Take(']')) return true;
    while (position_ < input_.size()) {
      if (input_[position_] == '{' || input_[position_] == '[' ||
          !SkipValue()) {
        return false;
      }
      SkipSpace();
      if (Take(']')) return true;
      if (!Take(',')) return false;
      SkipSpace();
    }
    return false;
  }

  bool SkipValue() {
    SkipSpace();
    if (position_ >= input_.size() || input_[position_] == '{') return false;
    if (input_[position_] == '"') return SkipString();
    if (input_[position_] == '[') return SkipArray();
    return SkipPrimitive();
  }

  std::string_view input_;
  std::size_t position_ = 0;
};

std::string CanonicalObject(const std::map<std::string, std::string>& values) {
  std::ostringstream output;
  output << '{';
  bool first = true;
  for (const auto& [key, value] : values) {
    output << (first ? "\n  " : ",\n  ") << '"' << EscapeJson(key)
           << "\": " << value;
    first = false;
  }
  if (!first) output << '\n';
  output << '}';
  return output.str();
}

std::optional<std::string> DecodeJsonString(std::string_view raw) {
  const std::string source = "{\"v\":" + std::string(raw) + "}";
  FlatJsonParser parser(source);
  auto parsed = parser.Parse();
  if (!parsed.Ok()) return std::nullopt;
  std::string_view value = Trim(parsed.Value().values.at("v"));
  if (value.size() < 2 || value.front() != '"' || value.back() != '"') {
    return std::nullopt;
  }
  // Reuse the parser's escape handling by parsing the string in a small array.
  std::size_t position = 1;
  std::string decoded;
  while (position + 1 < value.size()) {
    char character = value[position++];
    if (character != '\\') {
      decoded.push_back(character);
      continue;
    }
    if (position >= value.size() - 1) return std::nullopt;
    switch (value[position++]) {
      case '"':
        decoded.push_back('"');
        break;
      case '\\':
        decoded.push_back('\\');
        break;
      case '/':
        decoded.push_back('/');
        break;
      case 'b':
        decoded.push_back('\b');
        break;
      case 'f':
        decoded.push_back('\f');
        break;
      case 'n':
        decoded.push_back('\n');
        break;
      case 'r':
        decoded.push_back('\r');
        break;
      case 't':
        decoded.push_back('\t');
        break;
      default:
        return std::nullopt;
    }
  }
  return decoded;
}

std::optional<bool> DecodeJsonBool(std::string_view raw) {
  raw = Trim(raw);
  if (raw == "true") return true;
  if (raw == "false") return false;
  return std::nullopt;
}

std::optional<std::string> DecodeJsonNumber(std::string_view raw) {
  raw = Trim(raw);
  double number = 0.0;
  const auto parsed =
      std::from_chars(raw.data(), raw.data() + raw.size(), number);
  if (parsed.ec != std::errc() || parsed.ptr != raw.data() + raw.size() ||
      !std::isfinite(number)) {
    return std::nullopt;
  }
  return std::string(raw);
}

std::optional<std::vector<std::string>> DecodeScalarArray(std::string_view raw,
                                                          bool allow_numbers) {
  raw = Trim(raw);
  if (raw.size() < 2 || raw.front() != '[' || raw.back() != ']') {
    return std::nullopt;
  }
  std::vector<std::string> result;
  std::size_t position = 1;
  while (position + 1 < raw.size()) {
    while (position + 1 < raw.size() &&
           std::isspace(static_cast<unsigned char>(raw[position]))) {
      ++position;
    }
    if (position + 1 >= raw.size()) break;
    if (raw[position] == '"') {
      const std::size_t begin = position++;
      bool escaped = false;
      while (position < raw.size()) {
        const char character = raw[position++];
        if (!escaped && character == '"') break;
        escaped = !escaped && character == '\\';
        if (character != '\\') escaped = false;
      }
      auto value = DecodeJsonString(raw.substr(begin, position - begin));
      if (!value) return std::nullopt;
      result.push_back(std::move(*value));
    } else {
      if (!allow_numbers) return std::nullopt;
      const std::size_t begin = position;
      while (position < raw.size() && raw[position] != ',' &&
             raw[position] != ']') {
        ++position;
      }
      auto value = DecodeJsonNumber(raw.substr(begin, position - begin));
      if (!value) return std::nullopt;
      result.push_back(std::move(*value));
    }
    while (position + 1 < raw.size() &&
           std::isspace(static_cast<unsigned char>(raw[position]))) {
      ++position;
    }
    if (position + 1 >= raw.size()) break;
    if (raw[position] != ',') return std::nullopt;
    ++position;
  }
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
         positive(routing_violations) && positive(drc_violations) &&
         positive(xor_violations) && positive(lvs_errors) &&
         (!setup_wns || *setup_wns >= 0.0) &&
         (!setup_tns || *setup_tns >= 0.0) && (!hold_wns || *hold_wns >= 0.0) &&
         (!hold_tns || *hold_tns >= 0.0);
}

std::string_view OpenLane2Adapter::Name() const noexcept {
  return "OpenLane 2 Classic";
}

std::vector<ManagedFlowStageInfo> OpenLane2Adapter::Stages() const {
  return {{core::StageId::kRtlInput, "Verilator.Lint", {}, {}},
          {core::StageId::kSynthesis, "Yosys.Synthesis", {}, {}},
          {core::StageId::kFloorplan, "OpenROAD.Floorplan", {}, {}},
          {core::StageId::kPlacement, "OpenROAD.GlobalPlacement", {}, {}},
          {core::StageId::kClockTreeSynthesis, "OpenROAD.CTS", {}, {}},
          {core::StageId::kRouting, "OpenROAD.DetailedRouting", {}, {}},
          {core::StageId::kDrcLvs, "Magic.DRC", {}, {}},
          {core::StageId::kFinalOutputs, "FinalSnapshot", {}, {}}};
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
      L"2>/dev/null || true; cd \"$root\" || exit 44; "
      L"nix-shell shell.nix --run 'openlane --version'",
      L"designpp-openlane-probe", Utf8ToWide(profile.openlane_root)};
  if (!profile.wsl_distribution.empty()) {
    command.distribution = Utf8ToWide(profile.wsl_distribution);
  }
  return WithToolchainCompatibilityEvidence(std::move(command), "openlane2");
}

core::Status OpenLane2Adapter::ValidateAdvancedOverrides(
    std::string_view json) const {
  if (json.size() > 64 * 1024) {
    return {core::ErrorCode::kInvalidArgument,
            "OpenLane advanced overrides exceed 64 KiB", 0};
  }
  auto parsed = FlatJsonParser(json).Parse();
  if (!parsed.Ok()) return parsed.GetStatus();
  static const std::set<std::string_view> kReserved = {"DESIGN_NAME",
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
                                                       "CORE_AREA",
                                                       "FP_TAPCELL_DIST",
                                                       "FP_PDN_MULTILAYER",
                                                       "FP_PDN_CORE_RING",
                                                       "FP_PDN_ENABLE_RAILS",
                                                       "FP_PDN_VWIDTH",
                                                       "FP_PDN_HWIDTH",
                                                       "FP_PDN_VSPACING",
                                                       "FP_PDN_HSPACING",
                                                       "FP_PDN_VPITCH",
                                                       "FP_PDN_HPITCH",
                                                       "FP_PDN_VOFFSET",
                                                       "FP_PDN_HOFFSET",
                                                       "FP_PPL_MODE",
                                                       "FP_IO_MIN_DISTANCE",
                                                       "FP_IO_VLENGTH",
                                                       "FP_IO_HLENGTH",
                                                       "FP_IO_VTHICKNESS_MULT",
                                                       "FP_IO_HTHICKNESS_MULT",
                                                       "FP_IO_VEXTEND",
                                                       "FP_IO_HEXTEND",
                                                       "FP_IO_VLAYER",
                                                       "FP_IO_HLAYER",
                                                       "FP_PIN_ORDER_CFG",
                                                       "ERRORS_ON_UNMATCHED_IO",
                                                       "RUN_TAG"};
  for (const auto& [key, value] : parsed.Value().values) {
    if (kReserved.contains(key)) {
      return {core::ErrorCode::kInvalidArgument,
              "OpenLane advanced overrides contain reserved key " + key, 0};
    }
    if (key.starts_with("RUN_")) {
      return {core::ErrorCode::kInvalidArgument,
              "OpenLane RUN_* variables are managed by Design++", 0};
    }
    std::string lower(value);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](char character) {
                     return static_cast<char>(
                         std::tolower(static_cast<unsigned char>(character)));
                   });
    if (lower.find("dir::") != std::string::npos ||
        lower.find("/home/") != std::string::npos ||
        lower.find("/mnt/") != std::string::npos ||
        lower.find(":\\") != std::string::npos ||
        lower.find("../") != std::string::npos) {
      return {core::ErrorCode::kInvalidArgument,
              "Path-like OpenLane override values are not allowed", 0};
    }
  }
  return core::Status::Success();
}

core::Result<std::string> OpenLane2Adapter::BuildPinOrderConfiguration(
    const core::PhysicalImplementationConfiguration& configuration) const {
  const core::Status validation =
      core::ValidatePhysicalImplementationConfiguration(configuration);
  if (!validation.Ok()) return validation;
  std::ostringstream output;
  const auto write_side = [&output](std::string_view name,
                                    const core::IoPinSideConfiguration& side) {
    if (side.entries.empty()) return;
    output << '#' << name << '\n';
    if (side.minimum_distance_um) {
      output << "@min_distance=" << *side.minimum_distance_um << '\n';
    }
    output << (side.bit_major ? "@bit_major\n" : "@bus_major\n");
    for (const std::string& entry : side.entries) output << entry << '\n';
    output << '\n';
  };
  const auto& io = configuration.io_placement;
  write_side("N", io.north);
  write_side("S", io.south);
  write_side("E", io.east);
  write_side("W", io.west);
  return output.str();
}

core::Result<std::string> OpenLane2Adapter::EncodeEditableConfiguration(
    const core::PhysicalImplementationConfiguration& configuration) const {
  const core::Status validation =
      core::ValidatePhysicalImplementationConfiguration(configuration);
  if (!validation.Ok()) return validation;
  auto advanced = FlatJsonParser(configuration.advanced_overrides_json).Parse();
  if (!advanced.Ok()) return advanced.GetStatus();
  auto values = std::move(advanced).Value().values;
  const auto string_value = [](std::string_view value) {
    return "\"" + EscapeJson(value) + "\"";
  };
  const auto string_array =
      [&string_value](const std::vector<std::string>& items) {
        std::ostringstream output;
        output << '[';
        for (std::size_t index = 0; index < items.size(); ++index) {
          if (index != 0) output << ", ";
          output << string_value(items[index]);
        }
        output << ']';
        return output.str();
      };
  values["PDK"] = string_value(configuration.pdk);
  values["STD_CELL_LIBRARY"] =
      string_value(configuration.standard_cell_library);
  values["CLOCK_PORT"] = string_array(configuration.clock_ports);
  if (!configuration.clock_period_ns.empty()) {
    values["CLOCK_PERIOD"] = configuration.clock_period_ns;
  }
  values["FP_CORE_UTIL"] =
      std::to_string(configuration.core_utilization_percent);
  const auto optional_number =
      [&values](std::string_view key, const std::optional<std::string>& value) {
        if (value) values[std::string(key)] = *value;
      };
  optional_number("PL_TARGET_DENSITY_PCT",
                  configuration.placement_density_percent);
  if (!configuration.die_area.empty()) {
    values["DIE_AREA"] = string_array(configuration.die_area);
  }
  if (!configuration.core_area.empty()) {
    values["CORE_AREA"] = string_array(configuration.core_area);
  }
  optional_number("FP_TAPCELL_DIST", configuration.tap_cell_distance_um);
  const auto& pdn = configuration.power_distribution;
  values["FP_PDN_MULTILAYER"] = pdn.multilayer ? "true" : "false";
  values["FP_PDN_CORE_RING"] = pdn.core_ring ? "true" : "false";
  values["FP_PDN_ENABLE_RAILS"] = pdn.enable_rails ? "true" : "false";
  optional_number("FP_PDN_VWIDTH", pdn.vertical_width_um);
  optional_number("FP_PDN_HWIDTH", pdn.horizontal_width_um);
  optional_number("FP_PDN_VSPACING", pdn.vertical_spacing_um);
  optional_number("FP_PDN_HSPACING", pdn.horizontal_spacing_um);
  optional_number("FP_PDN_VPITCH", pdn.vertical_pitch_um);
  optional_number("FP_PDN_HPITCH", pdn.horizontal_pitch_um);
  optional_number("FP_PDN_VOFFSET", pdn.vertical_offset_um);
  optional_number("FP_PDN_HOFFSET", pdn.horizontal_offset_um);
  const auto& io = configuration.io_placement;
  values["FP_PPL_MODE"] = string_value(io.algorithm);
  values["ERRORS_ON_UNMATCHED_IO"] = string_value(io.unmatched_policy);
  optional_number("FP_IO_MIN_DISTANCE", io.minimum_distance_um);
  optional_number("FP_IO_VLENGTH", io.vertical_length_um);
  optional_number("FP_IO_HLENGTH", io.horizontal_length_um);
  optional_number("FP_IO_VTHICKNESS_MULT", io.vertical_thickness_multiplier);
  optional_number("FP_IO_HTHICKNESS_MULT", io.horizontal_thickness_multiplier);
  optional_number("FP_IO_VEXTEND", io.vertical_extension_um);
  optional_number("FP_IO_HEXTEND", io.horizontal_extension_um);
  if (io.vertical_layer) {
    values["FP_IO_VLAYER"] = string_value(*io.vertical_layer);
  }
  if (io.horizontal_layer) {
    values["FP_IO_HLAYER"] = string_value(*io.horizontal_layer);
  }
  const std::pair<const char*, const char*> inherited[] = {
      {"PDK", "pdk"},
      {"STD_CELL_LIBRARY", "standard_cell_library"},
      {"CLOCK_PERIOD", "clock_period_ns"},
      {"FP_CORE_UTIL", "core_utilization_percent"},
      {"FP_PDN_MULTILAYER", "pdn.multilayer"},
      {"FP_PDN_CORE_RING", "pdn.core_ring"},
      {"FP_PDN_ENABLE_RAILS", "pdn.enable_rails"},
      {"FP_PPL_MODE", "io.algorithm"},
      {"ERRORS_ON_UNMATCHED_IO", "io.unmatched_policy"}};
  for (const auto& [key, field] : inherited) {
    if (core::UsesAutomaticValue(configuration, field)) values.erase(key);
  }
  return CanonicalObject(values);
}

core::Result<core::PhysicalImplementationConfiguration>
OpenLane2Adapter::ApplyEditableConfiguration(
    std::string_view json,
    const core::PhysicalImplementationConfiguration& current) const {
  if (json.size() > 64 * 1024) {
    return core::Status{core::ErrorCode::kInvalidArgument,
                        "OpenLane editable JSON exceeds 64 KiB", 0};
  }
  auto parsed = FlatJsonParser(json).Parse();
  if (!parsed.Ok()) return parsed.GetStatus();
  auto values = std::move(parsed).Value().values;
  // The editable snapshot may be copied from a resolved OpenLane config.
  // Ignore this run-owned path without reading it or persisting it; Design++
  // deterministically regenerates the file from structured I/O settings.
  values.erase("FP_PIN_ORDER_CFG");
  static const std::set<std::string_view> kManaged = {"DESIGN_NAME",
                                                      "VERILOG_FILES",
                                                      "VERILOG_INCLUDE_DIRS",
                                                      "VERILOG_DEFINES",
                                                      "SYNTH_PARAMETERS",
                                                      "PNR_SDC_FILE",
                                                      "SIGNOFF_SDC_FILE",
                                                      "FALLBACK_SDC_FILE",
                                                      "FP_PIN_ORDER_CFG",
                                                      "FP_SIZING",
                                                      "RUN_TAG"};
  for (const auto& [key, value] : values) {
    if (kManaged.contains(key) || key.starts_with("RUN_")) {
      return core::Status{core::ErrorCode::kInvalidArgument,
                          "OpenLane key " + key + " is managed by Design++", 0};
    }
  }
  core::PhysicalImplementationConfiguration result = current;
  const auto defaults_json = EncodeEditableConfiguration({});
  if (!defaults_json.Ok()) return defaults_json.GetStatus();
  auto defaults = FlatJsonParser(defaults_json.Value()).Parse();
  if (!defaults.Ok()) return defaults.GetStatus();
  const std::pair<const char*, const char*> inherited[] = {
      {"PDK", "pdk"},
      {"STD_CELL_LIBRARY", "standard_cell_library"},
      {"CLOCK_PERIOD", "clock_period_ns"},
      {"FP_CORE_UTIL", "core_utilization_percent"},
      {"FP_PDN_MULTILAYER", "pdn.multilayer"},
      {"FP_PDN_CORE_RING", "pdn.core_ring"},
      {"FP_PDN_ENABLE_RAILS", "pdn.enable_rails"},
      {"FP_PPL_MODE", "io.algorithm"},
      {"ERRORS_ON_UNMATCHED_IO", "io.unmatched_policy"}};
  for (const auto& [key, field] : inherited) {
    std::erase(result.automatic_fields, std::string(field));
    const auto found = values.find(key);
    if (found == values.end() || found->second == "null" ||
        found->second == "\"\"") {
      result.automatic_fields.emplace_back(field);
      values[key] = defaults.Value().values.at(key);
    }
  }
  std::sort(result.automatic_fields.begin(), result.automatic_fields.end());
  const auto take_string = [&values](std::string_view key, std::string* output,
                                     bool required) {
    const auto found = values.find(std::string(key));
    if (found == values.end()) return !required;
    auto value = DecodeJsonString(found->second);
    if (!value) return false;
    *output = std::move(*value);
    values.erase(found);
    return true;
  };
  const auto take_number = [&values](std::string_view key,
                                     std::optional<std::string>* output) {
    const auto found = values.find(std::string(key));
    if (found == values.end()) {
      output->reset();
      return true;
    }
    auto value = DecodeJsonNumber(found->second);
    if (found->second == "null" || found->second == "\"\"") {
      output->reset();
      values.erase(found);
      return true;
    }
    if (!value) return false;
    *output = std::move(*value);
    values.erase(found);
    return true;
  };
  const auto take_bool = [&values](std::string_view key, bool* output) {
    const auto found = values.find(std::string(key));
    if (found == values.end()) return false;
    auto value = DecodeJsonBool(found->second);
    if (!value) return false;
    *output = *value;
    values.erase(found);
    return true;
  };
  const auto take_array = [&values](std::string_view key,
                                    std::vector<std::string>* output,
                                    bool allow_numbers) {
    const auto found = values.find(std::string(key));
    if (found == values.end()) {
      output->clear();
      return true;
    }
    auto decoded = DecodeScalarArray(found->second, allow_numbers);
    if (!decoded) return false;
    *output = std::move(*decoded);
    values.erase(found);
    return true;
  };
  std::string utilization;
  if (!take_string("PDK", &result.pdk, true)) {
    return core::Status{core::ErrorCode::kInvalidArgument,
                        "PDK must be a string", 0};
  }
  if (!take_string("STD_CELL_LIBRARY", &result.standard_cell_library, true)) {
    return core::Status{core::ErrorCode::kInvalidArgument,
                        "STD_CELL_LIBRARY must be a string", 0};
  }
  if (!take_array("CLOCK_PORT", &result.clock_ports, false)) {
    return core::Status{core::ErrorCode::kInvalidArgument,
                        "CLOCK_PORT must be a flat string array", 0};
  }
  const auto required_number = [&values](std::string_view key,
                                         std::string* output) {
    const auto found = values.find(std::string(key));
    if (found == values.end()) return false;
    auto value = DecodeJsonNumber(found->second);
    if (!value) return false;
    *output = std::move(*value);
    values.erase(found);
    return true;
  };
  const auto period = values.find("CLOCK_PERIOD");
  if (period == values.end()) {
    result.clock_period_ns.clear();
  } else {
    auto decoded = DecodeJsonNumber(period->second);
    if (!decoded) {
      return core::Status{core::ErrorCode::kInvalidArgument,
                          "CLOCK_PERIOD must be a number", 0};
    }
    result.clock_period_ns = std::move(*decoded);
    values.erase(period);
  }
  if (!required_number("FP_CORE_UTIL", &utilization)) {
    return core::Status{core::ErrorCode::kInvalidArgument,
                        "FP_CORE_UTIL must be a number", 0};
  }
  char* utilization_end = nullptr;
  const unsigned long parsed_utilization =
      std::strtoul(utilization.c_str(), &utilization_end, 10);
  if (utilization_end != utilization.c_str() + utilization.size()) {
    return core::Status{core::ErrorCode::kInvalidArgument,
                        "FP_CORE_UTIL must be an integer", 0};
  }
  result.core_utilization_percent =
      static_cast<std::uint32_t>(parsed_utilization);
  if (!take_number("PL_TARGET_DENSITY_PCT",
                   &result.placement_density_percent) ||
      !take_array("DIE_AREA", &result.die_area, true) ||
      !take_array("CORE_AREA", &result.core_area, true) ||
      !take_number("FP_TAPCELL_DIST", &result.tap_cell_distance_um)) {
    return core::Status{core::ErrorCode::kInvalidArgument,
                        "OpenLane floorplan variables have wrong types", 0};
  }
  auto& pdn = result.power_distribution;
  if (!take_bool("FP_PDN_MULTILAYER", &pdn.multilayer) ||
      !take_bool("FP_PDN_CORE_RING", &pdn.core_ring) ||
      !take_bool("FP_PDN_ENABLE_RAILS", &pdn.enable_rails) ||
      !take_number("FP_PDN_VWIDTH", &pdn.vertical_width_um) ||
      !take_number("FP_PDN_HWIDTH", &pdn.horizontal_width_um) ||
      !take_number("FP_PDN_VSPACING", &pdn.vertical_spacing_um) ||
      !take_number("FP_PDN_HSPACING", &pdn.horizontal_spacing_um) ||
      !take_number("FP_PDN_VPITCH", &pdn.vertical_pitch_um) ||
      !take_number("FP_PDN_HPITCH", &pdn.horizontal_pitch_um) ||
      !take_number("FP_PDN_VOFFSET", &pdn.vertical_offset_um) ||
      !take_number("FP_PDN_HOFFSET", &pdn.horizontal_offset_um)) {
    return core::Status{core::ErrorCode::kInvalidArgument,
                        "OpenLane PDN variables have wrong types", 0};
  }
  auto& io = result.io_placement;
  if (!take_string("FP_PPL_MODE", &io.algorithm, true) ||
      !take_string("ERRORS_ON_UNMATCHED_IO", &io.unmatched_policy, true) ||
      !take_number("FP_IO_MIN_DISTANCE", &io.minimum_distance_um) ||
      !take_number("FP_IO_VLENGTH", &io.vertical_length_um) ||
      !take_number("FP_IO_HLENGTH", &io.horizontal_length_um) ||
      !take_number("FP_IO_VTHICKNESS_MULT",
                   &io.vertical_thickness_multiplier) ||
      !take_number("FP_IO_HTHICKNESS_MULT",
                   &io.horizontal_thickness_multiplier) ||
      !take_number("FP_IO_VEXTEND", &io.vertical_extension_um) ||
      !take_number("FP_IO_HEXTEND", &io.horizontal_extension_um)) {
    return core::Status{core::ErrorCode::kInvalidArgument,
                        "OpenLane I/O variables have wrong types", 0};
  }
  io.vertical_layer.reset();
  io.horizontal_layer.reset();
  std::string layer;
  if (!take_string("FP_IO_VLAYER", &layer, false)) {
    return core::Status{core::ErrorCode::kInvalidArgument,
                        "FP_IO_VLAYER must be a string", 0};
  }
  if (!layer.empty()) io.vertical_layer = layer;
  layer.clear();
  if (!take_string("FP_IO_HLAYER", &layer, false)) {
    return core::Status{core::ErrorCode::kInvalidArgument,
                        "FP_IO_HLAYER must be a string", 0};
  }
  if (!layer.empty()) io.horizontal_layer = layer;
  result.advanced_overrides_json = CanonicalObject(values);
  core::Status validation =
      ValidateAdvancedOverrides(result.advanced_overrides_json);
  if (validation.Ok()) {
    validation = core::ValidatePhysicalImplementationConfiguration(result);
  }
  return validation.Ok()
             ? core::Result<core::PhysicalImplementationConfiguration>(
                   std::move(result))
             : core::Result<core::PhysicalImplementationConfiguration>(
                   std::move(validation));
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
  core::Status status =
      core::ValidatePhysicalImplementationConfiguration(request.configuration);
  return status.Ok() ? ValidateAdvancedOverrides(
                           request.configuration.advanced_overrides_json)
                     : status;
}

core::Result<OpenLanePlan> OpenLane2Adapter::BuildPlan(
    const OpenLaneRequest& input) const {
  auto request = input;
  request.configuration = core::ResolvePhysicalDefaults(input.configuration);
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
  if (!request.configuration.clock_period_ns.empty()) {
    config << ",\n  \"CLOCK_PERIOD\": "
           << request.configuration.clock_period_ns;
  }
  config << ",\n  \"FP_CORE_UTIL\": "
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
  if (request.configuration.core_area.size() == 4) {
    config << ",\n  \"CORE_AREA\": [";
    for (std::size_t index = 0; index < 4; ++index) {
      if (index != 0) config << ", ";
      config << request.configuration.core_area[index];
    }
    config << ']';
  }
  if (request.configuration.tap_cell_distance_um) {
    config << ",\n  \"FP_TAPCELL_DIST\": "
           << *request.configuration.tap_cell_distance_um;
  }
  const core::IoPlacementConfiguration& io = request.configuration.io_placement;
  config << ",\n  \"FP_PPL_MODE\": \"" << EscapeJson(io.algorithm) << '"'
         << ",\n  \"ERRORS_ON_UNMATCHED_IO\": \""
         << EscapeJson(io.unmatched_policy) << '"';
  const auto append_io_decimal = [&config](
                                     std::string_view name,
                                     const std::optional<std::string>& value) {
    if (value) config << ",\n  \"" << name << "\": " << *value;
  };
  append_io_decimal("FP_IO_MIN_DISTANCE", io.minimum_distance_um);
  append_io_decimal("FP_IO_VLENGTH", io.vertical_length_um);
  append_io_decimal("FP_IO_HLENGTH", io.horizontal_length_um);
  append_io_decimal("FP_IO_VTHICKNESS_MULT", io.vertical_thickness_multiplier);
  append_io_decimal("FP_IO_HTHICKNESS_MULT",
                    io.horizontal_thickness_multiplier);
  append_io_decimal("FP_IO_VEXTEND", io.vertical_extension_um);
  append_io_decimal("FP_IO_HEXTEND", io.horizontal_extension_um);
  if (io.vertical_layer) {
    config << ",\n  \"FP_IO_VLAYER\": \"" << EscapeJson(*io.vertical_layer)
           << '"';
  }
  if (io.horizontal_layer) {
    config << ",\n  \"FP_IO_HLAYER\": \"" << EscapeJson(*io.horizontal_layer)
           << '"';
  }
  if (!request.pin_order_cfg.empty()) {
    config << ",\n  \"FP_PIN_ORDER_CFG\": \"dir::constraints/"
           << EscapeJson(request.pin_order_cfg.filename().generic_string())
           << '"';
  }
  const core::PowerDistributionConfiguration& pdn =
      request.configuration.power_distribution;
  if (!core::UsesAutomaticValue(request.configuration, "pdn.multilayer"))
    config << ",\n  \"FP_PDN_MULTILAYER\": "
           << (pdn.multilayer ? "true" : "false");
  if (!core::UsesAutomaticValue(request.configuration, "pdn.core_ring"))
    config << ",\n  \"FP_PDN_CORE_RING\": "
           << (pdn.core_ring ? "true" : "false");
  if (!core::UsesAutomaticValue(request.configuration, "pdn.enable_rails"))
    config << ",\n  \"FP_PDN_ENABLE_RAILS\": "
           << (pdn.enable_rails ? "true" : "false");
  const auto append_pdn_decimal = [&config](
                                      std::string_view name,
                                      const std::optional<std::string>& value) {
    if (value) config << ",\n  \"" << name << "\": " << *value;
  };
  append_pdn_decimal("FP_PDN_VWIDTH", pdn.vertical_width_um);
  append_pdn_decimal("FP_PDN_HWIDTH", pdn.horizontal_width_um);
  append_pdn_decimal("FP_PDN_VSPACING", pdn.vertical_spacing_um);
  append_pdn_decimal("FP_PDN_HSPACING", pdn.horizontal_spacing_um);
  append_pdn_decimal("FP_PDN_VPITCH", pdn.vertical_pitch_um);
  append_pdn_decimal("FP_PDN_HPITCH", pdn.horizontal_pitch_um);
  append_pdn_decimal("FP_PDN_VOFFSET", pdn.vertical_offset_um);
  append_pdn_decimal("FP_PDN_HOFFSET", pdn.horizontal_offset_um);
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
    const bool pdn_too_small = lower.find("pdn-0185") != std::string::npos;
    diagnostic.code = pdn_too_small ? "OPENLANE-PDN-TOO-SMALL"
                      : severity == core::DiagnosticSeverity::kError
                          ? "OPENLANE-ERROR"
                          : "OPENLANE-WARNING";
    diagnostic.message =
        pdn_too_small
            ? line +
                  " Set Layout Setup > Die area to a larger absolute area "
                  "(for example 0,0,100,100 for a tiny sky130 design)."
            : line;
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
  metrics.global_route_congestion =
      JsonNumber(metrics_json, "route__congestion__global");
  if (!metrics.global_route_congestion) {
    metrics.global_route_congestion =
        JsonNumber(metrics_json, "route__congestion__global__total");
  }
  metrics.detailed_route_congestion =
      JsonNumber(metrics_json, "route__congestion__detailed");
  if (!metrics.detailed_route_congestion) {
    metrics.detailed_route_congestion =
        JsonNumber(metrics_json, "route__congestion__detailed__total");
  }
  metrics.global_route_overflow =
      JsonNumber(metrics_json, "route__overflow__global");
  if (!metrics.global_route_overflow) {
    metrics.global_route_overflow =
        JsonNumber(metrics_json, "route__congestion__global__overflow");
  }
  metrics.detailed_route_overflow =
      JsonNumber(metrics_json, "route__overflow__detailed");
  if (!metrics.detailed_route_overflow) {
    metrics.detailed_route_overflow =
        JsonNumber(metrics_json, "route__congestion__detailed__overflow");
  }
  metrics.routing_violations =
      JsonNumber(metrics_json, "route__violation__count");
  metrics.runtime_seconds = JsonNumber(metrics_json, "flow__runtime_sec");
  metrics.peak_memory_mb = JsonNumber(metrics_json, "flow__peak_memory_mb");
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

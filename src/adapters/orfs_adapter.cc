// Copyright 2026 The Design++ Authors

#include "designpp/adapters/orfs_adapter.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "designpp/adapters/orfs_flake_inputs.h"
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

std::string EscapeMake(std::string_view value) {
  std::string result;
  for (char character : value) {
    if (character == '\\' || character == '#' || character == '$' ||
        character == '=' || character == ' ' || character == '\t') {
      result.push_back('\\');
    }
    if (character == '\r' || character == '\n') {
      result.push_back(' ');
    } else {
      result.push_back(character);
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
           character == '_' || character == '.' || character == '-';
  });
}

bool IsSafeMakeValue(std::string_view value) {
  return value.find('\0') == std::string_view::npos &&
         value.find('\r') == std::string_view::npos &&
         value.find('\n') == std::string_view::npos &&
         value.find('$') == std::string_view::npos &&
         value.find('#') == std::string_view::npos &&
         value.find('=') == std::string_view::npos &&
         value.find(';') == std::string_view::npos &&
         value.find('|') == std::string_view::npos &&
         value.find('&') == std::string_view::npos &&
         value.find('`') == std::string_view::npos &&
         value.find("../") == std::string_view::npos &&
         value.find("/mnt/") == std::string_view::npos &&
         value.find("/home/") == std::string_view::npos &&
         value.find("\\") == std::string_view::npos;
}

bool IsPathLikeOverrideValue(std::string_view value) {
  return value.find('/') != std::string_view::npos ||
         value.find('\\') != std::string_view::npos ||
         value.find(':') != std::string_view::npos ||
         value.find("..") != std::string_view::npos || value.starts_with("~") ||
         value.starts_with(".");
}

bool IsManagedOrfsVariable(std::string_view key) {
  constexpr std::string_view kManaged[] = {"DESIGN_CONFIG",
                                           "DESIGN_NAME",
                                           "DESIGN_NICKNAME",
                                           "VERILOG_FILES",
                                           "VERILOG_INCLUDE_DIRS",
                                           "VERILOG_DEFINES",
                                           "SYNTH_PARAMETERS",
                                           "SDC_FILE",
                                           "PLATFORM",
                                           "FLOW_VARIANT",
                                           "WORK_HOME",
                                           "RESULTS_DIR",
                                           "LOG_DIR",
                                           "REPORTS_DIR",
                                           "OBJECTS_DIR",
                                           "IO_CONSTRAINTS",
                                           "PDN_TCL",
                                           "TAPCELL_TCL",
                                           "PLACE_PINS_ARGS",
                                           "FP_PIN_ORDER_CFG",
                                           "RUN_"};
  for (std::string_view managed : kManaged) {
    if (managed == "RUN_" ? key.starts_with(managed) : key == managed) {
      return true;
    }
  }
  // ORFS uses many *_FILE/*_DIR variables for generated outputs and scripts.
  // They are intentionally not exposed through the flat Make-variable JSON.
  // Tcl snippets that are ordinary scalar variables (for example
  // FASTROUTE_TCL) remain valid advanced values; only the explicitly managed
  // Tcl paths above are blocked.
  return key.find("FILE") != std::string_view::npos ||
         key.find("DIR") != std::string_view::npos;
}

bool IsSafeStagedPath(std::string_view value) {
  return !value.empty() && value.find('\0') == std::string::npos &&
         value.find('\n') == std::string::npos &&
         value.find('\r') == std::string::npos &&
         value.find('"') == std::string::npos &&
         value.find('\'') == std::string::npos &&
         value.find("..") == std::string::npos;
}

bool IsSha256(std::string_view value) {
  if (value.empty()) return true;
  if (value.size() != 64) return false;
  return std::all_of(value.begin(), value.end(), [](char character) {
    return std::isxdigit(static_cast<unsigned char>(character)) != 0;
  });
}

std::optional<std::string> NanosecondsToPicoseconds(std::string_view value) {
  double nanoseconds = 0.0;
  const auto parsed =
      std::from_chars(value.data(), value.data() + value.size(), nanoseconds);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
      !std::isfinite(nanoseconds) || nanoseconds <= 0.0) {
    return std::nullopt;
  }
  const double picoseconds = nanoseconds * 1000.0;
  std::array<char, 64> buffer{};
  const auto written =
      std::to_chars(buffer.data(), buffer.data() + buffer.size(), picoseconds,
                    std::chars_format::general, 15);
  if (written.ec != std::errc{}) return std::nullopt;
  return std::string(buffer.data(), written.ptr);
}

struct JsonEntry {
  std::string key;
  std::string raw_value;
};

class FlatJsonParser final {
 public:
  explicit FlatJsonParser(std::string_view input) : input_(input) {}

  core::Result<std::vector<JsonEntry>> Parse() {
    SkipSpace();
    if (!Take('{')) return Error("expected an object");
    SkipSpace();
    std::vector<JsonEntry> entries;
    std::set<std::string> keys;
    if (Take('}')) {
      SkipSpace();
      return position_ == input_.size()
                 ? core::Result<std::vector<JsonEntry>>(std::move(entries))
                 : Error("trailing characters");
    }
    while (position_ < input_.size()) {
      auto key = ParseString();
      if (!key || !IsIdentifier(*key)) return Error("invalid variable key");
      SkipSpace();
      if (!Take(':')) return Error("expected ':' after key");
      SkipSpace();
      const std::size_t begin = position_;
      if (!SkipValue()) return Error("invalid value for " + *key);
      const std::string raw(Trim(input_.substr(begin, position_ - begin)));
      if (!keys.insert(*key).second) return Error("duplicate key " + *key);
      if (raw.find('{') != std::string::npos ||
          raw.find('[') != std::string::npos) {
        if (raw.front() != '[' || raw.back() != ']' ||
            raw.find('[', 1) != std::string::npos) {
          return Error("nested values are not allowed for " + *key);
        }
      }
      if (!IsSafeMakeValue(raw)) {
        return Error("unsafe Make value for " + *key);
      }
      if (IsPathLikeOverrideValue(raw)) {
        return Error("path-like Make value for " + *key);
      }
      entries.push_back({std::move(*key), raw});
      SkipSpace();
      if (Take('}')) {
        SkipSpace();
        return position_ == input_.size()
                   ? core::Result<std::vector<JsonEntry>>(std::move(entries))
                   : Error("trailing characters");
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
            "ORFS variables JSON line " + std::to_string(line) + ", column " +
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
    if (!Take('"')) return std::nullopt;
    std::string value;
    while (position_ < input_.size()) {
      const char character = input_[position_++];
      if (character == '"') return value;
      if (static_cast<unsigned char>(character) < 0x20) return std::nullopt;
      if (character != '\\') {
        value.push_back(character);
        continue;
      }
      if (position_ >= input_.size()) return std::nullopt;
      const char escaped = input_[position_++];
      if (escaped != '"' && escaped != '\\' && escaped != '/') {
        return std::nullopt;
      }
      value.push_back(escaped);
    }
    return std::nullopt;
  }

  bool SkipString() { return ParseString().has_value(); }

  bool SkipPrimitive() {
    const std::size_t begin = position_;
    while (position_ < input_.size() && input_[position_] != ',' &&
           input_[position_] != '}' &&
           !std::isspace(static_cast<unsigned char>(input_[position_]))) {
      ++position_;
    }
    const std::string_view value = input_.substr(begin, position_ - begin);
    if (value == "true" || value == "false" || value == "null") return true;
    double number = 0.0;
    const auto parsed =
        std::from_chars(value.data(), value.data() + value.size(), number);
    return parsed.ec == std::errc{} &&
           parsed.ptr == value.data() + value.size() && std::isfinite(number);
  }

  bool SkipArray() {
    if (!Take('[')) return false;
    SkipSpace();
    if (Take(']')) return true;
    while (position_ < input_.size()) {
      if (input_[position_] == '{' || input_[position_] == '[') return false;
      if (input_[position_] == '"') {
        if (!SkipString()) return false;
      } else if (!SkipPrimitive()) {
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
    if (position_ >= input_.size()) return false;
    if (input_[position_] == '"') return SkipString();
    if (input_[position_] == '[') return SkipArray();
    if (input_[position_] == '{') return false;
    return SkipPrimitive();
  }

  std::string_view input_;
  std::size_t position_ = 0;
};

std::optional<std::string> DecodeString(std::string_view raw) {
  raw = Trim(raw);
  if (raw.size() < 2 || raw.front() != '"' || raw.back() != '"') {
    return std::nullopt;
  }
  std::string result;
  for (std::size_t index = 1; index + 1 < raw.size(); ++index) {
    if (raw[index] != '\\') {
      result.push_back(raw[index]);
      continue;
    }
    if (++index + 1 > raw.size()) return std::nullopt;
    const char escaped = raw[index];
    if (escaped != '"' && escaped != '\\' && escaped != '/') {
      return std::nullopt;
    }
    result.push_back(escaped);
  }
  return result;
}

std::string EscapeJson(std::string_view value) {
  std::string result;
  for (char character : value) {
    if (character == '\\' || character == '"') result.push_back('\\');
    if (character == '\n') {
      result += "\\n";
    } else if (character != '\r') {
      result.push_back(character);
    }
  }
  return result;
}

std::string LinuxJoin(std::string_view root, std::string_view child) {
  if (root.empty()) return std::string(child);
  if (root.back() == '/') return std::string(root) + std::string(child);
  return std::string(root) + "/" + std::string(child);
}

std::string StageName(core::StageId stage) {
  switch (stage) {
    case core::StageId::kSynthesis:
      return "synth";
    case core::StageId::kFloorplan:
      return "floorplan";
    case core::StageId::kPlacement:
      return "place";
    case core::StageId::kClockTreeSynthesis:
      return "cts";
    case core::StageId::kRouting:
      return "route";
    case core::StageId::kFinalOutputs:
      return "finish";
    default:
      return {};
  }
}

std::string GuiStageName(core::StageId stage) {
  switch (stage) {
    case core::StageId::kSynthesis:
      return "gui_synth";
    case core::StageId::kFloorplan:
      return "gui_floorplan";
    case core::StageId::kPlacement:
      return "gui_place";
    case core::StageId::kClockTreeSynthesis:
      return "gui_cts";
    case core::StageId::kRouting:
      return "gui_route";
    case core::StageId::kFinalOutputs:
      return "gui_final";
    default:
      return {};
  }
}

class MetricsJsonParser final {
 public:
  explicit MetricsJsonParser(std::string_view input) : input_(input) {}

  bool Parse(std::vector<std::pair<std::string, std::string>>* entries) {
    SkipSpace();
    if (position_ >= input_.size() || input_[position_] != '{' ||
        !ParseObject(entries)) {
      return false;
    }
    SkipSpace();
    return position_ == input_.size();
  }

 private:
  void SkipSpace() {
    while (position_ < input_.size() &&
           std::isspace(static_cast<unsigned char>(input_[position_]))) {
      ++position_;
    }
  }

  bool Take(char expected) {
    SkipSpace();
    if (position_ >= input_.size() || input_[position_] != expected) {
      return false;
    }
    ++position_;
    return true;
  }

  bool ParseString(std::string* value) {
    if (!Take('"')) return false;
    value->clear();
    while (position_ < input_.size()) {
      const char character = input_[position_++];
      if (character == '"') return true;
      if (static_cast<unsigned char>(character) < 0x20) return false;
      if (character != '\\') {
        value->push_back(character);
        continue;
      }
      if (position_ >= input_.size()) return false;
      const char escaped = input_[position_++];
      if (escaped == 'u') {
        if (position_ + 4 > input_.size()) return false;
        for (int index = 0; index < 4; ++index) {
          if (!std::isxdigit(static_cast<unsigned char>(
                  input_[position_ + static_cast<std::size_t>(index)]))) {
            return false;
          }
        }
        position_ += 4;
      } else if (escaped != '"' && escaped != '\\' && escaped != '/' &&
                 escaped != 'b' && escaped != 'f' && escaped != 'n' &&
                 escaped != 'r' && escaped != 't') {
        return false;
      }
    }
    return false;
  }

  bool ParseNumber(std::string* raw) {
    SkipSpace();
    const std::size_t begin = position_;
    if (position_ < input_.size() && input_[position_] == '-') ++position_;
    if (position_ >= input_.size()) return false;
    if (input_[position_] == '0') {
      ++position_;
    } else {
      if (!std::isdigit(static_cast<unsigned char>(input_[position_]))) {
        return false;
      }
      while (position_ < input_.size() &&
             std::isdigit(static_cast<unsigned char>(input_[position_]))) {
        ++position_;
      }
    }
    if (position_ < input_.size() && input_[position_] == '.') {
      ++position_;
      const std::size_t fraction_begin = position_;
      while (position_ < input_.size() &&
             std::isdigit(static_cast<unsigned char>(input_[position_]))) {
        ++position_;
      }
      if (fraction_begin == position_) return false;
    }
    if (position_ < input_.size() &&
        (input_[position_] == 'e' || input_[position_] == 'E')) {
      ++position_;
      if (position_ < input_.size() &&
          (input_[position_] == '+' || input_[position_] == '-')) {
        ++position_;
      }
      const std::size_t exponent_begin = position_;
      while (position_ < input_.size() &&
             std::isdigit(static_cast<unsigned char>(input_[position_]))) {
        ++position_;
      }
      if (exponent_begin == position_) return false;
    }
    *raw = std::string(input_.substr(begin, position_ - begin));
    double value = 0.0;
    const auto parsed =
        std::from_chars(raw->data(), raw->data() + raw->size(), value);
    return parsed.ec == std::errc{} &&
           parsed.ptr == raw->data() + raw->size() && std::isfinite(value);
  }

  bool ParseLiteral(std::string_view literal) {
    SkipSpace();
    if (input_.substr(position_, literal.size()) != literal) return false;
    position_ += literal.size();
    return true;
  }

  bool ParseArray(std::vector<std::pair<std::string, std::string>>* entries) {
    if (!Take('[')) return false;
    SkipSpace();
    if (Take(']')) return true;
    for (;;) {
      if (!ParseValue({}, entries)) return false;
      SkipSpace();
      if (Take(']')) return true;
      if (!Take(',')) return false;
    }
  }

  bool ParseObject(std::vector<std::pair<std::string, std::string>>* entries) {
    if (!Take('{')) return false;
    SkipSpace();
    if (Take('}')) return true;
    for (;;) {
      std::string key;
      if (!ParseString(&key) || !Take(':') || !ParseValue(key, entries)) {
        return false;
      }
      SkipSpace();
      if (Take('}')) return true;
      if (!Take(',')) return false;
    }
  }

  bool ParseValue(std::string_view key,
                  std::vector<std::pair<std::string, std::string>>* entries) {
    SkipSpace();
    if (position_ >= input_.size()) return false;
    if (input_[position_] == '{') return ParseObject(entries);
    if (input_[position_] == '[') return ParseArray(entries);
    if (input_[position_] == '"') {
      const std::size_t begin = position_;
      std::string ignored;
      if (!ParseString(&ignored)) return false;
      if (!key.empty()) {
        entries->emplace_back(
            key, std::string(input_.substr(begin, position_ - begin)));
      }
      return true;
    }
    if (input_.substr(position_, 4) == "true") {
      if (!ParseLiteral("true")) return false;
      if (!key.empty()) entries->emplace_back(key, "true");
      return true;
    }
    if (input_.substr(position_, 5) == "false") {
      if (!ParseLiteral("false")) return false;
      if (!key.empty()) entries->emplace_back(key, "false");
      return true;
    }
    if (input_.substr(position_, 4) == "null") {
      if (!ParseLiteral("null")) return false;
      if (!key.empty()) entries->emplace_back(key, "null");
      return true;
    }
    std::string raw;
    if (!ParseNumber(&raw)) return false;
    if (!key.empty()) entries->emplace_back(key, std::move(raw));
    return true;
  }

  std::string_view input_;
  std::size_t position_ = 0;
};

std::optional<double> FindMetric(
    const std::vector<std::pair<std::string, std::string>>& entries,
    std::initializer_list<std::string_view> names) {
  std::optional<double> result;
  for (const auto& [key, value] : entries) {
    const bool matches =
        std::any_of(names.begin(), names.end(), [&key](std::string_view name) {
          if (key == name) return true;
          if (key.size() <= name.size() + 2 || !key.ends_with(name)) {
            return false;
          }
          const std::size_t separator = key.size() - name.size() - 2;
          return key[separator] == '_' && key[separator + 1] == '_';
        });
    if (!matches) continue;
    double number = 0.0;
    const auto parsed =
        std::from_chars(value.data(), value.data() + value.size(), number);
    if (parsed.ec == std::errc{} && std::isfinite(number)) result = number;
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

}  // namespace

std::string_view OrfsAdapter::Name() const noexcept { return "ORFS"; }

std::vector<ManagedFlowStageInfo> OrfsAdapter::Stages() const {
  std::vector<ManagedFlowStageInfo> result;
  for (const core::StageId stage :
       {core::StageId::kSynthesis, core::StageId::kFloorplan,
        core::StageId::kPlacement, core::StageId::kClockTreeSynthesis,
        core::StageId::kRouting, core::StageId::kFinalOutputs}) {
    result.push_back({stage, TargetForStage(stage), GuiTargetForStage(stage),
                      CheckpointForStage(stage).filename().string()});
  }
  return result;
}

runtime::WslCommand OrfsAdapter::BuildProbeCommand() const {
  core::ToolchainProfile profile;
  profile.orfs_root = "~/.designpp/toolchains/orfs";
  return BuildProbeCommand(profile);
}

runtime::WslCommand OrfsAdapter::BuildProbeCommand(
    const core::ToolchainProfile& profile) const {
  runtime::WslCommand command;
  command.program = L"/bin/bash";
  command.arguments = {
      L"-lc",
      L"root=\"$1\"; case \"$root\" in '~/'*) root=\"$HOME/${root#\\~/}\";; "
      L"esac; " +
          std::wstring(kOrfsFlakeInputSelectionScript) +
          L"test -f \"$root/flow/Makefile\" || exit 44; "
          L"openroad_exe=\"${OPENROAD_EXE:-}\"; "
          L"if [ -z \"$openroad_exe\" ]; then "
          L"openroad_exe=\"$(command -v openroad 2>/dev/null || true)\"; fi; "
          L"if [ -z \"$openroad_exe\" ] && "
          L"[ -x \"$root/tools/install/OpenROAD/bin/openroad\" ]; then "
          L"openroad_exe=\"$root/tools/install/OpenROAD/bin/openroad\"; fi; "
          L"yosys_exe=\"${YOSYS_EXE:-}\"; "
          L"if [ -z \"$yosys_exe\" ]; then "
          L"yosys_exe=\"$(command -v yosys 2>/dev/null || true)\"; fi; "
          L"if [ -z \"$yosys_exe\" ] && "
          L"[ -x \"$root/tools/install/yosys/bin/yosys\" ]; then "
          L"yosys_exe=\"$root/tools/install/yosys/bin/yosys\"; fi; "
          L"probe_tools='command -v openroad >/dev/null 2>&1 && "
          L"command -v yosys >/dev/null 2>&1 && "
          L"command -v eqy >/dev/null 2>&1 && "
          L"yosys -p \"help read_liberty\" 2>/dev/null | "
          L"grep -Fq -- -unit_delay && "
          L"yosys -p \"help stat\" 2>/dev/null | grep -Fq -- -hierarchy && "
          L"yosys-abc -c \"read_lib -h\" 2>&1 | grep -q -- -m && "
          L"printf \"%s\\n\" \"help repair_timing\" \"exit\" | "
          L"openroad -no_init -exit /dev/stdin 2>&1 | grep -Fq -- -sequence'; "
          L"if [ -n \"$openroad_exe\" ] && [ -x \"$openroad_exe\" ] && "
          L"[ -n \"$yosys_exe\" ] && [ -x \"$yosys_exe\" ] && "
          L"\"$yosys_exe\" -p 'help read_liberty' 2>/dev/null | "
          L"grep -Fq -- -unit_delay && "
          L"\"$yosys_exe\" -p 'help stat' 2>/dev/null | "
          L"grep -Fq -- -hierarchy && "
          L"\"$(dirname \"$yosys_exe\")/yosys-abc\" -c 'read_lib -h' "
          L"2>&1 | grep -q -- -m && "
          L"printf '%s\\n' 'help repair_timing' 'exit' | "
          L"\"$openroad_exe\" -no_init -exit /dev/stdin 2>&1 | grep -Fq -- "
          L"-sequence; "
          L"then tool_mode=direct; "
          L"elif [ -f \"$root/flake.nix\" ] && "
          L"[ -f \"$root/tools/yosys/flake.nix\" ] && "
          L"command -v nix >/dev/null 2>&1 && "
          L"nix --extra-experimental-features 'nix-command flakes' "
          L"develop \"$root\" --no-write-lock-file --override-input yosys "
          L"\"$yosys_input\" "
          L"--override-input openroad "
          L"\"$openroad_input\" "
          L"--override-input eqy-src \"$eqy_input\" "
          L"--offline --max-jobs 0 --builders '' --option fallback false "
          L"--command /bin/bash -c \"$probe_tools\"; then "
          L"tool_mode=orfs-flake; "
          L"else echo 'ORFS tools are missing or incompatible; install the "
          L"checkout toolchain (stat -hierarchy, ABC read_lib -m, EQY and "
          L"OpenROAD repair_timing -sequence required), including tools/yosys, "
          L"tools/OpenROAD and tools/eqy. Prepare the environment in Tool "
          L"Check; "
          L"Run will not download or build missing tools.' >&2; exit 45; fi; "
          L"printf 'DESIGNPP_ORFS_TOOL_MODE=%s\\n' \"$tool_mode\"; "
          L"printf 'DESIGNPP_ORFS_READY\\n'; "
          L"if test -d \"$root/.git\"; then git -C \"$root\" rev-parse HEAD; "
          L"fi; "
          L"/usr/bin/make --version | head -n 1; "
          L"if [ \"$tool_mode\" = direct ]; then \"$yosys_exe\" -V; "
          L"\"$openroad_exe\" -version; else "
          L"nix --extra-experimental-features 'nix-command flakes' "
          L"develop \"$root\" --no-write-lock-file --override-input yosys "
          L"\"$yosys_input\" "
          L"--override-input openroad "
          L"\"$openroad_input\" "
          L"--override-input eqy-src \"$eqy_input\" "
          L"--offline --max-jobs 0 --builders '' --option fallback false "
          L"--command /bin/bash -c "
          L"'yosys -V; openroad -version; eqy --version'; fi",
      L"designpp-orfs-probe", Utf8ToWide(profile.orfs_root)};
  if (!profile.wsl_distribution.empty()) {
    command.distribution = Utf8ToWide(profile.wsl_distribution);
  }
  return WithToolchainCompatibilityEvidence(std::move(command), "orfs");
}

runtime::WslCommand OrfsAdapter::BuildPlatformDiscoveryCommand(
    const core::ToolchainProfile& profile) const {
  runtime::WslCommand command;
  command.program = L"/bin/bash";
  command.arguments = {
      L"-lc",
      L"root=\"$1\"; case \"$root\" in '~/'*) "
      L"root=\"$HOME/${root#\\~/}\";; esac; "
      L"platforms=\"$root/flow/platforms\"; "
      L"if [ ! -d \"$platforms\" ]; then exit 44; fi; "
      L"for platform in \"$platforms\"/*; do "
      L"[ -d \"$platform\" ] || continue; "
      L"name=\"${platform##*/}\"; "
      L"case \"$name\" in common|.git|.*) continue;; esac; "
      L"if [ -f \"$platform/config.mk\" ] || "
      L"[ -f \"$platform/Makefile\" ]; then "
      L"drc=no; lvs=no; "
      L"test -s \"$platform/drc/$name.lydrc\" && drc=yes; "
      L"test -s \"$platform/lvs/$name.lylvs\" && "
      L"find \"$platform\" -maxdepth 3 -type f "
      L"\\( -iname '*.cdl' -o -iname '*.spice' -o "
      L"-iname '*.sp' \\) -print -quit 2>/dev/null | "
      L"grep -q . && lvs=yes; "
      L"printf '%s|ready|%s|%s|\\n' \"$name\" \"$drc\" "
      L"\"$lvs\"; "
      L"else printf '%s|unavailable|no|no|missing config.mk or "
      L"Makefile\\n' \"$name\"; fi; "
      L"done",
      L"designpp-orfs-platforms", Utf8ToWide(profile.orfs_root)};
  if (!profile.wsl_distribution.empty()) {
    command.distribution = Utf8ToWide(profile.wsl_distribution);
  }
  return command;
}

core::Result<std::vector<OrfsPlatformCandidate>>
OrfsAdapter::ParsePlatformDiscovery(std::string_view output) const {
  std::vector<OrfsPlatformCandidate> candidates;
  std::istringstream lines{std::string(output)};
  std::string line;
  while (std::getline(lines, line)) {
    const std::size_t first = line.find('|');
    const std::size_t second = first == std::string::npos
                                   ? std::string::npos
                                   : line.find('|', first + 1);
    if (first == std::string::npos || second == std::string::npos) continue;
    const std::string name(line.substr(0, first));
    if (!IsIdentifier(name) || name == "common") continue;
    const std::size_t third = line.find('|', second + 1);
    const std::size_t fourth = third == std::string::npos
                                   ? std::string::npos
                                   : line.find('|', third + 1);
    OrfsPlatformCandidate candidate;
    candidate.name = name;
    candidate.runnable = line.substr(first + 1, second - first - 1) == "ready";
    if (third != std::string::npos && fourth != std::string::npos) {
      candidate.drc_ready =
          line.substr(second + 1, third - second - 1) == "yes";
      candidate.lvs_ready = line.substr(third + 1, fourth - third - 1) == "yes";
      candidate.reason = line.substr(fourth + 1);
    } else {
      candidate.reason = line.substr(second + 1);
    }
    candidates.push_back(std::move(candidate));
  }
  std::sort(candidates.begin(), candidates.end(),
            [](const OrfsPlatformCandidate& left,
               const OrfsPlatformCandidate& right) {
              return left.name < right.name;
            });
  return candidates;
}

core::Status OrfsAdapter::ValidateAdvancedVariables(
    std::string_view json) const {
  if (json.size() > 64 * 1024) {
    return {core::ErrorCode::kInvalidArgument,
            "ORFS advanced variables exceed 64 KiB", 0};
  }
  auto parsed = FlatJsonParser(json).Parse();
  if (!parsed.Ok()) return parsed.GetStatus();
  for (const JsonEntry& entry : parsed.Value()) {
    if (IsManagedOrfsVariable(entry.key)) {
      return {core::ErrorCode::kInvalidArgument,
              "ORFS variable is managed by Design++: " + entry.key, 0};
    }
  }
  return core::Status::Success();
}

core::Result<std::string> OrfsAdapter::CanonicalizeAdvancedVariables(
    std::string_view json) const {
  if (json.size() > 64 * 1024) {
    return core::Status{core::ErrorCode::kInvalidArgument,
                        "ORFS advanced variables exceed 64 KiB", 0};
  }
  auto parsed = FlatJsonParser(json).Parse();
  if (!parsed.Ok()) return parsed.GetStatus();
  std::vector<JsonEntry> entries = std::move(parsed).Value();
  for (const JsonEntry& entry : entries) {
    if (IsManagedOrfsVariable(entry.key)) {
      return core::Status{core::ErrorCode::kInvalidArgument,
                          "ORFS variable is managed by Design++: " + entry.key,
                          0};
    }
  }
  std::sort(entries.begin(), entries.end(),
            [](const JsonEntry& left, const JsonEntry& right) {
              return left.key < right.key;
            });
  std::ostringstream output;
  output << "{";
  for (std::size_t index = 0; index < entries.size(); ++index) {
    output << (index == 0 ? "\n  \"" : ",\n  \"") << entries[index].key
           << "\": " << entries[index].raw_value;
  }
  if (!entries.empty()) output << '\n';
  output << "}";
  if (output.tellp() > static_cast<std::streamoff>(64 * 1024)) {
    return core::Status{core::ErrorCode::kInvalidArgument,
                        "ORFS advanced variables exceed 64 KiB", 0};
  }
  return output.str();
}

core::Status OrfsAdapter::Validate(const OrfsRequest& request) const {
  if (!IsIdentifier(request.top_module) || request.sources.empty() ||
      request.cpu_threads == 0 || request.backend_workspace.empty() ||
      request.staging_workspace.empty() ||
      !IsIdentifier(request.configuration.orfs.platform) ||
      !IsIdentifier(request.configuration.orfs.flow_variant)) {
    return {core::ErrorCode::kInvalidArgument,
            "ORFS top, platform, workspace, or CPU budget is invalid", 0};
  }
  if (request.tool_mode != "direct" && request.tool_mode != "orfs-flake") {
    return {core::ErrorCode::kInvalidArgument,
            "ORFS execution environment was not selected by capability probe",
            0};
  }
  if (!core::IsSafeLinuxProfilePath(request.profile.orfs_root) ||
      !IsSafeStagedPath(request.staging_workspace) ||
      !IsSafeMakeValue(request.backend_workspace) ||
      (!request.parent_backend_workspace.empty() &&
       !IsSafeMakeValue(request.parent_backend_workspace)) ||
      !IsSha256(request.checkpoint_hash)) {
    return {core::ErrorCode::kInvalidArgument,
            "ORFS paths must be safe staged or profile-relative values", 0};
  }
  for (const OrfsSource& source : request.sources) {
    if (source.staged_path.empty() ||
        source.staged_path.filename() != source.staged_path) {
      return {core::ErrorCode::kInvalidArgument,
              "ORFS source paths must be staged file names", 0};
    }
  }
  const core::Status configuration =
      core::ValidatePhysicalImplementationConfiguration(request.configuration);
  if (!configuration.Ok()) return configuration;
  if (request.configuration.backend_id != "orfs") {
    return {core::ErrorCode::kInvalidArgument,
            "ORFS adapter requires an ORFS physical configuration", 0};
  }
  if (!request.sdc_path.empty() &&
      request.sdc_path.filename() != request.sdc_path) {
    return {core::ErrorCode::kInvalidArgument, "ORFS SDC path is not staged",
            0};
  }
  if (request.sdc_time_unit != "fs" && request.sdc_time_unit != "ps" &&
      request.sdc_time_unit != "ns" && request.sdc_time_unit != "us" &&
      request.sdc_time_unit != "ms" && request.sdc_time_unit != "s") {
    return {core::ErrorCode::kInvalidArgument,
            "ORFS SDC command time unit is invalid", 0};
  }
  if (StageName(request.target_stage).empty()) {
    return {core::ErrorCode::kInvalidArgument,
            "ORFS target stage is unsupported", 0};
  }
  return ValidateAdvancedVariables(
      request.configuration.orfs.advanced_variables_json);
}

std::string OrfsAdapter::TargetForStage(core::StageId stage) const {
  return StageName(stage);
}

std::string OrfsAdapter::GuiTargetForStage(core::StageId stage) const {
  return GuiStageName(stage);
}

core::StageId OrfsAdapter::ClassifyTarget(std::string_view target) const {
  if (target == "synth") return core::StageId::kSynthesis;
  if (target == "floorplan") return core::StageId::kFloorplan;
  if (target == "place") return core::StageId::kPlacement;
  if (target == "cts") return core::StageId::kClockTreeSynthesis;
  if (target == "route") return core::StageId::kRouting;
  return core::StageId::kFinalOutputs;
}

core::Result<OrfsPlan> OrfsAdapter::BuildPlan(const OrfsRequest& input) const {
  auto request = input;
  request.configuration = core::ResolvePhysicalDefaults(input.configuration);
  const core::Status validation = Validate(request);
  if (!validation.Ok()) return validation;
  const std::string target =
      request.full_flow && request.target_stage == core::StageId::kFinalOutputs
          ? "all"
          : TargetForStage(request.target_stage);
  std::ostringstream config;
  config
      << "# Generated by Design++ for ORFS; do not edit the ORFS checkout.\n";
  config << "export PLATFORM := " << request.configuration.orfs.platform
         << '\n';
  config << "export FLOW_VARIANT := " << request.configuration.orfs.flow_variant
         << '\n';
  config << "export WORK_HOME := " << EscapeMake(request.backend_workspace)
         << '\n';
  // OpenROAD's write_sdc output does not record the active command time unit.
  // Give every ORFS OpenROAD process the same run-scoped init file so an SDC
  // written by one stage is interpreted identically by the next stage.
  config << "export OPENROAD_EXE := "
            "$(WORK_HOME)/designpp-openroad-wrapper.sh\n";
  config << "export DESIGNPP_OPENROAD_INIT := "
            "$(WORK_HOME)/designpp-openroad-init.tcl\n";
  config << "export DESIGNPP_ORFS_ROOT := "
         << EscapeMake(request.profile.orfs_root) << '\n';
  config << "export DESIGN_NAME := " << request.top_module << '\n';
  config << "export VERILOG_FILES :=";
  for (const OrfsSource& source : request.sources) {
    config << ' '
           << EscapeMake(LinuxJoin(
                  request.staging_workspace,
                  "src/" + source.staged_path.filename().generic_string()));
  }
  config << "\n";
  // ASAP7 publishes its sequential Liberty through corner/model-specific
  // variables but does not connect that value to the generic DFF_LIB_FILE
  // consumed by synth.tcl. Keep this expression recursive: the platform
  // config is included after this generated config and supplies CORNER,
  // LIB_MODEL, and the selected file path.
  if (request.configuration.orfs.platform == "asap7") {
    config << "export DFF_LIB_FILE = "
           << "$(strip $($(CORNER)_$(LIB_MODEL)_DFF_LIB_FILE))\n";
  }
  if (!request.include_directories.empty()) {
    config << "export VERILOG_INCLUDE_DIRS :=";
    for (std::size_t index = 0; index < request.include_directories.size();
         ++index) {
      config << ' '
             << EscapeMake(
                    LinuxJoin(request.staging_workspace,
                              "include/include_" + std::to_string(index)));
    }
    config << "\n";
  }
  if (!request.defines.empty()) {
    config << "export VERILOG_DEFINES :=";
    for (const std::string& define : request.defines) {
      config << ' ' << EscapeMake(define);
    }
    config << "\n";
  }
  if (!request.parameters.empty()) {
    config << "export SYNTH_PARAMETERS :=";
    for (const std::string& parameter : request.parameters) {
      config << ' ' << EscapeMake(parameter);
    }
    config << "\n";
  }
  if (!request.sdc_path.empty()) {
    config << "export SDC_FILE := "
           << EscapeMake(
                  LinuxJoin(request.staging_workspace,
                            "constraints/" +
                                request.sdc_path.filename().generic_string()))
           << '\n';
  }
  if (request.effective_clock_period_ns) {
    config << "export CLOCK_PERIOD := " << *request.effective_clock_period_ns
           << '\n';
    const auto abc_period =
        NanosecondsToPicoseconds(*request.effective_clock_period_ns);
    if (abc_period) {
      config << "export ABC_CLOCK_PERIOD_IN_PS := " << *abc_period << '\n';
    }
  }
  if (!request.configuration.clock_ports.empty()) {
    config << "export CLOCK_PORT :=";
    for (const std::string& port : request.configuration.clock_ports) {
      config << ' ' << EscapeMake(port);
    }
    config << '\n';
  }
  if (request.configuration.die_area.size() != 4 ||
      request.configuration.core_area.size() != 4) {
    config << "export CORE_UTILIZATION := "
           << request.configuration.core_utilization_percent << '\n';
  }
  if (request.configuration.placement_density_percent) {
    const double value =
        std::strtod(request.configuration.placement_density_percent->c_str(),
                    nullptr) /
        100.0;
    config << "export PLACE_DENSITY := " << value << '\n';
  }
  const auto area = [&config](std::string_view name,
                              const std::vector<std::string>& values) {
    if (values.size() != 4) return;
    config << "export " << name << " :=";
    for (const std::string& value : values) config << ' ' << EscapeMake(value);
    config << '\n';
  };
  area("DIE_AREA", request.configuration.die_area);
  area("CORE_AREA", request.configuration.core_area);

  auto advanced =
      FlatJsonParser(request.configuration.orfs.advanced_variables_json)
          .Parse();
  if (!advanced.Ok()) return advanced.GetStatus();
  for (const JsonEntry& entry : advanced.Value()) {
    if (IsManagedOrfsVariable(entry.key)) {
      return core::Status{core::ErrorCode::kInvalidArgument,
                          "ORFS variable is managed by Design++: " + entry.key,
                          0};
    }
    if (entry.key.find("DIR") != std::string::npos ||
        entry.key.find("FILE") != std::string::npos) {
      return core::Status{
          core::ErrorCode::kInvalidArgument,
          "ORFS path variable is managed by Design++: " + entry.key, 0};
    }
    std::string value = entry.raw_value;
    if (auto decoded = DecodeString(value)) value = *decoded;
    if (value.front() == '[' && value.back() == ']') {
      value.erase(value.begin());
      value.pop_back();
      std::replace(value.begin(), value.end(), ',', ' ');
      value.erase(std::remove(value.begin(), value.end(), '"'), value.end());
    }
    config << "export " << entry.key << " := " << EscapeMake(Trim(value))
           << '\n';
  }

  const std::wstring root = Utf8ToWide(request.profile.orfs_root);
  const std::wstring workspace = Utf8ToWide(request.backend_workspace);
  const std::wstring staging = Utf8ToWide(request.staging_workspace);
  const std::wstring jobs = std::to_wstring(request.cpu_threads);
  const std::wstring target_wide = Utf8ToWide(target);
  const std::wstring variant =
      Utf8ToWide(request.configuration.orfs.flow_variant);
  const std::wstring parent = Utf8ToWide(request.parent_backend_workspace);
  const std::wstring checkpoint_hash = Utf8ToWide(request.checkpoint_hash);
  const std::wstring tool_mode = Utf8ToWide(request.tool_mode);
  const std::wstring script =
      L"set -eu; root=\"$1\"; workspace=\"$2\"; staging=\"$3\"; jobs=\"$4\"; "
      L"target=\"$5\"; variant=\"$6\"; parent=\"$7\"; "
      L"checkpoint_hash=\"$8\"; tool_mode=\"$9\"; "
      L"case \"$root\" in '~/'*) root=\"$HOME/${root#\\~/}\";; esac; " +
      std::wstring(kOrfsFlakeInputSelectionScript) +
      L"case \"$workspace\" in /*) ;; *) workspace=\"$HOME/${workspace#./}\";; "
      L"esac; "
      L"mkdir -p \"$workspace\"; "
      L"if [ -n \"$parent\" ]; then "
      L"case \"$parent\" in /*) parent_abs=\"$parent\";; *) "
      L"parent_abs=\"$HOME/${parent#./}\";; esac; "
      L"if [ ! -d \"$parent_abs\" ]; then exit 45; fi; "
      L"if [ -n \"$checkpoint_hash\" ] && ! find \"$parent_abs\" "
      L"-type f -name '*.odb' -exec sha256sum {} + | grep -q "
      L"\"^$checkpoint_hash \"; "
      L"then exit 45; fi; "
      L"cp -R \"$parent_abs\"/. \"$workspace\"/; "
      L"fi; "
      L"if [ -n \"$checkpoint_hash\" ] && [ -z \"$parent\" ] && ! find "
      L"\"$workspace\" -type f -name '*.odb' -exec sha256sum {} + | "
      L"grep -q \"^$checkpoint_hash \"; then exit 45; fi; "
      L"case \"$target\" in "
      L"synth) stale='1_synth.odb 2_floorplan.odb 3_place.odb 4_cts.odb "
      L"5_route.odb 6_final.odb';; "
      L"floorplan) stale='2_floorplan.odb 3_place.odb 4_cts.odb 5_route.odb "
      L"6_final.odb';; "
      L"place) stale='3_place.odb 4_cts.odb 5_route.odb 6_final.odb';; "
      L"cts) stale='4_cts.odb 5_route.odb 6_final.odb';; "
      L"route) stale='5_route.odb 6_final.odb';; "
      L"finish|all) stale='6_final.odb';; esac; "
      L"for checkpoint in $stale; do find \"$workspace\" -type f "
      L"-name \"$checkpoint\" -delete; done; "
      L"cp -R \"$staging\"/. \"$workspace\"/; "
      L"chmod 700 \"$workspace/designpp-openroad-wrapper.sh\"; "
      L"if [ \"$tool_mode\" = direct ]; then "
      L"exec /usr/bin/make --no-print-directory -C \"$root/flow\" -j 1 "
      L"DESIGN_CONFIG=\"$workspace/config.mk\" WORK_HOME=\"$workspace\" "
      L"FLOW_VARIANT=\"$variant\" NUM_CORES=\"$jobs\" \"$target\"; "
      L"fi; "
      L"if [ \"$tool_mode\" = orfs-flake ] && [ -f \"$root/flake.nix\" ]; "
      L"then exec nix --extra-experimental-features 'nix-command flakes' "
      L"develop \"$root\" --no-write-lock-file --override-input yosys "
      L"\"$yosys_input\" "
      L"--override-input openroad "
      L"\"$openroad_input\" "
      L"--override-input eqy-src \"$eqy_input\" "
      L"--offline --max-jobs 0 --builders '' --option fallback false "
      L"--command /usr/bin/make --no-print-directory "
      L"-C \"$root/flow\" -j 1 "
      L"DESIGN_CONFIG=\"$workspace/config.mk\" WORK_HOME=\"$workspace\" "
      L"FLOW_VARIANT=\"$variant\" NUM_CORES=\"$jobs\" \"$target\"; fi; "
      L"echo 'The probed ORFS execution environment is unavailable' >&2; "
      L"exit 45";
  OrfsPlan plan;
  plan.config_makefile = config.str();
  plan.openroad_wrapper =
      "#!/bin/bash\n"
      "set -eu\n"
      "real_openroad=$(command -v openroad 2>/dev/null || true)\n"
      "if [ -z \"$real_openroad\" ] && "
      "[ -x \"${DESIGNPP_ORFS_ROOT}/tools/install/OpenROAD/bin/openroad\" "
      "]; then\n"
      "  real_openroad="
      "\"${DESIGNPP_ORFS_ROOT}/tools/install/OpenROAD/bin/openroad\"\n"
      "fi\n"
      "if [ -z \"$real_openroad\" ]; then\n"
      "  echo 'Design++ cannot locate the probed OpenROAD executable' >&2\n"
      "  exit 127\n"
      "fi\n"
      "init=\"${DESIGNPP_OPENROAD_INIT:?}\"\n"
      "args=(\"$@\")\n"
      "if (( ${#args[@]} == 0 )); then\n"
      "  exec \"$real_openroad\"\n"
      "fi\n"
      "script_index=-1\n"
      "for index in \"${!args[@]}\"; do\n"
      "  candidate=${args[$index]}\n"
      "  if [[ \"$candidate\" == *.tcl && -f \"$candidate\" ]]; then\n"
      "    script_index=$index\n"
      "  fi\n"
      "done\n"
      "if (( script_index < 0 )); then\n"
      "  exec \"$real_openroad\" \"${args[@]}\"\n"
      "fi\n"
      "script=${args[$script_index]}\n"
      "combined=$(mktemp \"${TMPDIR:-/tmp}/designpp-openroad.XXXXXX.tcl\")\n"
      "cleanup() { rm -f -- \"$combined\"; }\n"
      "trap cleanup EXIT INT TERM\n"
      "printf 'source {%s}\\nsource {%s}\\n' \"$init\" \"$script\" > "
      "\"$combined\"\n"
      "args[$script_index]=$combined\n"
      "set +e\n"
      "\"$real_openroad\" \"${args[@]}\"\n"
      "status=$?\n"
      "set -e\n"
      "exit $status\n";
  plan.openroad_init =
      "# Design++ run-scoped OpenROAD command-unit contract.\n"
      "rename read_sdc designpp_original_read_sdc\n"
      "proc read_sdc {args} {\n"
      "  set_cmd_units -time " +
      request.sdc_time_unit +
      "\n  puts \"DESIGNPP_OPENROAD_SDC_TIME_UNIT=" + request.sdc_time_unit +
      "\"\n"
      "  uplevel 1 [list designpp_original_read_sdc {*}$args]\n"
      "}\n";
  plan.target = target;
  plan.execute.program = L"/bin/bash";
  plan.execute.arguments = {L"-lc", script,          L"designpp-orfs-run",
                            root,   workspace,       staging,
                            jobs,   target_wide,     variant,
                            parent, checkpoint_hash, tool_mode};
  plan.validate = plan.execute;
  plan.validate.arguments[1] =
      L"set -eu; root=\"$1\"; workspace=\"$2\"; staging=\"$3\"; jobs=\"$4\"; "
      L"target=\"$5\"; variant=\"$6\"; parent=\"$7\"; "
      L"checkpoint_hash=\"$8\"; tool_mode=\"$9\"; "
      L"case \"$root\" in '~/'*) root=\"$HOME/${root#\\~/}\";; esac; "
      L"case \"$workspace\" in /*) ;; *) workspace=\"$HOME/${workspace#./}\";; "
      L"esac; "
      L"if [ -n \"$parent\" ]; then "
      L"case \"$parent\" in /*) parent_abs=\"$parent\";; *) "
      L"parent_abs=\"$HOME/${parent#./}\";; esac; "
      L"if [ ! -d \"$parent_abs\" ]; then exit 45; fi; "
      L"if [ -n \"$checkpoint_hash\" ] && ! find \"$parent_abs\" "
      L"-type f -name '*.odb' -exec sha256sum {} + | grep -q "
      L"\"^$checkpoint_hash \"; "
      L"then exit 45; fi; "
      L"fi; "
      L"if [ -n \"$checkpoint_hash\" ] && [ -z \"$parent\" ] && ! find "
      L"\"$workspace\" -type f -name '*.odb' -exec sha256sum {} + | "
      L"grep -q \"^$checkpoint_hash \"; then exit 45; fi; "
      L"test -f \"$staging/config.mk\"; "
      L"test -d \"$root/flow/platforms/" +
      Utf8ToWide(request.configuration.orfs.platform) +
      L"\"; "
      L"case \"$target\" in synth|floorplan|place|cts|route|finish|all) ;; "
      L"*) exit 45;; esac; "
      L"case \"$tool_mode\" in direct) "
      L"test -x \"${OPENROAD_EXE:-$root/tools/install/OpenROAD/bin/openroad}\" "
      L"|| command -v openroad >/dev/null 2>&1; "
      L"test -x \"${YOSYS_EXE:-$root/tools/install/yosys/bin/yosys}\" "
      L"|| command -v yosys >/dev/null 2>&1;; "
      L"orfs-flake) test -f \"$root/flake.nix\" && "
      L"test -f \"$root/tools/yosys/flake.nix\" && command -v nix "
      L">/dev/null 2>&1;; *) exit 45;; esac";
  if (!request.profile.wsl_distribution.empty()) {
    plan.execute.distribution = Utf8ToWide(request.profile.wsl_distribution);
    plan.validate.distribution = plan.execute.distribution;
  }
  return plan;
}

std::optional<ManagedFlowProgress> OrfsAdapter::ParseProgress(
    std::string_view line) const {
  const std::string lower = [&line] {
    std::string value(line);
    std::transform(value.begin(), value.end(), value.begin(), [](char c) {
      return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    return value;
  }();
  const std::array<std::pair<std::string_view, core::StageId>, 6> stages = {{
      {"synth", core::StageId::kSynthesis},
      {"floorplan", core::StageId::kFloorplan},
      {"place", core::StageId::kPlacement},
      {"cts", core::StageId::kClockTreeSynthesis},
      {"route", core::StageId::kRouting},
      {"finish", core::StageId::kFinalOutputs},
  }};
  for (std::size_t index = 0; index < stages.size(); ++index) {
    const auto& [name, stage] = stages[index];
    if (lower.find(name) == std::string::npos) continue;
    ManagedFlowProgress progress;
    progress.step_id = std::string(name);
    progress.message = std::string(Trim(line));
    progress.stage = stage;
    progress.completed_steps = index;
    progress.total_steps = stages.size();
    return progress;
  }
  return std::nullopt;
}

std::vector<core::Diagnostic> OrfsAdapter::ParseDiagnostics(
    std::string_view output) const {
  std::vector<core::Diagnostic> diagnostics;
  std::istringstream lines{std::string(output)};
  std::string line;
  while (std::getline(lines, line)) {
    std::string lower = line;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](char c) {
      return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    const std::string_view trimmed = Trim(lower);
    const bool warning = trimmed.starts_with("warning") ||
                         lower.find(" warning:") != std::string::npos;
    const bool error = trimmed.starts_with("error") ||
                       lower.find(" error:") != std::string::npos ||
                       lower.find("fatal") != std::string::npos;
    if (!warning && !error) continue;
    core::Diagnostic diagnostic;
    diagnostic.severity = error ? core::DiagnosticSeverity::kError
                                : core::DiagnosticSeverity::kWarning;
    diagnostic.code = error ? "ORFS-ERROR" : "ORFS-WARNING";
    diagnostic.message = line;
    diagnostics.push_back(std::move(diagnostic));
  }
  return diagnostics;
}

core::Result<ManagedFlowMetrics> OrfsAdapter::ParseMetrics(
    std::string_view metrics_json) const {
  std::vector<std::pair<std::string, std::string>> entries;
  if (!MetricsJsonParser(metrics_json).Parse(&entries)) {
    return core::Status{core::ErrorCode::kCorruptData,
                        "ORFS metrics JSON is malformed", 0};
  }
  ManagedFlowMetrics metrics;
  metrics.raw_entries = std::move(entries);
  for (const auto& [key, value] : metrics.raw_entries) {
    metrics.raw[key] = value;
  }
  const auto& values = metrics.raw_entries;
  metrics.core_area = FindMetric(values, {"design__core__area", "core_area"});
  metrics.die_area = FindMetric(values, {"design__die__area", "die_area"});
  metrics.utilization =
      FindMetric(values, {"design__instance__utilization", "utilization"});
  metrics.instance_count =
      FindMetric(values, {"design__instance__count", "instance_count"});
  metrics.setup_wns = FindMetric(
      values, {"timing__setup__wns", "timing__setup__ws", "setup_wns"});
  metrics.setup_tns = FindMetric(values, {"timing__setup__tns", "setup_tns"});
  metrics.hold_wns =
      FindMetric(values, {"timing__hold__wns", "timing__hold__ws", "hold_wns"});
  metrics.hold_tns = FindMetric(values, {"timing__hold__tns", "hold_tns"});
  metrics.worst_setup_skew = FindMetric(
      values, {"clock__skew__worst_setup", "clock__skew__setup", "setup_skew"});
  metrics.worst_hold_skew = FindMetric(
      values, {"clock__skew__worst_hold", "clock__skew__hold", "hold_skew"});
  metrics.wire_length =
      FindMetric(values, {"route__wirelength", "wire_length"});
  metrics.antenna_violations =
      FindMetric(values, {"route__antenna_violation__count",
                          "antenna__violating__nets", "antenna_violations"});
  metrics.slew_violations = FindMetric(
      values, {"design__max_slew_violation__count", "slew_violations"});
  metrics.capacitance_violations = FindMetric(
      values, {"design__max_cap_violation__count", "capacitance_violations"});
  metrics.fanout_violations = FindMetric(
      values, {"design__max_fanout_violation__count", "fanout_violations"});
  metrics.global_route_congestion = FindMetric(
      values, {"route__congestion__global", "route__congestion__global__total",
               "global_route_congestion", "global_routing_congestion"});
  metrics.detailed_route_congestion = FindMetric(
      values,
      {"route__congestion__detailed", "route__congestion__detailed__total",
       "detailed_route_congestion", "detailed_routing_congestion"});
  metrics.global_route_overflow = FindMetric(
      values, {"route__overflow__global", "route__congestion__global__overflow",
               "global_route_overflow", "global_routing_overflow"});
  metrics.detailed_route_overflow = FindMetric(
      values,
      {"route__overflow__detailed", "route__congestion__detailed__overflow",
       "detailed_route_overflow", "detailed_routing_overflow"});
  metrics.routing_violations =
      FindMetric(values, {"route__violation__count", "route__drc_errors",
                          "route__violations", "routing_violations"});
  metrics.runtime_seconds =
      FindMetric(values, {"flow__runtime_sec", "flow__runtime_seconds",
                          "runtime_sec", "runtime_seconds"});
  metrics.peak_memory_mb =
      FindMetric(values, {"flow__peak_memory_mb", "flow__peak_memory",
                          "peak_memory_mb", "peak_memory"});
  metrics.drc_violations =
      FindMetric(values, {"magic__drc_error__count", "drc_violations"});
  metrics.xor_violations =
      FindMetric(values, {"klayout__xor_error__count",
                          "design__xor_difference__count", "xor_violations"});
  metrics.lvs_errors =
      FindMetric(values, {"design__lvs_error__count", "lvs_errors"});
  metrics.ir_drop_worst =
      FindMetric(values, {"ir__drop__worst", "ir_drop_worst"});
  return metrics;
}

std::filesystem::path OrfsAdapter::CheckpointForStage(
    core::StageId stage) const {
  const std::string target = StageName(stage);
  if (target == "synth") return L"1_synth.odb";
  if (target == "floorplan") return L"2_floorplan.odb";
  if (target == "place") return L"3_place.odb";
  if (target == "cts") return L"4_cts.odb";
  if (target == "route") return L"5_route.odb";
  if (target == "finish") return L"6_final.odb";
  return {};
}

ManagedFlowArtifactSet OrfsAdapter::DiscoverAvailableArtifacts(
    const std::filesystem::path& root) const {
  ManagedFlowArtifactSet artifacts;
  artifacts.resolved_config = FindNewest(root, {}, {"resolved.json"});
  artifacts.final_state = FindNewest(root, {}, {"state_out.json"});
  artifacts.metrics_json = FindNewest(
      root, {},
      {"metrics.json", "1_synth.json", "2_1_floorplan.json",
       "2_2_floorplan_io.json", "2_3_floorplan_tdms.json",
       "2_4_floorplan_macro.json", "2_5_floorplan_tapcell.json",
       "2_6_floorplan_pdn.json", "3_1_place_gp_skip_io.json",
       "3_2_place_iop.json", "3_3_place_gp.json", "3_4_place_resized.json",
       "3_5_place_dp.json", "4_1_cts.json", "4_2_cts_fillcell.json",
       "5_1_grt.json", "5_2_route.json", "6_report.json"});
  artifacts.metrics_csv = FindNewest(root, {}, {"metrics.csv"});
  artifacts.gds = FindNewest(root, {}, {"6_final.gds", "6_final.gdsii"});
  if (artifacts.gds.empty()) {
    artifacts.gds = FindNewest(root, {".gds", ".gdsii"});
  }
  artifacts.def = FindNewest(root, {}, {"6_final.def"});
  if (artifacts.def.empty()) artifacts.def = FindNewest(root, {".def"});
  artifacts.lef = FindNewest(root, {".lef"});
  artifacts.odb = FindNewest(root, {}, {"6_final.odb"});
  if (artifacts.odb.empty()) artifacts.odb = FindNewest(root, {".odb"});
  artifacts.gate_netlist = FindNewest(root, {}, {"6_final.v", "6_final.vg"});
  if (artifacts.gate_netlist.empty()) {
    artifacts.gate_netlist = FindNewest(root, {".v", ".vg"});
  }
  artifacts.power_netlist = FindNewest(root, {}, {"power.v", "pnl.v"});
  artifacts.sdf = FindNewest(root, {".sdf"});
  artifacts.spef = FindNewest(root, {".spef"});
  return artifacts;
}

core::Status OrfsAdapter::ValidateStageArtifacts(
    const ManagedFlowArtifactSet& artifacts, core::StageId stage) const {
  if (stage == core::StageId::kFinalOutputs) {
    return ValidateFinalArtifacts(artifacts);
  }
  if (artifacts.odb.empty()) {
    return {core::ErrorCode::kNotFound, "ORFS stage checkpoint ODB is missing",
            0};
  }
  std::error_code error;
  if (!std::filesystem::is_regular_file(artifacts.odb, error) || error ||
      std::filesystem::file_size(artifacts.odb, error) == 0) {
    return {core::ErrorCode::kCorruptData, "ORFS stage checkpoint ODB is empty",
            0};
  }
  return core::Status::Success();
}

core::Status OrfsAdapter::ValidateFinalArtifacts(
    const ManagedFlowArtifactSet& artifacts) const {
  // ORFS finish does not require a design LEF. Preserve one when a platform
  // emits it, but do not reject an otherwise complete final layout without it.
  const std::array<std::filesystem::path, 4> required = {
      artifacts.gds, artifacts.def, artifacts.odb, artifacts.gate_netlist};
  for (const auto& path : required) {
    if (path.empty()) {
      return {core::ErrorCode::kNotFound,
              "ORFS final physical artifact is missing", 0};
    }
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error ||
        std::filesystem::file_size(path, error) == 0) {
      return {core::ErrorCode::kCorruptData,
              "ORFS final physical artifact is empty", 0};
    }
  }
  return core::Status::Success();
}

}  // namespace designpp::adapters

// Copyright 2026 The Design++ Authors

#include "designpp/adapters/yosys_adapter.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <map>
#include <optional>
#include <sstream>
#include <variant>

#include "designpp/application/module_scanner.h"

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

std::string Quote(std::wstring_view value) {
  std::string result = "\"";
  for (const char character : WideToUtf8(value)) {
    if (character == '\\' || character == '"') result.push_back('\\');
    result.push_back(character);
  }
  result.push_back('"');
  return result;
}

std::optional<std::string_view> NumberValue(std::string_view json,
                                            std::string_view key) {
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
  std::size_t end = begin;
  while (end < json.size() && json[end] != ',' && json[end] != '}' &&
         !std::isspace(static_cast<unsigned char>(json[end]))) {
    ++end;
  }
  return json.substr(begin, end - begin);
}

struct JsonNumber {
  std::string text;
};

class JsonValue final {
 public:
  using Object = std::map<std::string, JsonValue>;
  using Array = std::vector<JsonValue>;
  using Value = std::variant<std::nullptr_t, bool, JsonNumber, std::string,
                             Object, Array>;

  explicit JsonValue(Value value) : value_(std::move(value)) {}

  template <typename T>
  [[nodiscard]] const T* Get() const {
    return std::get_if<T>(&value_);
  }

 private:
  Value value_;
};

class JsonParser final {
 public:
  explicit JsonParser(std::string_view input) : input_(input) {}

  [[nodiscard]] std::optional<JsonValue> Parse() {
    auto value = ParseValue();
    SkipWhitespace();
    return value && position_ == input_.size() ? std::move(value)
                                               : std::nullopt;
  }

 private:
  void SkipWhitespace() {
    while (position_ < input_.size() &&
           std::isspace(static_cast<unsigned char>(input_[position_]))) {
      ++position_;
    }
  }

  bool Take(char expected) {
    SkipWhitespace();
    if (position_ >= input_.size() || input_[position_] != expected) {
      return false;
    }
    ++position_;
    return true;
  }

  std::optional<std::string> ParseString() {
    if (!Take('"')) return std::nullopt;
    std::string result;
    while (position_ < input_.size()) {
      const char character = input_[position_++];
      if (character == '"') return result;
      if (character != '\\') {
        result.push_back(character);
        continue;
      }
      if (position_ >= input_.size()) return std::nullopt;
      const char escaped = input_[position_++];
      switch (escaped) {
        case '"':
        case '\\':
        case '/':
          result.push_back(escaped);
          break;
        case 'b':
          result.push_back('\b');
          break;
        case 'f':
          result.push_back('\f');
          break;
        case 'n':
          result.push_back('\n');
          break;
        case 'r':
          result.push_back('\r');
          break;
        case 't':
          result.push_back('\t');
          break;
        case 'u':
          if (position_ + 4 > input_.size()) return std::nullopt;
          position_ += 4;
          result.push_back('?');
          break;
        default:
          return std::nullopt;
      }
    }
    return std::nullopt;
  }

  std::optional<JsonValue> ParseValue() {
    SkipWhitespace();
    if (position_ >= input_.size()) return std::nullopt;
    if (input_[position_] == '{') return ParseObject();
    if (input_[position_] == '[') return ParseArray();
    if (input_[position_] == '"') {
      auto value = ParseString();
      return value ? std::optional<JsonValue>(JsonValue(std::move(*value)))
                   : std::nullopt;
    }
    for (const auto& literal :
         {std::pair<std::string_view, JsonValue::Value>{"true", true},
          {"false", false},
          {"null", nullptr}}) {
      if (input_.substr(position_, literal.first.size()) == literal.first) {
        position_ += literal.first.size();
        return JsonValue(literal.second);
      }
    }
    const std::size_t begin = position_;
    while (position_ < input_.size() &&
           (std::isdigit(static_cast<unsigned char>(input_[position_])) ||
            input_[position_] == '-' || input_[position_] == '+' ||
            input_[position_] == '.' || input_[position_] == 'e' ||
            input_[position_] == 'E')) {
      ++position_;
    }
    if (begin == position_) return std::nullopt;
    return JsonValue(
        JsonNumber{std::string(input_.substr(begin, position_ - begin))});
  }

  std::optional<JsonValue> ParseObject() {
    if (!Take('{')) return std::nullopt;
    JsonValue::Object object;
    if (Take('}')) return JsonValue(std::move(object));
    while (true) {
      auto key = ParseString();
      if (!key || !Take(':')) return std::nullopt;
      auto value = ParseValue();
      if (!value) return std::nullopt;
      object.insert_or_assign(std::move(*key), std::move(*value));
      if (Take('}')) return JsonValue(std::move(object));
      if (!Take(',')) return std::nullopt;
    }
  }

  std::optional<JsonValue> ParseArray() {
    if (!Take('[')) return std::nullopt;
    JsonValue::Array array;
    if (Take(']')) return JsonValue(std::move(array));
    while (true) {
      auto value = ParseValue();
      if (!value) return std::nullopt;
      array.push_back(std::move(*value));
      if (Take(']')) return JsonValue(std::move(array));
      if (!Take(',')) return std::nullopt;
    }
  }

  std::string_view input_;
  std::size_t position_ = 0;
};

const JsonValue* Member(const JsonValue::Object& object, std::string_view key) {
  const auto member = object.find(std::string(key));
  return member == object.end() ? nullptr : &member->second;
}

const JsonValue::Object* ObjectMember(const JsonValue::Object& object,
                                      std::string_view key) {
  const JsonValue* value = Member(object, key);
  return value == nullptr ? nullptr : value->Get<JsonValue::Object>();
}

std::optional<std::string> StringMember(const JsonValue::Object& object,
                                        std::string_view key) {
  const JsonValue* value = Member(object, key);
  if (value == nullptr) return std::nullopt;
  const std::string* text = value->Get<std::string>();
  return text == nullptr ? std::nullopt : std::optional<std::string>(*text);
}

std::optional<core::SchematicDirection> ParseDirection(std::string_view value) {
  if (value == "input") return core::SchematicDirection::kInput;
  if (value == "output") return core::SchematicDirection::kOutput;
  if (value == "inout") return core::SchematicDirection::kInout;
  return std::nullopt;
}

std::optional<int> IntMember(const JsonValue::Object& object,
                             std::string_view key) {
  const JsonValue* value = Member(object, key);
  const JsonNumber* number =
      value == nullptr ? nullptr : value->Get<JsonNumber>();
  if (number == nullptr) return std::nullopt;
  int result = 0;
  const auto parsed = std::from_chars(
      number->text.data(), number->text.data() + number->text.size(), result);
  return parsed.ec == std::errc() &&
                 parsed.ptr == number->text.data() + number->text.size()
             ? std::optional<int>(result)
             : std::nullopt;
}

std::optional<std::string> ScalarText(const JsonValue& value) {
  if (const auto* text = value.Get<std::string>()) return *text;
  if (const auto* number = value.Get<JsonNumber>()) return number->text;
  if (const auto* boolean = value.Get<bool>()) {
    return *boolean ? std::string("1") : std::string("0");
  }
  return std::nullopt;
}

std::map<std::string, std::string> ReadScalarObject(
    const JsonValue::Object* object) {
  std::map<std::string, std::string> result;
  if (object == nullptr) return result;
  for (const auto& [name, value] : *object) {
    const auto scalar = ScalarText(value);
    if (scalar) result.insert_or_assign(name, *scalar);
  }
  return result;
}

bool TypeContains(std::string_view type, std::string_view token) {
  return type.find(token) != std::string_view::npos;
}

std::string Uppercase(std::string_view text) {
  std::string result(text);
  std::transform(result.begin(), result.end(), result.begin(), [](char value) {
    return static_cast<char>(std::toupper(static_cast<unsigned char>(value)));
  });
  return result;
}

bool HasCellTypeToken(std::string_view type, std::string_view token) {
  std::size_t position = type.find(token);
  while (position != std::string_view::npos) {
    if (position == 0 ||
        !std::isalnum(static_cast<unsigned char>(type[position - 1]))) {
      return true;
    }
    position = type.find(token, position + 1);
  }
  return false;
}

core::SchematicNodeKind ClassifyNode(std::string_view type) {
  const std::string normalized = Uppercase(type);
  if (TypeContains(normalized, "DFF") || TypeContains(normalized, "DLATCH")) {
    return core::SchematicNodeKind::kRegister;
  }
  if (TypeContains(normalized, "MUX")) return core::SchematicNodeKind::kMux;
  if (TypeContains(normalized, "ADD") || TypeContains(normalized, "SUB") ||
      TypeContains(normalized, "MUL") || TypeContains(normalized, "DIV") ||
      TypeContains(normalized, "MOD") || TypeContains(normalized, "ALU")) {
    return core::SchematicNodeKind::kArithmetic;
  }
  if (TypeContains(normalized, "REDUCE_BOOL") ||
      TypeContains(normalized, "EQ") || TypeContains(normalized, "NE") ||
      TypeContains(normalized, "LT") || TypeContains(normalized, "LE") ||
      TypeContains(normalized, "GT") || TypeContains(normalized, "GE")) {
    return core::SchematicNodeKind::kComparator;
  }
  if (TypeContains(normalized, "MEM")) return core::SchematicNodeKind::kMemory;
  for (const std::string_view primitive :
       {"AND", "OR", "XOR", "XNOR", "NAND", "NOR", "NOT", "BUF"}) {
    if (HasCellTypeToken(normalized, primitive)) {
      return core::SchematicNodeKind::kPrimitive;
    }
  }
  return core::SchematicNodeKind::kGeneric;
}

std::string NodeLabel(std::string_view type, core::SchematicNodeKind kind) {
  const std::string normalized = Uppercase(type);
  switch (kind) {
    case core::SchematicNodeKind::kRegister:
      return TypeContains(normalized, "DFFE") ? "DFFE" : "DFF";
    case core::SchematicNodeKind::kMux:
      return "MUX";
    case core::SchematicNodeKind::kArithmetic:
      if (TypeContains(normalized, "SUB")) return "SUB";
      if (TypeContains(normalized, "MUL")) return "MUL";
      return "ADD";
    case core::SchematicNodeKind::kComparator:
      if (TypeContains(normalized, "REDUCE_BOOL")) return "ANY";
      if (TypeContains(normalized, "NE")) return "!=";
      if (TypeContains(normalized, "LT")) return "<";
      if (TypeContains(normalized, "LE")) return "<=";
      if (TypeContains(normalized, "GT")) return ">";
      if (TypeContains(normalized, "GE")) return ">=";
      return "==";
    case core::SchematicNodeKind::kMemory:
      return "MEM";
    case core::SchematicNodeKind::kModule:
      return std::string(type);
    case core::SchematicNodeKind::kGeneric:
      return std::string(type);
    default:
      break;
  }
  std::string label(type);
  label.erase(std::remove(label.begin(), label.end(), '$'), label.end());
  label.erase(std::remove(label.begin(), label.end(), '_'), label.end());
  return label.empty() ? std::string(type) : label;
}

core::SchematicPinRole PinRole(std::string_view name,
                               core::SchematicDirection direction) {
  if (direction == core::SchematicDirection::kOutput) {
    return core::SchematicPinRole::kOutput;
  }
  if (name == "C" || name == "CLK") return core::SchematicPinRole::kClock;
  if (name == "R" || name == "ARST" || name == "SRST" || name == "CLR") {
    return core::SchematicPinRole::kReset;
  }
  if (name == "SET") return core::SchematicPinRole::kSet;
  if (name == "E" || name == "EN" || name == "CE") {
    return core::SchematicPinRole::kEnable;
  }
  if (name == "S" || name == "SEL") return core::SchematicPinRole::kSelect;
  return core::SchematicPinRole::kData;
}

bool ParameterIsOne(const std::map<std::string, std::string>& parameters,
                    std::string_view name, bool default_value) {
  const auto found = parameters.find(std::string(name));
  if (found == parameters.end()) return default_value;
  return !found->second.empty() && found->second.back() == '1';
}

void DecodeSequentialPolarity(core::SchematicNode* node) {
  if (node->kind != core::SchematicNodeKind::kRegister) return;
  node->clock_positive = ParameterIsOne(node->parameters, "CLK_POLARITY", true);
  node->reset_positive =
      ParameterIsOne(node->parameters, "ARST_POLARITY", true);
  node->reset_value = ParameterIsOne(node->parameters, "ARST_VALUE", false);
  node->enable_positive = ParameterIsOne(node->parameters, "EN_POLARITY", true);
  if (node->type.starts_with("$_DFF_") && node->type.size() >= 9U) {
    node->clock_positive = node->type[6] != 'N';
    node->reset_positive = node->type[7] != 'N';
    node->reset_value = node->type[8] == '1';
  } else if (node->type.starts_with("$_DFFE_") && node->type.size() >= 11U) {
    node->clock_positive = node->type[7] != 'N';
    node->reset_positive = node->type[8] != 'N';
    node->reset_value = node->type[9] == '1';
    node->enable_positive = node->type[10] != 'N';
  }
}

bool ReadBits(const JsonValue::Object& object, std::vector<std::string>* bits) {
  const JsonValue* value = Member(object, "bits");
  const auto* array =
      value == nullptr ? nullptr : value->Get<JsonValue::Array>();
  if (array == nullptr || array->size() > 1'000'000) return false;
  bits->reserve(array->size());
  for (const JsonValue& item : *array) {
    if (const JsonNumber* number = item.Get<JsonNumber>()) {
      bits->push_back(number->text);
    } else if (const std::string* constant = item.Get<std::string>()) {
      bits->push_back("const:" + *constant);
    } else {
      return false;
    }
  }
  return true;
}

}  // namespace

runtime::WslCommand YosysAdapter::BuildProbeCommand() const {
  runtime::WslCommand command;
  command.program = L"yosys";
  command.arguments.push_back(L"-V");
  return command;
}

core::Status YosysAdapter::Validate(const SynthesisRequest& request) const {
  if (!IsIdentifier(request.project.top_module)) {
    return {core::ErrorCode::kInvalidArgument, "Top module is invalid", 0};
  }
  if (request.artifact_directory.empty()) {
    return {core::ErrorCode::kInvalidArgument,
            "Synthesis artifact directory is missing", 0};
  }
  std::vector<std::filesystem::path> rtl_files;
  for (const application::ResolvedSource& source : request.sources) {
    if (!source.enabled || source.view_kind != core::ViewKind::kVerilog) {
      continue;
    }
    if (!source.exists) {
      return {core::ErrorCode::kNotFound,
              "Enabled RTL source is missing: " + source.relative_path, 0};
    }
    rtl_files.push_back(source.windows_path);
  }
  if (rtl_files.empty()) {
    return {core::ErrorCode::kInvalidArgument,
            "At least one enabled RTL source is required", 0};
  }
  const auto modules =
      application::SystemVerilogModuleScanner().FindModuleNames(rtl_files);
  if (std::find(modules.begin(), modules.end(), request.project.top_module) ==
      modules.end()) {
    return {core::ErrorCode::kInvalidArgument,
            "Top module is not declared by the enabled RTL sources", 0};
  }
  for (const std::filesystem::path& liberty : request.liberty_files) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(liberty, error) || error) {
      return {core::ErrorCode::kNotFound, "Liberty file is missing", 0};
    }
  }
  return core::Status::Success();
}

core::Result<SynthesisPlan> YosysAdapter::BuildPlan(
    const SynthesisRequest& request,
    const runtime::PathMapper& path_mapper) const {
  const core::Status validation = Validate(request);
  if (!validation.Ok()) return validation;
  SynthesisPlan plan;
  plan.script_path = request.artifact_directory / L"synthesis.ys";
  plan.structural_schematic_path =
      request.artifact_directory / L"schematic-structural.json";
  plan.json_netlist_path = request.artifact_directory / L"netlist.json";
  plan.verilog_netlist_path = request.artifact_directory / L"netlist.v";
  plan.statistics_path = request.artifact_directory / L"statistics.json";
  plan.report_path = request.artifact_directory / L"synthesis.rpt";
  auto working = path_mapper.WindowsToWsl(request.artifact_directory);
  if (!working.Ok()) return working.GetStatus();

  std::ostringstream read;
  read << "read_verilog -sv";
  for (const std::string& define : request.project.defines) {
    read << ' ' << Quote(Utf8ToWide("-D" + define));
  }
  for (const std::string& directory : request.project.include_directories) {
    auto mapped = path_mapper.WindowsToWsl(
        request.sources.front().library_directory / Utf8ToWide(directory));
    if (!mapped.Ok()) return mapped.GetStatus();
    read << " -I" << Quote(mapped.Value());
  }
  for (const application::ResolvedSource& source : request.sources) {
    if (!source.enabled || source.view_kind != core::ViewKind::kVerilog) {
      continue;
    }
    auto mapped = path_mapper.WindowsToWsl(source.windows_path);
    if (!mapped.Ok()) return mapped.GetStatus();
    read << ' ' << Quote(mapped.Value());
  }
  plan.script_text = read.str() + "\n";
  plan.script_text +=
      "hierarchy -check -top " + request.project.top_module + "\n";
  plan.script_text += "design -save designpp_rtl\n";
  plan.script_text += "synth -top " + request.project.top_module + " -noabc";
  if (request.flatten) plan.script_text += " -flatten";
  plan.script_text += "\n";
  if (request.liberty_files.empty()) {
    plan.script_text += "abc -g AND,OR,XOR,XNOR,NAND,NOR\n";
  } else {
    auto liberty = path_mapper.WindowsToWsl(request.liberty_files.front());
    if (!liberty.Ok()) return liberty.GetStatus();
    plan.script_text += "dfflibmap -liberty " + Quote(liberty.Value()) + "\n";
    plan.script_text += "abc -liberty " + Quote(liberty.Value()) + "\n";
  }
  plan.script_text += "check\n";
  plan.script_text += "write_json netlist.json\n";
  plan.script_text += "write_verilog -noattr netlist.v\n";
  std::string liberty_option;
  if (!request.liberty_files.empty()) {
    auto liberty = path_mapper.WindowsToWsl(request.liberty_files.front());
    if (!liberty.Ok()) return liberty.GetStatus();
    liberty_option = " -liberty " + Quote(liberty.Value());
  }
  plan.script_text += "tee -o synthesis.rpt stat -top " +
                      request.project.top_module + liberty_option + "\n";
  plan.script_text += "tee -o statistics.json stat -json -top " +
                      request.project.top_module + liberty_option + "\n";
  plan.script_text += "design -load designpp_rtl\nproc\nopt\n";
  if (request.flatten) plan.script_text += "flatten\n";
  plan.script_text += "write_json schematic-structural.json\n";
  plan.execute.program = L"yosys";
  plan.execute.working_directory = working.Value();
  plan.execute.arguments = {L"-Q", L"-T", L"-s", L"synthesis.ys"};
  return plan;
}

std::vector<core::Diagnostic> YosysAdapter::ParseDiagnostics(
    std::string_view raw_output) const {
  std::vector<core::Diagnostic> diagnostics;
  std::istringstream lines{std::string(raw_output)};
  std::string line;
  while (std::getline(lines, line)) {
    const bool error = line.starts_with("ERROR:");
    const bool warning =
        line.starts_with("Warning:") || line.starts_with("WARNING:");
    if (!error && !warning) continue;
    core::Diagnostic diagnostic;
    diagnostic.severity = error ? core::DiagnosticSeverity::kError
                                : core::DiagnosticSeverity::kWarning;
    diagnostic.code = error ? "YOSYS" : "YOSYS-WARNING";
    diagnostic.message = line.substr(line.find(':') + 1);
    while (!diagnostic.message.empty() && diagnostic.message.front() == ' ') {
      diagnostic.message.erase(diagnostic.message.begin());
    }
    diagnostics.push_back(std::move(diagnostic));
  }
  return diagnostics;
}

core::Result<SynthesisMetrics> YosysAdapter::ParseStatistics(
    std::string_view json) const {
  if (json.empty() || json.size() > 16 * 1024 * 1024) {
    return core::Status{json.empty() ? core::ErrorCode::kCorruptData
                                     : core::ErrorCode::kFileTooLarge,
                        "Yosys statistics size is invalid", 0};
  }
  const auto cell_count = NumberValue(json, "num_cells");
  if (!cell_count) {
    return core::Status{core::ErrorCode::kCorruptData,
                        "Yosys statistics do not contain num_cells", 0};
  }
  SynthesisMetrics metrics;
  const auto parsed = std::from_chars(cell_count->data(),
                                      cell_count->data() + cell_count->size(),
                                      metrics.cell_count);
  if (parsed.ec != std::errc() ||
      parsed.ptr != cell_count->data() + cell_count->size()) {
    return core::Status{core::ErrorCode::kCorruptData,
                        "Yosys cell count is invalid", 0};
  }
  const auto area = NumberValue(json, "area");
  if (area && *area != "null") {
    const auto parsed_area = std::from_chars(
        area->data(), area->data() + area->size(), metrics.area);
    metrics.has_area = parsed_area.ec == std::errc() &&
                       parsed_area.ptr == area->data() + area->size() &&
                       metrics.area >= 0.0;
  }
  return metrics;
}

core::Result<core::SchematicModel> YosysAdapter::ParseNetlist(
    std::string_view json, std::string_view top_module) const {
  if (json.empty() || json.size() > 32 * 1024 * 1024) {
    return core::Status{json.empty() ? core::ErrorCode::kCorruptData
                                     : core::ErrorCode::kFileTooLarge,
                        "Yosys netlist size is invalid", 0};
  }
  auto parsed = JsonParser(json).Parse();
  const auto* root = parsed ? parsed->Get<JsonValue::Object>() : nullptr;
  const auto* modules =
      root == nullptr ? nullptr : ObjectMember(*root, "modules");
  const JsonValue* module_value =
      modules == nullptr ? nullptr : Member(*modules, top_module);
  const auto* module = module_value == nullptr
                           ? nullptr
                           : module_value->Get<JsonValue::Object>();
  if (module == nullptr) {
    return core::Status{core::ErrorCode::kCorruptData,
                        "Yosys netlist does not contain the top module", 0};
  }

  const auto* ports = ObjectMember(*module, "ports");
  const auto* cells = ObjectMember(*module, "cells");
  if (ports == nullptr || cells == nullptr || ports->size() > 10'000 ||
      cells->size() > 100'000) {
    return core::Status{core::ErrorCode::kCorruptData,
                        "Yosys netlist ports or cells are invalid", 0};
  }

  core::SchematicModel schematic;
  schematic.top_module = std::string(top_module);
  schematic.ports.reserve(ports->size());
  for (const auto& [name, value] : *ports) {
    const auto* object = value.Get<JsonValue::Object>();
    const auto direction_text =
        object == nullptr ? std::nullopt : StringMember(*object, "direction");
    const auto direction =
        direction_text ? ParseDirection(*direction_text) : std::nullopt;
    core::SchematicPort port;
    if (object == nullptr || !direction || !ReadBits(*object, &port.bits)) {
      return core::Status{core::ErrorCode::kCorruptData,
                          "Yosys module port is malformed", 0};
    }
    port.name = name;
    port.direction = *direction;
    schematic.ports.push_back(std::move(port));
  }

  const auto* netnames = ObjectMember(*module, "netnames");
  if (netnames != nullptr) {
    if (netnames->size() > 1'000'000) {
      return core::Status{core::ErrorCode::kCorruptData,
                          "Yosys net names are invalid", 0};
    }
    schematic.signals.reserve(netnames->size());
    for (const auto& [name, value] : *netnames) {
      const auto* object = value.Get<JsonValue::Object>();
      core::SchematicSignal signal;
      if (object == nullptr || !ReadBits(*object, &signal.bits)) {
        return core::Status{core::ErrorCode::kCorruptData,
                            "Yosys net name is malformed", 0};
      }
      signal.name = name;
      signal.offset = IntMember(*object, "offset").value_or(0);
      signal.upto = IntMember(*object, "upto").value_or(0) != 0;
      signal.hidden = IntMember(*object, "hide_name").value_or(0) != 0;
      schematic.signals.push_back(std::move(signal));
    }
  }

  schematic.nodes.reserve(cells->size());
  for (const auto& [name, value] : *cells) {
    const auto* object = value.Get<JsonValue::Object>();
    const auto type =
        object == nullptr ? std::nullopt : StringMember(*object, "type");
    const auto* directions =
        object == nullptr ? nullptr : ObjectMember(*object, "port_directions");
    const auto* connections =
        object == nullptr ? nullptr : ObjectMember(*object, "connections");
    if (!type || directions == nullptr || connections == nullptr ||
        connections->size() > 10'000) {
      return core::Status{core::ErrorCode::kCorruptData,
                          "Yosys cell is malformed", 0};
    }
    core::SchematicNode cell;
    cell.id = name;
    cell.type = *type;
    cell.kind = Member(*modules, *type) != nullptr
                    ? core::SchematicNodeKind::kModule
                    : ClassifyNode(*type);
    cell.label = NodeLabel(*type, cell.kind);
    cell.parameters = ReadScalarObject(ObjectMember(*object, "parameters"));
    cell.attributes = ReadScalarObject(ObjectMember(*object, "attributes"));
    cell.pins.reserve(connections->size());
    for (const auto& [port_name, connection] : *connections) {
      const auto direction_text = StringMember(*directions, port_name);
      const auto direction =
          direction_text ? ParseDirection(*direction_text) : std::nullopt;
      const auto* connection_bits = connection.Get<JsonValue::Array>();
      if (!direction || connection_bits == nullptr) {
        return core::Status{core::ErrorCode::kCorruptData,
                            "Yosys cell connection is malformed", 0};
      }
      JsonValue::Object bit_container;
      bit_container.emplace("bits", connection);
      core::SchematicPin port;
      if (!ReadBits(bit_container, &port.bits)) {
        return core::Status{core::ErrorCode::kCorruptData,
                            "Yosys cell connection bits are malformed", 0};
      }
      port.name = port_name;
      port.direction = *direction;
      port.role = PinRole(port_name, *direction);
      cell.pins.push_back(std::move(port));
    }
    DecodeSequentialPolarity(&cell);
    schematic.nodes.push_back(std::move(cell));
  }
  return schematic;
}

}  // namespace designpp::adapters

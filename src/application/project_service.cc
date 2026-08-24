// Copyright 2026 The Design++ Authors

#include "designpp/application/project_service.h"

#include <combaseapi.h>
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <thread>
#include <utility>
#include <variant>

namespace designpp::application {
namespace {

using core::ErrorCode;
using core::Status;

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

std::filesystem::path Utf8Path(std::string_view text) {
  std::u8string value;
  value.reserve(text.size());
  for (char character : text) value.push_back(static_cast<char8_t>(character));
  return std::filesystem::path(value);
}

std::string NewUuid() {
  GUID guid{};
  if (CoCreateGuid(&guid) != S_OK) return {};
  wchar_t buffer[40]{};
  const int uuid_length =
      StringFromGUID2(guid, buffer, static_cast<int>(std::size(buffer)));
  if (uuid_length == 0) {
    return {};
  }
  std::string value = WideToUtf8(buffer);
  value.erase(std::remove(value.begin(), value.end(), '{'), value.end());
  value.erase(std::remove(value.begin(), value.end(), '}'), value.end());
  std::transform(value.begin(), value.end(), value.begin(), [](char character) {
    return static_cast<char>(
        std::tolower(static_cast<unsigned char>(character)));
  });
  return value;
}

std::string UtcNow() {
  SYSTEMTIME time{};
  GetSystemTime(&time);
  char buffer[32]{};
  std::snprintf(buffer, sizeof(buffer), "%04u-%02u-%02uT%02u:%02u:%02uZ",
                time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
                time.wSecond);
  return buffer;
}

class JsonValue final {
 public:
  using Object = std::map<std::string, JsonValue>;
  using Array = std::vector<JsonValue>;
  using Value = std::variant<std::uint64_t, std::string, Object, Array>;

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
    SkipSpace();
    return value && position_ == input_.size() ? std::move(value)
                                               : std::nullopt;
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

  std::optional<std::string> ParseString() {
    if (!Take('"')) return std::nullopt;
    std::string value;
    while (position_ < input_.size()) {
      const char character = input_[position_++];
      if (character == '"') return value;
      if (character == '\\') {
        if (position_ >= input_.size()) return std::nullopt;
        const char escaped = input_[position_++];
        switch (escaped) {
          case '"':
          case '\\':
          case '/':
            value.push_back(escaped);
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
      } else {
        if (static_cast<unsigned char>(character) < 0x20) return std::nullopt;
        value.push_back(character);
      }
    }
    return std::nullopt;
  }

  std::optional<JsonValue> ParseValue() {
    SkipSpace();
    if (position_ >= input_.size()) return std::nullopt;
    if (input_[position_] == '{') return ParseObject();
    if (input_[position_] == '[') return ParseArray();
    if (input_[position_] == '"') {
      auto value = ParseString();
      return value ? std::optional<JsonValue>(JsonValue(std::move(*value)))
                   : std::nullopt;
    }
    const std::size_t start = position_;
    while (position_ < input_.size() &&
           std::isdigit(static_cast<unsigned char>(input_[position_]))) {
      ++position_;
    }
    if (start == position_) return std::nullopt;
    std::uint64_t number = 0;
    const auto parsed = std::from_chars(input_.data() + start,
                                        input_.data() + position_, number);
    return parsed.ec == std::errc()
               ? std::optional<JsonValue>(JsonValue(number))
               : std::nullopt;
  }

  std::optional<JsonValue> ParseObject() {
    if (!Take('{')) return std::nullopt;
    JsonValue::Object object;
    SkipSpace();
    if (Take('}')) return JsonValue(std::move(object));
    for (;;) {
      auto key = ParseString();
      if (!key || !Take(':')) return std::nullopt;
      auto value = ParseValue();
      if (!value ||
          !object.emplace(std::move(*key), std::move(*value)).second) {
        return std::nullopt;
      }
      if (Take('}')) return JsonValue(std::move(object));
      if (!Take(',')) return std::nullopt;
    }
  }

  std::optional<JsonValue> ParseArray() {
    if (!Take('[')) return std::nullopt;
    JsonValue::Array array;
    SkipSpace();
    if (Take(']')) return JsonValue(std::move(array));
    for (;;) {
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
  const auto iterator = object.find(std::string(key));
  return iterator == object.end() ? nullptr : &iterator->second;
}

bool ReadString(const JsonValue::Object& object, std::string_view key,
                std::string* output) {
  const JsonValue* value = Member(object, key);
  const std::string* text = value ? value->Get<std::string>() : nullptr;
  if (text == nullptr) return false;
  *output = *text;
  return true;
}

bool ReadNumber(const JsonValue::Object& object, std::string_view key,
                std::uint64_t* output) {
  const JsonValue* value = Member(object, key);
  const std::uint64_t* number = value ? value->Get<std::uint64_t>() : nullptr;
  if (number == nullptr) return false;
  *output = *number;
  return true;
}

bool ReadStringArray(const JsonValue::Object& object, std::string_view key,
                     std::vector<std::string>* output) {
  const JsonValue* value = Member(object, key);
  const auto* array = value ? value->Get<JsonValue::Array>() : nullptr;
  if (array == nullptr) return false;
  for (const JsonValue& item : *array) {
    const std::string* text = item.Get<std::string>();
    if (text == nullptr) return false;
    output->push_back(*text);
  }
  return true;
}

std::string EscapeJson(std::string_view value) {
  std::string result;
  for (char character : value) {
    switch (character) {
      case '"':
        result += "\\\"";
        break;
      case '\\':
        result += "\\\\";
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

core::Result<core::Project> DecodeProject(std::string_view json) {
  auto parsed = JsonParser(json).Parse();
  const auto* root = parsed ? parsed->Get<JsonValue::Object>() : nullptr;
  if (root == nullptr)
    return Status{ErrorCode::kCorruptData, "Invalid JSON", 0};
  core::Project project;
  std::uint64_t schema = 0;
  std::uint64_t cpu_budget = 0;
  std::string source_policy;
  std::string constraint_path;
  std::string toolchain_profile_id;
  if (!ReadNumber(*root, "schema_version", &schema) ||
      !ReadString(*root, "project_id", &project.id) ||
      !ReadNumber(*root, "revision", &project.revision) ||
      !ReadString(*root, "library_id", &project.library_id) ||
      !ReadString(*root, "cell_id", &project.cell_id) ||
      !ReadString(*root, "name", &project.name) ||
      !ReadString(*root, "top_module", &project.top_module) ||
      !ReadString(*root, "created_utc", &project.created_utc) ||
      !ReadString(*root, "modified_utc", &project.modified_utc) ||
      !ReadString(*root, "source_policy", &source_policy) ||
      !ReadString(*root, "constraint_path", &constraint_path) ||
      !ReadString(*root, "toolchain_profile_id", &toolchain_profile_id) ||
      !ReadNumber(*root, "cpu_budget", &cpu_budget) ||
      !ReadStringArray(*root, "include_directories",
                       &project.include_directories) ||
      !ReadStringArray(*root, "defines", &project.defines) ||
      !ReadStringArray(*root, "parameters", &project.parameters)) {
    return Status{ErrorCode::kCorruptData,
                  "Required project manifest field is missing", 0};
  }
  if (schema < 1 || schema > core::Project::kSchemaVersion) {
    return Status{ErrorCode::kUnsupportedSchema, "Unsupported project schema",
                  0};
  }
  if (source_policy != "auto_managed") {
    return Status{ErrorCode::kCorruptData, "Unknown source policy", 0};
  }
  // Schema v1 is migrated in memory and is only persisted by an explicit save.
  project.schema_version = core::Project::kSchemaVersion;
  project.cpu_budget = static_cast<std::uint32_t>(cpu_budget);
  if (!constraint_path.empty()) project.constraint_path = constraint_path;
  if (!toolchain_profile_id.empty()) {
    project.toolchain_profile_id = toolchain_profile_id;
  }
  const JsonValue* overrides_value = Member(*root, "source_overrides");
  const auto* overrides =
      overrides_value ? overrides_value->Get<JsonValue::Array>() : nullptr;
  if (overrides == nullptr) {
    return Status{ErrorCode::kCorruptData, "source_overrides is invalid", 0};
  }
  for (const JsonValue& value : *overrides) {
    const auto* object = value.Get<JsonValue::Object>();
    core::SourceOverride override_value;
    std::uint64_t enabled = 0;
    if (object == nullptr ||
        !ReadString(*object, "view_id", &override_value.view_id) ||
        !ReadString(*object, "relative_path", &override_value.relative_path) ||
        !ReadNumber(*object, "enabled", &enabled) ||
        !ReadString(*object, "role", &override_value.role) || enabled > 1) {
      return Status{ErrorCode::kCorruptData, "Source override is invalid", 0};
    }
    override_value.enabled = enabled == 1;
    project.source_overrides.push_back(std::move(override_value));
  }
  if (schema >= 2) {
    const JsonValue* configurations_value =
        Member(*root, "testbench_configurations");
    const auto* configurations =
        configurations_value ? configurations_value->Get<JsonValue::Array>()
                             : nullptr;
    if (configurations == nullptr) {
      return Status{ErrorCode::kCorruptData,
                    "testbench_configurations is invalid", 0};
    }
    for (const JsonValue& value : *configurations) {
      const auto* object = value.Get<JsonValue::Object>();
      core::TestbenchConfiguration configuration;
      std::uint64_t waveform_enabled = 0;
      if (object == nullptr ||
          !ReadString(*object, "view_id", &configuration.view_id) ||
          !ReadString(*object, "relative_path", &configuration.relative_path) ||
          !ReadString(*object, "top_module", &configuration.top_module) ||
          !ReadString(*object, "backend", &configuration.backend) ||
          !ReadNumber(*object, "waveform_enabled", &waveform_enabled) ||
          waveform_enabled > 1) {
        return Status{ErrorCode::kCorruptData,
                      "Testbench configuration is invalid", 0};
      }
      configuration.waveform_enabled = waveform_enabled == 1;
      if (schema >= 3) {
        if (!ReadString(*object, "runner", &configuration.runner) ||
            !ReadString(*object, "cocotb_module",
                        &configuration.cocotb_module) ||
            !ReadString(*object, "cocotb_testcase",
                        &configuration.cocotb_testcase) ||
            !ReadString(*object, "waveform_format",
                        &configuration.waveform_format)) {
          return Status{ErrorCode::kCorruptData,
                        "Testbench configuration is invalid", 0};
        }
      } else {
        configuration.runner = "hdl";
        configuration.waveform_format =
            configuration.waveform_enabled ? "vcd" : "none";
      }
      project.testbench_configurations.push_back(std::move(configuration));
    }
  }
  if (schema >= 4) {
    const JsonValue* synthesis_value = Member(*root, "synthesis");
    const auto* synthesis =
        synthesis_value ? synthesis_value->Get<JsonValue::Object>() : nullptr;
    const JsonValue* timing_value = Member(*root, "timing");
    const auto* timing =
        timing_value ? timing_value->Get<JsonValue::Object>() : nullptr;
    std::uint64_t flatten = 0;
    if (synthesis == nullptr || timing == nullptr ||
        !ReadStringArray(*synthesis, "liberty_paths",
                         &project.synthesis.liberty_paths) ||
        !ReadNumber(*synthesis, "flatten", &flatten) || flatten > 1 ||
        !ReadString(*timing, "corner_name", &project.timing.corner_name) ||
        !ReadStringArray(*timing, "liberty_paths",
                         &project.timing.liberty_paths) ||
        !ReadString(*timing, "sdc_path", &project.timing.sdc_path)) {
      return Status{ErrorCode::kCorruptData,
                    "Synthesis or timing configuration is invalid", 0};
    }
    project.synthesis.flatten = flatten == 1;
  }
  if (schema >= 5) {
    const char* configuration_name =
        schema >= 6 ? "physical_implementation" : "openlane";
    const JsonValue* implementation_value = Member(*root, configuration_name);
    const auto* implementation =
        implementation_value ? implementation_value->Get<JsonValue::Object>()
                             : nullptr;
    core::PhysicalImplementationConfiguration& configuration =
        project.physical_implementation;
    std::uint64_t utilization = 0;
    std::string placement_density;
    if (implementation == nullptr ||
        (schema >= 6 && !ReadString(*implementation, "backend_id",
                                    &configuration.backend_id)) ||
        !ReadString(*implementation, "pdk", &configuration.pdk) ||
        !ReadString(*implementation, "standard_cell_library",
                    &configuration.standard_cell_library) ||
        !ReadStringArray(*implementation, "clock_ports",
                         &configuration.clock_ports) ||
        !ReadString(*implementation, "clock_period_ns",
                    &configuration.clock_period_ns) ||
        !ReadNumber(*implementation, "core_utilization_percent",
                    &utilization) ||
        !ReadString(*implementation, "placement_density_percent",
                    &placement_density) ||
        !ReadStringArray(*implementation, "die_area",
                         &configuration.die_area) ||
        !ReadString(*implementation, "pnr_sdc_path",
                    &configuration.pnr_sdc_path) ||
        !ReadString(*implementation, "signoff_sdc_path",
                    &configuration.signoff_sdc_path) ||
        !ReadString(*implementation, "advanced_overrides_json",
                    &configuration.advanced_overrides_json)) {
      return Status{ErrorCode::kCorruptData,
                    "Physical implementation configuration is invalid", 0};
    }
    configuration.core_utilization_percent =
        static_cast<std::uint32_t>(utilization);
    if (!placement_density.empty()) {
      configuration.placement_density_percent = placement_density;
    }
  }
  Status validation = core::ValidateProject(project);
  return validation.Ok() ? core::Result<core::Project>(std::move(project))
                         : core::Result<core::Project>(std::move(validation));
}

std::string EncodeProject(const core::Project& project) {
  std::ostringstream output;
  auto quote = [&output](std::string_view value) {
    output << '"' << EscapeJson(value) << '"';
  };
  auto string_array = [&output, &quote](
                          std::string_view name,
                          const std::vector<std::string>& values) {
    output << ",\n  \"" << name << "\": [";
    for (std::size_t index = 0; index < values.size(); ++index) {
      if (index != 0) output << ", ";
      quote(values[index]);
    }
    output << ']';
  };
  output << "{\n  \"schema_version\": " << project.schema_version
         << ",\n  \"project_id\": ";
  quote(project.id);
  output << ",\n  \"revision\": " << project.revision
         << ",\n  \"library_id\": ";
  quote(project.library_id);
  output << ",\n  \"cell_id\": ";
  quote(project.cell_id);
  output << ",\n  \"name\": ";
  quote(project.name);
  output << ",\n  \"top_module\": ";
  quote(project.top_module);
  output << ",\n  \"created_utc\": ";
  quote(project.created_utc);
  output << ",\n  \"modified_utc\": ";
  quote(project.modified_utc);
  output << ",\n  \"source_policy\": \"auto_managed\""
         << ",\n  \"cpu_budget\": " << project.cpu_budget
         << ",\n  \"constraint_path\": ";
  quote(project.constraint_path.value_or(""));
  output << ",\n  \"toolchain_profile_id\": ";
  quote(project.toolchain_profile_id.value_or(""));
  string_array("include_directories", project.include_directories);
  string_array("defines", project.defines);
  string_array("parameters", project.parameters);
  output << ",\n  \"source_overrides\": [";
  for (std::size_t index = 0; index < project.source_overrides.size();
       ++index) {
    const core::SourceOverride& value = project.source_overrides[index];
    output << (index == 0 ? "" : ",") << "\n    {\"view_id\": ";
    quote(value.view_id);
    output << ", \"relative_path\": ";
    quote(value.relative_path);
    output << ", \"enabled\": " << (value.enabled ? 1 : 0) << ", \"role\": ";
    quote(value.role);
    output << '}';
  }
  output << "\n  ],\n  \"testbench_configurations\": [";
  for (std::size_t index = 0; index < project.testbench_configurations.size();
       ++index) {
    const core::TestbenchConfiguration& value =
        project.testbench_configurations[index];
    output << (index == 0 ? "" : ",") << "\n    {\"view_id\": ";
    quote(value.view_id);
    output << ", \"relative_path\": ";
    quote(value.relative_path);
    output << ", \"top_module\": ";
    quote(value.top_module);
    output << ", \"backend\": ";
    quote(value.backend);
    output << ", \"waveform_enabled\": " << (value.waveform_enabled ? 1 : 0)
           << ", \"runner\": ";
    quote(value.runner);
    output << ", \"cocotb_module\": ";
    quote(value.cocotb_module);
    output << ", \"cocotb_testcase\": ";
    quote(value.cocotb_testcase);
    output << ", \"waveform_format\": ";
    quote(value.waveform_format);
    output << '}';
  }
  output << "\n  ],\n  \"synthesis\": {\"liberty_paths\": [";
  for (std::size_t index = 0; index < project.synthesis.liberty_paths.size();
       ++index) {
    if (index != 0) output << ", ";
    quote(project.synthesis.liberty_paths[index]);
  }
  output << "], \"flatten\": " << (project.synthesis.flatten ? 1 : 0)
         << "},\n  \"timing\": {\"corner_name\": ";
  quote(project.timing.corner_name);
  output << ", \"liberty_paths\": [";
  for (std::size_t index = 0; index < project.timing.liberty_paths.size();
       ++index) {
    if (index != 0) output << ", ";
    quote(project.timing.liberty_paths[index]);
  }
  output << "], \"sdc_path\": ";
  quote(project.timing.sdc_path);
  const core::PhysicalImplementationConfiguration& implementation =
      project.physical_implementation;
  output << "},\n  \"physical_implementation\": {\"backend_id\": ";
  quote(implementation.backend_id);
  output << ", \"pdk\": ";
  quote(implementation.pdk);
  output << ", \"standard_cell_library\": ";
  quote(implementation.standard_cell_library);
  output << ", \"clock_ports\": [";
  for (std::size_t index = 0; index < implementation.clock_ports.size();
       ++index) {
    if (index != 0) output << ", ";
    quote(implementation.clock_ports[index]);
  }
  output << "], \"clock_period_ns\": ";
  quote(implementation.clock_period_ns);
  output << ", \"core_utilization_percent\": "
         << implementation.core_utilization_percent
         << ", \"placement_density_percent\": ";
  quote(implementation.placement_density_percent.value_or(""));
  output << ", \"die_area\": [";
  for (std::size_t index = 0; index < implementation.die_area.size(); ++index) {
    if (index != 0) output << ", ";
    quote(implementation.die_area[index]);
  }
  output << "], \"pnr_sdc_path\": ";
  quote(implementation.pnr_sdc_path);
  output << ", \"signoff_sdc_path\": ";
  quote(implementation.signoff_sdc_path);
  output << ", \"advanced_overrides_json\": ";
  quote(implementation.advanced_overrides_json);
  output << "}\n}\n";
  return output.str();
}

Status WriteAtomic(const std::filesystem::path& path, std::string_view bytes) {
  std::error_code filesystem_error;
  std::filesystem::create_directories(path.parent_path(), filesystem_error);
  if (filesystem_error) {
    return {ErrorCode::kIoError, "Cannot create project directory",
            static_cast<unsigned long>(filesystem_error.value())};
  }
  const std::filesystem::path temporary =
      path.parent_path() / (L".project-" + Utf8ToWide(NewUuid()) + L".tmp");
  HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return {ErrorCode::kIoError, "Cannot create temporary project",
            GetLastError()};
  }
  DWORD written = 0;
  const bool succeeded =
      bytes.size() <= MAXDWORD &&
      WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written,
                nullptr) &&
      written == bytes.size() && FlushFileBuffers(file);
  const DWORD error = succeeded ? ERROR_SUCCESS : GetLastError();
  CloseHandle(file);
  if (!succeeded) {
    DeleteFileW(temporary.c_str());
    return {ErrorCode::kIoError, "Cannot write project manifest", error};
  }
  const std::filesystem::path backup = path.wstring() + L".bak";
  const bool exists =
      GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
  const bool replaced =
      exists
          ? ReplaceFileW(path.c_str(), temporary.c_str(), backup.c_str(),
                         REPLACEFILE_WRITE_THROUGH, nullptr, nullptr) != FALSE
          : MoveFileExW(temporary.c_str(), path.c_str(),
                        MOVEFILE_WRITE_THROUGH) != FALSE;
  if (!replaced) {
    const DWORD replace_error = GetLastError();
    DeleteFileW(temporary.c_str());
    return {ErrorCode::kIoError, "Cannot atomically replace project",
            replace_error};
  }
  return Status::Success();
}

const core::Cell* FindCell(const LibraryRecord& library,
                           std::string_view cell_id) {
  const auto iterator = std::find_if(
      library.library.cells.begin(), library.library.cells.end(),
      [cell_id](const core::Cell& cell) { return cell.id == cell_id; });
  return iterator == library.library.cells.end() ? nullptr : &*iterator;
}

core::Project DefaultProject(const LibraryRecord& library,
                             const core::Cell& cell) {
  core::Project project;
  project.id = NewUuid();
  project.library_id = library.library.id;
  project.cell_id = cell.id;
  project.name = cell.name;
  project.created_utc = UtcNow();
  project.modified_utc = project.created_utc;
  const unsigned int logical_cpus = std::thread::hardware_concurrency();
  project.cpu_budget = logical_cpus > 1 ? logical_cpus - 1 : 1;
  return project;
}

bool IsManagedSourceKind(core::ViewKind kind) {
  return kind == core::ViewKind::kVerilog ||
         kind == core::ViewKind::kTestbench ||
         kind == core::ViewKind::kConstraints;
}

bool IsLegacyToolInputKind(core::ViewKind kind) {
  return kind == core::ViewKind::kSynthesis || kind == core::ViewKind::kTiming;
}

bool IsTimingInputFile(const core::ManagedFile& file) {
  std::string extension =
      std::filesystem::path(Utf8Path(file.relative_path)).extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return extension == ".lib" || extension == ".liberty" || extension == ".sdc";
}

bool IsLibertyFile(const core::ManagedFile& file) {
  std::string extension =
      std::filesystem::path(Utf8Path(file.relative_path)).extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return extension == ".lib" || extension == ".liberty";
}

bool IsWithin(const std::filesystem::path& root,
              const std::filesystem::path& candidate) {
  auto root_iterator = root.begin();
  auto candidate_iterator = candidate.begin();
  while (root_iterator != root.end()) {
    if (candidate_iterator == candidate.end() ||
        *root_iterator != *candidate_iterator) {
      return false;
    }
    ++root_iterator;
    ++candidate_iterator;
  }
  return true;
}

}  // namespace

struct ProjectWriterLease::Implementation final {
  HANDLE handle = INVALID_HANDLE_VALUE;
};

ProjectWriterLease::ProjectWriterLease() = default;

ProjectWriterLease::ProjectWriterLease(
    std::unique_ptr<Implementation> implementation)
    : implementation_(std::move(implementation)) {}

ProjectWriterLease::ProjectWriterLease(ProjectWriterLease&&) noexcept = default;
ProjectWriterLease& ProjectWriterLease::operator=(
    ProjectWriterLease&&) noexcept = default;

ProjectWriterLease::~ProjectWriterLease() {
  if (implementation_ && implementation_->handle != INVALID_HANDLE_VALUE) {
    OVERLAPPED overlapped{};
    UnlockFileEx(implementation_->handle, 0, 1, 0, &overlapped);
    CloseHandle(implementation_->handle);
  }
}

std::unique_ptr<ProjectWriterLease> ProjectWriterLease::TryAcquire(
    const std::filesystem::path& path) {
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  if (error) return nullptr;
  auto implementation = std::make_unique<Implementation>();
  implementation->handle =
      CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                  nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
  if (implementation->handle == INVALID_HANDLE_VALUE) return nullptr;
  OVERLAPPED overlapped{};
  if (!LockFileEx(implementation->handle,
                  LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0,
                  &overlapped)) {
    CloseHandle(implementation->handle);
    implementation->handle = INVALID_HANDLE_VALUE;
    return nullptr;
  }
  const std::string metadata = "pid=" + std::to_string(GetCurrentProcessId()) +
                               "\nacquired_utc=" + UtcNow() + "\n";
  SetFilePointer(implementation->handle, 0, nullptr, FILE_BEGIN);
  SetEndOfFile(implementation->handle);
  DWORD written = 0;
  static_cast<void>(WriteFile(implementation->handle, metadata.data(),
                              static_cast<DWORD>(metadata.size()), &written,
                              nullptr));
  static_cast<void>(FlushFileBuffers(implementation->handle));
  return std::unique_ptr<ProjectWriterLease>(
      new ProjectWriterLease(std::move(implementation)));
}

bool ProjectWriterLease::Acquired() const noexcept {
  return implementation_ && implementation_->handle != INVALID_HANDLE_VALUE;
}

core::Result<core::Project> ProjectStore::Load(
    const std::filesystem::path& path) const {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return Status{ErrorCode::kNotFound, "project.dpproj is missing", 0};
  }
  const std::string bytes((std::istreambuf_iterator<char>(input)),
                          std::istreambuf_iterator<char>());
  return DecodeProject(bytes);
}

Status ProjectStore::Save(const core::Project& project,
                          const std::filesystem::path& path,
                          std::uint64_t expected_revision) const {
  Status validation = core::ValidateProject(project);
  if (!validation.Ok()) return validation;
  if (expected_revision > 0) {
    auto current = Load(path);
    if (!current.Ok()) return current.GetStatus();
    if (current.Value().revision != expected_revision) {
      return {ErrorCode::kConflict, "Project revision changed", 0};
    }
  }
  return WriteAtomic(path, EncodeProject(project));
}

core::Result<ProjectDocument> ProjectService::OpenOrCreate(
    const LibraryRecord& library, std::string_view cell_id) const {
  const core::Cell* cell = FindCell(library, cell_id);
  if (cell == nullptr) {
    return Status{ErrorCode::kNotFound, "Cell does not exist", 0};
  }
  const std::filesystem::path cell_directory =
      library.directory / L"cells" / Utf8ToWide(cell_id);
  const std::filesystem::path project_path = cell_directory / L"project.dpproj";
  const std::filesystem::path lease_path =
      cell_directory / L".designpp" / L"project.writer.lease";
  ProjectDocument document;
  document.library_directory = library.directory;
  document.project_path = project_path;
  document.writer_lease = ProjectWriterLease::TryAcquire(lease_path);
  document.read_only = document.writer_lease == nullptr;
  std::error_code error;
  if (std::filesystem::exists(project_path, error)) {
    auto loaded = store_.Load(project_path);
    if (!loaded.Ok()) {
      document.project = DefaultProject(library, *cell);
      document.read_only = true;
      document.valid_configuration = false;
      document.diagnostic = loaded.GetStatus().message;
      document.writer_lease.reset();
      return document;
    }
    document.project = std::move(loaded).Value();
  } else {
    document.project = DefaultProject(library, *cell);
    if (document.read_only) {
      document.diagnostic =
          "Project is missing and another process owns the writer lease";
      return document;
    }
    Status saved = store_.Save(document.project, project_path, 0);
    if (!saved.Ok()) {
      document.read_only = true;
      document.valid_configuration = false;
      document.diagnostic = saved.message;
      document.writer_lease.reset();
      return document;
    }
  }
  if (document.project.library_id != library.library.id ||
      document.project.cell_id != cell_id) {
    return Status{ErrorCode::kConflict,
                  "Project does not belong to the selected Library/Cell", 0};
  }
  if (document.read_only) {
    document.diagnostic = "Another Design++ process owns this project";
  }
  return document;
}

Status ProjectService::Save(ProjectDocument* document) const {
  if (document == nullptr) {
    return {ErrorCode::kInvalidArgument, "Project document is missing", 0};
  }
  if (document->read_only || !document->writer_lease ||
      !document->writer_lease->Acquired()) {
    return {ErrorCode::kPermissionDenied, "Project is read-only", 0};
  }
  const std::uint64_t expected_revision = document->project.revision;
  core::Project updated = document->project;
  updated.revision = expected_revision + 1;
  updated.modified_utc = UtcNow();
  Status status =
      store_.Save(updated, document->project_path, expected_revision);
  if (status.Ok()) document->project = std::move(updated);
  return status;
}

Status ProjectService::Refresh(ProjectDocument* document) const {
  if (document == nullptr) {
    return {ErrorCode::kInvalidArgument, "Project document is missing", 0};
  }
  auto loaded = store_.Load(document->project_path);
  if (!loaded.Ok()) return loaded.GetStatus();
  if (loaded.Value().library_id != document->project.library_id ||
      loaded.Value().cell_id != document->project.cell_id) {
    return {ErrorCode::kConflict, "Project identity changed", 0};
  }
  document->project = std::move(loaded).Value();
  return Status::Success();
}

core::Result<std::vector<ResolvedSource>> ProjectService::ResolveSources(
    const LibraryRecord& library, std::string_view cell_id,
    const core::Project& project) const {
  if (project.library_id != library.library.id || project.cell_id != cell_id) {
    return Status{ErrorCode::kConflict, "Project identity does not match", 0};
  }
  const core::Cell* cell = FindCell(library, cell_id);
  if (cell == nullptr) {
    return Status{ErrorCode::kNotFound, "Cell does not exist", 0};
  }
  for (const core::TestbenchConfiguration& configuration :
       project.testbench_configurations) {
    const auto view = std::find_if(cell->views.begin(), cell->views.end(),
                                   [&](const core::View& value) {
                                     return value.id == configuration.view_id;
                                   });
    if (view == cell->views.end() || view->kind != core::ViewKind::kTestbench) {
      return Status{ErrorCode::kCorruptData,
                    "Testbench configuration references an invalid View", 0};
    }
  }
  std::vector<ResolvedSource> sources;
  for (const core::ManagedFile& file : library.library.files) {
    if (!IsLibertyFile(file)) continue;
    ResolvedSource source;
    source.view_name = "Library";
    source.view_kind = core::ViewKind::kConstraints;
    source.relative_path = file.relative_path;
    source.role = file.role;
    source.library_directory = library.directory;
    source.windows_path = library.directory / Utf8Path(file.relative_path);
    std::error_code error;
    source.exists =
        std::filesystem::is_regular_file(source.windows_path, error) && !error;
    if (source.exists) {
      const std::filesystem::path canonical_root =
          std::filesystem::weakly_canonical(library.directory, error);
      if (error) {
        return Status{ErrorCode::kIoError,
                      "Cannot canonicalize Library directory",
                      static_cast<unsigned long>(error.value())};
      }
      const std::filesystem::path canonical_source =
          std::filesystem::canonical(source.windows_path, error);
      if (error || !IsWithin(canonical_root, canonical_source)) {
        return Status{ErrorCode::kPermissionDenied,
                      "Managed library file escapes through a reparse point",
                      static_cast<unsigned long>(error.value())};
      }
    }
    sources.push_back(std::move(source));
  }
  for (const core::View& view : cell->views) {
    if (!IsManagedSourceKind(view.kind) && !IsLegacyToolInputKind(view.kind)) {
      continue;
    }
    for (const core::ManagedFile& file : view.files) {
      if (IsLegacyToolInputKind(view.kind) && !IsTimingInputFile(file)) {
        continue;
      }
      ResolvedSource source;
      source.view_id = view.id;
      source.view_name = view.name;
      source.view_kind = view.kind;
      source.relative_path = file.relative_path;
      source.role = file.role;
      source.library_directory = library.directory;
      source.windows_path = library.directory / Utf8Path(file.relative_path);
      const auto override_value = std::find_if(
          project.source_overrides.begin(), project.source_overrides.end(),
          [&view, &file](const core::SourceOverride& item) {
            return item.view_id == view.id &&
                   item.relative_path == file.relative_path;
          });
      if (override_value != project.source_overrides.end()) {
        source.enabled = override_value->enabled;
        if (!override_value->role.empty()) source.role = override_value->role;
      }
      std::error_code error;
      source.exists =
          std::filesystem::is_regular_file(source.windows_path, error) &&
          !error;
      if (source.exists) {
        const std::filesystem::path canonical_root =
            std::filesystem::weakly_canonical(library.directory, error);
        if (error) {
          return Status{ErrorCode::kIoError,
                        "Cannot canonicalize Library directory",
                        static_cast<unsigned long>(error.value())};
        }
        const std::filesystem::path canonical_source =
            std::filesystem::canonical(source.windows_path, error);
        if (error || !IsWithin(canonical_root, canonical_source)) {
          return Status{ErrorCode::kPermissionDenied,
                        "Managed source escapes through a reparse point",
                        static_cast<unsigned long>(error.value())};
        }
      }
      sources.push_back(std::move(source));
    }
  }
  return sources;
}

}  // namespace designpp::application

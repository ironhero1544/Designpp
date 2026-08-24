// Copyright 2026 The Design++ Authors

#include "designpp/application/toolchain_profile_store.h"

#include <shlobj.h>
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace designpp::application {
namespace {

using core::ErrorCode;
using core::Status;

constexpr std::uintmax_t kMaximumSettingsBytes = 1024 * 1024;
constexpr wchar_t kWriterMutexName[] =
    L"Local\\DesignPlusPlus.ToolchainProfileStore.v1";

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
    std::optional<JsonValue> value = ParseValue();
    SkipSpace();
    if (!value || position_ != input_.size()) return std::nullopt;
    return value;
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
      if (character != '\\') {
        if (static_cast<unsigned char>(character) < 0x20) return std::nullopt;
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

  std::optional<JsonValue> ParseValue() {
    SkipSpace();
    if (position_ >= input_.size()) return std::nullopt;
    if (input_[position_] == '{') return ParseObject();
    if (input_[position_] == '[') return ParseArray();
    if (input_[position_] == '"') {
      auto text = ParseString();
      return text ? std::optional<JsonValue>(JsonValue(std::move(*text)))
                  : std::nullopt;
    }
    const std::size_t start = position_;
    while (position_ < input_.size() &&
           std::isdigit(static_cast<unsigned char>(input_[position_]))) {
      ++position_;
    }
    if (start == position_) return std::nullopt;
    std::uint64_t number = 0;
    if (std::from_chars(input_.data() + start, input_.data() + position_,
                        number)
            .ec != std::errc()) {
      return std::nullopt;
    }
    return JsonValue(number);
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

class MutexLease final {
 public:
  MutexLease() = default;
  MutexLease(const MutexLease&) = delete;
  MutexLease& operator=(const MutexLease&) = delete;
  ~MutexLease() {
    if (mutex_ == nullptr) return;
    if (acquired_) ReleaseMutex(mutex_);
    CloseHandle(mutex_);
  }

  [[nodiscard]] Status Acquire() {
    mutex_ = CreateMutexW(nullptr, FALSE, kWriterMutexName);
    if (mutex_ == nullptr) {
      return {ErrorCode::kIoError, "Cannot open toolchain settings writer lock",
              GetLastError()};
    }
    const DWORD result = WaitForSingleObject(mutex_, 5000);
    if (result != WAIT_OBJECT_0 && result != WAIT_ABANDONED) {
      return {ErrorCode::kConflict,
              "Toolchain settings are being written by another process", 0};
    }
    acquired_ = true;
    return Status::Success();
  }

 private:
  HANDLE mutex_ = nullptr;
  bool acquired_ = false;
};

std::string EscapeJson(std::string_view input) {
  std::string output;
  for (const char character : input) {
    switch (character) {
      case '"':
        output += "\\\"";
        break;
      case '\\':
        output += "\\\\";
        break;
      case '\n':
        output += "\\n";
        break;
      case '\r':
        output += "\\r";
        break;
      case '\t':
        output += "\\t";
        break;
      default:
        output.push_back(character);
        break;
    }
  }
  return output;
}

const JsonValue* Member(const JsonValue::Object& object, std::string_view key) {
  const auto found = object.find(std::string(key));
  return found == object.end() ? nullptr : &found->second;
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

bool IsValidUtf8(std::string_view text) {
  if (text.empty()) return true;
  return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                             static_cast<int>(text.size()), nullptr, 0) > 0;
}

core::Result<core::ToolchainSettings> Decode(std::string_view json) {
  if (!IsValidUtf8(json)) {
    return Status{ErrorCode::kInvalidEncoding,
                  "Toolchain settings are not valid UTF-8", 0};
  }
  auto root_value = JsonParser(json).Parse();
  const auto* root =
      root_value ? root_value->Get<JsonValue::Object>() : nullptr;
  if (root == nullptr) {
    return Status{ErrorCode::kCorruptData, "Toolchain settings JSON is invalid",
                  0};
  }
  core::ToolchainSettings settings;
  std::uint64_t schema = 0;
  if (!ReadNumber(*root, "schema_version", &schema) ||
      !ReadString(*root, "selected_profile_id",
                  &settings.selected_profile_id)) {
    return Status{ErrorCode::kCorruptData,
                  "Toolchain settings required field is missing", 0};
  }
  if (schema != core::ToolchainSettings::kSchemaVersion) {
    return Status{ErrorCode::kUnsupportedSchema,
                  "Unsupported toolchain settings schema", 0};
  }
  settings.schema_version = static_cast<std::uint32_t>(schema);
  const JsonValue* profiles_value = Member(*root, "profiles");
  const auto* profiles =
      profiles_value ? profiles_value->Get<JsonValue::Array>() : nullptr;
  if (profiles == nullptr) {
    return Status{ErrorCode::kCorruptData,
                  "Toolchain profiles array is missing", 0};
  }
  for (const JsonValue& profile_value : *profiles) {
    const auto* object = profile_value.Get<JsonValue::Object>();
    core::ToolchainProfile profile;
    std::uint64_t cpu_budget = 0;
    if (object == nullptr || !ReadString(*object, "id", &profile.id) ||
        !ReadString(*object, "name", &profile.name) ||
        !ReadString(*object, "wsl_distribution", &profile.wsl_distribution) ||
        !ReadString(*object, "openlane_root", &profile.openlane_root) ||
        !ReadString(*object, "orfs_root", &profile.orfs_root) ||
        !ReadString(*object, "pdk_root", &profile.pdk_root) ||
        !ReadNumber(*object, "cpu_budget", &cpu_budget) ||
        cpu_budget > UINT32_MAX) {
      return Status{ErrorCode::kCorruptData, "Toolchain profile is malformed",
                    0};
    }
    profile.cpu_budget = static_cast<std::uint32_t>(cpu_budget);
    settings.profiles.push_back(std::move(profile));
  }
  const Status validation = core::ValidateToolchainSettings(settings);
  return validation.Ok()
             ? core::Result<core::ToolchainSettings>(std::move(settings))
             : core::Result<core::ToolchainSettings>(validation);
}

std::string Encode(const core::ToolchainSettings& settings) {
  std::ostringstream output;
  auto quote = [&output](std::string_view value) {
    output << '"' << EscapeJson(value) << '"';
  };
  output << "{\n  \"schema_version\": " << settings.schema_version
         << ",\n  \"selected_profile_id\": ";
  quote(settings.selected_profile_id);
  output << ",\n  \"profiles\": [";
  for (std::size_t index = 0; index < settings.profiles.size(); ++index) {
    const core::ToolchainProfile& profile = settings.profiles[index];
    output << (index == 0 ? "" : ",") << "\n    {\"id\": ";
    quote(profile.id);
    output << ", \"name\": ";
    quote(profile.name);
    output << ", \"wsl_distribution\": ";
    quote(profile.wsl_distribution);
    output << ", \"openlane_root\": ";
    quote(profile.openlane_root);
    output << ", \"orfs_root\": ";
    quote(profile.orfs_root);
    output << ", \"pdk_root\": ";
    quote(profile.pdk_root);
    output << ", \"cpu_budget\": " << profile.cpu_budget << '}';
  }
  output << "\n  ]\n}\n";
  return output.str();
}

core::Result<std::string> ReadFile(const std::filesystem::path& path) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error) || error) {
    return Status{ErrorCode::kNotFound,
                  "Toolchain settings file does not exist", 0};
  }
  const std::uintmax_t size = std::filesystem::file_size(path, error);
  if (error || size > kMaximumSettingsBytes) {
    return Status{ErrorCode::kFileTooLarge,
                  "Toolchain settings file is too large", 0};
  }
  std::ifstream input(path, std::ios::binary);
  std::string contents(static_cast<std::size_t>(size), '\0');
  input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
  if (!input && !contents.empty()) {
    return Status{ErrorCode::kIoError, "Cannot read toolchain settings file",
                  0};
  }
  return contents;
}

std::filesystem::path TemporaryPath(const std::filesystem::path& target) {
  return target.parent_path() /
         (target.filename().wstring() + L"." +
          std::to_wstring(GetCurrentProcessId()) + L".tmp");
}

}  // namespace

ToolchainProfileStore::ToolchainProfileStore() {
  auto path = DefaultSettingsPath();
  if (path.Ok()) {
    settings_path_ = std::move(path).Value();
  } else {
    initialization_status_ = path.GetStatus();
  }
}

ToolchainProfileStore::ToolchainProfileStore(
    std::filesystem::path settings_path)
    : settings_path_(std::move(settings_path)) {}

core::Result<core::ToolchainSettings> ToolchainProfileStore::Load() const {
  if (!initialization_status_.Ok()) return initialization_status_;
  auto contents = ReadFile(settings_path_);
  if (!contents.Ok()) return contents.GetStatus();
  return Decode(contents.Value());
}

core::Result<core::ToolchainSettings>
ToolchainProfileStore::LoadOrCreateDefaults() const {
  auto loaded = Load();
  if (loaded.Ok() || loaded.GetStatus().code != ErrorCode::kNotFound) {
    return loaded;
  }
  core::ToolchainSettings defaults = CreateDefaults();
  const Status saved = Save(defaults);
  return saved.Ok() ? core::Result<core::ToolchainSettings>(std::move(defaults))
                    : core::Result<core::ToolchainSettings>(saved);
}

Status ToolchainProfileStore::Save(
    const core::ToolchainSettings& settings) const {
  if (!initialization_status_.Ok()) return initialization_status_;
  const Status validation = core::ValidateToolchainSettings(settings);
  if (!validation.Ok()) return validation;
  MutexLease lease;
  const Status acquired = lease.Acquire();
  if (!acquired.Ok()) return acquired;
  std::error_code error;
  std::filesystem::create_directories(settings_path_.parent_path(), error);
  if (error) {
    return {ErrorCode::kIoError, "Cannot create toolchain settings directory",
            static_cast<unsigned long>(error.value())};
  }
  const std::filesystem::path temporary = TemporaryPath(settings_path_);
  const std::string bytes = Encode(settings);
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.flush();
    if (!output) {
      std::filesystem::remove(temporary, error);
      return {ErrorCode::kIoError, "Cannot write toolchain settings", 0};
    }
  }
  if (std::filesystem::exists(settings_path_, error) && !error) {
    std::filesystem::copy_file(
        settings_path_, settings_path_.wstring() + L".bak",
        std::filesystem::copy_options::overwrite_existing, error);
    error.clear();
  }
  if (!MoveFileExW(temporary.c_str(), settings_path_.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    const DWORD native_error = GetLastError();
    std::filesystem::remove(temporary, error);
    return {ErrorCode::kIoError, "Cannot replace toolchain settings",
            native_error};
  }
  return Status::Success();
}

const std::filesystem::path& ToolchainProfileStore::settings_path()
    const noexcept {
  return settings_path_;
}

core::ToolchainSettings ToolchainProfileStore::CreateDefaults() {
  const unsigned int logical_cpus = std::thread::hardware_concurrency();
  core::ToolchainProfile profile;
  profile.id = "default";
  profile.name = "Default WSL Toolchain";
  profile.openlane_root = "~/.designpp/toolchains/openlane2";
  profile.orfs_root = "~/.designpp/toolchains/orfs";
  profile.cpu_budget = logical_cpus > 1 ? logical_cpus - 1 : 1;
  core::ToolchainSettings settings;
  settings.selected_profile_id = profile.id;
  settings.profiles.push_back(std::move(profile));
  return settings;
}

core::Result<std::filesystem::path>
ToolchainProfileStore::DefaultSettingsPath() {
  PWSTR local_app_data = nullptr;
  const HRESULT result = SHGetKnownFolderPath(
      FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &local_app_data);
  if (FAILED(result) || local_app_data == nullptr) {
    if (local_app_data != nullptr) CoTaskMemFree(local_app_data);
    return Status{ErrorCode::kIoError,
                  "Cannot resolve LocalAppData for toolchain settings",
                  static_cast<unsigned long>(result)};
  }
  std::filesystem::path path(local_app_data);
  CoTaskMemFree(local_app_data);
  return path / L"DesignPlusPlus" / L"toolchain-profiles.json";
}

}  // namespace designpp::application

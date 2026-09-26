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
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "designpp/core/toolchain_compatibility.h"

namespace designpp::application {
namespace {

using core::ErrorCode;
using core::Status;

constexpr std::uintmax_t kMaximumSettingsBytes = 1024 * 1024;
constexpr wchar_t kWriterMutexName[] =
    L"Local\\DesignPlusPlus.ToolchainProfileStore.v1";
constexpr char kLegacyOpenLaneDefaultRoot[] =
    "~/.designpp/toolchains/openlane2";
constexpr char kLegacyOrfsDefaultRoot[] = "~/.designpp/toolchains/orfs";
constexpr char kDefaultOpenLanePdkRoot[] = "~/.volare";

std::string DefaultManagedRoot(std::string_view provider_id) {
  for (const core::ToolchainCompatibilityEntry& entry :
       core::ToolchainCompatibilityCatalog::Entries()) {
    if (entry.provider_id == provider_id) {
      return "~/.designpp/toolchains/environments/" +
             std::string(entry.bundle_id);
    }
  }
  return {};
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
  std::uint64_t revision = 1;
  if (!ReadNumber(*root, "schema_version", &schema) ||
      !ReadString(*root, "selected_profile_id",
                  &settings.selected_profile_id)) {
    return Status{ErrorCode::kCorruptData,
                  "Toolchain settings required field is missing", 0};
  }
  if (schema < 1 || schema > core::ToolchainSettings::kSchemaVersion) {
    return Status{ErrorCode::kUnsupportedSchema,
                  "Unsupported toolchain settings schema", 0};
  }
  settings.schema_version = core::ToolchainSettings::kSchemaVersion;
  if (schema >= 3 && !ReadNumber(*root, "revision", &revision)) {
    return Status{ErrorCode::kCorruptData,
                  "Toolchain settings revision is missing", 0};
  }
  settings.revision = revision;
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
    if (schema >= 2 &&
        (!ReadString(*object, "openlane_mode", &profile.openlane_mode) ||
         !ReadString(*object, "openlane_bundle_id",
                     &profile.openlane_bundle_id) ||
         !ReadString(*object, "orfs_mode", &profile.orfs_mode) ||
         !ReadString(*object, "orfs_bundle_id", &profile.orfs_bundle_id))) {
      return Status{ErrorCode::kCorruptData,
                    "Toolchain bundle selection is missing", 0};
    }
    if (schema >= 3 &&
        (!ReadString(*object, "active_openlane_environment_id",
                     &profile.active_openlane_environment_id) ||
         !ReadString(*object, "rollback_openlane_environment_id",
                     &profile.rollback_openlane_environment_id) ||
         !ReadString(*object, "active_orfs_environment_id",
                     &profile.active_orfs_environment_id) ||
         !ReadString(*object, "rollback_orfs_environment_id",
                     &profile.rollback_orfs_environment_id))) {
      return Status{ErrorCode::kCorruptData,
                    "Toolchain environment selection is missing", 0};
    }
    settings.profiles.push_back(std::move(profile));
  }
  if (schema >= 3) {
    const auto* environments = [&]() -> const JsonValue::Array* {
      const JsonValue* value = Member(*root, "environments");
      return value ? value->Get<JsonValue::Array>() : nullptr;
    }();
    const auto* recipes = [&]() -> const JsonValue::Array* {
      const JsonValue* value = Member(*root, "verification_recipes");
      return value ? value->Get<JsonValue::Array>() : nullptr;
    }();
    if (environments == nullptr || recipes == nullptr) {
      return Status{ErrorCode::kCorruptData,
                    "Toolchain environment catalog is missing", 0};
    }
    for (const JsonValue& value : *environments) {
      const auto* object = value.Get<JsonValue::Object>();
      core::InstalledToolchainEnvironment environment;
      std::uint64_t verified = 0;
      if (object == nullptr || !ReadString(*object, "id", &environment.id) ||
          !ReadString(*object, "provider_id", &environment.provider_id) ||
          !ReadString(*object, "bundle_id", &environment.bundle_id) ||
          !ReadString(*object, "root", &environment.root) ||
          !ReadString(*object, "executable", &environment.executable) ||
          !ReadString(*object, "version", &environment.version) ||
          !ReadString(*object, "fingerprint", &environment.fingerprint) ||
          !ReadNumber(*object, "verified", &verified) || verified > 1) {
        return Status{ErrorCode::kCorruptData,
                      "Installed toolchain environment is malformed", 0};
      }
      environment.verified = verified == 1;
      if (schema >= 4 &&
          (!ReadString(*object, "command_contract_id",
                       &environment.command_contract_id) ||
           !ReadString(*object, "framework_revision",
                       &environment.framework_revision) ||
           !ReadString(*object, "lock_hash", &environment.lock_hash))) {
        return Status{ErrorCode::kCorruptData,
                      "Toolchain compatibility evidence is malformed", 0};
      }
      // Older verification flags did not attest a command contract.
      if (schema < 4) environment.verified = false;
      settings.environments.push_back(std::move(environment));
    }
    for (const JsonValue& value : *recipes) {
      const auto* object = value.Get<JsonValue::Object>();
      core::VerificationRecipe recipe;
      std::uint64_t trusted = 0;
      std::uint64_t managed = 0;
      if (object == nullptr || !ReadString(*object, "id", &recipe.id) ||
          !ReadString(*object, "name", &recipe.name) ||
          !ReadString(*object, "engine", &recipe.engine) ||
          !ReadString(*object, "provider_id", &recipe.provider_id) ||
          !ReadString(*object, "platform", &recipe.platform) ||
          !ReadString(*object, "root", &recipe.root) ||
          !ReadString(*object, "entrypoint", &recipe.entrypoint) ||
          !ReadString(*object, "technology_file", &recipe.technology_file) ||
          !ReadString(*object, "reference_netlist",
                      &recipe.reference_netlist) ||
          !ReadString(*object, "setup_file", &recipe.setup_file) ||
          !ReadString(*object, "output_format", &recipe.output_format) ||
          !ReadString(*object, "content_hash", &recipe.content_hash) ||
          !ReadNumber(*object, "trusted", &trusted) || trusted > 1 ||
          !ReadNumber(*object, "managed", &managed) || managed > 1) {
        return Status{ErrorCode::kCorruptData,
                      "Physical verification recipe is malformed", 0};
      }
      recipe.trusted = trusted == 1;
      recipe.managed = managed == 1;
      settings.verification_recipes.push_back(std::move(recipe));
    }
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
         << ",\n  \"revision\": " << settings.revision
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
    output << ", \"openlane_mode\": ";
    quote(profile.openlane_mode);
    output << ", \"openlane_bundle_id\": ";
    quote(profile.openlane_bundle_id);
    output << ", \"orfs_mode\": ";
    quote(profile.orfs_mode);
    output << ", \"orfs_bundle_id\": ";
    quote(profile.orfs_bundle_id);
    output << ", \"active_openlane_environment_id\": ";
    quote(profile.active_openlane_environment_id);
    output << ", \"rollback_openlane_environment_id\": ";
    quote(profile.rollback_openlane_environment_id);
    output << ", \"active_orfs_environment_id\": ";
    quote(profile.active_orfs_environment_id);
    output << ", \"rollback_orfs_environment_id\": ";
    quote(profile.rollback_orfs_environment_id);
    output << ", \"cpu_budget\": " << profile.cpu_budget << '}';
  }
  output << "\n  ],\n  \"environments\": [";
  for (std::size_t index = 0; index < settings.environments.size(); ++index) {
    const core::InstalledToolchainEnvironment& environment =
        settings.environments[index];
    output << (index == 0 ? "" : ",") << "\n    {\"id\": ";
    quote(environment.id);
    output << ", \"provider_id\": ";
    quote(environment.provider_id);
    output << ", \"bundle_id\": ";
    quote(environment.bundle_id);
    output << ", \"root\": ";
    quote(environment.root);
    output << ", \"executable\": ";
    quote(environment.executable);
    output << ", \"version\": ";
    quote(environment.version);
    output << ", \"fingerprint\": ";
    quote(environment.fingerprint);
    output << ", \"command_contract_id\": ";
    quote(environment.command_contract_id);
    output << ", \"framework_revision\": ";
    quote(environment.framework_revision);
    output << ", \"lock_hash\": ";
    quote(environment.lock_hash);
    output << ", \"verified\": " << (environment.verified ? 1 : 0) << '}';
  }
  output << "\n  ],\n  \"verification_recipes\": [";
  for (std::size_t index = 0; index < settings.verification_recipes.size();
       ++index) {
    const core::VerificationRecipe& recipe =
        settings.verification_recipes[index];
    output << (index == 0 ? "" : ",") << "\n    {\"id\": ";
    quote(recipe.id);
    output << ", \"name\": ";
    quote(recipe.name);
    output << ", \"engine\": ";
    quote(recipe.engine);
    output << ", \"provider_id\": ";
    quote(recipe.provider_id);
    output << ", \"platform\": ";
    quote(recipe.platform);
    output << ", \"root\": ";
    quote(recipe.root);
    output << ", \"entrypoint\": ";
    quote(recipe.entrypoint);
    output << ", \"technology_file\": ";
    quote(recipe.technology_file);
    output << ", \"reference_netlist\": ";
    quote(recipe.reference_netlist);
    output << ", \"setup_file\": ";
    quote(recipe.setup_file);
    output << ", \"output_format\": ";
    quote(recipe.output_format);
    output << ", \"content_hash\": ";
    quote(recipe.content_hash);
    output << ", \"trusted\": " << (recipe.trusted ? 1 : 0)
           << ", \"managed\": " << (recipe.managed ? 1 : 0) << '}';
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
  auto decoded = Decode(contents.Value());
  if (!decoded.Ok()) return decoded.GetStatus();
  core::ToolchainSettings settings = std::move(decoded).Value();
  const std::string openlane_default = DefaultManagedRoot("openlane2");
  const std::string orfs_default = DefaultManagedRoot("orfs");
  for (core::ToolchainProfile& profile : settings.profiles) {
    if (profile.id != "default") continue;
    // Correct only the generated legacy paths in memory. Other paths and
    // activated environments are preserved; the saved file is not rewritten.
    if (!openlane_default.empty() && profile.openlane_mode == "custom" &&
        profile.active_openlane_environment_id.empty() &&
        profile.openlane_root == kLegacyOpenLaneDefaultRoot) {
      profile.openlane_root = openlane_default;
    }
    if (!orfs_default.empty() && profile.orfs_mode == "custom" &&
        profile.active_orfs_environment_id.empty() &&
        profile.orfs_root == kLegacyOrfsDefaultRoot) {
      profile.orfs_root = orfs_default;
    }
    if (profile.pdk_root.empty()) {
      profile.pdk_root = kDefaultOpenLanePdkRoot;
    }
  }
  return settings;
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
  return Save(settings, 0);
}

Status ToolchainProfileStore::Save(const core::ToolchainSettings& settings,
                                   std::uint64_t expected_revision) const {
  if (!initialization_status_.Ok()) return initialization_status_;
  const Status validation = core::ValidateToolchainSettings(settings);
  if (!validation.Ok()) return validation;
  MutexLease lease;
  const Status acquired = lease.Acquire();
  if (!acquired.Ok()) return acquired;
  if (expected_revision != 0) {
    auto current_bytes = ReadFile(settings_path_);
    if (!current_bytes.Ok()) return current_bytes.GetStatus();
    auto current = Decode(current_bytes.Value());
    if (!current.Ok()) return current.GetStatus();
    if (current.Value().revision != expected_revision ||
        settings.revision != expected_revision + 1) {
      return {ErrorCode::kExternalModification,
              "Toolchain settings changed in another process", 0};
    }
  }
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
  profile.openlane_root = DefaultManagedRoot("openlane2");
  profile.orfs_root = DefaultManagedRoot("orfs");
  profile.pdk_root = kDefaultOpenLanePdkRoot;
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

// Copyright 2026 The Design++ Authors

#include "designpp/core/toolchain_profile.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace designpp::core {
namespace {

bool HasControlCharacter(std::string_view value) {
  return std::any_of(value.begin(), value.end(), [](unsigned char character) {
    return character < 0x20 || character == 0x7f;
  });
}

bool HasParentComponent(std::string_view path) {
  std::size_t start = 0;
  while (start <= path.size()) {
    const std::size_t end = path.find('/', start);
    const std::string_view component =
        path.substr(start, end == std::string_view::npos ? path.size() - start
                                                         : end - start);
    if (component == "..") return true;
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return false;
}

bool IsSafeId(std::string_view value) {
  return !value.empty() && !HasControlCharacter(value) &&
         std::all_of(value.begin(), value.end(), [](unsigned char character) {
           return std::isalnum(character) || character == '-' ||
                  character == '_' || character == '.' || character == ':';
         });
}

}  // namespace

bool IsSafeLinuxProfilePath(std::string_view path) {
  if (path.empty()) return true;
  if (HasControlCharacter(path) || path.find('\\') != std::string_view::npos ||
      HasParentComponent(path)) {
    return false;
  }
  return path.front() == '/' || path.starts_with("~/");
}

Status ValidateToolchainSettings(const ToolchainSettings& settings) {
  if (settings.schema_version != ToolchainSettings::kSchemaVersion) {
    return {ErrorCode::kUnsupportedSchema,
            "Unsupported toolchain settings schema", 0};
  }
  if (settings.revision == 0 || settings.profiles.empty() ||
      settings.selected_profile_id.empty()) {
    return {ErrorCode::kInvalidArgument,
            "Toolchain settings require a selected profile", 0};
  }
  std::set<std::string> ids;
  std::set<std::string> environment_ids;
  std::set<std::string> recipe_ids;
  bool selected_found = false;
  for (const ToolchainProfile& profile : settings.profiles) {
    const auto valid_selection = [](const std::string& mode,
                                    const std::string& bundle) {
      return (mode == "custom" && bundle.empty()) ||
             (mode == "managed" && !bundle.empty() &&
              std::all_of(bundle.begin(), bundle.end(), [](unsigned char ch) {
                return std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.';
              }));
    };
    if (!valid_selection(profile.openlane_mode, profile.openlane_bundle_id) ||
        !valid_selection(profile.orfs_mode, profile.orfs_bundle_id)) {
      return {ErrorCode::kInvalidArgument, "Invalid toolchain bundle selection",
              0};
    }
    if (profile.id.empty() || profile.name.empty() ||
        HasControlCharacter(profile.id) || HasControlCharacter(profile.name) ||
        HasControlCharacter(profile.wsl_distribution) ||
        profile.cpu_budget == 0 || profile.cpu_budget > 65535 ||
        !IsSafeLinuxProfilePath(profile.openlane_root) ||
        !IsSafeLinuxProfilePath(profile.orfs_root) ||
        !IsSafeLinuxProfilePath(profile.pdk_root)) {
      return {ErrorCode::kInvalidArgument,
              "Toolchain profile contains an invalid field", 0};
    }
    if (!ids.insert(profile.id).second) {
      return {ErrorCode::kConflict, "Toolchain profile IDs must be unique", 0};
    }
    selected_found =
        selected_found || profile.id == settings.selected_profile_id;
  }
  if (!selected_found) {
    return {ErrorCode::kNotFound, "Selected toolchain profile does not exist",
            0};
  }
  for (const InstalledToolchainEnvironment& environment :
       settings.environments) {
    if (!IsSafeId(environment.id) || !IsSafeId(environment.provider_id) ||
        !environment_ids.insert(environment.id).second ||
        !IsSafeLinuxProfilePath(environment.root) ||
        !IsSafeLinuxProfilePath(environment.executable) ||
        environment.fingerprint.empty() ||
        HasControlCharacter(environment.version) ||
        HasControlCharacter(environment.fingerprint)) {
      return {ErrorCode::kInvalidArgument,
              "Installed toolchain environment is invalid", 0};
    }
  }
  for (const VerificationRecipe& recipe : settings.verification_recipes) {
    const bool valid_engine =
        recipe.engine == "klayout_drc" || recipe.engine == "klayout_lvs" ||
        recipe.engine == "magic_drc" || recipe.engine == "netgen_lvs";
    if (!IsSafeId(recipe.id) || recipe.name.empty() || !valid_engine ||
        !recipe_ids.insert(recipe.id).second ||
        !IsSafeLinuxProfilePath(recipe.root) || recipe.entrypoint.empty() ||
        HasParentComponent(recipe.entrypoint) ||
        HasControlCharacter(recipe.entrypoint) || recipe.content_hash.empty()) {
      return {ErrorCode::kInvalidArgument,
              "Physical verification recipe is invalid", 0};
    }
  }
  const auto environment_exists = [&environment_ids](const std::string& id) {
    return id.empty() || environment_ids.contains(id);
  };
  for (const ToolchainProfile& profile : settings.profiles) {
    if (!environment_exists(profile.active_openlane_environment_id) ||
        !environment_exists(profile.rollback_openlane_environment_id) ||
        !environment_exists(profile.active_orfs_environment_id) ||
        !environment_exists(profile.rollback_orfs_environment_id)) {
      return {ErrorCode::kNotFound,
              "Toolchain profile references an unknown environment", 0};
    }
  }
  return Status::Success();
}

}  // namespace designpp::core

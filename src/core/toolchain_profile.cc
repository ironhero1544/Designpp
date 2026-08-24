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
  if (settings.profiles.empty() || settings.selected_profile_id.empty()) {
    return {ErrorCode::kInvalidArgument,
            "Toolchain settings require a selected profile", 0};
  }
  std::set<std::string> ids;
  bool selected_found = false;
  for (const ToolchainProfile& profile : settings.profiles) {
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
  return Status::Success();
}

}  // namespace designpp::core

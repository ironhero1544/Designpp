// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_CORE_TOOLCHAIN_PROFILE_H_
#define DESIGNPP_CORE_TOOLCHAIN_PROFILE_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "designpp/core/status.h"

namespace designpp::core {

struct ToolchainProfile {
  std::string id;
  std::string name;
  std::string wsl_distribution;
  std::string openlane_root;
  std::string orfs_root;
  std::string pdk_root;
  std::uint32_t cpu_budget = 1;
};

struct ToolchainSettings {
  static constexpr std::uint32_t kSchemaVersion = 1;

  std::uint32_t schema_version = kSchemaVersion;
  std::string selected_profile_id;
  std::vector<ToolchainProfile> profiles;
};

// Validates identity, CPU limits, and Linux path syntax without accessing WSL.
[[nodiscard]] Status ValidateToolchainSettings(
    const ToolchainSettings& settings);

// Accepts an absolute Linux path or a home-relative path beginning with `~/`.
[[nodiscard]] bool IsSafeLinuxProfilePath(std::string_view path);

}  // namespace designpp::core

#endif  // DESIGNPP_CORE_TOOLCHAIN_PROFILE_H_

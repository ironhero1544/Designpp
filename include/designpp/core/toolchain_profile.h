// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_CORE_TOOLCHAIN_PROFILE_H_
#define DESIGNPP_CORE_TOOLCHAIN_PROFILE_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "designpp/core/status.h"

namespace designpp::core {

struct InstalledToolchainEnvironment {
  std::string id;
  std::string provider_id;
  std::string bundle_id;
  std::string root;
  std::string executable;
  std::string version;
  std::string fingerprint;
  bool verified = false;
};

struct VerificationRecipe {
  std::string id;
  std::string name;
  std::string engine;
  std::string provider_id;
  std::string platform;
  std::string root;
  std::string entrypoint;
  std::string technology_file;
  std::string reference_netlist;
  std::string setup_file;
  std::string output_format;
  std::string content_hash;
  bool trusted = false;
  bool managed = false;
};

struct ToolchainProfile {
  std::string id;
  std::string name;
  std::string wsl_distribution;
  std::string openlane_root;
  std::string orfs_root;
  std::string pdk_root;
  std::uint32_t cpu_budget = 1;
  // Existing installations remain custom until explicitly bound to a bundle.
  std::string openlane_mode = "custom";
  std::string openlane_bundle_id;
  std::string orfs_mode = "custom";
  std::string orfs_bundle_id;
  std::string active_openlane_environment_id;
  std::string rollback_openlane_environment_id;
  std::string active_orfs_environment_id;
  std::string rollback_orfs_environment_id;
};

struct ToolchainSettings {
  static constexpr std::uint32_t kSchemaVersion = 3;

  std::uint32_t schema_version = kSchemaVersion;
  std::uint64_t revision = 1;
  std::string selected_profile_id;
  std::vector<ToolchainProfile> profiles;
  std::vector<InstalledToolchainEnvironment> environments;
  std::vector<VerificationRecipe> verification_recipes;
};

// Validates identity, CPU limits, and Linux path syntax without accessing WSL.
[[nodiscard]] Status ValidateToolchainSettings(
    const ToolchainSettings& settings);

// Accepts an absolute Linux path or a home-relative path beginning with `~/`.
[[nodiscard]] bool IsSafeLinuxProfilePath(std::string_view path);

}  // namespace designpp::core

#endif  // DESIGNPP_CORE_TOOLCHAIN_PROFILE_H_

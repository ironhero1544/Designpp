// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_CORE_TOOLCHAIN_COMPATIBILITY_H_
#define DESIGNPP_CORE_TOOLCHAIN_COMPATIBILITY_H_

#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "designpp/core/status.h"

namespace designpp::core {

struct ToolchainDependency {
  std::string_view name;
  std::string_view revision;
};

struct ToolchainCompatibilityEntry {
  std::string_view provider_id;
  std::string_view bundle_id;
  std::string_view version;
  std::string_view revision;
  std::string_view command_contract_id;
  std::span<const ToolchainDependency> dependencies;
  std::span<const std::string_view> required_features;
  std::string_view fixture_id;
};

// Entries describe supported contracts, not proof of an installation's health.
class ToolchainCompatibilityCatalog final {
 public:
  [[nodiscard]] static std::span<const ToolchainCompatibilityEntry> Entries();
  [[nodiscard]] static const ToolchainCompatibilityEntry* Find(
      std::string_view provider_id, std::string_view bundle_id);
  [[nodiscard]] static Status Validate(
      std::span<const ToolchainCompatibilityEntry> entries);
};

// Actual observations must come from a fresh probe of the selected environment.
struct ToolchainCompatibilityEvidence {
  std::string provider_id;
  std::string bundle_id;
  std::string framework_revision;
  std::string lock_hash;
  std::string environment_fingerprint;
  std::string command_contract_id;
  std::vector<std::pair<std::string, std::string>> dependency_revisions;
  std::vector<std::string> features;
};

[[nodiscard]] Status ValidateToolchainCompatibility(
    const ToolchainCompatibilityEvidence& evidence);

}  // namespace designpp::core

#endif  // DESIGNPP_CORE_TOOLCHAIN_COMPATIBILITY_H_

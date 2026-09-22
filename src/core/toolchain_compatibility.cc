// Copyright 2026 The Design++ Authors

#include "designpp/core/toolchain_compatibility.h"

#include <algorithm>
#include <array>
#include <set>
#include <utility>

namespace designpp::core {
namespace {

constexpr ToolchainDependency kOrfsDependencies[] = {
    {"openroad", "0e2d771c5ec38f232493c2afea738ea0200cb972"},
    {"yosys", "d3e297fcd479247322f83d14f42b3556db7acdfb"},
    {"abc", "8e401543d3ecf65e3a3631c7a271793a4d356cb0"},
    {"eqy", "eff96db01293848b993651caa52d747f191be02e"}};
constexpr std::string_view kOrfsFeatures[] = {"yosys.read_liberty.unit_delay",
                                              "yosys.stat.hierarchy",
                                              "abc.read_lib.m",
                                              "openroad.repair_timing.sequence",
                                              "eqy.version",
                                              "orfs.make.targets"};
constexpr std::string_view kOpenlaneFeatures[] = {
    "openlane.version.2.3.10", "openlane.cli.run", "openlane.classic.flow"};
constexpr std::array<ToolchainCompatibilityEntry, 2> kEntries = {{
    {"openlane2",
     "openlane2-2.3.10",
     "2.3.10",
     "a7b0e6dba75ee7e891ff3d7824b29473d9cad289",
     "openlane2-classic-v1",
     {},
     kOpenlaneFeatures,
     "openlane2-rtl-to-gds-v1"},
    {"orfs", "orfs-26Q2", "26Q2", "036d106273e66855cd5214d49518fd0f0df7de61",
     "orfs-26q2-v1", kOrfsDependencies, kOrfsFeatures, "orfs-rtl-to-gds-v1"},
}};

bool IsHash(std::string_view value, std::size_t length) {
  return value.size() == length &&
         std::all_of(value.begin(), value.end(), [](char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f');
         });
}

}  // namespace

std::span<const ToolchainCompatibilityEntry>
ToolchainCompatibilityCatalog::Entries() {
  return kEntries;
}

const ToolchainCompatibilityEntry* ToolchainCompatibilityCatalog::Find(
    std::string_view provider_id, std::string_view bundle_id) {
  for (const auto& entry : kEntries) {
    if (entry.provider_id == provider_id && entry.bundle_id == bundle_id) {
      return &entry;
    }
  }
  return nullptr;
}

Status ToolchainCompatibilityCatalog::Validate(
    std::span<const ToolchainCompatibilityEntry> entries) {
  std::set<std::string_view> bundles;
  std::set<std::string_view> contracts;
  for (const auto& entry : entries) {
    if (entry.provider_id.empty() || entry.bundle_id.empty() ||
        entry.version.empty() || !IsHash(entry.revision, 40) ||
        entry.command_contract_id.empty() || entry.fixture_id.empty() ||
        entry.required_features.empty() ||
        !bundles.insert(entry.bundle_id).second ||
        !contracts.insert(entry.command_contract_id).second) {
      return {ErrorCode::kInvalidArgument,
              "Invalid or duplicate toolchain compatibility contract", 0};
    }
    std::set<std::string_view> dependencies;
    for (const auto& dependency : entry.dependencies) {
      if (dependency.name.empty() || !IsHash(dependency.revision, 40) ||
          !dependencies.insert(dependency.name).second) {
        return {ErrorCode::kInvalidArgument,
                "Invalid toolchain dependency contract", 0};
      }
    }
  }
  return Status::Success();
}

Status ValidateToolchainCompatibility(
    const ToolchainCompatibilityEvidence& evidence) {
  const auto* entry = ToolchainCompatibilityCatalog::Find(evidence.provider_id,
                                                          evidence.bundle_id);
  if (entry == nullptr) {
    return {ErrorCode::kNotFound, "Unsupported toolchain bundle", 0};
  }
  if (evidence.framework_revision != entry->revision ||
      evidence.command_contract_id != entry->command_contract_id) {
    return {ErrorCode::kConflict, "Toolchain command contract does not match",
            0};
  }
  if (!IsHash(evidence.lock_hash, 64) ||
      !IsHash(evidence.environment_fingerprint, 64)) {
    return {ErrorCode::kCorruptData,
            "Toolchain lock or environment fingerprint is missing", 0};
  }
  for (const auto& required : entry->dependencies) {
    const auto count = std::count_if(evidence.dependency_revisions.begin(),
                                     evidence.dependency_revisions.end(),
                                     [&required](const auto& actual) {
                                       return actual.first == required.name;
                                     });
    const auto match = std::find(
        evidence.dependency_revisions.begin(),
        evidence.dependency_revisions.end(),
        std::pair(std::string(required.name), std::string(required.revision)));
    if (count != 1 || match == evidence.dependency_revisions.end()) {
      return {ErrorCode::kConflict,
              "Toolchain dependency mismatch: " + std::string(required.name),
              0};
    }
  }
  for (const auto required : entry->required_features) {
    if (std::find(evidence.features.begin(), evidence.features.end(),
                  required) == evidence.features.end()) {
      return {ErrorCode::kNotFound,
              "Required toolchain feature is missing: " + std::string(required),
              0};
    }
  }
  return Status::Success();
}

}  // namespace designpp::core

// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_RUNTIME_SETUP_CATALOG_H_
#define DESIGNPP_RUNTIME_SETUP_CATALOG_H_

#include <optional>
#include <string>
#include <vector>

#include "designpp/runtime/process_runner.h"
#include "designpp/runtime/tool_catalog.h"

namespace designpp::runtime {

enum class OutputEncoding {
  kUtf8,
  kUtf16LittleEndian,
};

enum class OrfsBuildPolicy {
  kCachedOnly,
  kAllowLocalBuild,
};

// Describes one sequential setup command and its output encoding.
struct SetupStep {
  std::wstring title;
  ProcessRequest request;
  OutputEncoding output_encoding = OutputEncoding::kUtf8;
  bool continue_after_failure = false;
  bool requires_elevation = false;
  std::optional<std::uint32_t> restart_required_exit_code;
  std::string required_output_marker;
};

// Builds Windows-side WSL2 and Ubuntu setup steps.
[[nodiscard]] std::vector<SetupStep> BuildWslSetupSteps();

// Uses a discovered WSL2 distribution without modifying its Linux files.
[[nodiscard]] std::vector<SetupStep> BuildExistingWslSetupSteps(
    std::wstring distribution);

// Installs a separate DesignPlusPlus Ubuntu distribution when supported.
[[nodiscard]] std::vector<SetupStep> BuildDedicatedUbuntuSetupSteps();

// Builds all supported WSL-side tool providers, including OpenLane and ORFS.
[[nodiscard]] std::vector<SetupStep> BuildCompleteToolSetupSteps(
    OrfsBuildPolicy orfs_build_policy = OrfsBuildPolicy::kCachedOnly);

// Removes only abandoned Design++ managed checkout candidates under install
// locks. Shared Nix store paths and prepared environments are preserved.
[[nodiscard]] std::vector<SetupStep> BuildBuildCacheCleanupSteps();

/** Builds removal steps for every tool owned exclusively by Design++. */
[[nodiscard]] std::vector<SetupStep> BuildCompleteToolRemoveSteps();

// Builds an idempotent install/update plan for one selected tool.
[[nodiscard]] std::vector<SetupStep> BuildToolInstallSteps(
    ToolId tool_id,
    OrfsBuildPolicy orfs_build_policy = OrfsBuildPolicy::kCachedOnly);

// Read-only inventory unless prepare is explicitly requested. Model data is
// installed atomically in an immutable version directory, outside ORFS.
[[nodiscard]] ProcessRequest BuildAsap7ModelRequest(bool prepare);

// Builds a removal plan for one selected tool. An empty plan means removal is
// intentionally unsupported because the tool is shared or externally owned.
[[nodiscard]] std::vector<SetupStep> BuildToolRemoveSteps(ToolId tool_id);

}  // namespace designpp::runtime

#endif  // DESIGNPP_RUNTIME_SETUP_CATALOG_H_

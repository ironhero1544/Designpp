// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_ADAPTERS_ORFS_ADAPTER_H_
#define DESIGNPP_ADAPTERS_ORFS_ADAPTER_H_

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "designpp/adapters/managed_flow_adapter.h"
#include "designpp/core/project.h"
#include "designpp/core/toolchain_profile.h"

namespace designpp::adapters {

struct OrfsSource {
  std::string relative_path;
  std::filesystem::path staged_path;
};

struct OrfsRequest {
  core::ToolchainProfile profile;
  core::PhysicalImplementationConfiguration configuration;
  std::string top_module;
  std::vector<OrfsSource> sources;
  std::vector<std::string> include_directories;
  std::vector<std::string> defines;
  std::vector<std::string> parameters;
  std::filesystem::path sdc_path;
  std::string sdc_time_unit = "ns";
  std::optional<std::string> effective_clock_period_ns;
  std::string backend_workspace;
  std::string staging_workspace;
  // Set only for Rebuild From.  The service supplies a prior lineage
  // workspace so the generated plan can copy verified prerequisites into a
  // new child lineage without mutating the original checkout.
  std::string parent_backend_workspace;
  std::uint32_t cpu_threads = 1;
  core::StageId target_stage = core::StageId::kFinalOutputs;
  bool full_flow = false;
  std::string resume_step;
  std::string checkpoint_hash;
  // Capability-selected execution environment.  The service obtains this
  // from BuildProbeCommand output so command generation never guesses that an
  // unrelated toolchain shell is compatible with the ORFS checkout.
  std::string tool_mode;
};

struct OrfsPlan {
  std::string config_makefile;
  std::string openroad_wrapper;
  std::string openroad_init;
  runtime::WslCommand validate;
  runtime::WslCommand execute;
  std::string target;
};

struct OrfsPlatformCandidate {
  std::string name;
  bool runnable = false;
  std::string reason;
  bool drc_ready = false;
  bool lvs_ready = false;
};

// Adapter for the upstream OpenROAD Flow Scripts checkout.  It generates an
// external DESIGN_CONFIG and never mutates the ORFS repository.
class OrfsAdapter final : public ManagedFlowAdapter {
 public:
  [[nodiscard]] std::string_view Name() const noexcept override;
  [[nodiscard]] runtime::WslCommand BuildProbeCommand() const override;
  [[nodiscard]] runtime::WslCommand BuildProbeCommand(
      const core::ToolchainProfile& profile) const;
  // Enumerates platform directories without modifying the ORFS checkout.
  [[nodiscard]] runtime::WslCommand BuildPlatformDiscoveryCommand(
      const core::ToolchainProfile& profile) const;
  [[nodiscard]] core::Result<std::vector<OrfsPlatformCandidate>>
  ParsePlatformDiscovery(std::string_view output) const;
  [[nodiscard]] core::Status Validate(const OrfsRequest& request) const;
  [[nodiscard]] core::Status ValidateAdvancedVariables(
      std::string_view json) const;
  // Validates and emits a deterministic, canonical flat JSON object for the
  // ORFS variables editor.
  [[nodiscard]] core::Result<std::string> CanonicalizeAdvancedVariables(
      std::string_view json) const;
  [[nodiscard]] core::Result<OrfsPlan> BuildPlan(
      const OrfsRequest& request) const;
  [[nodiscard]] std::optional<ManagedFlowProgress> ParseProgress(
      std::string_view line) const override;
  [[nodiscard]] std::vector<core::Diagnostic> ParseDiagnostics(
      std::string_view output) const override;
  [[nodiscard]] core::Result<ManagedFlowMetrics> ParseMetrics(
      std::string_view metrics_json) const override;
  [[nodiscard]] std::vector<ManagedFlowStageInfo> Stages() const override;
  [[nodiscard]] core::StageId ClassifyTarget(std::string_view target) const;
  [[nodiscard]] std::string TargetForStage(core::StageId stage) const;
  [[nodiscard]] std::string GuiTargetForStage(core::StageId stage) const;
  [[nodiscard]] std::filesystem::path CheckpointForStage(
      core::StageId stage) const;
  [[nodiscard]] ManagedFlowArtifactSet DiscoverAvailableArtifacts(
      const std::filesystem::path& root) const;
  [[nodiscard]] core::Status ValidateStageArtifacts(
      const ManagedFlowArtifactSet& artifacts, core::StageId stage) const;
  [[nodiscard]] core::Status ValidateFinalArtifacts(
      const ManagedFlowArtifactSet& artifacts) const;
};

}  // namespace designpp::adapters

#endif  // DESIGNPP_ADAPTERS_ORFS_ADAPTER_H_

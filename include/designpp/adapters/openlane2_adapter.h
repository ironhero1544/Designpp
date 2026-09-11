// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_ADAPTERS_OPENLANE2_ADAPTER_H_
#define DESIGNPP_ADAPTERS_OPENLANE2_ADAPTER_H_

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "designpp/adapters/managed_flow_adapter.h"
#include "designpp/core/project.h"
#include "designpp/core/toolchain_profile.h"

namespace designpp::adapters {

struct OpenLaneSource {
  std::string relative_path;
  std::filesystem::path staged_path;
};

struct OpenLaneRequest {
  core::ToolchainProfile profile;
  core::PhysicalImplementationConfiguration configuration;
  std::string top_module;
  std::vector<OpenLaneSource> sources;
  std::vector<std::string> include_directories;
  std::vector<std::string> defines;
  std::vector<std::string> parameters;
  std::filesystem::path pnr_sdc;
  std::filesystem::path signoff_sdc;
  std::filesystem::path pin_order_cfg;
  std::string backend_workspace;
  std::string staging_workspace;
  std::uint32_t cpu_threads = 1;
  std::string resume_step;
  std::string checkpoint_hash;
};

struct OpenLanePlan {
  std::string config_json;
  runtime::WslCommand validate;
  runtime::WslCommand execute;
};

class OpenLane2Adapter final : public ManagedFlowAdapter {
 public:
  [[nodiscard]] std::string_view Name() const noexcept override;
  [[nodiscard]] runtime::WslCommand BuildProbeCommand() const override;
  [[nodiscard]] runtime::WslCommand BuildProbeCommand(
      const core::ToolchainProfile& profile) const;
  [[nodiscard]] core::Status Validate(const OpenLaneRequest& request) const;
  [[nodiscard]] core::Status ValidateAdvancedOverrides(
      std::string_view json) const;
  [[nodiscard]] core::Result<std::string> BuildPinOrderConfiguration(
      const core::PhysicalImplementationConfiguration& configuration) const;
  [[nodiscard]] core::Result<std::string> EncodeEditableConfiguration(
      const core::PhysicalImplementationConfiguration& configuration) const;
  [[nodiscard]] core::Result<core::PhysicalImplementationConfiguration>
  ApplyEditableConfiguration(
      std::string_view json,
      const core::PhysicalImplementationConfiguration& current) const;
  [[nodiscard]] core::Result<OpenLanePlan> BuildPlan(
      const OpenLaneRequest& request) const;
  [[nodiscard]] std::optional<ManagedFlowProgress> ParseProgress(
      std::string_view line) const override;
  [[nodiscard]] std::vector<core::Diagnostic> ParseDiagnostics(
      std::string_view output) const override;
  [[nodiscard]] core::Result<ManagedFlowMetrics> ParseMetrics(
      std::string_view metrics_json) const override;
  [[nodiscard]] std::vector<ManagedFlowStageInfo> Stages() const override;
  [[nodiscard]] core::StageId ClassifyStep(std::string_view step_id) const;
  [[nodiscard]] ManagedFlowArtifactSet DiscoverAvailableArtifacts(
      const std::filesystem::path& root) const;
  [[nodiscard]] core::Status ValidateFinalArtifacts(
      const ManagedFlowArtifactSet& artifacts) const;
  [[nodiscard]] core::Result<ManagedFlowArtifactSet> DiscoverArtifacts(
      const std::filesystem::path& root) const;
};

}  // namespace designpp::adapters

#endif  // DESIGNPP_ADAPTERS_OPENLANE2_ADAPTER_H_

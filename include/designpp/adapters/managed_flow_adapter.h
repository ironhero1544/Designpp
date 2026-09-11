// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_ADAPTERS_MANAGED_FLOW_ADAPTER_H_
#define DESIGNPP_ADAPTERS_MANAGED_FLOW_ADAPTER_H_

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "designpp/core/diagnostic.h"
#include "designpp/core/flow.h"
#include "designpp/core/status.h"
#include "designpp/runtime/wsl_executor.h"

namespace designpp::adapters {

struct ManagedFlowProgress {
  std::string step_id;
  std::string message;
  core::StageId stage = core::StageId::kRtlInput;
  std::size_t completed_steps = 0;
  std::size_t total_steps = 0;
};

struct ManagedFlowMetrics {
  std::optional<double> core_area;
  std::optional<double> die_area;
  std::optional<double> utilization;
  std::optional<double> instance_count;
  std::optional<double> setup_wns;
  std::optional<double> setup_tns;
  std::optional<double> hold_wns;
  std::optional<double> hold_tns;
  std::optional<double> worst_setup_skew;
  std::optional<double> worst_hold_skew;
  std::optional<double> wire_length;
  std::optional<double> antenna_violations;
  std::optional<double> slew_violations;
  std::optional<double> capacitance_violations;
  std::optional<double> fanout_violations;
  std::optional<double> global_route_congestion;
  std::optional<double> detailed_route_congestion;
  std::optional<double> global_route_overflow;
  std::optional<double> detailed_route_overflow;
  std::optional<double> routing_violations;
  std::optional<double> runtime_seconds;
  std::optional<double> peak_memory_mb;
  std::optional<double> drc_violations;
  std::optional<double> xor_violations;
  std::optional<double> lvs_errors;
  std::optional<double> ir_drop_worst;
  std::map<std::string, std::string> raw;
  // Ordered values are retained for backends such as ORFS whose per-step
  // metric files can legally repeat a metric key.
  std::vector<std::pair<std::string, std::string>> raw_entries;

  [[nodiscard]] bool Passed() const noexcept;
};

struct ManagedFlowArtifactSet {
  std::filesystem::path resolved_config;
  std::filesystem::path final_state;
  std::filesystem::path metrics_json;
  std::filesystem::path metrics_csv;
  std::filesystem::path gds;
  std::filesystem::path def;
  std::filesystem::path lef;
  std::filesystem::path odb;
  std::filesystem::path gate_netlist;
  std::filesystem::path power_netlist;
  std::filesystem::path sdf;
  std::filesystem::path spef;
};

struct ManagedFlowStageInfo {
  core::StageId stage = core::StageId::kRtlInput;
  std::string backend_target;
  std::string gui_target;
  std::string checkpoint_name;
};

class ManagedFlowAdapter {
 public:
  virtual ~ManagedFlowAdapter() = default;

  [[nodiscard]] virtual std::string_view Name() const noexcept = 0;
  [[nodiscard]] virtual runtime::WslCommand BuildProbeCommand() const = 0;
  [[nodiscard]] virtual std::optional<ManagedFlowProgress> ParseProgress(
      std::string_view line) const = 0;
  [[nodiscard]] virtual std::vector<core::Diagnostic> ParseDiagnostics(
      std::string_view output) const = 0;
  [[nodiscard]] virtual core::Result<ManagedFlowMetrics> ParseMetrics(
      std::string_view metrics_json) const = 0;

  // Backend-neutral stage capabilities.  Adapters that do not expose
  // incremental targets may leave these defaults empty.
  [[nodiscard]] virtual std::vector<ManagedFlowStageInfo> Stages() const {
    return {};
  }
};

}  // namespace designpp::adapters

#endif  // DESIGNPP_ADAPTERS_MANAGED_FLOW_ADAPTER_H_

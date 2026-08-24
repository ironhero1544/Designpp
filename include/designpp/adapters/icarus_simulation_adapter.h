// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_ADAPTERS_ICARUS_SIMULATION_ADAPTER_H_
#define DESIGNPP_ADAPTERS_ICARUS_SIMULATION_ADAPTER_H_

#include "designpp/adapters/simulation_adapter.h"

namespace designpp::adapters {

class IcarusSimulationAdapter final : public SimulationAdapter {
 public:
  [[nodiscard]] runtime::WslCommand BuildProbeCommand() const override;
  [[nodiscard]] core::Status Validate(
      const SimulationRequest& request) const override;
  [[nodiscard]] core::Result<SimulationPlan> BuildPlan(
      const SimulationRequest& request,
      const runtime::PathMapper& path_mapper) const override;
  [[nodiscard]] std::vector<core::Diagnostic> ParseDiagnostics(
      std::string_view raw_output) const override;
};

}  // namespace designpp::adapters

#endif  // DESIGNPP_ADAPTERS_ICARUS_SIMULATION_ADAPTER_H_

// Copyright 2026 The Design++ Authors

#include "designpp/adapters/simulation_adapter_factory.h"

#include <memory>

#include "designpp/adapters/icarus_simulation_adapter.h"
#include "designpp/adapters/verilator_simulation_adapter.h"

namespace designpp::adapters {

std::unique_ptr<SimulationAdapter> CreateSimulationAdapter(
    std::string_view backend) {
  if (backend == "icarus") return std::make_unique<IcarusSimulationAdapter>();
  if (backend == "verilator") {
    return std::make_unique<VerilatorSimulationAdapter>();
  }
  return nullptr;
}

std::string_view SimulationBackendDisplayName(std::string_view backend) {
  if (backend == "icarus") return "Icarus Verilog";
  if (backend == "verilator") return "Verilator";
  return "Unsupported simulator";
}

}  // namespace designpp::adapters

// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_ADAPTERS_SIMULATION_ADAPTER_FACTORY_H_
#define DESIGNPP_ADAPTERS_SIMULATION_ADAPTER_FACTORY_H_

#include <memory>
#include <string_view>

#include "designpp/adapters/simulation_adapter.h"

namespace designpp::adapters {

// Returns an adapter for a persisted simulation backend identifier, or null
// when the identifier is unsupported.
[[nodiscard]] std::unique_ptr<SimulationAdapter> CreateSimulationAdapter(
    std::string_view backend);

// Returns the user-facing name of a supported persisted backend identifier.
[[nodiscard]] std::string_view SimulationBackendDisplayName(
    std::string_view backend);

}  // namespace designpp::adapters

#endif  // DESIGNPP_ADAPTERS_SIMULATION_ADAPTER_FACTORY_H_

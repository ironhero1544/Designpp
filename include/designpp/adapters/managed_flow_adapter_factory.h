// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_ADAPTERS_MANAGED_FLOW_ADAPTER_FACTORY_H_
#define DESIGNPP_ADAPTERS_MANAGED_FLOW_ADAPTER_FACTORY_H_

#include <memory>
#include <string_view>

#include "designpp/adapters/managed_flow_adapter.h"

namespace designpp::adapters {

// Creates the managed-flow adapter registered for a backend id.  Unknown ids
// return nullptr so callers can report a configuration diagnostic without
// falling back to a different tool.
[[nodiscard]] std::unique_ptr<ManagedFlowAdapter> CreateManagedFlowAdapter(
    std::string_view backend_id);

}  // namespace designpp::adapters

#endif  // DESIGNPP_ADAPTERS_MANAGED_FLOW_ADAPTER_FACTORY_H_

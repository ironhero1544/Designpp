// Copyright 2026 The Design++ Authors

#include "designpp/adapters/managed_flow_adapter_factory.h"

#include <memory>

#include "designpp/adapters/openlane2_adapter.h"
#include "designpp/adapters/orfs_adapter.h"

namespace designpp::adapters {

std::unique_ptr<ManagedFlowAdapter> CreateManagedFlowAdapter(
    std::string_view backend_id) {
  if (backend_id == "openlane2") {
    return std::make_unique<OpenLane2Adapter>();
  }
  if (backend_id == "orfs") return std::make_unique<OrfsAdapter>();
  return nullptr;
}

}  // namespace designpp::adapters

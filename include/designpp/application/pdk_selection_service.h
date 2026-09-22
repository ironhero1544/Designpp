// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_APPLICATION_PDK_SELECTION_SERVICE_H_
#define DESIGNPP_APPLICATION_PDK_SELECTION_SERVICE_H_

#include <cstdint>
#include <string>

#include "designpp/application/project_service.h"
#include "designpp/core/toolchain_profile.h"

namespace designpp::application {

struct PdkSelection {
  std::string backend;
  std::string name;
  std::string standard_cell_library;
};

struct PdkCellContext {
  core::ToolchainProfile profile;
  std::uint64_t revision = 0;
  PdkSelection selection;
};

// Blocking disk operations; call only from a worker. Applies only technology
// fields to the captured Cell under the existing writer/revision contract.
class PdkSelectionService final {
 public:
  [[nodiscard]] core::Result<PdkCellContext> Load(
      const LibraryRecord& library, const std::string& cell_id) const;
  [[nodiscard]] core::Result<std::uint64_t> Apply(
      const LibraryRecord& library, const std::string& cell_id,
      std::uint64_t expected_revision, const PdkSelection& selection) const;
};

}  // namespace designpp::application
#endif  // DESIGNPP_APPLICATION_PDK_SELECTION_SERVICE_H_

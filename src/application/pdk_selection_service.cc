// Copyright 2026 The Design++ Authors

#include "designpp/application/pdk_selection_service.h"

#include <algorithm>

#include "designpp/application/toolchain_profile_store.h"

namespace designpp::application {

core::Result<PdkCellContext> PdkSelectionService::Load(
    const LibraryRecord& library, const std::string& cell_id) const {
  auto opened = ProjectService{}.OpenOrCreate(library, cell_id);
  if (!opened.Ok()) return opened.GetStatus();
  const auto& project = opened.Value().project;
  auto settings = ToolchainProfileStore{}.LoadOrCreateDefaults();
  if (!settings.Ok()) return settings.GetStatus();
  const auto id = project.toolchain_profile_id.value_or(
      settings.Value().selected_profile_id);
  const auto& profiles = settings.Value().profiles;
  const auto found = std::find_if(profiles.begin(), profiles.end(),
                                  [&id](const auto& p) { return p.id == id; });
  if (found == profiles.end()) {
    return core::Status{core::ErrorCode::kNotFound,
                        "Cell toolchain profile is missing", 0};
  }
  const auto& configuration = project.physical_implementation;
  return PdkCellContext{
      *found,
      project.revision,
      {configuration.backend_id,
       configuration.backend_id == "orfs" ? configuration.orfs.platform
                                          : configuration.pdk,
       configuration.standard_cell_library}};
}

core::Result<std::uint64_t> PdkSelectionService::Apply(
    const LibraryRecord& library, const std::string& cell_id,
    std::uint64_t expected_revision, const PdkSelection& selection) const {
  if ((selection.backend != "orfs" && selection.backend != "openlane2") ||
      selection.name.empty() ||
      (selection.backend == "openlane2" &&
       selection.standard_cell_library.empty())) {
    return core::Status{core::ErrorCode::kInvalidArgument,
                        "Select a discovered PDK and cell library", 0};
  }
  ProjectService service;
  auto opened = service.OpenForCoordinatedUpdate(library, cell_id);
  if (!opened.Ok()) return opened.GetStatus();
  auto document = std::move(opened).Value();
  if (document.project.revision != expected_revision) {
    return core::Status{core::ErrorCode::kConflict,
                        "Cell changed. Refresh before applying the PDK.", 0};
  }
  auto& config = document.project.physical_implementation;
  config.backend_id = selection.backend;
  if (selection.backend == "orfs") {
    config.orfs.platform = selection.name;
    std::erase(config.automatic_fields, "orfs.platform");
  } else {
    config.pdk = selection.name;
    config.standard_cell_library = selection.standard_cell_library;
    std::erase(config.automatic_fields, "pdk");
    std::erase(config.automatic_fields, "standard_cell_library");
  }
  const auto status = service.Save(&document);
  if (!status.Ok()) return status;
  return document.project.revision;
}
}  // namespace designpp::application

// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_GUI_SCHEMATIC_BUILD_SERVICE_H_
#define DESIGNPP_GUI_SCHEMATIC_BUILD_SERVICE_H_

#include <cstdint>
#include <functional>
#include <memory>

#include "designpp/core/status.h"
#include "designpp/gui/schematic_scene.h"
#include "designpp/gui/schematic_scene_builder.h"

namespace designpp::gui {

struct SchematicBuildEvent {
  std::uint64_t generation = 0;
  core::Status status;
  std::shared_ptr<const SchematicScene> scene;
};

using SchematicBuildEventSink = std::function<void(SchematicBuildEvent)>;

// Builds one immutable schematic scene on a bounded worker. Completion is
// delivered at most once and the sink must marshal to its owning GUI thread.
class SchematicBuildService final {
 public:
  SchematicBuildService();
  SchematicBuildService(const SchematicBuildService&) = delete;
  SchematicBuildService& operator=(const SchematicBuildService&) = delete;
  ~SchematicBuildService();

  [[nodiscard]] core::Status Start(std::uint64_t generation,
                                   SchematicBuildRequest request,
                                   SchematicBuildEventSink sink);
  void Cancel() noexcept;
  void Shutdown() noexcept;

 private:
  struct Implementation;
  std::shared_ptr<Implementation> implementation_;
};

}  // namespace designpp::gui

#endif  // DESIGNPP_GUI_SCHEMATIC_BUILD_SERVICE_H_

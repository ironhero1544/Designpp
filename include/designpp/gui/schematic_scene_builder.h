// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_GUI_SCHEMATIC_SCENE_BUILDER_H_
#define DESIGNPP_GUI_SCHEMATIC_SCENE_BUILDER_H_

#include <functional>

#include "designpp/core/schematic.h"
#include "designpp/core/status.h"
#include "designpp/gui/schematic_scene.h"

namespace designpp::gui {

struct SchematicBuildRequest {
  core::SchematicModel model;
  SchematicViewMode mode = SchematicViewMode::kReadable;
};

class SchematicSceneBuilder final {
 public:
  using CancellationCheck = std::function<bool()>;

  [[nodiscard]] core::Result<SchematicScene> Build(
      SchematicBuildRequest request,
      CancellationCheck cancelled = [] { return false; }) const;
};

}  // namespace designpp::gui

#endif  // DESIGNPP_GUI_SCHEMATIC_SCENE_BUILDER_H_

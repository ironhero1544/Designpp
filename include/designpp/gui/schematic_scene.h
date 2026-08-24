// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_GUI_SCHEMATIC_SCENE_H_
#define DESIGNPP_GUI_SCHEMATIC_SCENE_H_

#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include "designpp/core/diagnostic.h"
#include "designpp/core/schematic.h"
#include "designpp/gui/orthogonal_edge_router.h"

namespace designpp::gui {

enum class SchematicViewMode {
  kReadable,
  kGate,
};

struct SchematicScenePin {
  std::string name;
  core::SchematicDirection direction = core::SchematicDirection::kInput;
  core::SchematicPinRole role = core::SchematicPinRole::kData;
  std::vector<std::string> bits;
  OrthogonalPoint point;
  bool active_low = false;
};

struct SchematicSceneNode {
  std::string id;
  std::string type;
  std::string label;
  std::string instance_label;
  core::SchematicNodeKind kind = core::SchematicNodeKind::kGeneric;
  OrthogonalRect bounds;
  std::vector<SchematicScenePin> pins;
};

struct SchematicSceneTerminal {
  std::string label;
  core::SchematicDirection direction = core::SchematicDirection::kInput;
  std::vector<std::string> bits;
  OrthogonalPoint point;
};

struct SchematicScene {
  std::string top_module;
  SchematicViewMode mode = SchematicViewMode::kReadable;
  int width = 800;
  int height = 500;
  std::vector<SchematicSceneNode> nodes;
  std::vector<SchematicSceneTerminal> terminals;
  OrthogonalRoutingResult routing;
  std::map<std::string, std::size_t> net_widths;
  core::SchematicSummary summary;
  std::vector<core::Diagnostic> diagnostics;
};

}  // namespace designpp::gui

#endif  // DESIGNPP_GUI_SCHEMATIC_SCENE_H_

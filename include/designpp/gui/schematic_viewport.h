// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_GUI_SCHEMATIC_VIEWPORT_H_
#define DESIGNPP_GUI_SCHEMATIC_VIEWPORT_H_

#include "designpp/gui/orthogonal_edge_router.h"

namespace designpp::gui {

enum class SchematicDetail { kOverview, kSymbols, kFull };

struct SchematicViewport {
  OrthogonalRect logical_bounds;
  SchematicDetail detail = SchematicDetail::kFull;
};

[[nodiscard]] double CalculateSchematicFitZoom(
    int content_width, int content_height, int client_width, int client_height,
    double dpi_scale, int horizontal_margin, int vertical_margin);

[[nodiscard]] SchematicViewport CalculateSchematicViewport(
    int client_width, int client_height, int scroll_x, int scroll_y,
    double effective_scale, int logical_margin);

[[nodiscard]] bool SchematicRectIsVisible(const OrthogonalRect& bounds,
                                          const OrthogonalRect& viewport);
[[nodiscard]] bool SchematicPointIsVisible(OrthogonalPoint point,
                                           const OrthogonalRect& viewport);
[[nodiscard]] bool SchematicSegmentIsVisible(OrthogonalPoint start,
                                             OrthogonalPoint end,
                                             const OrthogonalRect& viewport);

}  // namespace designpp::gui

#endif  // DESIGNPP_GUI_SCHEMATIC_VIEWPORT_H_

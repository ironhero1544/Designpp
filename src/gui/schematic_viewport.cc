// Copyright 2026 The Design++ Authors

#include "designpp/gui/schematic_viewport.h"

#include <algorithm>
#include <cmath>

namespace designpp::gui {
namespace {

constexpr double kMinimumZoom = 0.001;
constexpr double kMaximumFitZoom = 2.0;

}  // namespace

double CalculateSchematicFitZoom(int content_width, int content_height,
                                 int client_width, int client_height,
                                 double dpi_scale, int horizontal_margin,
                                 int vertical_margin) {
  if (content_width <= 0 || content_height <= 0 || client_width <= 0 ||
      client_height <= 0 || dpi_scale <= 0.0) {
    return 1.0;
  }
  const double horizontal =
      static_cast<double>(std::max(1, client_width - horizontal_margin)) /
      (content_width * dpi_scale);
  const double vertical =
      static_cast<double>(std::max(1, client_height - vertical_margin)) /
      (content_height * dpi_scale);
  return std::clamp(std::min(horizontal, vertical), kMinimumZoom,
                    kMaximumFitZoom);
}

SchematicViewport CalculateSchematicViewport(int client_width,
                                             int client_height, int scroll_x,
                                             int scroll_y,
                                             double effective_scale,
                                             int logical_margin) {
  if (effective_scale <= 0.0) return {};
  SchematicViewport result;
  result.logical_bounds = {
      static_cast<int>(std::floor(scroll_x / effective_scale)) - logical_margin,
      static_cast<int>(std::floor(scroll_y / effective_scale)) - logical_margin,
      static_cast<int>(std::ceil((scroll_x + client_width) / effective_scale)) +
          logical_margin,
      static_cast<int>(
          std::ceil((scroll_y + client_height) / effective_scale)) +
          logical_margin};
  result.detail = effective_scale < 0.45  ? SchematicDetail::kOverview
                  : effective_scale < 0.8 ? SchematicDetail::kSymbols
                                          : SchematicDetail::kFull;
  return result;
}

bool SchematicRectIsVisible(const OrthogonalRect& bounds,
                            const OrthogonalRect& viewport) {
  return bounds.right >= viewport.left && bounds.left <= viewport.right &&
         bounds.bottom >= viewport.top && bounds.top <= viewport.bottom;
}

bool SchematicPointIsVisible(OrthogonalPoint point,
                             const OrthogonalRect& viewport) {
  return point.x >= viewport.left && point.x <= viewport.right &&
         point.y >= viewport.top && point.y <= viewport.bottom;
}

bool SchematicSegmentIsVisible(OrthogonalPoint start, OrthogonalPoint end,
                               const OrthogonalRect& viewport) {
  const OrthogonalRect segment{
      std::min(start.x, end.x), std::min(start.y, end.y),
      std::max(start.x, end.x), std::max(start.y, end.y)};
  return SchematicRectIsVisible(segment, viewport);
}

}  // namespace designpp::gui

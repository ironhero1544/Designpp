// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_GUI_ORTHOGONAL_EDGE_ROUTER_H_
#define DESIGNPP_GUI_ORTHOGONAL_EDGE_ROUTER_H_

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace designpp::gui {

struct OrthogonalPoint {
  int x = 0;
  int y = 0;

  bool operator==(const OrthogonalPoint&) const = default;
};

struct OrthogonalRect {
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;
};

struct OrthogonalNet {
  std::string id;
  OrthogonalPoint source;
  std::vector<OrthogonalPoint> sinks;
  std::optional<int> preferred_track_y;
};

struct OrthogonalRoute {
  std::string net_id;
  std::vector<OrthogonalPoint> points;
};

struct OrthogonalBridge {
  OrthogonalPoint point;
  bool horizontal_over = true;
};

struct OrthogonalRoutingRequest {
  int width = 0;
  int height = 0;
  int grid = 8;
  int level_spacing = 192;
  int obstacle_clearance = 8;
  bool fast_mode = false;
  std::function<bool()> cancelled = [] { return false; };
  std::vector<OrthogonalRect> obstacles;
  std::vector<OrthogonalNet> nets;
};

struct OrthogonalRoutingResult {
  std::vector<OrthogonalRoute> routes;
  std::vector<OrthogonalPoint> junctions;
  std::vector<OrthogonalBridge> bridges;
  std::vector<std::string> unrouted_net_ids;
};

// Routes directed schematic nets using only horizontal and vertical segments.
// Obstacles are never crossed. Calls are deterministic for the same request.
class OrthogonalEdgeRouter final {
 public:
  [[nodiscard]] OrthogonalRoutingResult Route(
      const OrthogonalRoutingRequest& request) const;
};

}  // namespace designpp::gui

#endif  // DESIGNPP_GUI_ORTHOGONAL_EDGE_ROUTER_H_

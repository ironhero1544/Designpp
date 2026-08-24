// Copyright 2026 The Design++ Authors

#include <algorithm>
#include <cmath>

#include "CppUnitTest.h"
#include "designpp/gui/orthogonal_edge_router.h"

using Microsoft::VisualStudio::CppUnitTestFramework::Assert;

namespace designpp::tests {
namespace {

bool RouteContainsHorizontalSegmentAt(const gui::OrthogonalRoute& route, int y,
                                      int minimum_x, int maximum_x) {
  for (std::size_t index = 1; index < route.points.size(); ++index) {
    const gui::OrthogonalPoint start = route.points[index - 1];
    const gui::OrthogonalPoint end = route.points[index];
    if (start.y == y && end.y == y && std::min(start.x, end.x) <= minimum_x &&
        std::max(start.x, end.x) >= maximum_x) {
      return true;
    }
  }
  return false;
}

bool RouteCrossesRect(const gui::OrthogonalRoute& route,
                      const gui::OrthogonalRect& rect) {
  for (std::size_t index = 1; index < route.points.size(); ++index) {
    const gui::OrthogonalPoint start = route.points[index - 1];
    const gui::OrthogonalPoint end = route.points[index];
    if (start.y == end.y && start.y > rect.top && start.y < rect.bottom &&
        std::max(start.x, end.x) > rect.left &&
        std::min(start.x, end.x) < rect.right) {
      return true;
    }
    if (start.x == end.x && start.x > rect.left && start.x < rect.right &&
        std::max(start.y, end.y) > rect.top &&
        std::min(start.y, end.y) < rect.bottom) {
      return true;
    }
  }
  return false;
}

}  // namespace

TEST_CLASS(OrthogonalEdgeRouterTests){
  public : TEST_METHOD(RoutesOnlyOrthogonalSegmentsAndMarksFanout){
      gui::OrthogonalRoutingRequest request;
request.width = 800;
request.height = 500;
request.grid = 8;
request.level_spacing = 192;
request.nets.push_back({"select", {80, 200}, {{300, 120}, {300, 280}}});

const gui::OrthogonalRoutingResult result =
    gui::OrthogonalEdgeRouter().Route(request);

Assert::IsFalse(result.routes.empty());
Assert::IsFalse(result.junctions.empty());
for (const gui::OrthogonalRoute& route : result.routes) {
  for (std::size_t index = 1; index < route.points.size(); ++index) {
    const bool orthogonal =
        route.points[index - 1].x == route.points[index].x ||
        route.points[index - 1].y == route.points[index].y;
    Assert::IsTrue(orthogonal);
  }
}
}  // namespace designpp::tests

TEST_METHOD(AdjacentConnectionUsesUnifiedObstacleCost) {
  gui::OrthogonalRoutingRequest request;
  request.width = 600;
  request.height = 400;
  request.grid = 8;
  request.level_spacing = 192;
  request.obstacle_clearance = 8;
  request.obstacles.push_back({260, 170, 320, 230});
  request.nets.push_back({"adjacent", {200, 200}, {{390, 200}}});

  const gui::OrthogonalRoutingResult result =
      gui::OrthogonalEdgeRouter().Route(request);

  const bool has_detour = std::any_of(
      result.routes.begin(), result.routes.end(),
      [](const gui::OrthogonalRoute& route) {
        return std::any_of(
            route.points.begin(), route.points.end(),
            [](gui::OrthogonalPoint point) { return point.y != 200; });
      });
  Assert::IsTrue(has_detour);
}

TEST_METHOD(VerticalEntryAvoidsGateObstacle) {
  gui::OrthogonalRoutingRequest request;
  request.width = 900;
  request.height = 600;
  request.grid = 8;
  request.obstacle_clearance = 8;
  request.obstacles.push_back({216, 180, 232, 280});
  request.nets.push_back({"vertical", {200, 100}, {{650, 400}}});

  const gui::OrthogonalRoutingResult result =
      gui::OrthogonalEdgeRouter().Route(request);

  const gui::OrthogonalRect expanded_obstacle{208, 172, 240, 288};
  const bool crosses_obstacle =
      std::any_of(result.routes.begin(), result.routes.end(),
                  [&expanded_obstacle](const gui::OrthogonalRoute& route) {
                    return RouteCrossesRect(route, expanded_obstacle);
                  });
  Assert::IsFalse(crosses_obstacle);
}

TEST_METHOD(MultiSinkNetUsesMedianSharedTrunk) {
  gui::OrthogonalRoutingRequest request;
  request.width = 900;
  request.height = 600;
  request.grid = 8;
  request.nets.push_back(
      {"fanout", {160, 80}, {{600, 160}, {650, 240}, {620, 320}}});

  const gui::OrthogonalRoutingResult result =
      gui::OrthogonalEdgeRouter().Route(request);

  const bool has_median_trunk = std::any_of(
      result.routes.begin(), result.routes.end(),
      [](const gui::OrthogonalRoute& route) {
        return RouteContainsHorizontalSegmentAt(route, 240, 184, 576);
      });
  Assert::IsTrue(has_median_trunk);
  Assert::IsTrue(result.junctions.size() >= 2U);
}

TEST_METHOD(DistinctNetCrossingProducesSingleBridge) {
  gui::OrthogonalRoutingRequest request;
  request.width = 900;
  request.height = 600;
  request.grid = 8;
  request.nets.push_back({"horizontal", {100, 200}, {{700, 200}}});
  request.nets.push_back({"vertical", {400, 80}, {{400, 360}}});

  const gui::OrthogonalRoutingResult result =
      gui::OrthogonalEdgeRouter().Route(request);

  Assert::IsFalse(result.bridges.empty());
  Assert::IsTrue(result.bridges.size() <= 2U);
}

TEST_METHOD(UnalignedPinDoesNotCreateSnapTailOrFalseJunction) {
  gui::OrthogonalRoutingRequest request;
  request.width = 600;
  request.height = 400;
  request.grid = 8;
  request.nets.push_back({"unaligned", {80, 203}, {{400, 203}}});

  const gui::OrthogonalRoutingResult result =
      gui::OrthogonalEdgeRouter().Route(request);

  const auto source_stub = std::find_if(
      result.routes.begin(), result.routes.end(),
      [](const gui::OrthogonalRoute& route) {
        return !route.points.empty() && route.points.front().x == 80 &&
               route.points.front().y == 203;
      });
  Assert::IsTrue(source_stub != result.routes.end());
  Assert::IsTrue(source_stub->points.size() >= 2U);
  Assert::AreEqual(203, source_stub->points.back().y);
  Assert::IsTrue(result.junctions.empty());
}

TEST_METHOD(DoglegStopsWhenItReachesSharedTrunk) {
  gui::OrthogonalRoutingRequest request;
  request.width = 700;
  request.height = 500;
  request.grid = 8;
  request.obstacle_clearance = 8;
  request.obstacles.push_back({120, 140, 136, 220});
  request.nets.push_back({"dogleg", {100, 100}, {{500, 300}}});

  const gui::OrthogonalRoutingResult result =
      gui::OrthogonalEdgeRouter().Route(request);

  const auto source_connection = std::find_if(
      result.routes.begin(), result.routes.end(),
      [](const gui::OrthogonalRoute& route) {
        return !route.points.empty() && route.points.front().x == 128 &&
               route.points.front().y == 100;
      });
  Assert::IsTrue(source_connection != result.routes.end());
  Assert::AreEqual<std::size_t>(3U, source_connection->points.size());
  Assert::AreEqual(source_connection->points[1].x,
                   source_connection->points[2].x);
}

TEST_METHOD(MultiSinkConnectionsNeverBacktrackIntoPinStubs) {
  gui::OrthogonalRoutingRequest request;
  request.width = 800;
  request.height = 600;
  request.grid = 8;
  request.nets.push_back({"fanout", {100, 100}, {{400, 260}, {600, 420}}});

  const gui::OrthogonalRoutingResult result =
      gui::OrthogonalEdgeRouter().Route(request);
  const auto find_connection = [&result](int x, int y) {
    return std::find_if(result.routes.begin(), result.routes.end(),
                        [x, y](const gui::OrthogonalRoute& route) {
                          return !route.points.empty() &&
                                 route.points.front().x == x &&
                                 route.points.front().y == y;
                        });
  };

  const auto source = find_connection(128, 100);
  const auto first_sink = find_connection(376, 260);
  const auto second_sink = find_connection(576, 420);
  Assert::IsTrue(source != result.routes.end());
  Assert::IsTrue(first_sink != result.routes.end());
  Assert::IsTrue(second_sink != result.routes.end());
  Assert::IsTrue(source->points.size() >= 2U);
  Assert::IsTrue(first_sink->points.size() >= 2U);
  Assert::IsTrue(second_sink->points.size() >= 2U);
  Assert::IsTrue(source->points[1].x >= source->points[0].x);
  Assert::IsTrue(first_sink->points[1].x <= first_sink->points[0].x);
  Assert::IsTrue(second_sink->points[1].x <= second_sink->points[0].x);
}

TEST_METHOD(DenseObstacleFieldRetainsBoundaryDetour) {
  gui::OrthogonalRoutingRequest request;
  request.width = 900;
  request.height = 600;
  request.grid = 8;
  request.obstacle_clearance = 8;
  request.obstacles = {
      {200, 64, 280, 520},
      {320, 40, 400, 496},
      {440, 88, 520, 544},
      {560, 56, 640, 512},
  };
  request.nets.push_back({"boundary", {120, 300}, {{720, 300}}});

  const gui::OrthogonalRoutingResult result =
      gui::OrthogonalEdgeRouter().Route(request);

  Assert::IsTrue(result.unrouted_net_ids.empty());
  Assert::IsFalse(result.routes.empty());
  for (const gui::OrthogonalRoute& route : result.routes) {
    for (const gui::OrthogonalRect& obstacle : request.obstacles) {
      Assert::IsFalse(RouteCrossesRect(route, obstacle));
    }
  }
}

TEST_METHOD(FastModeKeepsFanoutJunctionsWithBoundedSearch) {
  gui::OrthogonalRoutingRequest request;
  request.width = 1'200;
  request.height = 800;
  request.grid = 8;
  request.fast_mode = true;
  request.nets.push_back(
      {"fanout", {120, 120}, {{800, 240}, {850, 400}, {900, 560}}});

  const gui::OrthogonalRoutingResult result =
      gui::OrthogonalEdgeRouter().Route(request);

  Assert::IsTrue(result.unrouted_net_ids.empty());
  Assert::IsFalse(result.routes.empty());
  Assert::IsFalse(result.junctions.empty());
  Assert::IsTrue(result.bridges.empty());
}

TEST_METHOD(CancellationStopsRoutingInsideNetLoop) {
  gui::OrthogonalRoutingRequest request;
  request.width = 800;
  request.height = 500;
  request.grid = 8;
  request.cancelled = [] { return true; };
  request.nets.push_back({"cancelled", {80, 80}, {{700, 400}}});

  const gui::OrthogonalRoutingResult result =
      gui::OrthogonalEdgeRouter().Route(request);

  Assert::IsTrue(result.routes.empty());
}
}
;

}  // namespace designpp::tests

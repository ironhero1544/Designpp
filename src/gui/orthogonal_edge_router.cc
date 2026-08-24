// Copyright 2026 The Design++ Authors

#include "designpp/gui/orthogonal_edge_router.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <string_view>
#include <utility>

namespace designpp::gui {
namespace {

constexpr int kOverlapPenalty = 2'000;
constexpr int kCrossingPenalty = 180;
constexpr int kParallelClearancePenalty = 60;
constexpr int kBendPenalty = 32;
constexpr std::size_t kMaximumTrackCandidates = 16U;
constexpr std::size_t kMaximumDoglegLanes = 16U;

struct Segment {
  std::string net_id;
  OrthogonalPoint start;
  OrthogonalPoint end;
};

struct PointLess {
  bool operator()(const OrthogonalPoint& left,
                  const OrthogonalPoint& right) const {
    return left.x != right.x ? left.x < right.x : left.y < right.y;
  }
};

int Minimum(int first, int second) { return std::min(first, second); }

int Maximum(int first, int second) { return std::max(first, second); }

bool IsHorizontal(const Segment& segment) {
  return segment.start.y == segment.end.y;
}

bool IsVertical(const Segment& segment) {
  return segment.start.x == segment.end.x;
}

bool RangesOverlap(int first_start, int first_end, int second_start,
                   int second_end) {
  return Maximum(first_start, second_start) < Minimum(first_end, second_end);
}

bool RangeContains(int value, int first, int second) {
  return value >= Minimum(first, second) && value <= Maximum(first, second);
}

bool SegmentCrossesObstacle(const Segment& segment,
                            const OrthogonalRect& obstacle, int clearance) {
  if (IsHorizontal(segment)) {
    return RangesOverlap(segment.start.x, segment.end.x,
                         obstacle.left - clearance,
                         obstacle.right + clearance) &&
           segment.start.y > obstacle.top - clearance &&
           segment.start.y < obstacle.bottom + clearance;
  }
  if (IsVertical(segment)) {
    return RangesOverlap(segment.start.y, segment.end.y,
                         obstacle.top - clearance,
                         obstacle.bottom + clearance) &&
           segment.start.x > obstacle.left - clearance &&
           segment.start.x < obstacle.right + clearance;
  }
  return true;
}

bool SegmentIsClear(const Segment& segment,
                    const OrthogonalRoutingRequest& request) {
  return std::none_of(request.obstacles.begin(), request.obstacles.end(),
                      [&segment, &request](const OrthogonalRect& obstacle) {
                        return SegmentCrossesObstacle(
                            segment, obstacle, request.obstacle_clearance);
                      });
}

int SegmentLength(const Segment& segment) {
  return std::abs(segment.end.x - segment.start.x) +
         std::abs(segment.end.y - segment.start.y);
}

bool SegmentsCross(const Segment& first, const Segment& second,
                   OrthogonalPoint* crossing) {
  if (IsHorizontal(first) == IsHorizontal(second)) return false;
  const Segment& horizontal = IsHorizontal(first) ? first : second;
  const Segment& vertical = IsHorizontal(first) ? second : first;
  const OrthogonalPoint point{vertical.start.x, horizontal.start.y};
  if (!RangeContains(point.x, horizontal.start.x, horizontal.end.x) ||
      !RangeContains(point.y, vertical.start.y, vertical.end.y)) {
    return false;
  }
  if (crossing != nullptr) *crossing = point;
  return true;
}

int CongestionCost(const Segment& candidate,
                   const std::vector<Segment>& occupied, int grid) {
  int cost = 0;
  for (const Segment& existing : occupied) {
    if (candidate.net_id == existing.net_id) continue;
    if (IsHorizontal(candidate) == IsHorizontal(existing)) {
      if (IsHorizontal(candidate)) {
        if (candidate.start.y == existing.start.y &&
            RangesOverlap(candidate.start.x, candidate.end.x, existing.start.x,
                          existing.end.x)) {
          cost += kOverlapPenalty;
        } else if (std::abs(candidate.start.y - existing.start.y) < grid &&
                   RangesOverlap(candidate.start.x, candidate.end.x,
                                 existing.start.x, existing.end.x)) {
          cost += kParallelClearancePenalty;
        }
      } else {
        if (candidate.start.x == existing.start.x &&
            RangesOverlap(candidate.start.y, candidate.end.y, existing.start.y,
                          existing.end.y)) {
          cost += kOverlapPenalty;
        } else if (std::abs(candidate.start.x - existing.start.x) < grid &&
                   RangesOverlap(candidate.start.y, candidate.end.y,
                                 existing.start.y, existing.end.y)) {
          cost += kParallelClearancePenalty;
        }
      }
      continue;
    }
    OrthogonalPoint crossing;
    if (SegmentsCross(candidate, existing, &crossing)) {
      cost += kCrossingPenalty;
    }
  }
  return cost;
}

void AppendPoint(std::vector<OrthogonalPoint>* points, OrthogonalPoint point) {
  if (!points->empty() && points->back() == point) return;
  if (points->size() >= 2U) {
    const OrthogonalPoint& previous = (*points)[points->size() - 2U];
    const OrthogonalPoint& current = points->back();
    if ((previous.x == current.x && current.x == point.x) ||
        (previous.y == current.y && current.y == point.y)) {
      points->back() = point;
      return;
    }
  }
  points->push_back(point);
}

std::vector<Segment> RouteSegments(const OrthogonalRoute& route) {
  std::vector<Segment> segments;
  for (std::size_t index = 1; index < route.points.size(); ++index) {
    if (route.points[index - 1] == route.points[index]) continue;
    segments.push_back(
        {route.net_id, route.points[index - 1], route.points[index]});
  }
  return segments;
}

void CommitRoute(OrthogonalRoute route, OrthogonalRoutingResult* result,
                 std::vector<Segment>* occupied) {
  const std::vector<Segment> segments = RouteSegments(route);
  occupied->insert(occupied->end(), segments.begin(), segments.end());
  result->routes.push_back(std::move(route));
}

int SnapToGrid(int value, int grid) {
  return static_cast<int>(std::lround(static_cast<double>(value) / grid)) *
         grid;
}

void AddCoordinate(std::vector<int>* coordinates, int value, int minimum,
                   int maximum, int grid) {
  coordinates->push_back(std::clamp(SnapToGrid(value, grid), minimum, maximum));
}

std::vector<int> PrioritizeCoordinates(std::vector<int> coordinates,
                                       int preferred,
                                       std::size_t maximum_count) {
  std::sort(coordinates.begin(), coordinates.end());
  coordinates.erase(std::unique(coordinates.begin(), coordinates.end()),
                    coordinates.end());
  std::stable_sort(
      coordinates.begin(), coordinates.end(), [preferred](int left, int right) {
        const int left_distance = std::abs(left - preferred);
        const int right_distance = std::abs(right - preferred);
        return left_distance != right_distance ? left_distance < right_distance
                                               : left < right;
      });
  if (coordinates.size() > maximum_count) {
    coordinates.resize(maximum_count);
  }
  return coordinates;
}

std::vector<int> CandidateDoglegLanes(OrthogonalPoint terminal,
                                      int trunk_minimum_x, int trunk_maximum_x,
                                      const OrthogonalRoutingRequest& request,
                                      const std::vector<Segment>& occupied) {
  const int minimum_x = trunk_minimum_x;
  const int maximum_x = std::max(minimum_x, trunk_maximum_x);
  std::vector<int> lanes;
  AddCoordinate(&lanes, terminal.x, minimum_x, maximum_x, request.grid);
  for (int offset = 1; offset <= 4; ++offset) {
    AddCoordinate(&lanes, terminal.x - request.grid * offset, minimum_x,
                  maximum_x, request.grid);
    AddCoordinate(&lanes, terminal.x + request.grid * offset, minimum_x,
                  maximum_x, request.grid);
  }
  if (request.fast_mode) {
    return PrioritizeCoordinates(std::move(lanes), terminal.x, 3U);
  }
  for (const OrthogonalRect& obstacle : request.obstacles) {
    AddCoordinate(&lanes,
                  obstacle.left - request.obstacle_clearance - request.grid,
                  minimum_x, maximum_x, request.grid);
    AddCoordinate(&lanes,
                  obstacle.right + request.obstacle_clearance + request.grid,
                  minimum_x, maximum_x, request.grid);
  }
  for (const Segment& segment : occupied) {
    if (!IsVertical(segment)) continue;
    AddCoordinate(&lanes, segment.start.x - request.grid, minimum_x, maximum_x,
                  request.grid);
    AddCoordinate(&lanes, segment.start.x + request.grid, minimum_x, maximum_x,
                  request.grid);
  }
  return PrioritizeCoordinates(std::move(lanes), terminal.x,
                               kMaximumDoglegLanes);
}

std::vector<int> CandidateTrunkTracks(
    OrthogonalPoint source_escape,
    const std::vector<OrthogonalPoint>& sink_escapes, int preferred_y,
    const OrthogonalRoutingRequest& request,
    const std::vector<Segment>& occupied) {
  const int minimum_y = request.grid * 4;
  const int maximum_y = std::max(minimum_y, request.height - request.grid * 3);
  std::vector<int> tracks;
  AddCoordinate(&tracks, preferred_y, minimum_y, maximum_y, request.grid);
  AddCoordinate(&tracks, source_escape.y, minimum_y, maximum_y, request.grid);
  for (const OrthogonalPoint sink : sink_escapes) {
    AddCoordinate(&tracks, sink.y, minimum_y, maximum_y, request.grid);
  }
  for (int offset = 1; offset <= 4; ++offset) {
    AddCoordinate(&tracks, preferred_y - request.grid * offset, minimum_y,
                  maximum_y, request.grid);
    AddCoordinate(&tracks, preferred_y + request.grid * offset, minimum_y,
                  maximum_y, request.grid);
  }
  if (request.fast_mode) {
    return PrioritizeCoordinates(std::move(tracks), preferred_y, 3U);
  }
  for (const OrthogonalRect& obstacle : request.obstacles) {
    AddCoordinate(&tracks, obstacle.top - request.obstacle_clearance, minimum_y,
                  maximum_y, request.grid);
    AddCoordinate(&tracks, obstacle.bottom + request.obstacle_clearance,
                  minimum_y, maximum_y, request.grid);
  }
  for (const Segment& segment : occupied) {
    if (!IsHorizontal(segment)) continue;
    AddCoordinate(&tracks, segment.start.y - request.grid, minimum_y, maximum_y,
                  request.grid);
    AddCoordinate(&tracks, segment.start.y + request.grid, minimum_y, maximum_y,
                  request.grid);
  }
  return PrioritizeCoordinates(std::move(tracks), preferred_y,
                               kMaximumTrackCandidates);
}

OrthogonalPoint EscapePoint(OrthogonalPoint pin, int grid, bool source) {
  const int offset = grid * 3 * (source ? 1 : -1);
  return {SnapToGrid(pin.x + offset, grid), pin.y};
}

int SegmentsCost(const std::vector<Segment>& candidate,
                 const OrthogonalRoutingRequest& request,
                 const std::vector<Segment>& occupied) {
  int cost = 0;
  for (const Segment& segment : candidate) {
    if (!SegmentIsClear(segment, request)) {
      return std::numeric_limits<int>::max();
    }
    cost += SegmentLength(segment);
    if (!request.fast_mode) {
      cost += CongestionCost(segment, occupied, request.grid);
    }
  }
  return cost +
         std::max(0, static_cast<int>(candidate.size()) - 1) * kBendPenalty;
}

std::optional<OrthogonalRoute> BestTrunkConnection(
    std::string_view net_id, OrthogonalPoint terminal, int trunk_y,
    int lane_minimum_x, int lane_maximum_x,
    const OrthogonalRoutingRequest& request,
    const std::vector<Segment>& occupied, int* connection_cost) {
  OrthogonalRoute best_route;
  int best_cost = std::numeric_limits<int>::max();
  const auto consider_lane = [&](int lane_x) {
    OrthogonalRoute route;
    route.net_id = std::string(net_id);
    AppendPoint(&route.points, terminal);
    AppendPoint(&route.points, {lane_x, terminal.y});
    AppendPoint(&route.points, {lane_x, trunk_y});
    const int cost = SegmentsCost(RouteSegments(route), request, occupied);
    if (cost < best_cost) {
      best_cost = cost;
      best_route = std::move(route);
    }
  };

  consider_lane(terminal.x);
  const int direct_length = std::abs(terminal.y - trunk_y);
  if (best_cost == direct_length) {
    *connection_cost = best_cost;
    return best_route;
  }
  for (const int lane_x : CandidateDoglegLanes(
           terminal, lane_minimum_x, lane_maximum_x, request, occupied)) {
    if (lane_x != terminal.x) consider_lane(lane_x);
  }
  // The prioritized list intentionally stays bounded, but a dense schematic
  // can push the only valid detour far from the pin. Always retain the two
  // channel boundaries as last-resort dogleg lanes.
  consider_lane(lane_minimum_x);
  consider_lane(lane_maximum_x);
  if (best_cost == std::numeric_limits<int>::max()) return std::nullopt;
  *connection_cost = best_cost;
  return best_route;
}

struct RoutingCandidate {
  int cost = std::numeric_limits<int>::max();
  std::vector<OrthogonalRoute> routes;
};

std::optional<RoutingCandidate> BuildRoutingCandidate(
    std::string_view net_id, OrthogonalPoint source_escape,
    const std::vector<OrthogonalPoint>& sink_escapes, int trunk_y,
    int preferred_y, bool routes_right, const OrthogonalRoutingRequest& request,
    const std::vector<Segment>& occupied) {
  int minimum_x = source_escape.x;
  int maximum_x = source_escape.x;
  for (const OrthogonalPoint sink : sink_escapes) {
    minimum_x = std::min(minimum_x, sink.x);
    maximum_x = std::max(maximum_x, sink.x);
  }

  RoutingCandidate candidate;
  candidate.cost = std::abs(trunk_y - preferred_y);
  std::vector<OrthogonalRoute> connections;
  int attachment_minimum_x = maximum_x;
  int attachment_maximum_x = minimum_x;

  const auto add_connection = [&](OrthogonalPoint terminal, bool source) {
    const bool lane_moves_right = source == routes_right;
    const int lane_minimum_x = lane_moves_right ? terminal.x : minimum_x;
    const int lane_maximum_x = lane_moves_right ? maximum_x : terminal.x;
    int connection_cost = 0;
    std::optional<OrthogonalRoute> connection = BestTrunkConnection(
        net_id, terminal, trunk_y, lane_minimum_x, lane_maximum_x, request,
        occupied, &connection_cost);
    if (!connection.has_value()) return false;
    candidate.cost += connection_cost;
    const OrthogonalPoint attachment = connection->points.back();
    attachment_minimum_x = std::min(attachment_minimum_x, attachment.x);
    attachment_maximum_x = std::max(attachment_maximum_x, attachment.x);
    connections.push_back(std::move(*connection));
    return true;
  };
  if (!add_connection(source_escape, true)) return std::nullopt;
  for (const OrthogonalPoint sink : sink_escapes) {
    if (!add_connection(sink, false)) return std::nullopt;
  }
  OrthogonalRoute trunk;
  trunk.net_id = std::string(net_id);
  AppendPoint(&trunk.points, {attachment_minimum_x, trunk_y});
  AppendPoint(&trunk.points, {attachment_maximum_x, trunk_y});
  const int trunk_cost = SegmentsCost(RouteSegments(trunk), request, occupied);
  if (trunk_cost == std::numeric_limits<int>::max()) return std::nullopt;
  candidate.cost += trunk_cost;
  candidate.routes.push_back(std::move(trunk));
  candidate.routes.insert(candidate.routes.end(),
                          std::make_move_iterator(connections.begin()),
                          std::make_move_iterator(connections.end()));
  return candidate;
}

void AddPinStub(std::string_view net_id, OrthogonalPoint pin,
                OrthogonalPoint escape, OrthogonalRoutingResult* result,
                std::vector<Segment>* occupied) {
  OrthogonalRoute route;
  route.net_id = std::string(net_id);
  AppendPoint(&route.points, pin);
  AppendPoint(&route.points, {escape.x, pin.y});
  AppendPoint(&route.points, escape);
  CommitRoute(std::move(route), result, occupied);
}

std::set<OrthogonalPoint, PointLess> CandidateConnectionPoints(
    const std::vector<Segment>& segments) {
  std::set<OrthogonalPoint, PointLess> points;
  for (const Segment& segment : segments) {
    points.insert(segment.start);
    points.insert(segment.end);
  }
  for (std::size_t first = 0; first < segments.size(); ++first) {
    for (std::size_t second = first + 1; second < segments.size(); ++second) {
      OrthogonalPoint crossing;
      if (SegmentsCross(segments[first], segments[second], &crossing)) {
        points.insert(crossing);
      }
    }
  }
  return points;
}

int DirectionDegreeAt(OrthogonalPoint point,
                      const std::vector<Segment>& segments) {
  bool left = false;
  bool right = false;
  bool up = false;
  bool down = false;
  for (const Segment& segment : segments) {
    if (IsHorizontal(segment) && point.y == segment.start.y &&
        RangeContains(point.x, segment.start.x, segment.end.x)) {
      left = left || Minimum(segment.start.x, segment.end.x) < point.x;
      right = right || Maximum(segment.start.x, segment.end.x) > point.x;
    } else if (IsVertical(segment) && point.x == segment.start.x &&
               RangeContains(point.y, segment.start.y, segment.end.y)) {
      up = up || Minimum(segment.start.y, segment.end.y) < point.y;
      down = down || Maximum(segment.start.y, segment.end.y) > point.y;
    }
  }
  return static_cast<int>(left) + static_cast<int>(right) +
         static_cast<int>(up) + static_cast<int>(down);
}

void AnalyzeJunctions(const std::vector<Segment>& occupied,
                      OrthogonalRoutingResult* result) {
  std::map<std::string, std::vector<Segment>> by_net;
  for (const Segment& segment : occupied) {
    by_net[segment.net_id].push_back(segment);
  }
  for (const auto& [net_id, segments] : by_net) {
    static_cast<void>(net_id);
    for (const OrthogonalPoint point : CandidateConnectionPoints(segments)) {
      if (DirectionDegreeAt(point, segments) >= 3) {
        result->junctions.push_back(point);
      }
    }
  }
}

void AnalyzeTopology(const std::vector<Segment>& occupied, int grid,
                     OrthogonalRoutingResult* result) {
  AnalyzeJunctions(occupied, result);

  for (std::size_t first = 0; first < occupied.size(); ++first) {
    for (std::size_t second = first + 1; second < occupied.size(); ++second) {
      if (occupied[first].net_id == occupied[second].net_id) continue;
      OrthogonalPoint crossing;
      if (!SegmentsCross(occupied[first], occupied[second], &crossing)) {
        continue;
      }
      const auto near_endpoint = [grid, crossing](const Segment& segment) {
        const auto distance = [crossing](OrthogonalPoint point) {
          return std::abs(point.x - crossing.x) +
                 std::abs(point.y - crossing.y);
        };
        return distance(segment.start) < grid * 2 ||
               distance(segment.end) < grid * 2;
      };
      if (near_endpoint(occupied[first]) || near_endpoint(occupied[second])) {
        continue;
      }
      const bool crowded =
          std::any_of(result->bridges.begin(), result->bridges.end(),
                      [crossing, grid](const OrthogonalBridge& bridge) {
                        return std::abs(bridge.point.x - crossing.x) +
                                   std::abs(bridge.point.y - crossing.y) <
                               grid * 3;
                      });
      const bool is_junction =
          std::any_of(result->junctions.begin(), result->junctions.end(),
                      [crossing, grid](OrthogonalPoint junction) {
                        return std::abs(junction.x - crossing.x) +
                                   std::abs(junction.y - crossing.y) <
                               grid * 2;
                      });
      if (!crowded && !is_junction) {
        result->bridges.push_back({crossing, true});
      }
    }
  }
}

}  // namespace

OrthogonalRoutingResult OrthogonalEdgeRouter::Route(
    const OrthogonalRoutingRequest& request) const {
  OrthogonalRoutingResult result;
  if (request.width <= 0 || request.height <= 0 || request.grid <= 0) {
    return result;
  }

  std::vector<OrthogonalNet> nets = request.nets;
  std::stable_sort(nets.begin(), nets.end(),
                   [](const OrthogonalNet& left, const OrthogonalNet& right) {
                     if (left.sinks.size() != right.sinks.size()) {
                       return left.sinks.size() > right.sinks.size();
                     }
                     const auto span = [](const OrthogonalNet& net) {
                       int result = 0;
                       for (const OrthogonalPoint sink : net.sinks) {
                         result =
                             std::max(result, std::abs(sink.x - net.source.x));
                       }
                       return result;
                     };
                     return span(left) > span(right);
                   });

  std::vector<Segment> occupied;
  for (const OrthogonalNet& net : nets) {
    if (request.cancelled()) return {};
    if (net.sinks.empty()) continue;
    const bool routes_right =
        std::accumulate(net.sinks.begin(), net.sinks.end(), 0LL,
                        [&net](long long total, OrthogonalPoint sink) {
                          return total + sink.x - net.source.x;
                        }) >= 0;
    const OrthogonalPoint source_escape =
        EscapePoint(net.source, request.grid, true);
    std::vector<OrthogonalPoint> sink_escapes;
    sink_escapes.reserve(net.sinks.size());
    for (const OrthogonalPoint sink : net.sinks) {
      sink_escapes.push_back(EscapePoint(sink, request.grid, false));
    }

    std::vector<int> sink_y;
    sink_y.reserve(sink_escapes.size());
    for (const OrthogonalPoint sink : sink_escapes) sink_y.push_back(sink.y);
    std::sort(sink_y.begin(), sink_y.end());
    const int preferred_y =
        net.preferred_track_y.value_or(sink_y[sink_y.size() / 2U]);
    std::optional<RoutingCandidate> best_candidate;
    for (const int candidate_y : CandidateTrunkTracks(
             source_escape, sink_escapes, preferred_y, request, occupied)) {
      if (request.cancelled()) return {};
      std::optional<RoutingCandidate> candidate = BuildRoutingCandidate(
          net.id, source_escape, sink_escapes, candidate_y, preferred_y,
          routes_right, request, occupied);
      if (candidate.has_value() && (!best_candidate.has_value() ||
                                    candidate->cost < best_candidate->cost)) {
        best_candidate = std::move(candidate);
        if (request.fast_mode) break;
      }
    }
    if (!best_candidate.has_value()) {
      const int top_track = request.grid * 4;
      const int bottom_track =
          std::max(top_track, request.height - request.grid * 3);
      for (const int candidate_y : {top_track, bottom_track}) {
        std::optional<RoutingCandidate> candidate = BuildRoutingCandidate(
            net.id, source_escape, sink_escapes, candidate_y, preferred_y,
            routes_right, request, occupied);
        if (candidate.has_value() && (!best_candidate.has_value() ||
                                      candidate->cost < best_candidate->cost)) {
          best_candidate = std::move(candidate);
        }
      }
    }
    if (!best_candidate.has_value()) {
      result.unrouted_net_ids.push_back(net.id);
      continue;
    }

    AddPinStub(net.id, net.source, source_escape, &result, &occupied);
    for (OrthogonalRoute& route : best_candidate->routes) {
      CommitRoute(std::move(route), &result, &occupied);
    }
    for (std::size_t index = 0; index < net.sinks.size(); ++index) {
      AddPinStub(net.id, net.sinks[index], sink_escapes[index], &result,
                 &occupied);
    }
  }

  if (!request.fast_mode) {
    AnalyzeTopology(occupied, request.grid, &result);
  } else {
    AnalyzeJunctions(occupied, &result);
  }
  return result;
}

}  // namespace designpp::gui

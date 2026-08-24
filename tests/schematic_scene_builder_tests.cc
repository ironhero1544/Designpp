// Copyright 2026 The Design++ Authors

#include <CppUnitTest.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>

#include "designpp/gui/schematic_build_service.h"
#include "designpp/gui/schematic_scene_builder.h"

using Microsoft::VisualStudio::CppUnitTestFramework::Assert;

namespace designpp::tests {
namespace {

core::SchematicPin Pin(std::string name, core::SchematicDirection direction,
                       core::SchematicPinRole role,
                       std::vector<std::string> bits) {
  return {std::move(name), direction, role, std::move(bits)};
}

core::SchematicModel TimerModel() {
  core::SchematicModel model;
  model.top_module = "timer_1s";
  model.ports = {
      {"clk", core::SchematicDirection::kInput, {"2"}},
      {"rst_n", core::SchematicDirection::kInput, {"3"}},
      {"start", core::SchematicDirection::kInput, {"4"}},
      {"done", core::SchematicDirection::kOutput, {"5"}},
  };
  std::vector<std::string> count_bits;
  std::vector<std::string> next_bits;
  for (int bit = 0; bit < 26; ++bit) {
    count_bits.push_back(std::to_string(100 + bit));
    next_bits.push_back(std::to_string(200 + bit));
  }
  model.signals.push_back({"cnt", count_bits, 0, false, false});
  core::SchematicNode counter;
  counter.id = "$counter";
  counter.type = "$adffe";
  counter.label = "DFFE";
  counter.kind = core::SchematicNodeKind::kRegister;
  counter.reset_positive = false;
  counter.pins = {
      Pin("D", core::SchematicDirection::kInput, core::SchematicPinRole::kData,
          next_bits),
      Pin("CLK", core::SchematicDirection::kInput,
          core::SchematicPinRole::kClock, {"2"}),
      Pin("ARST", core::SchematicDirection::kInput,
          core::SchematicPinRole::kReset, {"3"}),
      Pin("Q", core::SchematicDirection::kOutput,
          core::SchematicPinRole::kOutput, count_bits),
  };
  core::SchematicNode add;
  add.id = "$add";
  add.type = "$add";
  add.label = "+";
  add.kind = core::SchematicNodeKind::kArithmetic;
  add.pins = {
      Pin("A", core::SchematicDirection::kInput, core::SchematicPinRole::kData,
          count_bits),
      Pin("B", core::SchematicDirection::kInput, core::SchematicPinRole::kData,
          {"const:1"}),
      Pin("Y", core::SchematicDirection::kOutput,
          core::SchematicPinRole::kOutput, next_bits),
  };
  core::SchematicNode done;
  done.id = "$done";
  done.type = "$adff";
  done.label = "DFF";
  done.kind = core::SchematicNodeKind::kRegister;
  done.reset_positive = false;
  done.pins = {
      Pin("D", core::SchematicDirection::kInput, core::SchematicPinRole::kData,
          {"300"}),
      Pin("CLK", core::SchematicDirection::kInput,
          core::SchematicPinRole::kClock, {"2"}),
      Pin("ARST", core::SchematicDirection::kInput,
          core::SchematicPinRole::kReset, {"3"}),
      Pin("Q", core::SchematicDirection::kOutput,
          core::SchematicPinRole::kOutput, {"5"}),
  };
  model.nodes = {std::move(counter), std::move(add), std::move(done)};
  return model;
}

bool RectanglesOverlap(const gui::OrthogonalRect& left,
                       const gui::OrthogonalRect& right) {
  return left.left < right.right && left.right > right.left &&
         left.top < right.bottom && left.bottom > right.top;
}

bool SegmentEntersRectangle(gui::OrthogonalPoint first,
                            gui::OrthogonalPoint second,
                            const gui::OrthogonalRect& rectangle) {
  if (first.y == second.y) {
    return first.y > rectangle.top && first.y < rectangle.bottom &&
           std::max(first.x, second.x) > rectangle.left &&
           std::min(first.x, second.x) < rectangle.right;
  }
  if (first.x == second.x) {
    return first.x > rectangle.left && first.x < rectangle.right &&
           std::max(first.y, second.y) > rectangle.top &&
           std::min(first.y, second.y) < rectangle.bottom;
  }
  return true;
}

bool RouteTouchesPoint(const gui::OrthogonalRoutingResult& routing,
                       gui::OrthogonalPoint point) {
  return std::any_of(
      routing.routes.begin(), routing.routes.end(), [point](const auto& route) {
        return std::find(route.points.begin(), route.points.end(), point) !=
               route.points.end();
      });
}

}  // namespace

TEST_CLASS(SchematicSceneBuilderTests){
  public :
      TEST_METHOD(SequentialBoundaryBuildsReadableCounterWithoutFallbackColumn){
          gui::SchematicBuildRequest request;
request.model = TimerModel();
request.mode = gui::SchematicViewMode::kReadable;
auto built = gui::SchematicSceneBuilder().Build(std::move(request));

Assert::IsTrue(built.Ok());
const gui::SchematicScene& scene = built.Value();
Assert::AreEqual<std::size_t>(3U, scene.nodes.size());
Assert::AreEqual<std::size_t>(1U, scene.summary.register_bank_count);
const auto counter =
    std::find_if(scene.nodes.begin(), scene.nodes.end(), [](const auto& node) {
      return node.kind == core::SchematicNodeKind::kRegisterBank;
    });
Assert::IsTrue(counter != scene.nodes.end());
Assert::AreEqual(std::string("cnt[25:0]"), counter->label);
const auto add =
    std::find_if(scene.nodes.begin(), scene.nodes.end(), [](const auto& node) {
      return node.kind == core::SchematicNodeKind::kArithmetic;
    });
Assert::IsTrue(add != scene.nodes.end());
Assert::IsTrue(add->bounds.left > counter->bounds.left);
Assert::IsTrue(scene.routing.unrouted_net_ids.empty());
for (std::size_t first = 0; first < scene.nodes.size(); ++first) {
  for (std::size_t second = first + 1; second < scene.nodes.size(); ++second) {
    Assert::IsFalse(RectanglesOverlap(scene.nodes[first].bounds,
                                      scene.nodes[second].bounds));
  }
}
for (const gui::OrthogonalRoute& route : scene.routing.routes) {
  for (std::size_t index = 1; index < route.points.size(); ++index) {
    for (const gui::SchematicSceneNode& node : scene.nodes) {
      Assert::IsFalse(SegmentEntersRectangle(route.points[index - 1],
                                             route.points[index], node.bounds));
    }
  }
}
for (const gui::SchematicSceneNode& node : scene.nodes) {
  for (const gui::SchematicScenePin& pin : node.pins) {
    Assert::IsTrue(RouteTouchesPoint(scene.routing, pin.point));
  }
}
}  // namespace designpp::tests

TEST_METHOD(CombinationalCycleProducesDiagnosticAndDistinctLayout) {
  core::SchematicModel model;
  model.top_module = "loop";
  core::SchematicNode first;
  first.id = "a";
  first.type = "$_NOT_";
  first.label = "NOT";
  first.kind = core::SchematicNodeKind::kPrimitive;
  first.pins = {
      Pin("A", core::SchematicDirection::kInput, core::SchematicPinRole::kData,
          {"11"}),
      Pin("Y", core::SchematicDirection::kOutput,
          core::SchematicPinRole::kOutput, {"10"}),
  };
  core::SchematicNode second = first;
  second.id = "b";
  second.pins.front().bits = {"10"};
  second.pins.back().bits = {"11"};
  model.nodes = {std::move(first), std::move(second)};
  auto built = gui::SchematicSceneBuilder().Build(
      {std::move(model), gui::SchematicViewMode::kReadable});

  Assert::IsTrue(built.Ok());
  Assert::AreEqual<std::size_t>(1U,
                                built.Value().summary.combinational_loop_count);
  Assert::IsFalse(built.Value().diagnostics.empty());
  Assert::IsFalse(RectanglesOverlap(built.Value().nodes[0].bounds,
                                    built.Value().nodes[1].bounds));
}

TEST_METHOD(CompositeInputUsesScalarDriversWithoutUndrivenPins) {
  core::SchematicModel model;
  model.top_module = "composite";
  model.ports = {
      {"left", core::SchematicDirection::kInput, {"2"}},
      {"right", core::SchematicDirection::kInput, {"3"}},
      {"y", core::SchematicDirection::kOutput, {"8"}},
  };
  core::SchematicNode first;
  first.id = "first";
  first.type = "$_BUF_";
  first.label = "BUF";
  first.kind = core::SchematicNodeKind::kPrimitive;
  first.pins = {
      Pin("A", core::SchematicDirection::kInput, core::SchematicPinRole::kData,
          {"2"}),
      Pin("Y", core::SchematicDirection::kOutput,
          core::SchematicPinRole::kOutput, {"6"}),
  };
  core::SchematicNode second = first;
  second.id = "second";
  second.pins.front().bits = {"3"};
  second.pins.back().bits = {"7"};
  core::SchematicNode reduce;
  reduce.id = "reduce";
  reduce.type = "$reduce_bool";
  reduce.label = "ANY";
  reduce.kind = core::SchematicNodeKind::kComparator;
  reduce.pins = {
      Pin("A", core::SchematicDirection::kInput, core::SchematicPinRole::kData,
          {"6", "7"}),
      Pin("Y", core::SchematicDirection::kOutput,
          core::SchematicPinRole::kOutput, {"8"}),
  };
  model.nodes = {std::move(first), std::move(second), std::move(reduce)};

  auto built = gui::SchematicSceneBuilder().Build(
      {std::move(model), gui::SchematicViewMode::kReadable});

  Assert::IsTrue(built.Ok());
  const auto reduced =
      std::find_if(built.Value().nodes.begin(), built.Value().nodes.end(),
                   [](const auto& node) { return node.id == "reduce"; });
  Assert::IsTrue(reduced != built.Value().nodes.end());
  Assert::AreEqual<std::size_t>(3U, reduced->pins.size());
  Assert::AreEqual(std::string("A[0]"), reduced->pins[0].name);
  Assert::AreEqual(std::string("A[1]"), reduced->pins[1].name);
  Assert::IsTrue(built.Value().routing.unrouted_net_ids.empty());
  const auto undriven =
      std::find_if(built.Value().diagnostics.begin(),
                   built.Value().diagnostics.end(), [](const auto& diagnostic) {
                     return diagnostic.code == "SCHEMATIC-UNDRIVEN";
                   });
  Assert::IsTrue(undriven == built.Value().diagnostics.end());
}

TEST_METHOD(VectorSliceRemainsOneReadableBusPin) {
  core::SchematicModel model;
  model.top_module = "slice";
  model.ports = {
      {"a", core::SchematicDirection::kInput, {"2", "3", "4", "5"}},
      {"y", core::SchematicDirection::kOutput, {"8", "9"}},
  };
  core::SchematicNode producer;
  producer.id = "producer";
  producer.type = "$add";
  producer.label = "ADD";
  producer.kind = core::SchematicNodeKind::kArithmetic;
  producer.pins = {
      Pin("A", core::SchematicDirection::kInput, core::SchematicPinRole::kData,
          {"2", "3", "4", "5"}),
      Pin("Y", core::SchematicDirection::kOutput,
          core::SchematicPinRole::kOutput, {"10", "11", "12", "13"}),
  };
  core::SchematicNode consumer;
  consumer.id = "consumer";
  consumer.type = "$eq";
  consumer.label = "==";
  consumer.kind = core::SchematicNodeKind::kComparator;
  consumer.pins = {
      Pin("A", core::SchematicDirection::kInput, core::SchematicPinRole::kData,
          {"10", "11"}),
      Pin("Y", core::SchematicDirection::kOutput,
          core::SchematicPinRole::kOutput, {"8", "9"}),
  };
  model.nodes = {std::move(producer), std::move(consumer)};

  auto built = gui::SchematicSceneBuilder().Build(
      {std::move(model), gui::SchematicViewMode::kReadable});

  Assert::IsTrue(built.Ok());
  const auto consumed =
      std::find_if(built.Value().nodes.begin(), built.Value().nodes.end(),
                   [](const auto& node) { return node.id == "consumer"; });
  Assert::IsTrue(consumed != built.Value().nodes.end());
  Assert::AreEqual<std::size_t>(2U, consumed->pins.size());
  Assert::AreEqual(std::string("A"), consumed->pins.front().name);
  Assert::IsTrue(built.Value().routing.unrouted_net_ids.empty());
  const auto undriven =
      std::find_if(built.Value().diagnostics.begin(),
                   built.Value().diagnostics.end(), [](const auto& diagnostic) {
                     return diagnostic.code == "SCHEMATIC-UNDRIVEN";
                   });
  Assert::IsTrue(undriven == built.Value().diagnostics.end());
}

TEST_METHOD(LongVectorConstantUsesCompactReadableLabel) {
  core::SchematicModel model;
  model.top_module = "constant";
  model.ports = {{"y", core::SchematicDirection::kOutput, {"8"}}};
  core::SchematicNode comparator;
  comparator.id = "compare";
  comparator.type = "$eq";
  comparator.label = "==";
  comparator.kind = core::SchematicNodeKind::kComparator;
  comparator.pins = {
      Pin("A", core::SchematicDirection::kInput, core::SchematicPinRole::kData,
          {"const:1", "const:0", "const:1", "const:1", "const:0", "const:1",
           "const:0", "const:1", "const:0", "const:1", "const:0", "const:0",
           "const:1", "const:1", "const:0", "const:1"}),
      Pin("Y", core::SchematicDirection::kOutput,
          core::SchematicPinRole::kOutput, {"8"}),
  };
  model.nodes = {std::move(comparator)};

  auto built = gui::SchematicSceneBuilder().Build(
      {std::move(model), gui::SchematicViewMode::kReadable});

  Assert::IsTrue(built.Ok());
  const auto constant = std::find_if(
      built.Value().terminals.begin(), built.Value().terminals.end(),
      [](const auto& terminal) { return terminal.label.starts_with("16'h"); });
  Assert::IsTrue(constant != built.Value().terminals.end());
  Assert::IsTrue(constant->label.size() <= 8U);
  const auto route = std::find_if(
      built.Value().routing.routes.begin(), built.Value().routing.routes.end(),
      [](const auto& candidate) {
        return candidate.net_id.starts_with("constant:");
      });
  Assert::IsTrue(route != built.Value().routing.routes.end());
  Assert::AreEqual<std::size_t>(16U,
                                built.Value().net_widths.at(route->net_id));
  Assert::AreEqual<std::size_t>(2U, route->points.size());
  Assert::AreEqual(72, route->points.back().x - route->points.front().x);
  Assert::AreEqual(route->points.front().y, route->points.back().y);
}

TEST_METHOD(OversizedGateViewFailsWithoutPartialScene) {
  core::SchematicModel model;
  model.top_module = "large";
  model.nodes.resize(2'001);
  auto built = gui::SchematicSceneBuilder().Build(
      {std::move(model), gui::SchematicViewMode::kGate});
  Assert::IsFalse(built.Ok());
  Assert::IsTrue(built.GetStatus().code == core::ErrorCode::kFileTooLarge);
}

TEST_METHOD(GateViewRejectsMoreThanEightThousandConnections) {
  core::SchematicModel model;
  model.top_module = "large_connections";
  core::SchematicNode node;
  node.id = "wide";
  node.type = "vendor_wide";
  node.label = "vendor_wide";
  node.kind = core::SchematicNodeKind::kGeneric;
  node.pins.reserve(8'001);
  for (int index = 0; index < 8'001; ++index) {
    node.pins.push_back(
        Pin("A" + std::to_string(index), core::SchematicDirection::kInput,
            core::SchematicPinRole::kData, {std::to_string(index + 2)}));
  }
  model.nodes.push_back(std::move(node));
  auto built = gui::SchematicSceneBuilder().Build(
      {std::move(model), gui::SchematicViewMode::kGate});
  Assert::IsFalse(built.Ok());
  Assert::IsTrue(built.GetStatus().code == core::ErrorCode::kFileTooLarge);
}

TEST_METHOD(RegisterBankDoesNotMergeDifferentClocksOrPolarity) {
  core::SchematicModel model;
  model.top_module = "controls";
  model.signals.push_back({"state", {"10", "11"}, 0, false, false});
  for (int index = 0; index < 2; ++index) {
    core::SchematicNode node;
    node.id = "state" + std::to_string(index);
    node.type = "$adff";
    node.label = "DFF";
    node.kind = core::SchematicNodeKind::kRegister;
    node.clock_positive = index == 0;
    node.pins = {
        Pin("D", core::SchematicDirection::kInput,
            core::SchematicPinRole::kData, {"const:0"}),
        Pin("CLK", core::SchematicDirection::kInput,
            core::SchematicPinRole::kClock, {std::to_string(index + 2)}),
        Pin("Q", core::SchematicDirection::kOutput,
            core::SchematicPinRole::kOutput, {std::to_string(index + 10)}),
    };
    model.nodes.push_back(std::move(node));
  }
  auto built = gui::SchematicSceneBuilder().Build(
      {std::move(model), gui::SchematicViewMode::kReadable});
  Assert::IsTrue(built.Ok());
  Assert::AreEqual<std::size_t>(2U, built.Value().nodes.size());
  Assert::AreEqual<std::size_t>(0U, built.Value().summary.register_bank_count);
}

TEST_METHOD(DeterministicLargeSceneUsesBoundedFastRouter) {
  const auto make_model = [] {
    core::SchematicModel model;
    model.top_module = "generated_stress";
    model.ports.push_back({"a", core::SchematicDirection::kInput, {"1"}});
    model.ports.push_back({"y", core::SchematicDirection::kOutput, {"1801"}});
    model.nodes.reserve(1'800);
    for (int index = 0; index < 1'800; ++index) {
      const std::string input_bit = std::to_string(index + 1);
      const std::string output_bit = std::to_string(index + 2);
      core::SchematicNode node;
      node.id = "gate_" + std::to_string(index);
      node.type = "$_AND_";
      node.label = "AND";
      node.kind = core::SchematicNodeKind::kPrimitive;
      node.pins = {
          Pin("A", core::SchematicDirection::kInput,
              core::SchematicPinRole::kData, {input_bit}),
          Pin("B", core::SchematicDirection::kInput,
              core::SchematicPinRole::kData, {input_bit}),
          Pin("C", core::SchematicDirection::kInput,
              core::SchematicPinRole::kData, {input_bit}),
          Pin("Y", core::SchematicDirection::kOutput,
              core::SchematicPinRole::kOutput, {output_bit}),
      };
      model.nodes.push_back(std::move(node));
    }
    return model;
  };

  const auto started = std::chrono::steady_clock::now();
  auto first = gui::SchematicSceneBuilder().Build(
      {make_model(), gui::SchematicViewMode::kReadable});
  auto second = gui::SchematicSceneBuilder().Build(
      {make_model(), gui::SchematicViewMode::kReadable});
  const auto elapsed = std::chrono::steady_clock::now() - started;
  Assert::IsTrue(first.Ok());
  Assert::IsTrue(second.Ok());
  Assert::AreEqual<std::size_t>(1'800U, first.Value().nodes.size());
  Assert::IsTrue(
      std::any_of(first.Value().diagnostics.begin(),
                  first.Value().diagnostics.end(), [](const auto& diagnostic) {
                    return diagnostic.code == "SCHEMATIC-FAST-ROUTING";
                  }));
  Assert::AreEqual(first.Value().width, second.Value().width);
  Assert::AreEqual(first.Value().height, second.Value().height);
  Assert::AreEqual(first.Value().nodes.front().bounds.left,
                   second.Value().nodes.front().bounds.left);
  Assert::AreEqual(first.Value().routing.routes.size(),
                   second.Value().routing.routes.size());
  Assert::IsTrue(elapsed < std::chrono::seconds(15));
}

TEST_METHOD(BuildServiceDeliversTerminalEventExactlyOnce) {
  gui::SchematicBuildService service;
  std::mutex mutex;
  std::condition_variable completed;
  int completion_count = 0;
  gui::SchematicBuildEvent terminal;
  gui::SchematicBuildRequest request;
  request.model = TimerModel();
  request.mode = gui::SchematicViewMode::kReadable;
  const core::Status started = service.Start(
      17, std::move(request), [&](gui::SchematicBuildEvent event) {
        {
          std::scoped_lock lock(mutex);
          ++completion_count;
          terminal = std::move(event);
        }
        completed.notify_one();
      });
  Assert::IsTrue(started.Ok());
  std::unique_lock lock(mutex);
  Assert::IsTrue(completed.wait_for(lock, std::chrono::seconds(5),
                                    [&] { return completion_count == 1; }));
  Assert::AreEqual(17ULL, terminal.generation);
  Assert::IsTrue(terminal.status.Ok());
  Assert::IsNotNull(terminal.scene.get());
  lock.unlock();
  service.Cancel();
  service.Shutdown();
  std::scoped_lock final_lock(mutex);
  Assert::AreEqual(1, completion_count);
}
}
;

}  // namespace designpp::tests

// Copyright 2026 The Design++ Authors

#include "designpp/gui/schematic_scene_builder.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iterator>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <string_view>
#include <utility>

namespace designpp::gui {
namespace {

constexpr std::size_t kMaximumGateNodes = 2'000;
constexpr std::size_t kMaximumGateConnections = 8'000;
constexpr int kGrid = 8;
constexpr int kColumnSpacing = 192;
constexpr int kRowSpacing = 112;
constexpr int kNodeWidth = 84;
constexpr int kStartX = 180;
constexpr int kStartY = 72;

bool IsConstant(std::string_view bit) { return bit.starts_with("const:"); }

bool IsSequential(const core::SchematicNode& node) {
  return node.kind == core::SchematicNodeKind::kRegister ||
         node.kind == core::SchematicNodeKind::kRegisterBank;
}

std::string BitKey(const std::vector<std::string>& bits) {
  std::string key;
  for (const std::string& bit : bits) {
    if (!key.empty()) key.push_back(',');
    key.append(bit);
  }
  return key;
}

const core::SchematicPin* FindPin(const core::SchematicNode& node,
                                  core::SchematicPinRole role) {
  const auto found =
      std::find_if(node.pins.begin(), node.pins.end(),
                   [role](const auto& pin) { return pin.role == role; });
  return found == node.pins.end() ? nullptr : &*found;
}

std::string ControlSignature(const core::SchematicNode& node) {
  std::string result = node.type;
  for (const core::SchematicPin& pin : node.pins) {
    if (pin.role == core::SchematicPinRole::kClock ||
        pin.role == core::SchematicPinRole::kReset ||
        pin.role == core::SchematicPinRole::kSet ||
        pin.role == core::SchematicPinRole::kEnable) {
      result.append("|").append(pin.name).append("=").append(BitKey(pin.bits));
    }
  }
  result.append(node.clock_positive ? "|cp" : "|cn");
  result.append(node.reset_positive ? "|rp" : "|rn");
  result.append(node.enable_positive ? "|ep" : "|en");
  return result;
}

std::string SignalLabel(const core::SchematicSignal& signal) {
  if (signal.bits.size() <= 1U) return signal.name;
  const int first = signal.offset;
  const int last = signal.offset + static_cast<int>(signal.bits.size()) - 1;
  return signal.name + "[" + std::to_string(signal.upto ? first : last) + ":" +
         std::to_string(signal.upto ? last : first) + "]";
}

void ExpandCompositeInputPins(const core::SchematicModel& model,
                              std::vector<core::SchematicNode>* nodes) {
  std::set<std::string> vector_nets;
  std::vector<std::vector<std::string>> vector_sources;
  for (const core::SchematicSignal& signal : model.signals) {
    if (!signal.hidden && signal.bits.size() > 1U) {
      vector_nets.insert(BitKey(signal.bits));
    }
  }
  for (const core::SchematicPort& port : model.ports) {
    if (port.direction == core::SchematicDirection::kInput &&
        port.bits.size() > 1U) {
      vector_sources.push_back(port.bits);
    }
  }
  for (const core::SchematicNode& node : *nodes) {
    for (const core::SchematicPin& pin : node.pins) {
      if (pin.direction != core::SchematicDirection::kInput &&
          pin.bits.size() > 1U) {
        vector_nets.insert(BitKey(pin.bits));
        vector_sources.push_back(pin.bits);
      }
    }
  }
  std::map<std::string, std::vector<const std::vector<std::string>*>>
      sources_by_first_bit;
  for (const std::vector<std::string>& source : vector_sources) {
    for (const std::string& bit : source) {
      sources_by_first_bit[bit].push_back(&source);
    }
  }
  for (core::SchematicNode& node : *nodes) {
    std::vector<core::SchematicPin> expanded;
    for (const core::SchematicPin& pin : node.pins) {
      const bool all_constant =
          std::all_of(pin.bits.begin(), pin.bits.end(), IsConstant);
      bool is_vector_slice = false;
      if (!pin.bits.empty()) {
        const auto candidates = sources_by_first_bit.find(pin.bits.front());
        if (candidates != sources_by_first_bit.end()) {
          is_vector_slice =
              std::any_of(candidates->second.begin(), candidates->second.end(),
                          [&pin](const auto* source) {
                            return std::search(source->begin(), source->end(),
                                               pin.bits.begin(),
                                               pin.bits.end()) != source->end();
                          });
        }
      }
      if (pin.direction != core::SchematicDirection::kInput ||
          pin.bits.size() <= 1U || all_constant ||
          vector_nets.contains(BitKey(pin.bits)) || is_vector_slice) {
        expanded.push_back(pin);
        continue;
      }
      for (std::size_t index = 0; index < pin.bits.size(); ++index) {
        core::SchematicPin bit_pin = pin;
        bit_pin.name += "[" + std::to_string(index) + "]";
        bit_pin.bits = {pin.bits[index]};
        expanded.push_back(std::move(bit_pin));
      }
    }
    node.pins = std::move(expanded);
  }
}

std::vector<core::SchematicNode> NormalizeReadableNodes(
    const core::SchematicModel& model, core::SchematicSummary* summary) {
  std::vector<core::SchematicNode> result = model.nodes;
  std::set<std::string> consumed;
  std::vector<core::SchematicNode> banks;
  std::map<std::string, const core::SchematicNode*> sequential_by_output_bit;
  for (const core::SchematicNode& node : model.nodes) {
    if (!IsSequential(node)) continue;
    const core::SchematicPin* output =
        FindPin(node, core::SchematicPinRole::kOutput);
    if (output == nullptr || output->bits.size() != 1U) continue;
    sequential_by_output_bit.try_emplace(output->bits.front(), &node);
  }
  for (const core::SchematicSignal& signal : model.signals) {
    if (signal.hidden || signal.bits.size() <= 1U) continue;
    std::vector<const core::SchematicNode*> members;
    std::string signature;
    bool valid = true;
    for (const std::string& bit : signal.bits) {
      const auto member = sequential_by_output_bit.find(bit);
      if (member == sequential_by_output_bit.end()) {
        valid = false;
        break;
      }
      const core::SchematicPin* data =
          FindPin(*member->second, core::SchematicPinRole::kData);
      if (data == nullptr || data->bits.size() != 1U) {
        valid = false;
        break;
      }
      const std::string member_signature = ControlSignature(*member->second);
      if (signature.empty()) signature = member_signature;
      if (signature != member_signature) {
        valid = false;
        break;
      }
      members.push_back(member->second);
    }
    if (!valid || members.size() != signal.bits.size()) continue;

    core::SchematicNode bank = *members.front();
    bank.id = "bank:" + signal.name;
    bank.kind = core::SchematicNodeKind::kRegisterBank;
    bank.label = SignalLabel(signal);
    core::SchematicPin* bank_data = nullptr;
    core::SchematicPin* bank_output = nullptr;
    for (core::SchematicPin& pin : bank.pins) {
      if (pin.role == core::SchematicPinRole::kData) bank_data = &pin;
      if (pin.role == core::SchematicPinRole::kOutput) bank_output = &pin;
    }
    if (bank_data == nullptr || bank_output == nullptr) continue;
    bank_data->bits.clear();
    bank_output->bits = signal.bits;
    for (const core::SchematicNode* member : members) {
      bank_data->bits.push_back(
          FindPin(*member, core::SchematicPinRole::kData)->bits.front());
      consumed.insert(member->id);
    }
    banks.push_back(std::move(bank));
  }

  result.erase(std::remove_if(result.begin(), result.end(),
                              [&consumed](const core::SchematicNode& node) {
                                return consumed.contains(node.id);
                              }),
               result.end());
  result.insert(result.end(), std::make_move_iterator(banks.begin()),
                std::make_move_iterator(banks.end()));
  for (core::SchematicNode& node : result) {
    if (IsSequential(node)) {
      const core::SchematicPin* output =
          FindPin(node, core::SchematicPinRole::kOutput);
      if (output != nullptr) {
        const auto signal = std::find_if(
            model.signals.begin(), model.signals.end(),
            [&output](const core::SchematicSignal& candidate) {
              return !candidate.hidden && candidate.bits == output->bits;
            });
        if (signal != model.signals.end()) node.label = SignalLabel(*signal);
        if (output->bits.size() > 1U) {
          node.kind = core::SchematicNodeKind::kRegisterBank;
        }
      }
    }
    if (node.kind == core::SchematicNodeKind::kRegisterBank) {
      ++summary->register_bank_count;
    }
    if (node.kind == core::SchematicNodeKind::kGeneric) {
      ++summary->unsupported_cell_count;
    }
  }
  ExpandCompositeInputPins(model, &result);
  return result;
}

std::vector<int> CalculateDepths(const core::SchematicModel& model,
                                 const std::vector<core::SchematicNode>& nodes,
                                 core::SchematicSummary* summary) {
  std::map<std::string, int> bit_depth;
  for (const core::SchematicPort& port : model.ports) {
    if (port.direction != core::SchematicDirection::kInput) continue;
    for (const std::string& bit : port.bits) bit_depth[bit] = 0;
  }
  std::vector<int> depths(nodes.size(), 0);
  for (std::size_t index = 0; index < nodes.size(); ++index) {
    if (!IsSequential(nodes[index])) continue;
    depths[index] = 1;
    for (const core::SchematicPin& pin : nodes[index].pins) {
      if (pin.direction == core::SchematicDirection::kInput) continue;
      for (const std::string& bit : pin.bits) bit_depth[bit] = 1;
    }
  }

  for (std::size_t pass = 0; pass < nodes.size(); ++pass) {
    bool progressed = false;
    for (std::size_t index = 0; index < nodes.size(); ++index) {
      if (depths[index] != 0 || IsSequential(nodes[index])) continue;
      bool ready = true;
      int input_depth = 0;
      for (const core::SchematicPin& pin : nodes[index].pins) {
        if (pin.direction != core::SchematicDirection::kInput) continue;
        if (pin.role == core::SchematicPinRole::kClock ||
            pin.role == core::SchematicPinRole::kReset ||
            pin.role == core::SchematicPinRole::kSet ||
            pin.role == core::SchematicPinRole::kEnable) {
          continue;
        }
        for (const std::string& bit : pin.bits) {
          if (IsConstant(bit)) continue;
          const auto found = bit_depth.find(bit);
          if (found == bit_depth.end()) {
            ready = false;
            break;
          }
          input_depth = std::max(input_depth, found->second);
        }
        if (!ready) break;
      }
      if (!ready) continue;
      depths[index] = input_depth + 1;
      for (const core::SchematicPin& pin : nodes[index].pins) {
        if (pin.direction == core::SchematicDirection::kInput) continue;
        for (const std::string& bit : pin.bits) bit_depth[bit] = depths[index];
      }
      progressed = true;
    }
    if (!progressed) break;
  }

  std::map<std::string, std::size_t> drivers;
  for (std::size_t index = 0; index < nodes.size(); ++index) {
    if (IsSequential(nodes[index])) continue;
    for (const core::SchematicPin& pin : nodes[index].pins) {
      if (pin.direction == core::SchematicDirection::kInput) continue;
      for (const std::string& bit : pin.bits) drivers[bit] = index;
    }
  }
  std::vector<std::vector<std::size_t>> edges(nodes.size());
  for (std::size_t index = 0; index < nodes.size(); ++index) {
    if (depths[index] != 0) continue;
    for (const core::SchematicPin& pin : nodes[index].pins) {
      if (pin.direction != core::SchematicDirection::kInput) continue;
      for (const std::string& bit : pin.bits) {
        const auto driver = drivers.find(bit);
        if (driver != drivers.end() && depths[driver->second] == 0) {
          edges[driver->second].push_back(index);
        }
      }
    }
  }

  std::vector<int> discovery(nodes.size(), -1);
  std::vector<int> low(nodes.size(), 0);
  std::vector<bool> on_stack(nodes.size(), false);
  std::vector<std::size_t> stack;
  std::vector<std::vector<std::size_t>> components;
  int next_discovery = 0;
  std::function<void(std::size_t)> visit = [&](std::size_t node) {
    discovery[node] = low[node] = next_discovery++;
    stack.push_back(node);
    on_stack[node] = true;
    for (const std::size_t next : edges[node]) {
      if (discovery[next] < 0) {
        visit(next);
        low[node] = std::min(low[node], low[next]);
      } else if (on_stack[next]) {
        low[node] = std::min(low[node], discovery[next]);
      }
    }
    if (low[node] != discovery[node]) return;
    std::vector<std::size_t> component;
    while (!stack.empty()) {
      const std::size_t member = stack.back();
      stack.pop_back();
      on_stack[member] = false;
      component.push_back(member);
      if (member == node) break;
    }
    components.push_back(std::move(component));
  };
  for (std::size_t index = 0; index < nodes.size(); ++index) {
    if (depths[index] == 0 && discovery[index] < 0) visit(index);
  }

  int fallback_depth = 1;
  for (const int depth : depths)
    fallback_depth = std::max(fallback_depth, depth);
  for (const std::vector<std::size_t>& component : components) {
    bool loop = component.size() > 1U;
    if (!loop && !component.empty()) {
      const std::size_t member = component.front();
      loop = std::find(edges[member].begin(), edges[member].end(), member) !=
             edges[member].end();
    }
    if (loop) ++summary->combinational_loop_count;
    ++fallback_depth;
    for (const std::size_t member : component) depths[member] = fallback_depth;
  }
  return depths;
}

std::size_t ConnectionCount(const core::SchematicModel& model) {
  std::size_t count = 0;
  for (const core::SchematicNode& node : model.nodes) {
    for (const core::SchematicPin& pin : node.pins) count += pin.bits.size();
  }
  return count;
}

struct PendingNet {
  std::optional<OrthogonalPoint> driver;
  std::vector<OrthogonalPoint> sinks;
  std::vector<OrthogonalPoint> feedback_sinks;
  std::vector<std::string> bits;
  std::size_t width = 1;
};

void AddDriver(std::map<std::string, PendingNet>* nets,
               std::map<std::string, std::size_t>* net_widths,
               const std::vector<std::string>& bits, OrthogonalPoint point) {
  const std::string key = BitKey(bits);
  PendingNet& net = (*nets)[key];
  net.driver = point;
  net.bits = bits;
  net.width = bits.size();
  (*net_widths)[key] = bits.size();

  // Keep the vector connection for normal bus rendering, but also publish each
  // bit as an alias. Yosys frequently connects a narrower slice of an operator
  // output to a consumer, so an exact vector-key lookup alone loses valid nets.
  if (bits.size() <= 1U) return;
  for (const std::string& bit : bits) {
    PendingNet& bit_net = (*nets)[bit];
    bit_net.driver = point;
    bit_net.bits = {bit};
    bit_net.width = 1U;
    (*net_widths)[bit] = 1U;
  }
}

std::string ConstantLabel(const std::vector<std::string>& bits) {
  if (bits.size() == 1U) return bits.front().substr(6);
  if (bits.size() <= 64U &&
      std::all_of(bits.begin(), bits.end(), [](const std::string& bit) {
        return bit == "const:0" || bit == "const:1";
      })) {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < bits.size(); ++index) {
      if (bits[index] == "const:1") value |= std::uint64_t{1} << index;
    }
    constexpr char kHexDigits[] = "0123456789ABCDEF";
    std::string hexadecimal;
    do {
      hexadecimal.push_back(kHexDigits[value & 0xFU]);
      value >>= 4U;
    } while (value != 0U);
    std::reverse(hexadecimal.begin(), hexadecimal.end());
    return std::to_string(bits.size()) + "'h" + hexadecimal;
  }
  std::string value;
  value.reserve(bits.size());
  for (auto bit = bits.rbegin(); bit != bits.rend(); ++bit) {
    value.append(bit->substr(6));
  }
  if (std::all_of(value.begin(), value.end(),
                  [&value](char digit) { return digit == value.front(); })) {
    return std::to_string(bits.size()) + "'b" + value.substr(0, 1);
  }
  if (value.size() <= 10U) {
    return std::to_string(bits.size()) + "'b" + value;
  }
  return std::to_string(bits.size()) + "'b" + value.substr(0, 4) + "..." +
         value.substr(value.size() - 4U);
}

}  // namespace

core::Result<SchematicScene> SchematicSceneBuilder::Build(
    SchematicBuildRequest request, CancellationCheck cancelled) const {
  if (cancelled()) {
    return core::Status{core::ErrorCode::kCancelled,
                        "Schematic build cancelled", 0};
  }
  if (request.mode == SchematicViewMode::kGate &&
      (request.model.nodes.size() > kMaximumGateNodes ||
       ConnectionCount(request.model) > kMaximumGateConnections)) {
    return core::Status{core::ErrorCode::kFileTooLarge,
                        "Gate schematic exceeds 2000 nodes or 8000 connections",
                        0};
  }

  SchematicScene scene;
  scene.top_module = request.model.top_module;
  scene.mode = request.mode;
  scene.summary.raw_cell_count = request.model.nodes.size();
  std::vector<core::SchematicNode> nodes =
      request.mode == SchematicViewMode::kReadable
          ? NormalizeReadableNodes(request.model, &scene.summary)
          : request.model.nodes;
  if (request.mode == SchematicViewMode::kGate) {
    for (const core::SchematicNode& node : nodes) {
      if (node.kind == core::SchematicNodeKind::kGeneric) {
        ++scene.summary.unsupported_cell_count;
      }
    }
  }
  scene.summary.displayed_node_count = nodes.size();
  for (const core::SchematicSignal& signal : request.model.signals) {
    if (!signal.hidden && signal.bits.size() > 1U) ++scene.summary.bus_count;
  }

  const std::vector<int> depths =
      CalculateDepths(request.model, nodes, &scene.summary);
  int maximum_depth = 1;
  std::map<int, int> rows_per_depth;
  for (const int depth : depths) {
    maximum_depth = std::max(maximum_depth, depth);
    ++rows_per_depth[depth];
  }
  int maximum_rows = 1;
  for (const auto& [depth, count] : rows_per_depth) {
    static_cast<void>(depth);
    maximum_rows = std::max(maximum_rows, count);
  }
  std::map<int, int> next_row;
  scene.nodes.reserve(nodes.size());
  for (std::size_t index = 0; index < nodes.size(); ++index) {
    if (cancelled()) {
      return core::Status{core::ErrorCode::kCancelled,
                          "Schematic build cancelled", 0};
    }
    const core::SchematicNode& node = nodes[index];
    const int depth = depths[index];
    const int row = next_row[depth]++;
    const int input_count = static_cast<int>(
        std::count_if(node.pins.begin(), node.pins.end(), [](const auto& pin) {
          return pin.direction == core::SchematicDirection::kInput;
        }));
    const int output_count = static_cast<int>(node.pins.size()) - input_count;
    const int pin_count = std::max({2, input_count, output_count});
    const int node_height = IsSequential(node)
                                ? std::max(100, 48 + pin_count * 16)
                                : std::max(84, 34 + pin_count * 16);
    const int level_top =
        (maximum_rows - rows_per_depth[depth]) * kRowSpacing / 2;
    SchematicSceneNode layout;
    layout.id = node.id;
    layout.type = node.type;
    layout.label = node.label;
    if (request.mode == SchematicViewMode::kGate) {
      layout.instance_label = "U" + std::to_string(index + 1U);
    }
    layout.kind = node.kind;
    layout.bounds = {kStartX + (depth - 1) * kColumnSpacing,
                     kStartY + level_top + row * kRowSpacing,
                     kStartX + (depth - 1) * kColumnSpacing + kNodeWidth,
                     kStartY + level_top + row * kRowSpacing + node_height};
    int input_index = 0;
    int output_index = 0;
    for (const core::SchematicPin& pin : node.pins) {
      const bool input = pin.direction == core::SchematicDirection::kInput;
      const int ordinal = input ? input_index++ : output_index++;
      const int total = input ? input_count : output_count;
      const int pin_area_top = IsSequential(node) ? 30 : 16;
      const int y =
          layout.bounds.top + pin_area_top +
          ((ordinal + 1) * (node_height - pin_area_top - 18)) / (total + 1);
      bool active_low = false;
      if (pin.role == core::SchematicPinRole::kClock) {
        active_low = !node.clock_positive;
      } else if (pin.role == core::SchematicPinRole::kReset) {
        active_low = !node.reset_positive;
      } else if (pin.role == core::SchematicPinRole::kEnable) {
        active_low = !node.enable_positive;
      }
      layout.pins.push_back(
          {pin.name,
           pin.direction,
           pin.role,
           pin.bits,
           {input ? layout.bounds.left : layout.bounds.right, y},
           active_low});
    }
    scene.nodes.push_back(std::move(layout));
  }

  const int output_x = kStartX + maximum_depth * kColumnSpacing + 110;
  int input_row = 0;
  int output_row = 0;
  for (const core::SchematicPort& port : request.model.ports) {
    const bool input = port.direction == core::SchematicDirection::kInput;
    if (request.mode == SchematicViewMode::kGate && port.bits.size() > 1U) {
      for (std::size_t bit = 0; bit < port.bits.size(); ++bit) {
        scene.terminals.push_back(
            {port.name + "[" + std::to_string(bit) + "]",
             port.direction,
             {port.bits[bit]},
             {input ? 96 : output_x,
              kStartY + (input ? input_row++ : output_row++) * 40 + 30}});
      }
    } else {
      scene.terminals.push_back(
          {port.bits.size() > 1U
               ? port.name + "[" +
                     std::to_string(static_cast<int>(port.bits.size()) - 1) +
                     ":0]"
               : port.name,
           port.direction,
           port.bits,
           {input ? 96 : output_x,
            kStartY + (input ? input_row++ : output_row++) * 40 + 30}});
    }
  }

  std::map<std::string, PendingNet> nets;
  for (const SchematicSceneTerminal& terminal : scene.terminals) {
    const std::string key = BitKey(terminal.bits);
    PendingNet& net = nets[key];
    net.bits = terminal.bits;
    net.width = terminal.bits.size();
    scene.net_widths[key] = terminal.bits.size();
    if (terminal.direction == core::SchematicDirection::kInput) {
      AddDriver(&nets, &scene.net_widths, terminal.bits, terminal.point);
    } else {
      net.sinks.push_back(terminal.point);
    }
  }
  for (const SchematicSceneNode& node : scene.nodes) {
    for (const SchematicScenePin& pin : node.pins) {
      if (pin.bits.empty()) {
        continue;
      }
      if (std::all_of(pin.bits.begin(), pin.bits.end(), IsConstant)) {
        if (pin.direction == core::SchematicDirection::kInput) {
          const OrthogonalPoint terminal{pin.point.x - 72, pin.point.y};
          scene.terminals.push_back({ConstantLabel(pin.bits),
                                     core::SchematicDirection::kInput, pin.bits,
                                     terminal});
          OrthogonalRoute route;
          route.net_id =
              "constant:" + BitKey(pin.bits) + ":" + node.id + ":" + pin.name;
          route.points = {terminal, pin.point};
          scene.net_widths[route.net_id] = pin.bits.size();
          scene.routing.routes.push_back(std::move(route));
        }
        continue;
      }
      const std::string key = BitKey(pin.bits);
      PendingNet& net = nets[key];
      net.bits = pin.bits;
      net.width = pin.bits.size();
      scene.net_widths[key] = pin.bits.size();
      if (pin.direction == core::SchematicDirection::kInput) {
        if ((node.kind == core::SchematicNodeKind::kRegister ||
             node.kind == core::SchematicNodeKind::kRegisterBank) &&
            pin.role == core::SchematicPinRole::kData) {
          net.feedback_sinks.push_back(pin.point);
        } else {
          net.sinks.push_back(pin.point);
        }
      } else {
        AddDriver(&nets, &scene.net_widths, pin.bits, pin.point);
      }
    }
  }

  scene.width = output_x + 120;
  scene.height = std::max(420, kStartY + maximum_rows * kRowSpacing + 120);
  OrthogonalRoutingRequest routing_request;
  routing_request.width = scene.width;
  routing_request.height = scene.height;
  routing_request.grid = kGrid;
  routing_request.level_spacing = kColumnSpacing;
  routing_request.obstacle_clearance = kGrid;
  routing_request.cancelled = cancelled;
  for (const SchematicSceneNode& node : scene.nodes) {
    routing_request.obstacles.push_back(node.bounds);
  }
  std::size_t undriven_count = 0;
  for (auto& [id, net] : nets) {
    if (!net.driver && net.bits.size() > 1U) {
      std::optional<OrthogonalPoint> common_driver;
      bool compatible_slice = true;
      for (const std::string& bit : net.bits) {
        const auto bit_net = nets.find(bit);
        if (bit_net == nets.end() || !bit_net->second.driver) {
          compatible_slice = false;
          break;
        }
        if (!common_driver) {
          common_driver = bit_net->second.driver;
        } else if (*common_driver != *bit_net->second.driver) {
          compatible_slice = false;
          break;
        }
      }
      if (compatible_slice) net.driver = common_driver;
    }
    if (!net.driver) {
      std::vector<OrthogonalPoint> undriven_sinks = net.sinks;
      undriven_sinks.insert(undriven_sinks.end(), net.feedback_sinks.begin(),
                            net.feedback_sinks.end());
      for (const OrthogonalPoint sink : undriven_sinks) {
        const OrthogonalPoint terminal{sink.x - 24, sink.y};
        scene.terminals.push_back(
            {"NC", core::SchematicDirection::kInput, {id}, terminal});
        scene.routing.routes.push_back({"undriven:" + id, {terminal, sink}});
        ++undriven_count;
      }
      continue;
    }
    std::vector<OrthogonalPoint> true_feedback;
    for (const OrthogonalPoint sink : net.feedback_sinks) {
      if (sink.x <= net.driver->x) {
        true_feedback.push_back(sink);
      } else {
        net.sinks.push_back(sink);
      }
    }
    if (!net.sinks.empty()) {
      routing_request.nets.push_back(
          {id, *net.driver, net.sinks, std::nullopt});
    }
    if (!true_feedback.empty()) {
      routing_request.nets.push_back(
          {id, *net.driver, std::move(true_feedback), scene.height - 72});
    }
  }
  routing_request.fast_mode = scene.nodes.size() > 250U ||
                              routing_request.nets.size() > 400U ||
                              ConnectionCount(request.model) > 1'500U;
  OrthogonalRoutingResult routed =
      OrthogonalEdgeRouter().Route(routing_request);
  if (cancelled()) {
    return core::Status{core::ErrorCode::kCancelled,
                        "Schematic build cancelled", 0};
  }
  scene.routing.routes.insert(scene.routing.routes.end(),
                              std::make_move_iterator(routed.routes.begin()),
                              std::make_move_iterator(routed.routes.end()));
  scene.routing.junctions = std::move(routed.junctions);
  scene.routing.bridges = std::move(routed.bridges);
  scene.routing.unrouted_net_ids = std::move(routed.unrouted_net_ids);
  if (!scene.routing.unrouted_net_ids.empty()) {
    core::Diagnostic diagnostic;
    diagnostic.severity = core::DiagnosticSeverity::kWarning;
    diagnostic.code = "SCHEMATIC-UNROUTED";
    diagnostic.message =
        std::to_string(scene.routing.unrouted_net_ids.size()) +
        " net(s) could not be routed without crossing a symbol";
    scene.diagnostics.push_back(std::move(diagnostic));
  }
  if (routing_request.fast_mode) {
    core::Diagnostic diagnostic;
    diagnostic.severity = core::DiagnosticSeverity::kInfo;
    diagnostic.code = "SCHEMATIC-FAST-ROUTING";
    diagnostic.message =
        "Large schematic used bounded obstacle-safe routing for faster layout";
    scene.diagnostics.push_back(std::move(diagnostic));
  }
  if (undriven_count > 0U) {
    core::Diagnostic diagnostic;
    diagnostic.severity = core::DiagnosticSeverity::kWarning;
    diagnostic.code = "SCHEMATIC-UNDRIVEN";
    diagnostic.message =
        std::to_string(undriven_count) + " input pin(s) have no net driver";
    scene.diagnostics.push_back(std::move(diagnostic));
  }
  if (scene.summary.combinational_loop_count > 0) {
    core::Diagnostic diagnostic;
    diagnostic.severity = core::DiagnosticSeverity::kWarning;
    diagnostic.code = "SCHEMATIC-COMB-LOOP";
    diagnostic.message =
        "Combinational loop condensed into stable layout groups";
    scene.diagnostics.push_back(std::move(diagnostic));
  }
  return scene;
}

}  // namespace designpp::gui

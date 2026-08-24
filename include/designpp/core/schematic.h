// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_CORE_SCHEMATIC_H_
#define DESIGNPP_CORE_SCHEMATIC_H_

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace designpp::core {

enum class SchematicDirection {
  kInput,
  kOutput,
  kInout,
};

enum class SchematicNodeKind {
  kPrimitive,
  kRegister,
  kRegisterBank,
  kArithmetic,
  kComparator,
  kMux,
  kMemory,
  kModule,
  kGeneric,
};

enum class SchematicPinRole {
  kData,
  kOutput,
  kClock,
  kReset,
  kSet,
  kEnable,
  kSelect,
};

struct SchematicSignal {
  std::string name;
  std::vector<std::string> bits;
  int offset = 0;
  bool upto = false;
  bool hidden = false;
};

struct SchematicPort {
  std::string name;
  SchematicDirection direction = SchematicDirection::kInput;
  std::vector<std::string> bits;
};

struct SchematicPin {
  std::string name;
  SchematicDirection direction = SchematicDirection::kInput;
  SchematicPinRole role = SchematicPinRole::kData;
  std::vector<std::string> bits;
};

struct SchematicNode {
  std::string id;
  std::string type;
  std::string label;
  SchematicNodeKind kind = SchematicNodeKind::kGeneric;
  std::vector<SchematicPin> pins;
  std::map<std::string, std::string> parameters;
  std::map<std::string, std::string> attributes;
  bool clock_positive = true;
  bool reset_positive = true;
  bool enable_positive = true;
  bool reset_value = false;
};

struct SchematicModel {
  std::string top_module;
  std::vector<SchematicPort> ports;
  std::vector<SchematicSignal> signals;
  std::vector<SchematicNode> nodes;
};

struct SchematicSummary {
  std::size_t raw_cell_count = 0;
  std::size_t displayed_node_count = 0;
  std::size_t register_bank_count = 0;
  std::size_t bus_count = 0;
  std::size_t unsupported_cell_count = 0;
  std::size_t combinational_loop_count = 0;
};

}  // namespace designpp::core

#endif  // DESIGNPP_CORE_SCHEMATIC_H_

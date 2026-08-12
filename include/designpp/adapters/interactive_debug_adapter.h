// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_ADAPTERS_INTERACTIVE_DEBUG_ADAPTER_H_
#define DESIGNPP_ADAPTERS_INTERACTIVE_DEBUG_ADAPTER_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "designpp/adapters/simulation_adapter.h"

namespace designpp::adapters {

enum class DebugProtocolEventKind {
  kPromptReady,
  kContinued,
  kStopped,
  kTimeChanged,
  kScopeChanged,
  kScopeItems,
  kEvaluatedValue,
  kSourceStopLocation,
  kProtocolError,
};

struct DebugScopeItem {
  std::string kind;
  std::string name;
  std::string value;
};

struct DebugProtocolEvent {
  DebugProtocolEventKind kind = DebugProtocolEventKind::kProtocolError;
  std::string text;
  std::string simulation_time;
  std::string scope;
  std::vector<DebugScopeItem> scope_items;
  std::string file;
  std::uint32_t line = 0;
};

struct DebugPlan {
  SimulationPlan simulation;
};

class InteractiveDebugAdapter {
 public:
  virtual ~InteractiveDebugAdapter() = default;
  [[nodiscard]] virtual core::Result<DebugPlan> BuildDebugPlan(
      const SimulationRequest& request,
      const runtime::PathMapper& path_mapper) const = 0;
  [[nodiscard]] virtual core::Status ValidateConsoleCommand(
      std::string_view command) const = 0;
};

}  // namespace designpp::adapters

#endif  // DESIGNPP_ADAPTERS_INTERACTIVE_DEBUG_ADAPTER_H_

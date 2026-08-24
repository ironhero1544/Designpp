// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_ADAPTERS_ICARUS_DEBUG_ADAPTER_H_
#define DESIGNPP_ADAPTERS_ICARUS_DEBUG_ADAPTER_H_

#include <string>
#include <string_view>
#include <vector>

#include "designpp/adapters/interactive_debug_adapter.h"

namespace designpp::adapters {

class IcarusDebugProtocolParser final {
 public:
  void SetPendingCommand(std::string command);
  [[nodiscard]] std::vector<DebugProtocolEvent> Feed(std::string_view bytes);
  [[nodiscard]] std::vector<DebugProtocolEvent> Finish();

 private:
  [[nodiscard]] std::vector<DebugProtocolEvent> ParseResponse(
      std::string response);

  std::string buffer_;
  std::string pending_command_;
};

class IcarusDebugAdapter final : public InteractiveDebugAdapter {
 public:
  [[nodiscard]] core::Result<DebugPlan> BuildDebugPlan(
      const SimulationRequest& request,
      const runtime::PathMapper& path_mapper) const override;
  [[nodiscard]] core::Status ValidateConsoleCommand(
      std::string_view command) const override;
};

}  // namespace designpp::adapters

#endif  // DESIGNPP_ADAPTERS_ICARUS_DEBUG_ADAPTER_H_

// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_ADAPTERS_VERILATOR_ADAPTER_H_
#define DESIGNPP_ADAPTERS_VERILATOR_ADAPTER_H_

#include "designpp/adapters/tool_adapter.h"

namespace designpp::adapters {

class VerilatorAdapter final : public ToolAdapter {
 public:
  [[nodiscard]] runtime::WslCommand BuildProbeCommand() const override;
  [[nodiscard]] core::Status Validate(
      const core::Project& project,
      const std::vector<application::ResolvedSource>& sources) const override;
  [[nodiscard]] core::Result<runtime::WslCommand> BuildCommand(
      const core::Project& project,
      const std::vector<application::ResolvedSource>& sources,
      const runtime::PathMapper& path_mapper,
      const ToolCapabilities& capabilities) const override;
  [[nodiscard]] std::vector<core::Diagnostic> ParseDiagnostics(
      std::string_view raw_output) const override;
};

}  // namespace designpp::adapters

#endif  // DESIGNPP_ADAPTERS_VERILATOR_ADAPTER_H_

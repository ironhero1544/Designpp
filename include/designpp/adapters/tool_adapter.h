// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_ADAPTERS_TOOL_ADAPTER_H_
#define DESIGNPP_ADAPTERS_TOOL_ADAPTER_H_

#include <string>
#include <string_view>
#include <vector>

#include "designpp/application/project_service.h"
#include "designpp/core/diagnostic.h"
#include "designpp/core/project.h"
#include "designpp/core/status.h"
#include "designpp/runtime/path_mapper.h"
#include "designpp/runtime/wsl_executor.h"

namespace designpp::adapters {

struct ToolCapabilities {
  std::string name;
  std::string version;
  bool supports_parallel_jobs = false;
};

class ToolAdapter {
 public:
  virtual ~ToolAdapter() = default;
  [[nodiscard]] virtual runtime::WslCommand BuildProbeCommand() const = 0;
  [[nodiscard]] virtual core::Status Validate(
      const core::Project& project,
      const std::vector<application::ResolvedSource>& sources) const = 0;
  [[nodiscard]] virtual core::Result<runtime::WslCommand> BuildCommand(
      const core::Project& project,
      const std::vector<application::ResolvedSource>& sources,
      const runtime::PathMapper& path_mapper,
      const ToolCapabilities& capabilities) const = 0;
  [[nodiscard]] virtual std::vector<core::Diagnostic> ParseDiagnostics(
      std::string_view raw_output) const = 0;
};

}  // namespace designpp::adapters

#endif  // DESIGNPP_ADAPTERS_TOOL_ADAPTER_H_

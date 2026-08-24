// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_ADAPTERS_SIMULATION_ADAPTER_H_
#define DESIGNPP_ADAPTERS_SIMULATION_ADAPTER_H_

#include <filesystem>
#include <string>
#include <vector>

#include "designpp/application/project_service.h"
#include "designpp/core/diagnostic.h"
#include "designpp/core/project.h"
#include "designpp/core/status.h"
#include "designpp/runtime/path_mapper.h"
#include "designpp/runtime/wsl_executor.h"

namespace designpp::adapters {

struct SimulationRequest {
  core::Project project;
  std::vector<application::ResolvedSource> sources;
  std::string testbench_view_id;
  std::string testbench_relative_path;
  std::string testbench_top;
  bool waveform_enabled = true;
  std::string waveform_format = "vcd";
  std::filesystem::path artifact_directory;
  // Optional ASCII-safe directory for tools whose generated Makefiles cannot
  // build from a project path containing spaces or non-ASCII characters.
  std::filesystem::path build_directory;
};

struct SimulationPlan {
  runtime::WslCommand compile;
  runtime::WslCommand execute;
  std::filesystem::path wrapper_path;
  std::string wrapper_text;
  std::filesystem::path binary_path;
  std::filesystem::path waveform_path;
  std::string input_hash;
};

class SimulationAdapter {
 public:
  virtual ~SimulationAdapter() = default;
  [[nodiscard]] virtual runtime::WslCommand BuildProbeCommand() const = 0;
  [[nodiscard]] virtual core::Status Validate(
      const SimulationRequest& request) const = 0;
  [[nodiscard]] virtual core::Result<SimulationPlan> BuildPlan(
      const SimulationRequest& request,
      const runtime::PathMapper& path_mapper) const = 0;
  [[nodiscard]] virtual std::vector<core::Diagnostic> ParseDiagnostics(
      std::string_view raw_output) const = 0;
};

}  // namespace designpp::adapters

#endif  // DESIGNPP_ADAPTERS_SIMULATION_ADAPTER_H_

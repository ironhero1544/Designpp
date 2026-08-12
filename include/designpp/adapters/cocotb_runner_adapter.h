// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_ADAPTERS_COCOTB_RUNNER_ADAPTER_H_
#define DESIGNPP_ADAPTERS_COCOTB_RUNNER_ADAPTER_H_

#include <filesystem>
#include <string>

#include "designpp/adapters/simulation_adapter.h"

namespace designpp::adapters {

struct CocotbRequest {
  SimulationRequest simulation;
  std::string module;
  std::string testcase_filter;
  std::wstring makefiles_directory;
  std::string backend = "icarus";
};

struct CocotbPlan {
  runtime::WslCommand execute;
  std::filesystem::path results_path;
  std::filesystem::path waveform_path;
};

class CocotbRunnerAdapter final {
 public:
  [[nodiscard]] runtime::WslCommand BuildProbeCommand() const;
  [[nodiscard]] runtime::WslCommand BuildMakefilesProbeCommand() const;
  [[nodiscard]] core::Status Validate(const CocotbRequest& request) const;
  [[nodiscard]] core::Result<CocotbPlan> BuildPlan(
      const CocotbRequest& request,
      const runtime::PathMapper& path_mapper) const;
};

}  // namespace designpp::adapters

#endif  // DESIGNPP_ADAPTERS_COCOTB_RUNNER_ADAPTER_H_

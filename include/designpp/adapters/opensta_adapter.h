// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_ADAPTERS_OPENSTA_ADAPTER_H_
#define DESIGNPP_ADAPTERS_OPENSTA_ADAPTER_H_

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "designpp/core/diagnostic.h"
#include "designpp/core/status.h"
#include "designpp/runtime/path_mapper.h"
#include "designpp/runtime/wsl_executor.h"

namespace designpp::adapters {

struct TimingRequest {
  std::string top_module;
  std::string corner_name;
  std::filesystem::path netlist_path;
  std::vector<std::filesystem::path> liberty_files;
  std::filesystem::path sdc_path;
  std::filesystem::path artifact_directory;
};

struct TimingPlan {
  runtime::WslCommand execute;
  std::filesystem::path script_path;
  std::string script_text;
  std::filesystem::path report_path;
};

struct TimingViolation {
  std::string corner;
  std::string startpoint;
  std::string endpoint;
  std::string check_type;
  double slack = 0.0;
};

struct TimingMetrics {
  double wns = 0.0;
  double tns = 0.0;
  std::vector<TimingViolation> violations;

  [[nodiscard]] bool Passed() const noexcept {
    return wns >= 0.0 && tns >= 0.0 && violations.empty();
  }
};

class OpenStaAdapter final {
 public:
  [[nodiscard]] runtime::WslCommand BuildProbeCommand() const;
  [[nodiscard]] core::Status Validate(const TimingRequest& request) const;
  [[nodiscard]] core::Result<TimingPlan> BuildPlan(
      const TimingRequest& request,
      const runtime::PathMapper& path_mapper) const;
  [[nodiscard]] core::Result<TimingMetrics> ParseReport(
      std::string_view raw_output, std::string_view corner_name) const;
  [[nodiscard]] std::vector<core::Diagnostic> ParseDiagnostics(
      std::string_view raw_output) const;
};

}  // namespace designpp::adapters

#endif  // DESIGNPP_ADAPTERS_OPENSTA_ADAPTER_H_

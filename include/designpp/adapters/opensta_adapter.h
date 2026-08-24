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
  std::uint32_t cpu_threads = 1;
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

struct TimingCheckMetrics {
  double wns = 0.0;
  double tns = 0.0;
  bool has_paths = false;
  std::size_t violation_count = 0;
};

struct TimingViolationCounts {
  std::size_t setup = 0;
  std::size_t hold = 0;
  std::size_t recovery = 0;
  std::size_t removal = 0;
  std::size_t unknown = 0;
};

struct TimingMetrics {
  TimingCheckMetrics setup;
  TimingCheckMetrics hold;
  TimingViolationCounts violation_counts;
  std::vector<TimingViolation> violations;
  bool violations_truncated = false;

  [[nodiscard]] bool Passed() const noexcept {
    return setup.has_paths && hold.has_paths && setup.wns >= 0.0 &&
           setup.tns >= 0.0 && hold.wns >= 0.0 && hold.tns >= 0.0 &&
           violations.empty();
  }
};

class OpenStaAdapter final {
 public:
  [[nodiscard]] runtime::WslCommand BuildProbeCommand() const;
  [[nodiscard]] core::Status Validate(const TimingRequest& request) const;
  // Checks SDC constructs that are known to be unsupported by the managed
  // OpenSTA command set before starting the external process.
  [[nodiscard]] core::Status ValidateSdcText(std::string_view text) const;
  // Reconciles path metrics with recovery/removal violations. The operation
  // is idempotent so persisted or in-memory results can be normalized again
  // at presentation boundaries.
  [[nodiscard]] TimingMetrics NormalizeMetrics(TimingMetrics metrics) const;
  [[nodiscard]] core::Result<TimingPlan> BuildPlan(
      const TimingRequest& request,
      const runtime::PathMapper& path_mapper) const;
  [[nodiscard]] core::Result<TimingMetrics> ParseReport(
      std::string_view raw_output, std::string_view report,
      std::string_view corner_name) const;
  [[nodiscard]] std::vector<core::Diagnostic> ParseDiagnostics(
      std::string_view raw_output) const;
};

}  // namespace designpp::adapters

#endif  // DESIGNPP_ADAPTERS_OPENSTA_ADAPTER_H_

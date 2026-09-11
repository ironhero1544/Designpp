// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_APPLICATION_PREPARED_PHYSICAL_INPUTS_H_
#define DESIGNPP_APPLICATION_PREPARED_PHYSICAL_INPUTS_H_

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "designpp/core/status.h"

namespace designpp::application {

struct ManagedFlowRunRequest;

struct PreparedPhysicalFile {
  std::string relative_path;
  std::filesystem::path staged_path;
  std::string contents;
  std::string sha256;
};

struct PreparedPhysicalInclude {
  std::string configured_path;
  std::vector<PreparedPhysicalFile> files;
};

struct PreparedPhysicalClock {
  std::string name;
  std::string target_port;
  double period_ns = 0.0;
};

// Immutable bytes and derived metadata used by compatibility checks, staging,
// fingerprinting, and adapter configuration for one ORFS attempt.
struct PreparedPhysicalInputs {
  std::string tool_version;
  std::vector<PreparedPhysicalFile> rtl_sources;
  std::vector<PreparedPhysicalInclude> include_directories;
  std::string sdc_contents;
  std::string sdc_provenance;
  std::string sdc_sha256;
  std::string sdc_time_unit = "ns";
  std::vector<PreparedPhysicalClock> clocks;
  std::optional<double> effective_clock_period_ns;
  std::string effective_clock_period_text;
  bool sdc_auto_generated = false;
  bool unconstrained_warning = false;
  std::string configuration_contract_hex;
  std::string fingerprint_manifest;
  std::string fingerprint;
};

[[nodiscard]] core::Result<std::shared_ptr<const PreparedPhysicalInputs>>
PrepareOrfsPhysicalInputs(const ManagedFlowRunRequest& request,
                          std::string_view tool_version);

}  // namespace designpp::application

#endif  // DESIGNPP_APPLICATION_PREPARED_PHYSICAL_INPUTS_H_

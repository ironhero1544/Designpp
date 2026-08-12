// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_CORE_PROJECT_H_
#define DESIGNPP_CORE_PROJECT_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "designpp/core/status.h"

namespace designpp::core {

enum class SourcePolicy { kAutoManaged };

struct SourceOverride {
  std::string view_id;
  std::string relative_path;
  bool enabled = true;
  std::string role;
};

struct TestbenchConfiguration {
  std::string view_id;
  std::string relative_path;
  std::string top_module;
  std::string backend = "icarus";
  bool waveform_enabled = true;
  std::string runner = "hdl";
  std::string cocotb_module;
  std::string cocotb_testcase;
  std::string waveform_format = "vcd";
};

struct SynthesisConfiguration {
  std::vector<std::string> liberty_paths;
  bool flatten = false;
};

// Stored now so a subsequent OpenSTA vertical slice can consume a stable
// project contract and a compatible synthesis netlist.
struct TimingConfiguration {
  std::string corner_name = "typical";
  std::vector<std::string> liberty_paths;
  std::string sdc_path;
};

struct Project {
  static constexpr std::uint32_t kSchemaVersion = 4;

  std::uint32_t schema_version = kSchemaVersion;
  std::string id;
  std::uint64_t revision = 1;
  std::string library_id;
  std::string cell_id;
  std::string name;
  std::string top_module;
  std::string created_utc;
  std::string modified_utc;
  SourcePolicy source_policy = SourcePolicy::kAutoManaged;
  std::vector<SourceOverride> source_overrides;
  std::vector<TestbenchConfiguration> testbench_configurations;
  SynthesisConfiguration synthesis;
  TimingConfiguration timing;
  std::vector<std::string> include_directories;
  std::vector<std::string> defines;
  std::vector<std::string> parameters;
  std::optional<std::string> constraint_path;
  std::optional<std::string> toolchain_profile_id;
  std::uint32_t cpu_budget = 1;
};

[[nodiscard]] Status ValidateProject(const Project& project);
[[nodiscard]] bool IsSafeRelativePath(std::string_view path);

}  // namespace designpp::core

#endif  // DESIGNPP_CORE_PROJECT_H_

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

struct PowerDistributionConfiguration {
  bool multilayer = true;
  bool core_ring = false;
  bool enable_rails = true;
  std::optional<std::string> vertical_width_um;
  std::optional<std::string> horizontal_width_um;
  std::optional<std::string> vertical_spacing_um;
  std::optional<std::string> horizontal_spacing_um;
  std::optional<std::string> vertical_pitch_um;
  std::optional<std::string> horizontal_pitch_um;
  std::optional<std::string> vertical_offset_um;
  std::optional<std::string> horizontal_offset_um;
};

struct IoPinSideConfiguration {
  std::optional<std::string> minimum_distance_um;
  bool bit_major = false;
  std::vector<std::string> entries;
};

struct IoPlacementConfiguration {
  std::string algorithm = "matching";
  std::optional<std::string> minimum_distance_um;
  std::optional<std::string> vertical_length_um;
  std::optional<std::string> horizontal_length_um;
  std::optional<std::string> vertical_thickness_multiplier;
  std::optional<std::string> horizontal_thickness_multiplier;
  std::optional<std::string> vertical_extension_um;
  std::optional<std::string> horizontal_extension_um;
  std::optional<std::string> vertical_layer;
  std::optional<std::string> horizontal_layer;
  std::string unmatched_policy = "both";
  IoPinSideConfiguration north;
  IoPinSideConfiguration south;
  IoPinSideConfiguration east;
  IoPinSideConfiguration west;
};

// Configuration owned by the OpenROAD Flow Scripts backend.  It is kept
// alongside the common physical implementation settings so switching
// backends does not discard the other backend's configuration.
struct OrfsConfiguration {
  std::string platform = "sky130hd";
  std::string flow_variant = "base";
  std::string advanced_variables_json = "{}";
};

struct PhysicalImplementationConfiguration {
  // Schema 11: fields whose value is inherited, rather than user-specified.
  // Optional numeric fields continue to represent inheritance with nullopt.
  std::vector<std::string> automatic_fields;
  std::string backend_id = "openlane2";
  std::string pdk = "sky130A";
  std::string standard_cell_library = "sky130_fd_sc_hd";
  std::vector<std::string> clock_ports;
  std::string clock_period_ns = "10.0";
  std::uint32_t core_utilization_percent = 40;
  std::optional<std::string> placement_density_percent;
  std::vector<std::string> die_area;
  std::vector<std::string> core_area;
  std::optional<std::string> tap_cell_distance_um;
  std::string pnr_sdc_path;
  std::string signoff_sdc_path;
  PowerDistributionConfiguration power_distribution;
  IoPlacementConfiguration io_placement;
  std::string advanced_overrides_json = "{}";
  OrfsConfiguration orfs;
};

// Selects physical-verification recipes for one Cell. Recipe contents are
// owned by the toolchain settings so multiple Cells can share an immutable
// rule revision without sharing mutable editor state.
struct PhysicalVerificationConfiguration {
  std::string drc_recipe_id;
  std::string lvs_recipe_id;
  std::string top_cell;
  std::string power_net;
  std::string ground_net;
  std::string parameters_json = "{}";
};

struct Project {
  static constexpr std::uint32_t kSchemaVersion = 12;

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
  PhysicalImplementationConfiguration physical_implementation;
  PhysicalVerificationConfiguration physical_verification;
  std::vector<std::string> include_directories;
  std::vector<std::string> defines;
  std::vector<std::string> parameters;
  std::optional<std::string> constraint_path;
  std::optional<std::string> toolchain_profile_id;
  std::uint32_t cpu_budget = 1;
};

[[nodiscard]] Status ValidateProject(const Project& project);
[[nodiscard]] bool UsesAutomaticValue(
    const PhysicalImplementationConfiguration& configuration,
    std::string_view field);
[[nodiscard]] PhysicalImplementationConfiguration ResolvePhysicalDefaults(
    PhysicalImplementationConfiguration configuration);
[[nodiscard]] Status ValidatePhysicalImplementationConfiguration(
    const PhysicalImplementationConfiguration& configuration);
[[nodiscard]] bool IsSafeRelativePath(std::string_view path);

}  // namespace designpp::core

#endif  // DESIGNPP_CORE_PROJECT_H_

// Copyright 2026 The Design++ Authors

#include "designpp/core/project.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <set>
#include <thread>

namespace designpp::core {
namespace {

bool IsUuid(std::string_view value) {
  if (value.size() != 36) return false;
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) {
      if (value[index] != '-') return false;
    } else if (!std::isxdigit(static_cast<unsigned char>(value[index]))) {
      return false;
    }
  }
  return true;
}

bool IsHdlIdentifier(std::string_view value) {
  if (value.empty() ||
      !(std::isalpha(static_cast<unsigned char>(value.front())) ||
        value.front() == '_' || value.front() == '$')) {
    return false;
  }
  return std::all_of(value.begin() + 1, value.end(), [](char character) {
    return std::isalnum(static_cast<unsigned char>(character)) ||
           character == '_' || character == '$';
  });
}

bool IsDecimal(std::string_view value, double minimum, double maximum) {
  if (value.empty() || value.size() > 32) return false;
  std::string text(value);
  char* end = nullptr;
  const double number = std::strtod(text.c_str(), &end);
  return end == text.c_str() + text.size() && std::isfinite(number) &&
         number >= minimum && number <= maximum;
}

bool IsSafeOpenLaneOverrides(std::string_view json) {
  if (json.size() > 64 * 1024) return false;
  const std::size_t first = json.find_first_not_of(" \t\r\n");
  const std::size_t last = json.find_last_not_of(" \t\r\n");
  if (first == std::string_view::npos || json[first] != '{' ||
      json[last] != '}') {
    return false;
  }
  constexpr std::string_view kForbidden[] = {"\"DESIGN_NAME\"",
                                             "\"VERILOG_FILES\"",
                                             "\"VERILOG_INCLUDE_DIRS\"",
                                             "\"VERILOG_DEFINES\"",
                                             "\"SYNTH_PARAMETERS\"",
                                             "\"PDK\"",
                                             "\"STD_CELL_LIBRARY\"",
                                             "\"CLOCK_PORT\"",
                                             "\"CLOCK_PERIOD\"",
                                             "\"PNR_SDC_FILE\"",
                                             "\"SIGNOFF_SDC_FILE\"",
                                             "\"FALLBACK_SDC_FILE\"",
                                             "\"FP_CORE_UTIL\"",
                                             "\"PL_TARGET_DENSITY_PCT\"",
                                             "\"DIE_AREA\"",
                                             "\"CORE_AREA\"",
                                             "\"FP_TAPCELL_DIST\"",
                                             "\"FP_PDN_MULTILAYER\"",
                                             "\"FP_PDN_CORE_RING\"",
                                             "\"FP_PDN_ENABLE_RAILS\"",
                                             "\"FP_PDN_VWIDTH\"",
                                             "\"FP_PDN_HWIDTH\"",
                                             "\"FP_PDN_VSPACING\"",
                                             "\"FP_PDN_HSPACING\"",
                                             "\"FP_PDN_VPITCH\"",
                                             "\"FP_PDN_HPITCH\"",
                                             "\"FP_PDN_VOFFSET\"",
                                             "\"FP_PDN_HOFFSET\"",
                                             "\"FP_PPL_MODE\"",
                                             "\"FP_IO_MIN_DISTANCE\"",
                                             "\"FP_IO_VLENGTH\"",
                                             "\"FP_IO_HLENGTH\"",
                                             "\"FP_IO_VTHICKNESS_MULT\"",
                                             "\"FP_IO_HTHICKNESS_MULT\"",
                                             "\"FP_IO_VEXTEND\"",
                                             "\"FP_IO_HEXTEND\"",
                                             "\"FP_IO_VLAYER\"",
                                             "\"FP_IO_HLAYER\"",
                                             "\"FP_PIN_ORDER_CFG\"",
                                             "\"ERRORS_ON_UNMATCHED_IO\"",
                                             "\"RUN_"};
  for (std::string_view token : kForbidden) {
    if (json.find(token) != std::string_view::npos) return false;
  }
  return json.find("dir::") == std::string_view::npos &&
         json.find("/home/") == std::string_view::npos &&
         json.find("/mnt/") == std::string_view::npos &&
         json.find(":\\") == std::string_view::npos &&
         json.find("../") == std::string_view::npos;
}

bool IsSafeOrfsOverrides(std::string_view json) {
  if (json.size() > 64 * 1024) return false;
  const std::size_t first = json.find_first_not_of(" \t\r\n");
  const std::size_t last = json.find_last_not_of(" \t\r\n");
  if (first == std::string_view::npos || json[first] != '{' ||
      json[last] != '}') {
    return false;
  }
  // ORFS values are deliberately restricted to flat, scalar Make values.
  // Paths, assignments, and shell expansion are generated by the adapter and
  // must never be supplied through this JSON surface.
  constexpr std::string_view kForbidden[] = {"\"DESIGN_CONFIG\"",
                                             "\"DESIGN_NAME\"",
                                             "\"DESIGN_NICKNAME\"",
                                             "\"VERILOG_FILES\"",
                                             "\"VERILOG_INCLUDE_DIRS\"",
                                             "\"VERILOG_DEFINES\"",
                                             "\"SYNTH_PARAMETERS\"",
                                             "\"SDC_FILE\"",
                                             "\"PLATFORM\"",
                                             "\"FLOW_VARIANT\"",
                                             "\"WORK_HOME\"",
                                             "\"RESULTS_DIR\"",
                                             "\"LOG_DIR\"",
                                             "\"REPORTS_DIR\"",
                                             "\"OBJECTS_DIR\"",
                                             "\"IO_CONSTRAINTS\"",
                                             "\"PDN_TCL\"",
                                             "\"TAPCELL_TCL\"",
                                             "\"PLACE_PINS_ARGS\"",
                                             "\"RUN_"};
  for (std::string_view token : kForbidden) {
    if (json.find(token) != std::string_view::npos) return false;
  }
  return json.find("$HOME") == std::string_view::npos &&
         json.find("$(") == std::string_view::npos &&
         json.find("${") == std::string_view::npos &&
         json.find("../") == std::string_view::npos &&
         json.find("/mnt/") == std::string_view::npos &&
         json.find("/home/") == std::string_view::npos &&
         json.find(":\\") == std::string_view::npos;
}

}  // namespace

bool IsSafeRelativePath(std::string_view path) {
  if (path.empty() || path.find('\0') != std::string_view::npos) return false;
  const std::filesystem::path candidate(path);
  if (candidate.is_absolute() || candidate.has_root_name() ||
      candidate.has_root_directory()) {
    return false;
  }
  for (const auto& component : candidate) {
    if (component == "..") return false;
  }
  return true;
}

bool UsesAutomaticValue(
    const PhysicalImplementationConfiguration& configuration,
    std::string_view field) {
  return std::find(configuration.automatic_fields.begin(),
                   configuration.automatic_fields.end(),
                   field) != configuration.automatic_fields.end();
}

PhysicalImplementationConfiguration ResolvePhysicalDefaults(
    PhysicalImplementationConfiguration configuration) {
  const PhysicalImplementationConfiguration defaults;
  const auto automatic = [&](std::string_view field) {
    return UsesAutomaticValue(configuration, field);
  };
  if (automatic("pdk")) configuration.pdk = defaults.pdk;
  if (automatic("standard_cell_library"))
    configuration.standard_cell_library = defaults.standard_cell_library;
  if (automatic("clock_period_ns"))
    configuration.clock_period_ns =
        configuration.clock_ports.empty() ? "" : defaults.clock_period_ns;
  if (automatic("core_utilization_percent"))
    configuration.core_utilization_percent = defaults.core_utilization_percent;
  if (automatic("orfs.flow_variant"))
    configuration.orfs.flow_variant = defaults.orfs.flow_variant;
  if (automatic("io.algorithm"))
    configuration.io_placement.algorithm = defaults.io_placement.algorithm;
  if (automatic("io.unmatched_policy"))
    configuration.io_placement.unmatched_policy =
        defaults.io_placement.unmatched_policy;
  return configuration;
}

Status ValidatePhysicalImplementationConfiguration(
    const PhysicalImplementationConfiguration& input) {
  const auto implementation = ResolvePhysicalDefaults(input);
  const std::set<std::string> allowed_automatic = {"pdk",
                                                   "standard_cell_library",
                                                   "clock_period_ns",
                                                   "core_utilization_percent",
                                                   "orfs.platform",
                                                   "orfs.flow_variant",
                                                   "io.algorithm",
                                                   "io.unmatched_policy",
                                                   "pdn.multilayer",
                                                   "pdn.core_ring",
                                                   "pdn.enable_rails"};
  std::set<std::string> seen;
  for (const auto& field : implementation.automatic_fields) {
    if (!allowed_automatic.contains(field) || !seen.insert(field).second) {
      return {ErrorCode::kInvalidArgument,
              "Invalid automatic setting: " + field, 0};
    }
  }
  if (implementation.backend_id != "openlane2" &&
      implementation.backend_id != "orfs") {
    return {ErrorCode::kInvalidArgument,
            "Physical implementation backend is invalid", 0};
  }
  if ((!implementation.clock_ports.empty() ||
       !implementation.clock_period_ns.empty()) &&
      !IsDecimal(implementation.clock_period_ns, 0.000001,
                 std::numeric_limits<double>::max())) {
    return {ErrorCode::kInvalidArgument,
            "Physical implementation clock period is invalid", 0};
  }
  if (implementation.core_utilization_percent == 0 ||
      implementation.core_utilization_percent >= 100) {
    return {ErrorCode::kInvalidArgument,
            "Physical implementation core utilization is invalid", 0};
  }
  if (implementation.backend_id == "openlane2") {
    if (!IsHdlIdentifier(implementation.pdk)) {
      return {ErrorCode::kInvalidArgument, "OpenLane PDK is invalid", 0};
    }
    if (!IsHdlIdentifier(implementation.standard_cell_library)) {
      return {ErrorCode::kInvalidArgument,
              "OpenLane standard-cell library is invalid", 0};
    }
    if (!IsSafeOpenLaneOverrides(implementation.advanced_overrides_json)) {
      return {ErrorCode::kInvalidArgument,
              "OpenLane advanced overrides are invalid", 0};
    }
  }
  if (implementation.backend_id == "orfs" &&
      (implementation.orfs.platform.empty() ||
       !IsHdlIdentifier(implementation.orfs.platform) ||
       implementation.orfs.flow_variant.empty() ||
       !IsHdlIdentifier(implementation.orfs.flow_variant) ||
       !IsSafeOrfsOverrides(implementation.orfs.advanced_variables_json))) {
    return {ErrorCode::kInvalidArgument,
            "ORFS platform, flow variant, or variables are invalid", 0};
  }
  for (const std::string& port : implementation.clock_ports) {
    if (!IsHdlIdentifier(port)) {
      return {ErrorCode::kInvalidArgument,
              "Physical implementation clock port is invalid", 0};
    }
  }
  if (implementation.placement_density_percent &&
      !IsDecimal(*implementation.placement_density_percent, 1.0, 100.0)) {
    return {ErrorCode::kInvalidArgument,
            "Physical implementation placement density is invalid", 0};
  }
  if (!implementation.die_area.empty() && implementation.die_area.size() != 4) {
    return {ErrorCode::kInvalidArgument,
            "Physical implementation die area is invalid", 0};
  }
  if (implementation.backend_id == "orfs" && !implementation.die_area.empty() &&
      implementation.core_area.empty()) {
    return {ErrorCode::kInvalidArgument,
            "ORFS explicit floorplan requires both Die area and Core area", 0};
  }
  for (const std::string& coordinate : implementation.die_area) {
    if (!IsDecimal(coordinate, 0.0, std::numeric_limits<double>::max())) {
      return {ErrorCode::kInvalidArgument,
              "Physical implementation die area is invalid", 0};
    }
  }
  if (implementation.die_area.size() == 4 &&
      (std::strtod(implementation.die_area[2].c_str(), nullptr) <=
           std::strtod(implementation.die_area[0].c_str(), nullptr) ||
       std::strtod(implementation.die_area[3].c_str(), nullptr) <=
           std::strtod(implementation.die_area[1].c_str(), nullptr))) {
    return {ErrorCode::kInvalidArgument,
            "Physical implementation die area upper bounds are invalid", 0};
  }
  if (!implementation.core_area.empty() &&
      implementation.core_area.size() != 4) {
    return {ErrorCode::kInvalidArgument,
            "Physical implementation core area is invalid", 0};
  }
  for (const std::string& coordinate : implementation.core_area) {
    if (!IsDecimal(coordinate, 0.0, std::numeric_limits<double>::max())) {
      return {ErrorCode::kInvalidArgument,
              "Physical implementation core area is invalid", 0};
    }
  }
  if (!implementation.core_area.empty()) {
    if (implementation.die_area.size() != 4) {
      return {ErrorCode::kInvalidArgument,
              "Core area requires a matching die area", 0};
    }
    const auto coordinate = [](const std::vector<std::string>& area,
                               std::size_t index) {
      return std::strtod(area[index].c_str(), nullptr);
    };
    if (coordinate(implementation.core_area, 0) <
            coordinate(implementation.die_area, 0) ||
        coordinate(implementation.core_area, 1) <
            coordinate(implementation.die_area, 1) ||
        coordinate(implementation.core_area, 2) >
            coordinate(implementation.die_area, 2) ||
        coordinate(implementation.core_area, 3) >
            coordinate(implementation.die_area, 3) ||
        coordinate(implementation.core_area, 2) <=
            coordinate(implementation.core_area, 0) ||
        coordinate(implementation.core_area, 3) <=
            coordinate(implementation.core_area, 1)) {
      return {ErrorCode::kInvalidArgument,
              "Core area must be a valid rectangle inside the die area", 0};
    }
  }
  if (implementation.backend_id == "openlane2" &&
      implementation.tap_cell_distance_um &&
      !IsDecimal(*implementation.tap_cell_distance_um, 0.000001,
                 std::numeric_limits<double>::max())) {
    return {ErrorCode::kInvalidArgument,
            "Tap cell distance must be a positive decimal value in um", 0};
  }
  if ((!implementation.pnr_sdc_path.empty() &&
       !IsSafeRelativePath(implementation.pnr_sdc_path)) ||
      (!implementation.signoff_sdc_path.empty() &&
       !IsSafeRelativePath(implementation.signoff_sdc_path))) {
    return {ErrorCode::kCorruptData,
            "Physical implementation SDC path is invalid", 0};
  }
  // OpenLane-only I/O, tap-cell, and PDN values remain persisted while ORFS
  // is selected, but ORFS neither consumes nor validates them. This lets a
  // Cell switch backends without an inactive backend blocking the save.
  if (implementation.backend_id == "orfs") return Status::Success();
  const PowerDistributionConfiguration& pdn = implementation.power_distribution;
  const auto positive = [](const std::optional<std::string>& value) {
    return !value ||
           IsDecimal(*value, 0.000001, std::numeric_limits<double>::max());
  };
  const auto nonnegative = [](const std::optional<std::string>& value) {
    return !value || IsDecimal(*value, 0.0, std::numeric_limits<double>::max());
  };
  if (!positive(pdn.vertical_width_um) || !positive(pdn.horizontal_width_um) ||
      !positive(pdn.vertical_spacing_um) ||
      !positive(pdn.horizontal_spacing_um) ||
      !positive(pdn.vertical_pitch_um) || !positive(pdn.horizontal_pitch_um) ||
      !nonnegative(pdn.vertical_offset_um) ||
      !nonnegative(pdn.horizontal_offset_um)) {
    return {ErrorCode::kInvalidArgument,
            "PDN dimensions must be valid non-negative decimal values", 0};
  }
  const IoPlacementConfiguration& io = implementation.io_placement;
  if (io.algorithm != "matching" && io.algorithm != "random_equidistant" &&
      io.algorithm != "annealing") {
    return {ErrorCode::kInvalidArgument, "I/O placement algorithm is invalid",
            0};
  }
  if (io.unmatched_policy != "none" &&
      io.unmatched_policy != "unmatched_design" &&
      io.unmatched_policy != "unmatched_cfg" && io.unmatched_policy != "both") {
    return {ErrorCode::kInvalidArgument, "I/O unmatched-pin policy is invalid",
            0};
  }
  if (!positive(io.minimum_distance_um) || !positive(io.vertical_length_um) ||
      !positive(io.horizontal_length_um) ||
      !positive(io.vertical_thickness_multiplier) ||
      !positive(io.horizontal_thickness_multiplier) ||
      !nonnegative(io.vertical_extension_um) ||
      !nonnegative(io.horizontal_extension_um)) {
    return {ErrorCode::kInvalidArgument, "I/O placement dimensions are invalid",
            0};
  }
  for (const auto& layer : {&io.vertical_layer, &io.horizontal_layer}) {
    if (*layer && !IsHdlIdentifier(**layer)) {
      return {ErrorCode::kInvalidArgument,
              "I/O placement routing layer is invalid", 0};
    }
  }
  std::size_t total_pin_order_bytes = 0;
  for (const IoPinSideConfiguration* side :
       {&io.north, &io.south, &io.east, &io.west}) {
    if (!positive(side->minimum_distance_um)) {
      return {ErrorCode::kInvalidArgument,
              "I/O pin-order minimum distance is invalid", 0};
    }
    if (side->entries.size() > 4096) {
      return {ErrorCode::kInvalidArgument,
              "I/O pin-order contains too many entries", 0};
    }
    for (const std::string& entry : side->entries) {
      total_pin_order_bytes += entry.size();
      if (entry.empty() || entry.size() > 512 || entry.front() == '#' ||
          entry.front() == '@' || entry.find('\0') != std::string::npos ||
          entry.find('\r') != std::string::npos ||
          entry.find('\n') != std::string::npos) {
        return {ErrorCode::kInvalidArgument, "I/O pin-order entry is invalid",
                0};
      }
      if (entry.front() == '$' &&
          (entry.size() == 1 ||
           !std::all_of(entry.begin() + 1, entry.end(),
                        [](char character) {
                          return std::isdigit(
                              static_cast<unsigned char>(character));
                        }) ||
           std::strtoull(entry.c_str() + 1, nullptr, 10) == 0)) {
        return {ErrorCode::kInvalidArgument, "I/O virtual pin entry is invalid",
                0};
      }
    }
  }
  if (total_pin_order_bytes > 64 * 1024) {
    return {ErrorCode::kInvalidArgument, "I/O pin-order exceeds 64 KiB", 0};
  }
  return Status::Success();
}

Status ValidateProject(const Project& project) {
  if (project.schema_version != Project::kSchemaVersion) {
    return {ErrorCode::kUnsupportedSchema, "Unsupported project schema", 0};
  }
  if (!IsUuid(project.id) || !IsUuid(project.library_id) ||
      !IsUuid(project.cell_id)) {
    return {ErrorCode::kCorruptData, "Project identity is invalid", 0};
  }
  if (project.revision == 0 || project.name.empty() ||
      project.created_utc.empty() || project.modified_utc.empty()) {
    return {ErrorCode::kCorruptData, "Required project field is missing", 0};
  }
  const unsigned int logical_cpus = std::thread::hardware_concurrency();
  const std::uint32_t maximum = logical_cpus > 1 ? logical_cpus - 1 : 1;
  if (project.cpu_budget == 0 || project.cpu_budget > maximum) {
    return {ErrorCode::kInvalidArgument, "Project CPU budget is invalid", 0};
  }
  std::set<std::pair<std::string, std::string>> override_keys;
  for (const SourceOverride& override_value : project.source_overrides) {
    if (!IsUuid(override_value.view_id) ||
        !IsSafeRelativePath(override_value.relative_path) ||
        !override_keys
             .emplace(override_value.view_id, override_value.relative_path)
             .second) {
      return {ErrorCode::kCorruptData, "Source override is invalid", 0};
    }
  }
  std::set<std::pair<std::string, std::string>> testbench_keys;
  for (const TestbenchConfiguration& configuration :
       project.testbench_configurations) {
    const bool valid_backend = configuration.backend == "icarus" ||
                               configuration.backend == "verilator";
    const bool valid_runner =
        configuration.runner == "hdl" || configuration.runner == "cocotb";
    const bool valid_waveform = configuration.waveform_format == "none" ||
                                configuration.waveform_format == "vcd" ||
                                configuration.waveform_format == "fst";
    const bool valid_cocotb = configuration.runner != "cocotb" ||
                              IsHdlIdentifier(configuration.cocotb_module);
    if (!IsUuid(configuration.view_id) ||
        !IsSafeRelativePath(configuration.relative_path) ||
        !IsHdlIdentifier(configuration.top_module) || !valid_backend ||
        !valid_runner || !valid_waveform || !valid_cocotb ||
        configuration.waveform_enabled !=
            (configuration.waveform_format != "none") ||
        !testbench_keys
             .emplace(configuration.view_id, configuration.relative_path)
             .second) {
      return {ErrorCode::kCorruptData, "Testbench configuration is invalid", 0};
    }
  }
  for (const std::string& path : project.synthesis.liberty_paths) {
    if (!IsSafeRelativePath(path)) {
      return {ErrorCode::kCorruptData, "Synthesis Liberty path is invalid", 0};
    }
  }
  if (project.timing.corner_name.empty()) {
    return {ErrorCode::kCorruptData, "Timing corner is invalid", 0};
  }
  for (const std::string& path : project.timing.liberty_paths) {
    if (!IsSafeRelativePath(path)) {
      return {ErrorCode::kCorruptData, "Timing Liberty path is invalid", 0};
    }
  }
  if (!project.timing.sdc_path.empty() &&
      !IsSafeRelativePath(project.timing.sdc_path)) {
    return {ErrorCode::kCorruptData, "Timing SDC path is invalid", 0};
  }
  const Status implementation_status =
      ValidatePhysicalImplementationConfiguration(
          project.physical_implementation);
  if (!implementation_status.Ok()) return implementation_status;
  const PhysicalVerificationConfiguration& verification =
      project.physical_verification;
  const auto valid_text = [](std::string_view value) {
    return value.size() <= 256 && std::none_of(value.begin(), value.end(),
                                               [](unsigned char character) {
                                                 return character < 0x20 ||
                                                        character == 0x7f;
                                               });
  };
  if (!valid_text(verification.drc_recipe_id) ||
      !valid_text(verification.lvs_recipe_id) ||
      !valid_text(verification.top_cell) ||
      !valid_text(verification.power_net) ||
      !valid_text(verification.ground_net) ||
      verification.parameters_json.size() > 64 * 1024 ||
      verification.parameters_json.empty() ||
      verification.parameters_json.front() != '{' ||
      verification.parameters_json.back() != '}') {
    return {ErrorCode::kInvalidArgument,
            "Physical verification configuration is invalid", 0};
  }
  for (const std::string& directory : project.include_directories) {
    if (!IsSafeRelativePath(directory)) {
      return {ErrorCode::kCorruptData, "Include path escapes the Library", 0};
    }
  }
  if (project.constraint_path &&
      !IsSafeRelativePath(*project.constraint_path)) {
    return {ErrorCode::kCorruptData, "Constraint path escapes the Library", 0};
  }
  return Status::Success();
}

}  // namespace designpp::core

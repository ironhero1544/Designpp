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
  const PhysicalImplementationConfiguration& implementation =
      project.physical_implementation;
  if (implementation.backend_id != "openlane2" ||
      !IsHdlIdentifier(implementation.pdk) ||
      !IsHdlIdentifier(implementation.standard_cell_library) ||
      !IsDecimal(implementation.clock_period_ns, 0.000001,
                 std::numeric_limits<double>::max()) ||
      implementation.core_utilization_percent == 0 ||
      implementation.core_utilization_percent >= 100 ||
      !IsSafeOpenLaneOverrides(implementation.advanced_overrides_json)) {
    return {ErrorCode::kInvalidArgument,
            "Physical implementation configuration is invalid", 0};
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
  if ((!implementation.pnr_sdc_path.empty() &&
       !IsSafeRelativePath(implementation.pnr_sdc_path)) ||
      (!implementation.signoff_sdc_path.empty() &&
       !IsSafeRelativePath(implementation.signoff_sdc_path))) {
    return {ErrorCode::kCorruptData,
            "Physical implementation SDC path is invalid", 0};
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

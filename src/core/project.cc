// Copyright 2026 The Design++ Authors

#include "designpp/core/project.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
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

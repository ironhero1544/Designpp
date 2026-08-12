// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_ADAPTERS_YOSYS_ADAPTER_H_
#define DESIGNPP_ADAPTERS_YOSYS_ADAPTER_H_

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "designpp/adapters/tool_adapter.h"

namespace designpp::adapters {

struct SynthesisRequest {
  core::Project project;
  std::vector<application::ResolvedSource> sources;
  std::filesystem::path artifact_directory;
  std::vector<std::filesystem::path> liberty_files;
  bool flatten = false;
};

struct SynthesisPlan {
  runtime::WslCommand execute;
  std::filesystem::path script_path;
  std::string script_text;
  std::filesystem::path json_netlist_path;
  std::filesystem::path verilog_netlist_path;
  std::filesystem::path statistics_path;
  std::filesystem::path report_path;
};

struct SynthesisMetrics {
  std::uint64_t cell_count = 0;
  double area = 0.0;
  bool has_area = false;
};

enum class NetlistPortDirection {
  kInput,
  kOutput,
  kInout,
};

struct NetlistPort {
  std::string name;
  NetlistPortDirection direction = NetlistPortDirection::kInput;
  std::vector<std::string> bits;
};

struct NetlistCellPort {
  std::string name;
  NetlistPortDirection direction = NetlistPortDirection::kInput;
  std::vector<std::string> bits;
};

struct NetlistCell {
  std::string name;
  std::string type;
  std::vector<NetlistCellPort> ports;
};

struct GateSchematic {
  std::string top_module;
  std::vector<NetlistPort> ports;
  std::vector<NetlistCell> cells;
};

class YosysAdapter final {
 public:
  [[nodiscard]] runtime::WslCommand BuildProbeCommand() const;
  [[nodiscard]] core::Status Validate(const SynthesisRequest& request) const;
  [[nodiscard]] core::Result<SynthesisPlan> BuildPlan(
      const SynthesisRequest& request,
      const runtime::PathMapper& path_mapper) const;
  [[nodiscard]] std::vector<core::Diagnostic> ParseDiagnostics(
      std::string_view raw_output) const;
  [[nodiscard]] core::Result<SynthesisMetrics> ParseStatistics(
      std::string_view json) const;
  [[nodiscard]] core::Result<GateSchematic> ParseNetlist(
      std::string_view json, std::string_view top_module) const;
};

}  // namespace designpp::adapters

#endif  // DESIGNPP_ADAPTERS_YOSYS_ADAPTER_H_

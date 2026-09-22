// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_ADAPTERS_PHYSICAL_VERIFICATION_ADAPTER_H_
#define DESIGNPP_ADAPTERS_PHYSICAL_VERIFICATION_ADAPTER_H_

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "designpp/core/status.h"
#include "designpp/core/toolchain_profile.h"
#include "designpp/runtime/wsl_executor.h"

namespace designpp::adapters {

enum class VerificationCheck { kDrc, kLvs };

struct VerificationMarker {
  std::string category;
  std::string cell;
  std::string layer;
  std::string description;
  std::string geometry;
};

struct VerificationResult {
  bool parsed = false;
  bool passed = false;
  bool comparison_completed = false;
  std::size_t violation_count = 0;
  std::vector<VerificationMarker> markers;
  std::string detail;
};

// Immutable evidence available after a verification process terminates.
// DRC commonly produces a marker database, while KLayout LVS reports its
// comparison conclusion on stdout even when no standalone database is written.
struct VerificationResultArtifacts {
  std::string report;
  std::string process_output;
  std::string report_format;
  bool report_exists = false;
  std::string comparison_summary;
};

struct VerificationCommandInput {
  core::VerificationRecipe recipe;
  VerificationCheck check = VerificationCheck::kDrc;
  std::wstring gds_path;
  std::wstring schematic_path;
  std::wstring extracted_path;
  std::wstring report_path;
  std::wstring odb_path;
  std::wstring model_path;
  std::wstring staged_model_path;
  std::wstring design_cdl_path;
  std::wstring combined_cdl_path;
  std::wstring preparation_script_path;
  std::wstring top_cell;
  std::wstring distribution;
  std::wstring toolchain_root;
  std::wstring verification_script_path;
};

class PhysicalVerificationAdapter {
 public:
  virtual ~PhysicalVerificationAdapter() = default;

  [[nodiscard]] virtual std::string_view Engine() const noexcept = 0;
  [[nodiscard]] virtual VerificationCheck Check() const noexcept = 0;
  [[nodiscard]] virtual runtime::WslCommand BuildProbeCommand(
      const VerificationCommandInput& input) const = 0;
  [[nodiscard]] virtual core::Result<runtime::WslCommand> BuildCommand(
      const VerificationCommandInput& input) const = 0;
  // Builds the Tcl body used to export a design CDL from a final ODB.
  [[nodiscard]] virtual core::Result<std::string> BuildPreparationScript(
      const VerificationCommandInput& input) const;
  // Builds the ordered model staging, CDL export, and model combination
  // commands. Tool-specific command construction remains in the adapter.
  [[nodiscard]] virtual core::Result<std::vector<runtime::WslCommand>>
  BuildPreparationCommands(const VerificationCommandInput& input) const;
  [[nodiscard]] virtual core::Result<VerificationResult> ParseReport(
      std::string_view report) const = 0;
  [[nodiscard]] virtual core::Result<VerificationResult> ParseResult(
      const VerificationResultArtifacts& artifacts) const;
};

[[nodiscard]] std::unique_ptr<PhysicalVerificationAdapter>
CreatePhysicalVerificationAdapter(std::string_view engine);

// KLayout-native driver: applies the hash-verified sky130hd extraction contract
// and reads the comparison database independently of process log messages.
[[nodiscard]] std::string BuildKLayoutLvsDriver();

// Converts the standalone CDL '/' before an X-instance model to SPICE syntax.
// Explicit resistor values named 'short' become zero ohms.
// Preserves nets, pin order, comments, and line numbers. Ambiguous separators
// fail rather than changing circuit connectivity.
[[nodiscard]] core::Result<std::string> NormalizeCdlForSpice(
    std::string_view contents);

}  // namespace designpp::adapters

#endif  // DESIGNPP_ADAPTERS_PHYSICAL_VERIFICATION_ADAPTER_H_

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
  std::size_t violation_count = 0;
  std::vector<VerificationMarker> markers;
  std::string detail;
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
};

[[nodiscard]] std::unique_ptr<PhysicalVerificationAdapter>
CreatePhysicalVerificationAdapter(std::string_view engine);

}  // namespace designpp::adapters

#endif  // DESIGNPP_ADAPTERS_PHYSICAL_VERIFICATION_ADAPTER_H_

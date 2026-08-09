// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_RUNTIME_SETUP_CATALOG_H_
#define DESIGNPP_RUNTIME_SETUP_CATALOG_H_

#include <string>
#include <vector>

#include "designpp/runtime/process_runner.h"
#include "designpp/runtime/tool_catalog.h"

namespace designpp::runtime {

enum class OutputEncoding {
  kUtf8,
  kUtf16LittleEndian,
};

// Describes one sequential setup command and its output encoding.
struct SetupStep {
  std::wstring title;
  ProcessRequest request;
  OutputEncoding output_encoding = OutputEncoding::kUtf8;
  bool continue_after_failure = false;
  bool requires_elevation = false;
};

// Builds Windows-side WSL2 and Ubuntu setup steps.
[[nodiscard]] std::vector<SetupStep> BuildWslSetupSteps();

// Builds all supported WSL-side tool providers, including OpenLane and ORFS.
[[nodiscard]] std::vector<SetupStep> BuildCompleteToolSetupSteps();

/** Builds removal steps for every tool owned exclusively by Design++. */
[[nodiscard]] std::vector<SetupStep> BuildCompleteToolRemoveSteps();

// Builds an idempotent install/update plan for one selected tool.
[[nodiscard]] std::vector<SetupStep> BuildToolInstallSteps(ToolId tool_id);

// Builds a removal plan for one selected tool. An empty plan means removal is
// intentionally unsupported because the tool is shared or externally owned.
[[nodiscard]] std::vector<SetupStep> BuildToolRemoveSteps(ToolId tool_id);

}  // namespace designpp::runtime

#endif  // DESIGNPP_RUNTIME_SETUP_CATALOG_H_

// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_RUNTIME_TOOL_CATALOG_H_
#define DESIGNPP_RUNTIME_TOOL_CATALOG_H_

#include <string>
#include <vector>

#include "designpp/runtime/wsl_executor.h"

namespace designpp::runtime {

enum class ToolId {
  kVerilator,
  kIcarusVerilog,
  kCocotb,
  kYosys,
  kOpenSta,
  kOpenRoad,
  kMagic,
  kNetgen,
  kKlayout,
  kGtkWave,
  kOpenLane2,
  kOrfs,
  kDocker,
  kNix,
};

enum class InstallMethod {
  kApt,
  kPythonEnvironment,
  kManagedFlow,
  kExternal,
};

// Describes one EDA tool and the command used to identify its installation.
struct ToolDefinition {
  ToolId id;
  std::wstring display_name;
  std::wstring purpose;
  WslCommand probe_command;
  InstallMethod install_method;
  std::wstring install_hint;
  bool required = true;
};

// Builds the complete tool catalog in stable display order.
[[nodiscard]] std::vector<ToolDefinition> BuildToolCatalog();

// Returns a localized display name for an installation method.
[[nodiscard]] std::wstring InstallMethodName(InstallMethod method);

}  // namespace designpp::runtime

#endif  // DESIGNPP_RUNTIME_TOOL_CATALOG_H_

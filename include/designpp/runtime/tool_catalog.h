// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_RUNTIME_TOOL_CATALOG_H_
#define DESIGNPP_RUNTIME_TOOL_CATALOG_H_

#include <string>
#include <string_view>
#include <vector>

#include "designpp/runtime/process_runner.h"

namespace designpp::runtime {

enum class ToolId {
  kWebView2,
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
  kAsap7Models,
};

enum class InstallMethod {
  kWindowsRuntime,
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
  ProcessRequest probe_request;
  InstallMethod install_method;
  std::wstring install_hint;
  bool required = true;
};

// Builds the complete tool catalog in stable display order.
[[nodiscard]] std::vector<ToolDefinition> BuildToolCatalog();

// Parses only recognized version output; progress and diagnostics are not
// versions.
[[nodiscard]] std::wstring ParseToolVersion(ToolId id,
                                            std::wstring_view output);

// Returns a localized display name for an installation method.
[[nodiscard]] std::wstring InstallMethodName(InstallMethod method);

}  // namespace designpp::runtime

#endif  // DESIGNPP_RUNTIME_TOOL_CATALOG_H_

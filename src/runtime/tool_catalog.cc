// Copyright 2026 The Design++ Authors

#include "designpp/runtime/tool_catalog.h"

#include <cwctype>
#include <sstream>
#include <string_view>
#include <utility>

#include "designpp/core/toolchain_compatibility.h"
#include "designpp/runtime/setup_catalog.h"
#include "designpp/runtime/wsl_executor.h"

namespace designpp::runtime {
namespace {

ToolDefinition MakeTool(ToolId id, std::wstring display_name,
                        std::wstring purpose, std::wstring program,
                        std::vector<std::wstring> arguments,
                        InstallMethod install_method, std::wstring install_hint,
                        bool required = true) {
  WslCommand command;
  command.program = std::move(program);
  command.arguments = std::move(arguments);
  return ToolDefinition{id,
                        std::move(display_name),
                        std::move(purpose),
                        WslExecutor::BuildRequest(command),
                        install_method,
                        std::move(install_hint),
                        required};
}

ToolDefinition MakeWindowsTool(ToolId id, std::wstring display_name,
                               std::wstring purpose, ProcessRequest request,
                               InstallMethod install_method,
                               std::wstring install_hint,
                               bool required = true) {
  return ToolDefinition{id,
                        std::move(display_name),
                        std::move(purpose),
                        std::move(request),
                        install_method,
                        std::move(install_hint),
                        required};
}

ProcessRequest WebView2ProbeRequest() {
  ProcessRequest request;
  request.executable = L"powershell.exe";
  request.arguments = {
      L"-NoProfile", L"-NonInteractive", L"-Command",
      L"[Console]::OutputEncoding=[Text.UTF8Encoding]::new(); "
      L"$id='{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}'; "
      L"$paths=@("
      L"('HKLM:\\SOFTWARE\\Microsoft\\EdgeUpdate\\Clients\\'+$id),"
      L"('HKLM:\\SOFTWARE\\WOW6432Node\\Microsoft\\EdgeUpdate\\Clients\\'+$id),"
      L"('HKCU:\\SOFTWARE\\Microsoft\\EdgeUpdate\\Clients\\'+$id),"
      L"('HKCU:\\SOFTWARE\\WOW6432Node\\Microsoft\\EdgeUpdate\\Clients\\'+$id))"
      L"; "
      L"$versions=@($paths | ForEach-Object { "
      L"if(Test-Path $_){(Get-ItemProperty $_ -ErrorAction "
      L"SilentlyContinue).pv} "
      L"} | Where-Object {$_ -and $_ -ne '0.0.0.0'}); "
      L"if($versions.Count -eq 0){exit 1}; "
      L"$versions | Sort-Object {[version]$_} -Descending | Select-Object "
      L"-First 1"};
  return request;
}

ToolDefinition MakeManagedTool(ToolId id, std::wstring name,
                               std::wstring provider) {
  std::string provider_id;
  provider_id.reserve(provider.size());
  for (const wchar_t character : provider) {
    provider_id.push_back(static_cast<char>(character));
  }
  const core::ToolchainCompatibilityEntry* catalog_entry = nullptr;
  for (const auto& entry : core::ToolchainCompatibilityCatalog::Entries()) {
    if (entry.provider_id == provider_id) {
      catalog_entry = &entry;
      break;
    }
  }
  if (catalog_entry == nullptr) {
    return MakeTool(id, std::move(name), L"Managed environment inventory",
                    L"/bin/false", {}, InstallMethod::kManagedFlow,
                    L"Unsupported managed environment");
  }
  const auto widen_ascii = [](std::string_view text) {
    return std::wstring(text.begin(), text.end());
  };

  // Inventory is intentionally read-only. The completion marker is written only
  // after the explicit preparation flow has passed its capability checks.
  return MakeTool(
      id, std::move(name), L"Managed environment inventory", L"/bin/bash",
      {L"-lc",
       L"root=\"$HOME/.designpp/toolchains/environments/$2\"; "
       L"if [ ! -d \"$root\" ] && "
       L"[ -d \"$HOME/.designpp/toolchains/$1\" ]; then "
       L"root=\"$HOME/.designpp/toolchains/$1\"; fi; "
       L"if [ ! -d \"$root\" ]; then "
       L"printf '%s\\n' DESIGNPP_ENVIRONMENT_MISSING; exit 44; fi; "
       L"marker=\"$root/.designpp-environment\"; "
       L"if [ ! -f \"$marker\" ]; then "
       L"printf '%s\\n' DESIGNPP_ENVIRONMENT_PREPARATION_REQUIRED; exit 78; "
       L"fi; "
       L"marker_provider=$(sed -n 's/^provider=//p' \"$marker\"); "
       L"version=$(sed -n 's/^version=//p' \"$marker\"); "
       L"commit=$(sed -n 's/^commit=//p' \"$marker\"); "
       L"test \"$marker_provider\" = \"$1\" || { printf '%s\\n' "
       L"DESIGNPP_ENVIRONMENT_PREPARATION_REQUIRED; exit 78; }; "
       L"test \"$version\" = \"$3\" && test \"$commit\" = \"$4\" || { "
       L"printf '%s\\n' DESIGNPP_ENVIRONMENT_PREPARATION_REQUIRED; "
       L"exit 78; }; "
       L"test \"$(git -C \"$root\" rev-parse HEAD 2>/dev/null)\" = "
       L"\"$commit\" || { printf '%s\\n' "
       L"DESIGNPP_ENVIRONMENT_PREPARATION_REQUIRED; exit 78; }; "
       L"printf 'DESIGNPP_MANAGED_VERSION=%s\\n' \"$version\"",
       L"designpp-managed-inventory", provider,
       widen_ascii(catalog_entry->bundle_id),
       widen_ascii(catalog_entry->version),
       widen_ascii(catalog_entry->revision)},
      InstallMethod::kManagedFlow, L"Existing environment / unverified");
}

}  // namespace

std::wstring ParseToolVersion(ToolId id, std::wstring_view output) {
  if (output.size() > 65536) return {};
  std::wistringstream lines{std::wstring(output)};
  std::wstring line;
  while (std::getline(lines, line)) {
    const auto start = line.find_first_not_of(L" \t\r");
    if (start == std::wstring::npos) continue;
    line.erase(0, start);
    constexpr std::wstring_view kManagedVersionPrefix =
        L"DESIGNPP_MANAGED_VERSION=";
    const bool managed_tool = id == ToolId::kOpenSta ||
                              id == ToolId::kOpenRoad ||
                              id == ToolId::kOpenLane2 || id == ToolId::kOrfs;
    if (managed_tool && line.starts_with(kManagedVersionPrefix)) {
      std::wstring version = line.substr(kManagedVersionPrefix.size());
      if (!version.empty() &&
          version.find_first_not_of(L"0123456789abcdefghijklmnopqrstuvwxyzABCDE"
                                    L"FGHIJKLMNOPQRSTUVWXYZ.-+_:") ==
              std::wstring::npos) {
        return version;
      }
      continue;
    }
    if (id == ToolId::kNix && line.starts_with(L"nix (")) {
      const auto close = line.find(L") ");
      if (close != std::wstring::npos) {
        std::wstring version = line.substr(close + 2);
        const auto end = version.find_first_of(L" \t\r,");
        if (end != std::wstring::npos) version.resize(end);
        if (!version.empty() && std::iswdigit(version.front()) &&
            version.find(L'.') != std::wstring::npos &&
            version.find_first_not_of(L"0123456789abcdefghijklmnopqrstuvwxyzABC"
                                      L"DEFGHIJKLMNOPQRSTUVWXYZ.-+_:") ==
                std::wstring::npos) {
          return version;
        }
      }
      continue;
    }
    const wchar_t* prefix = nullptr;
    switch (id) {
      case ToolId::kVerilator:
        prefix = L"Verilator ";
        break;
      case ToolId::kIcarusVerilog:
        prefix = L"Icarus Verilog version ";
        break;
      case ToolId::kYosys:
        prefix = L"Yosys ";
        break;
      case ToolId::kCocotb:
        prefix = L"Version: ";
        break;
      case ToolId::kKlayout:
        prefix = L"KLayout ";
        break;
      case ToolId::kGtkWave:
        prefix = L"GTKWave Analyzer v";
        break;
      case ToolId::kDocker:
        prefix = L"Docker version ";
        break;
      case ToolId::kAsap7Models:
        prefix = L"ASAP7 CDL ";
        break;
      case ToolId::kNix:
        prefix = L"nix (Nix) ";
        break;
      default:
        break;
    }
    if (prefix) {
      if (!line.starts_with(prefix)) continue;
      line.erase(0, std::wstring_view(prefix).size());
    } else if (id != ToolId::kWebView2 && id != ToolId::kNetgen &&
               id != ToolId::kMagic && id != ToolId::kOpenSta &&
               id != ToolId::kOpenRoad) {
      continue;
    }
    const auto end = line.find_first_of(L" \t\r,");
    const std::wstring version = line.substr(0, end);
    if (version.empty() || !std::iswdigit(version.front()) ||
        version.find(L'.') == std::wstring::npos)
      continue;
    if (version.find_first_not_of(L"0123456789abcdefghijklmnopqrstuvwxyzABCDEFG"
                                  L"HIJKLMNOPQRSTUVWXYZ.-+_:~") !=
        std::wstring::npos)
      continue;
    return version;
  }
  return {};
}

std::vector<ToolDefinition> BuildToolCatalog() {
  std::vector<ToolDefinition> tools;
  tools.reserve(18);
  tools.push_back(MakeWindowsTool(
      ToolId::kWebView2, L"Microsoft Edge WebView2 Runtime",
      L"Monaco HDL editor host", WebView2ProbeRequest(),
      InstallMethod::kWindowsRuntime,
      L"Microsoft Evergreen Runtime (shared Windows component)"));
  tools.push_back(MakeTool(ToolId::kVerilator, L"Verilator",
                           L"SystemVerilog lint / simulation", L"verilator",
                           {L"--version"}, InstallMethod::kApt,
                           L"Ubuntu package: verilator"));
  tools.push_back(MakeTool(ToolId::kIcarusVerilog, L"Icarus Verilog",
                           L"Verilog simulation", L"iverilog", {L"-V"},
                           InstallMethod::kApt, L"Ubuntu package: iverilog"));
  tools.push_back(MakeTool(
      ToolId::kCocotb, L"cocotb", L"Python testbench", L"/bin/bash",
      {L"-lc",
       L"\"$HOME/.designpp/venv/bin/python\" -m pip "
       L"show cocotb"},
      InstallMethod::kPythonEnvironment, L"Design++ Python environment"));
  tools.push_back(MakeTool(ToolId::kYosys, L"Yosys", L"RTL synthesis", L"yosys",
                           {L"-V"}, InstallMethod::kApt,
                           L"Ubuntu package: yosys"));
  tools.push_back(
      MakeManagedTool(ToolId::kOpenSta, L"OpenSTA — OpenLane", L"openlane2"));
  tools.push_back(
      MakeManagedTool(ToolId::kOpenRoad, L"OpenROAD — OpenLane", L"openlane2"));
  tools.push_back(
      MakeManagedTool(ToolId::kOpenRoad, L"OpenROAD — ORFS", L"orfs"));
  tools.push_back(MakeTool(ToolId::kMagic, L"Magic", L"DRC / layout", L"magic",
                           {L"--version"}, InstallMethod::kApt,
                           L"Ubuntu package: magic"));
  // Ubuntu's `netgen` package is an unrelated 3D mesh generator. Query the
  // LVS package directly so the probe never starts either tool's GUI.
  tools.push_back(MakeTool(ToolId::kNetgen, L"Netgen LVS",
                           L"Layout versus schematic", L"/usr/bin/dpkg-query",
                           {L"-W", L"-f=${Version}\\n", L"netgen-lvs"},
                           InstallMethod::kApt, L"Ubuntu package: netgen-lvs"));
  tools.push_back(MakeTool(ToolId::kKlayout, L"KLayout",
                           L"GDSII / LEF / DEF viewer", L"klayout", {L"-v"},
                           InstallMethod::kApt, L"Ubuntu package: klayout"));
  tools.push_back(MakeTool(ToolId::kGtkWave, L"GTKWave", L"Waveform viewer",
                           L"gtkwave", {L"--version"}, InstallMethod::kApt,
                           L"Ubuntu package: gtkwave"));
  tools.push_back(
      MakeManagedTool(ToolId::kOpenLane2, L"OpenLane 2", L"openlane2"));
  tools.push_back(MakeManagedTool(ToolId::kOrfs, L"ORFS", L"orfs"));
  tools.push_back({ToolId::kAsap7Models, L"ASAP7 CDL models",
                   L"Cell reference models; KLayout LVS recipe still required",
                   BuildAsap7ModelRequest(false), InstallMethod::kManagedFlow,
                   L"Download hash-pinned official R/L/SL/SRAM CDL models. "
                   L"No Calibre or ORFS rebuild required.",
                   false});
  tools.push_back(MakeTool(ToolId::kDocker, L"Docker CLI",
                           L"Container execution provider", L"docker",
                           {L"--version"}, InstallMethod::kExternal,
                           L"Optional Docker Desktop WSL integration", false));
  tools.push_back(MakeTool(
      ToolId::kNix, L"Nix", L"Reproducible OpenLane environment", L"/bin/bash",
      {L"-lc",
       L". /nix/var/nix/profiles/default/etc/profile.d/nix-daemon.sh "
       L"2>/dev/null || true; nix --version"},
      InstallMethod::kManagedFlow, L"Official OpenLane Nix installer"));
  return tools;
}

std::wstring InstallMethodName(InstallMethod method) {
  switch (method) {
    case InstallMethod::kWindowsRuntime:
      return L"Windows Runtime";
    case InstallMethod::kApt:
      return L"APT";
    case InstallMethod::kPythonEnvironment:
      return L"Python venv";
    case InstallMethod::kManagedFlow:
      return L"OpenLane / ORFS";
    case InstallMethod::kExternal:
      return L"외부 설치";
  }
  return L"알 수 없음";
}

}  // namespace designpp::runtime

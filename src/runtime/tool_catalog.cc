// Copyright 2026 The Design++ Authors

#include "designpp/runtime/tool_catalog.h"

#include <utility>

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
                        std::move(command),
                        install_method,
                        std::move(install_hint),
                        required};
}

}  // namespace

std::vector<ToolDefinition> BuildToolCatalog() {
  std::vector<ToolDefinition> tools;
  tools.reserve(14);
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
  tools.push_back(MakeTool(
      ToolId::kOpenSta, L"OpenSTA", L"Static timing analysis", L"/bin/bash",
      {L"-lc",
       L". /nix/var/nix/profiles/default/etc/profile.d/nix-daemon.sh "
       L"2>/dev/null || true; nix-shell "
       L"\"$HOME/.designpp/toolchains/openlane2/shell.nix\" "
       L"--run 'sta -version'"},
      InstallMethod::kManagedFlow, L"Provided by OpenLane 2 Nix environment"));
  tools.push_back(MakeTool(
      ToolId::kOpenRoad, L"OpenROAD", L"Physical design", L"/bin/bash",
      {L"-lc",
       L". /nix/var/nix/profiles/default/etc/profile.d/nix-daemon.sh "
       L"2>/dev/null || true; nix-shell "
       L"\"$HOME/.designpp/toolchains/openlane2/shell.nix\" "
       L"--run 'openroad -version'"},
      InstallMethod::kManagedFlow, L"Provided by OpenLane 2 Nix environment"));
  tools.push_back(MakeTool(ToolId::kMagic, L"Magic", L"DRC / layout", L"magic",
                           {L"--version"}, InstallMethod::kApt,
                           L"Ubuntu package: magic"));
  // Ubuntu's `netgen` package is an unrelated 3D mesh generator. Query the
  // LVS package directly so the probe never starts either tool's GUI.
  tools.push_back(MakeTool(
      ToolId::kNetgen, L"Netgen LVS", L"Layout versus schematic",
      L"/usr/bin/dpkg-query", {L"-W", L"-f=${Version}\\n", L"netgen-lvs"},
      InstallMethod::kApt, L"Ubuntu package: netgen-lvs"));
  tools.push_back(MakeTool(ToolId::kKlayout, L"KLayout",
                           L"GDSII / LEF / DEF viewer", L"klayout", {L"-v"},
                           InstallMethod::kApt, L"Ubuntu package: klayout"));
  tools.push_back(MakeTool(ToolId::kGtkWave, L"GTKWave", L"Waveform viewer",
                           L"gtkwave", {L"--version"}, InstallMethod::kApt,
                           L"Ubuntu package: gtkwave"));
  tools.push_back(MakeTool(
      ToolId::kOpenLane2, L"OpenLane 2", L"Managed RTL-to-GDS flow",
      L"/bin/bash",
      {L"-lc",
       L". /nix/var/nix/profiles/default/etc/profile.d/nix-daemon.sh "
       L"2>/dev/null || true; nix-shell "
       L"\"$HOME/.designpp/toolchains/openlane2/shell.nix\" "
       L"--run 'openlane --version'"},
      InstallMethod::kManagedFlow, L"Official OpenLane Nix environment"));
  tools.push_back(
      MakeTool(ToolId::kOrfs, L"ORFS", L"OpenROAD Flow Scripts", L"/bin/bash",
               {L"-lc",
                L"test -f \"$HOME/.designpp/toolchains/orfs/flow/Makefile\" && "
                L"git -C \"$HOME/.designpp/toolchains/orfs\" describe --always "
                L"--dirty"},
               InstallMethod::kManagedFlow, L"Official ORFS Git repository"));
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

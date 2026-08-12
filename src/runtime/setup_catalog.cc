// Copyright 2026 The Design++ Authors

#include "designpp/runtime/setup_catalog.h"

#include <utility>

#include "designpp/runtime/wsl_executor.h"

namespace designpp::runtime {
namespace {

ProcessRequest MakePowerShellRequest(std::wstring script) {
  ProcessRequest request;
  request.executable = L"powershell.exe";
  request.arguments = {L"-NoProfile", L"-NonInteractive", L"-Command",
                       std::move(script)};
  return request;
}

ProcessRequest MakeRootWslRequest(std::wstring program,
                                  std::vector<std::wstring> arguments) {
  ProcessRequest request;
  request.executable = L"wsl.exe";
  request.arguments = {L"--user", L"root", L"--exec", std::move(program)};
  request.arguments.insert(request.arguments.end(), arguments.begin(),
                           arguments.end());
  return request;
}

std::wstring AptPackageName(ToolId tool_id) {
  switch (tool_id) {
    case ToolId::kVerilator:
      return L"verilator";
    case ToolId::kIcarusVerilog:
      return L"iverilog";
    case ToolId::kYosys:
      return L"yosys";
    case ToolId::kMagic:
      return L"magic";
    case ToolId::kNetgen:
      return L"netgen-lvs";
    case ToolId::kKlayout:
      return L"klayout";
    case ToolId::kGtkWave:
      return L"gtkwave";
    default:
      return {};
  }
}

SetupStep WebView2InstallStep() {
  return {
      L"Microsoft Edge WebView2 Evergreen Runtime 설치 / 복구",
      MakePowerShellRequest(
          L"$operation=Join-Path ([IO.Path]::GetTempPath()) "
          L"('DesignPlusPlus-WebView2-'+[guid]::NewGuid().ToString('N')); "
          L"$installer=Join-Path $operation 'MicrosoftEdgeWebview2Setup.exe'; "
          L"New-Item -ItemType Directory -Path $operation -ErrorAction Stop "
          L"| Out-Null; try { "
          L"Invoke-WebRequest -UseBasicParsing -Uri "
          L"'https://go.microsoft.com/fwlink/p/?LinkId=2124703' "
          L"-OutFile $installer -ErrorAction Stop; "
          L"$process=Start-Process -FilePath $installer "
          L"-ArgumentList '/silent','/install' -Wait -PassThru; "
          L"exit $process.ExitCode } finally { "
          L"if(Test-Path -LiteralPath $operation){"
          L"Remove-Item -LiteralPath $operation -Recurse -Force} }"),
      OutputEncoding::kUtf16LittleEndian, false, true};
}

}  // namespace

std::vector<SetupStep> BuildWslSetupSteps() {
  std::vector<SetupStep> steps;
  steps.push_back({L"WSL2 및 Ubuntu 관리자 설정",
                   MakePowerShellRequest(
                       L"$distros = @(wsl.exe --list --quiet 2>$null); "
                       L"if ($distros -notcontains 'Ubuntu') { "
                       L"wsl.exe --install --distribution Ubuntu --no-launch; "
                       L"if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE } }; "
                       L"wsl.exe --set-default-version 2; exit $LASTEXITCODE"),
                   OutputEncoding::kUtf16LittleEndian, false, true});

  WslCommand probe;
  probe.program = L"/usr/bin/uname";
  probe.arguments = {L"-a"};
  steps.push_back({L"WSL2 Linux 실행 확인", WslExecutor::BuildRequest(probe),
                   OutputEncoding::kUtf8, false});
  return steps;
}

std::vector<SetupStep> BuildCompleteToolSetupSteps() {
  std::vector<SetupStep> steps;
  steps.push_back({L"APT 패키지 인덱스 업데이트",
                   MakeRootWslRequest(L"/usr/bin/apt-get", {L"update"}),
                   OutputEncoding::kUtf8, false});
  steps.push_back({L"기본 EDA 패키지 설치",
                   MakeRootWslRequest(
                       L"/usr/bin/apt-get",
                       {L"install", L"-y", L"verilator", L"iverilog", L"yosys",
                        L"gtkwave", L"magic", L"netgen-lvs", L"klayout",
                        L"python3", L"python3-pip", L"python3-venv", L"curl",
                        L"git", L"ca-certificates", L"xz-utils", L"make"}),
                   OutputEncoding::kUtf8, false});

  WslCommand python_environment;
  python_environment.program = L"/bin/bash";
  python_environment.arguments = {
      L"-lc",
      L"python3 -m venv \"$HOME/.designpp/venv\" && "
      L"\"$HOME/.designpp/venv/bin/python\" -m pip install --upgrade pip "
      L"cocotb"};
  steps.push_back({L"Design++ Python 환경 및 cocotb 설치",
                   WslExecutor::BuildRequest(python_environment),
                   OutputEncoding::kUtf8, false});

  WslCommand nix_install;
  nix_install.program = L"/bin/bash";
  nix_install.arguments = {
      L"-lc",
      L"if command -v nix >/dev/null 2>&1 || "
      L"test -x /nix/var/nix/profiles/default/bin/nix; then "
      L"echo 'Nix is already installed.'; exit 0; fi; "
      L"curl --proto '=https' --tlsv1.2 -sSf -L "
      L"https://install.determinate.systems/nix | "
      L"sh -s -- install --no-confirm --extra-conf \""
      L"extra-substituters = https://openlane.cachix.org\n"
      L"extra-trusted-public-keys = "
      L"openlane.cachix.org-1:qqdwh+QMNGmZAuyeQJTH9ErW57OWSvdtuwfBKdS254E="
      L"\""};
  steps.push_back(
      {L"Nix 및 OpenLane binary cache 설치",
       MakeRootWslRequest(nix_install.program, nix_install.arguments),
       OutputEncoding::kUtf8, false});

  WslCommand openlane_install;
  openlane_install.program = L"/bin/bash";
  openlane_install.arguments = {
      L"-lc",
      L"set -e; . /nix/var/nix/profiles/default/etc/profile.d/nix-daemon.sh "
      L"2>/dev/null || true; mkdir -p \"$HOME/.designpp/toolchains\"; "
      L"if test -d \"$HOME/.designpp/toolchains/openlane2/.git\"; then "
      L"git -C \"$HOME/.designpp/toolchains/openlane2\" pull --ff-only; "
      L"else git clone --depth 1 https://github.com/efabless/openlane2 "
      L"\"$HOME/.designpp/toolchains/openlane2\"; fi; "
      L"nix-shell \"$HOME/.designpp/toolchains/openlane2/shell.nix\" "
      L"--run 'openlane --smoke-test'"};
  steps.push_back({L"OpenLane 2와 managed EDA toolchain 설치",
                   WslExecutor::BuildRequest(openlane_install),
                   OutputEncoding::kUtf8, false});

  WslCommand orfs_install;
  orfs_install.program = L"/bin/bash";
  orfs_install.arguments = {
      L"-lc",
      L"set -e; mkdir -p \"$HOME/.designpp/toolchains\"; "
      L"if test -d \"$HOME/.designpp/toolchains/orfs/.git\"; then "
      L"git -C \"$HOME/.designpp/toolchains/orfs\" pull --ff-only; "
      L"else git clone --depth 1 --recursive "
      L"https://github.com/The-OpenROAD-Project/OpenROAD-flow-scripts "
      L"\"$HOME/.designpp/toolchains/orfs\"; fi; "
      L"git -C \"$HOME/.designpp/toolchains/orfs\" submodule update "
      L"--init --recursive"};
  steps.push_back({L"OpenROAD Flow Scripts 설치",
                   WslExecutor::BuildRequest(orfs_install),
                   OutputEncoding::kUtf8, false});
  steps.push_back(WebView2InstallStep());
  return steps;
}

std::vector<SetupStep> BuildCompleteToolRemoveSteps() {
  std::vector<SetupStep> steps;

  WslCommand managed_files;
  managed_files.program = L"/bin/bash";
  managed_files.arguments = {
      L"-lc",
      L"rm -rf -- \"$HOME/.designpp/toolchains/openlane2\" "
      L"\"$HOME/.designpp/toolchains/orfs\" "
      L"\"$HOME/.designpp/venv\""};
  steps.push_back({L"Design++ managed toolchain 파일 삭제",
                   WslExecutor::BuildRequest(managed_files),
                   OutputEncoding::kUtf8, false});

  steps.push_back({L"APT EDA 패키지 전체 삭제",
                   MakeRootWslRequest(
                       L"/usr/bin/apt-get",
                       {L"remove", L"-y", L"verilator", L"iverilog", L"yosys",
                        L"gtkwave", L"magic", L"netgen-lvs", L"klayout"}),
                   OutputEncoding::kUtf8, false});
  return steps;
}

std::vector<SetupStep> BuildToolInstallSteps(ToolId tool_id) {
  if (tool_id == ToolId::kWebView2) {
    return {WebView2InstallStep()};
  }
  const std::wstring package = AptPackageName(tool_id);
  if (!package.empty()) {
    return {
        {L"APT 패키지 인덱스 업데이트",
         MakeRootWslRequest(L"/usr/bin/apt-get", {L"update"}),
         OutputEncoding::kUtf8, false},
        {L"APT 패키지 설치: " + package,
         MakeRootWslRequest(L"/usr/bin/apt-get", {L"install", L"-y", package}),
         OutputEncoding::kUtf8, false}};
  }

  if (tool_id == ToolId::kCocotb) {
    WslCommand command;
    command.program = L"/bin/bash";
    command.arguments = {
        L"-lc",
        L"python3 -m venv \"$HOME/.designpp/venv\" && "
        L"\"$HOME/.designpp/venv/bin/python\" -m pip install --upgrade "
        L"pip cocotb"};
    return {{L"cocotb 설치 / 업데이트", WslExecutor::BuildRequest(command),
             OutputEncoding::kUtf8, false}};
  }

  std::vector<SetupStep> complete = BuildCompleteToolSetupSteps();
  if (tool_id == ToolId::kNix) {
    return {std::move(complete[3])};
  }
  if (tool_id == ToolId::kOpenLane2 || tool_id == ToolId::kOpenRoad ||
      tool_id == ToolId::kOpenSta) {
    std::vector<SetupStep> steps;
    steps.push_back(std::move(complete[3]));
    steps.push_back(std::move(complete[4]));
    return steps;
  }
  if (tool_id == ToolId::kOrfs) {
    return {std::move(complete[5])};
  }
  return {};
}

std::vector<SetupStep> BuildToolRemoveSteps(ToolId tool_id) {
  const std::wstring package = AptPackageName(tool_id);
  if (!package.empty()) {
    return {
        {L"APT 패키지 삭제: " + package,
         MakeRootWslRequest(L"/usr/bin/apt-get", {L"remove", L"-y", package}),
         OutputEncoding::kUtf8, false}};
  }

  WslCommand command;
  command.program = L"/bin/bash";
  if (tool_id == ToolId::kCocotb) {
    command.arguments = {
        L"-lc",
        L"if test -x \"$HOME/.designpp/venv/bin/python\"; then "
        L"\"$HOME/.designpp/venv/bin/python\" -m pip uninstall -y cocotb; "
        L"else echo 'cocotb environment is not installed.'; fi"};
    return {{L"cocotb 삭제", WslExecutor::BuildRequest(command),
             OutputEncoding::kUtf8, false}};
  }
  if (tool_id == ToolId::kOpenLane2) {
    command.arguments = {L"-lc",
                         L"rm -rf -- \"$HOME/.designpp/toolchains/openlane2\""};
    return {{L"Design++ OpenLane 2 checkout 삭제",
             WslExecutor::BuildRequest(command), OutputEncoding::kUtf8, false}};
  }
  if (tool_id == ToolId::kOrfs) {
    command.arguments = {L"-lc",
                         L"rm -rf -- \"$HOME/.designpp/toolchains/orfs\""};
    return {{L"Design++ ORFS checkout 삭제", WslExecutor::BuildRequest(command),
             OutputEncoding::kUtf8, false}};
  }
  return {};
}

}  // namespace designpp::runtime

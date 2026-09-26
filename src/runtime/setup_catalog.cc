// Copyright 2026 The Design++ Authors

#include "designpp/runtime/setup_catalog.h"

#include <utility>

#include "designpp/core/toolchain_compatibility.h"
#include "designpp/runtime/wsl_executor.h"

namespace designpp::runtime {
namespace {

std::wstring CatalogText(std::string_view text) {
  return std::wstring(text.begin(), text.end());
}

// The 26Q2 flake otherwise links a distribution ABC that lacks read_lib -m.
// Keep the recipe correction explicit and apply it only to the pinned
// candidate.
constexpr wchar_t kOrfsYosysRecipePatch[] =
    LR"patch(--- a/flake.nix
+++ b/flake.nix
@@ -14,7 +14,9 @@
         };
         # TODO: don't override src when ./abc is empty
         # which happens when the command used is `nix build` and not `nix build ?submodules=1`
-        abc-verifier = pkgs.abc-verifier;
+        abc-verifier = pkgs.abc-verifier.overrideAttrs (_: {
+          src = ./abc;
+        });
         yosys = pkgs.clangStdenv.mkDerivation {
           name = "yosys";
           src = ./. ;
@@ -30,1 +32,1 @@
-            make -j$(nproc) ABCEXTERNAL=yosys-abc
+            make -j$NIX_BUILD_CORES ABCEXTERNAL=yosys-abc PREFIX=$out
)patch";

constexpr wchar_t kOrfsAbcTestRecipePatch[] =
    LR"patch(--- a/flake.nix
+++ b/flake.nix
@@ -17,3 +17,10 @@
-        abc-verifier = pkgs.abc-verifier.overrideAttrs (_: {
+        abc-verifier = pkgs.abc-verifier.overrideAttrs (old: {
           src = ./abc;
+          cmakeFlags = (old.cmakeFlags or []) ++ [
+            "-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=${pkgs.fetchzip {
+              url = "https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip";
+              sha256 = "sha256-t0RchAHTJbuI5YW4uyBPykTvcjy90JW9AOPNjIhwh6U=";
+            }}"
+            "-DFETCHCONTENT_FULLY_DISCONNECTED=ON"
+          ];
         });
)patch";

constexpr wchar_t kOrfsEqyRecipePatch[] =
    LR"patch(--- a/flake.nix
+++ b/flake.nix
@@ -14,2 +14,6 @@
     };
+    eqy-src = {
+      url = "git+https://github.com/YosysHQ/eqy?rev=eff96db01293848b993651caa52d747f191be02e";
+      flake = false;
+    };
   };
@@ -17,1 +21,1 @@
-  outputs = { self, nixpkgs, flake-utils, openroad, yosys }: flake-utils.lib.eachDefaultSystem (
+  outputs = { self, nixpkgs, flake-utils, openroad, yosys, eqy-src }: flake-utils.lib.eachDefaultSystem (
@@ -21,2 +25,18 @@
         pkgs = nixpkgs.legacyPackages.${system};
+        eqy = pkgs.stdenv.mkDerivation {
+          pname = "eqy";
+          version = "2026-03-31-eff96db";
+          src = eqy-src;
+          nativeBuildInputs = [ pkgs.clang pkgs.gnumake ];
+          buildInputs = [
+            pkgs.libffi pkgs.readline pkgs.tcl pkgs.zlib
+            yosys.packages.${system}.default
+          ];
+          postPatch = ''
+            printf '%s' eff96db01293848b993651caa52d747f191be02e > .gittag
+          '';
+          installPhase = ''
+            make install PREFIX=$out
+          '';
+        };
       in {
@@ -24,1 +38,2 @@
         buildInputs = [
+          eqy
           openroad.packages.${system}.default
)patch";

// The caller must hold the matching installation lock. Legacy candidates are
// identified by exact Git origin; new candidates carry an ownership marker.
constexpr wchar_t kCandidateCleanupScript[] =
    LR"script(
cleanup_candidates() {
  local candidate_name="$1" candidate_provider="$2" expected_origin="$3"
  local stale stale_origin failed=0
  for stale in "$toolchains"/."$candidate_name"-candidate.*; do
    [ -d "$stale" ] && [ ! -L "$stale" ] || continue
    [ ! -e "$stale/.designpp-environment" ] || continue
    if [ -f "$stale/.designpp-candidate" ]; then
      grep -Fxq 'owner=designpp' "$stale/.designpp-candidate" || continue
      grep -Fxq "provider=$candidate_provider" "$stale/.designpp-candidate" || continue
    else
      stale_origin=$(git -C "$stale" remote get-url origin 2>/dev/null || true)
      [ "$stale_origin" = "$expected_origin" ] || continue
    fi
    if rm -rf -- "$stale"; then
      printf 'Removed abandoned %s candidate: %s\n' "$candidate_provider" "$stale"
    else
      printf 'Could not remove abandoned %s candidate: %s\n' "$candidate_provider" "$stale" >&2
      failed=1
    fi
  done
  return "$failed"
}
)script";

ProcessRequest MakePowerShellRequest(std::wstring script,
                                     bool allow_failure_prompt = false) {
  ProcessRequest request;
  request.executable = L"powershell.exe";
  request.arguments = {L"-NoProfile"};
  if (!allow_failure_prompt) {
    request.arguments.push_back(L"-NonInteractive");
  }
  request.arguments.push_back(L"-Command");
  request.arguments.push_back(
      L"$Host.UI.RawUI.WindowTitle='Design++ 관리자 설정'; "
      L"Write-Host 'Design++ 관리자 설정을 시작합니다.'; " +
      std::move(script));
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
  steps.push_back(
      {L"WSL2 및 Ubuntu 관리자 설정",
       MakePowerShellRequest(
           L"function FailSetup($stage, $code) { "
           L"Write-Host ('WSL2 setup failed at: ' + $stage + "
           L"'; exit code: ' + $code) -ForegroundColor Red; "
           L"Read-Host 'Press Enter after recording the error'; "
           L"exit $code }; "
           L"$distros = @(wsl.exe --list --quiet 2>$null); "
           L"$stateDir = Join-Path $env:LOCALAPPDATA 'Design++'; "
           L"$pending = Join-Path $stateDir 'ubuntu-bootstrap.pending'; "
           L"$newUbuntu = $distros -notcontains 'Ubuntu'; "
           L"if ($newUbuntu) { "
           L"Write-Output 'Installing Ubuntu'; "
           L"wsl.exe --install --distribution Ubuntu --no-launch; "
           L"if ($LASTEXITCODE -ne 0) { "
           L"FailSetup 'install Ubuntu' $LASTEXITCODE }; "
           L"New-Item -ItemType Directory -Force -Path $stateDir | "
           L"Out-Null; New-Item -ItemType File -Force -Path $pending | "
           L"Out-Null }; "
           L"Write-Output 'Updating WSL'; "
           L"wsl.exe --update; "
           L"if ($LASTEXITCODE -ne 0) { "
           L"FailSetup 'update WSL' $LASTEXITCODE }; "
           L"Write-Output 'Setting WSL default version to 2'; "
           L"wsl.exe --set-default-version 2; "
           L"if ($LASTEXITCODE -ne 0) { "
           L"FailSetup 'set-default-version' $LASTEXITCODE }; "
           L"$managedBootstrap = $newUbuntu -or (Test-Path $pending); "
           L"Write-Output 'Starting Ubuntu initialization'; "
           L"wsl.exe --distribution Ubuntu --user root --exec "
           L"/usr/bin/true; if ($LASTEXITCODE -ne 0) { "
           L"Write-Error 'Ubuntu is installed but cannot start. "
           L"Restart Windows, then run WSL2 setup again.'; exit 3010 "
           L"}; if ($managedBootstrap) { "
           L"Write-Output 'Provisioning the Design++ Ubuntu user'; "
           L"wsl.exe --distribution Ubuntu --user root --exec "
           L"/bin/sh -c \"id -u designpp >/dev/null 2>&1 || "
           L"useradd --create-home --shell /bin/bash designpp; "
           L"printf '\\n[user]\\ndefault=designpp\\n' >> "
           L"/etc/wsl.conf\"; "
           L"if ($LASTEXITCODE -ne 0) { "
           L"FailSetup 'create designpp user' $LASTEXITCODE }; "
           L"wsl.exe --terminate Ubuntu; "
           L"if ($LASTEXITCODE -ne 0) { "
           L"Write-Warning 'Ubuntu did not stop cleanly; its default user "
           L"will apply after the next Windows restart.' } } else { "
           L"Write-Output 'Converting the existing Ubuntu distribution to "
           L"WSL2'; wsl.exe --set-version Ubuntu 2; "
           L"if ($LASTEXITCODE -ne 0) { "
           L"FailSetup 'convert Ubuntu to WSL2' $LASTEXITCODE } }; "
           L"Write-Output 'Selecting Ubuntu as the default distribution'; "
           L"wsl.exe --set-default Ubuntu; "
           L"if ($LASTEXITCODE -ne 0) { "
           L"FailSetup 'select Ubuntu' $LASTEXITCODE }; "
           L"if ($managedBootstrap) { Remove-Item -Force $pending }; exit 0",
           true),
       OutputEncoding::kUtf16LittleEndian, false, true, 3010});

  WslCommand probe;
  probe.distribution = L"Ubuntu";
  probe.program = L"/bin/sh";
  probe.arguments = {
      L"-c",
      L"case \"$(uname -r)\" in *WSL2*|*wsl2*) "
      L"printf '%s\\n' DESIGNPP_WSL2_READY ;; *) exit 78 ;; esac"};
  SetupStep verification{L"WSL2 Linux 실행 확인",
                         WslExecutor::BuildRequest(probe),
                         OutputEncoding::kUtf8, false};
  verification.required_output_marker = "DESIGNPP_WSL2_READY";
  steps.push_back(std::move(verification));
  return steps;
}

std::vector<SetupStep> BuildExistingWslSetupSteps(std::wstring distribution) {
  if (distribution.empty()) return {};
  ProcessRequest update;
  update.executable = L"wsl.exe";
  update.arguments = {L"--update"};
  WslCommand probe;
  probe.distribution = distribution;
  probe.program = L"/bin/sh";
  probe.arguments = {
      L"-c",
      L"case \"$(uname -r)\" in *WSL2*|*wsl2*) "
      L"printf '%s\\n' DESIGNPP_WSL2_READY ;; *) exit 78 ;; esac"};
  SetupStep verification{L"선택한 WSL2 배포판 실행 확인",
                         WslExecutor::BuildRequest(probe),
                         OutputEncoding::kUtf8, false};
  verification.required_output_marker = "DESIGNPP_WSL2_READY";
  ProcessRequest select;
  select.executable = L"wsl.exe";
  select.arguments = {L"--set-default", std::move(distribution)};
  return {{L"WSL 업데이트", std::move(update),
           OutputEncoding::kUtf16LittleEndian, false, true},
          std::move(verification),
          {L"선택한 WSL 배포판을 기본값으로 설정", std::move(select),
           OutputEncoding::kUtf16LittleEndian, false}};
}

std::vector<SetupStep> BuildDedicatedUbuntuSetupSteps() {
  std::vector<SetupStep> steps;
  steps.push_back(
      {L"Design++ 전용 Ubuntu 관리자 설정",
       MakePowerShellRequest(
           L"function FailSetup($stage, $code) { "
           L"Write-Host ('Design++ Ubuntu setup failed at: ' + $stage + "
           L"'; exit code: ' + $code) -ForegroundColor Red; "
           L"Read-Host 'Press Enter after recording the error'; "
           L"exit $code }; "
           L"$target='DesignPlusPlus'; "
           L"$stateDir=Join-Path $env:LOCALAPPDATA 'Design++'; "
           L"$pending=Join-Path $stateDir 'dedicated-ubuntu.pending'; "
           L"$installed=@(wsl.exe --list --quiet 2>$null); "
           L"if (($installed -contains $target) -and "
           L"!(Test-Path $pending)) { "
           L"FailSetup 'distribution already exists' 73 }; "
           L"wsl.exe --update; if ($LASTEXITCODE -ne 0) { "
           L"FailSetup 'update WSL' $LASTEXITCODE }; "
           L"wsl.exe --set-default-version 2; "
           L"if ($LASTEXITCODE -ne 0) { "
           L"FailSetup 'set-default-version' $LASTEXITCODE }; "
           L"if ($installed -notcontains $target) { "
           L"wsl.exe --install --distribution Ubuntu --name $target "
           L"--no-launch; if ($LASTEXITCODE -ne 0) { "
           L"FailSetup 'install named Ubuntu' $LASTEXITCODE }; "
           L"New-Item -ItemType Directory -Force -Path $stateDir | "
           L"Out-Null; New-Item -ItemType File -Force -Path $pending | "
           L"Out-Null }; "
           L"wsl.exe --distribution $target --user root --exec "
           L"/usr/bin/true; if ($LASTEXITCODE -ne 0) { exit 3010 }; "
           L"wsl.exe --distribution $target --user root --exec "
           L"/bin/sh -c \"id -u designpp >/dev/null 2>&1 || "
           L"useradd --create-home --shell /bin/bash designpp; "
           L"printf '\\n[user]\\ndefault=designpp\\n' >> "
           L"/etc/wsl.conf\"; if ($LASTEXITCODE -ne 0) { "
           L"FailSetup 'create designpp user' $LASTEXITCODE }; "
           L"wsl.exe --terminate $target; "
           L"if ($LASTEXITCODE -ne 0) { "
           L"FailSetup 'terminate Ubuntu' $LASTEXITCODE }; "
           L"Remove-Item -Force $pending; exit 0",
           true),
       OutputEncoding::kUtf16LittleEndian, false, true, 3010});

  WslCommand probe;
  probe.distribution = L"DesignPlusPlus";
  probe.program = L"/bin/sh";
  probe.arguments = {
      L"-c",
      L"case \"$(uname -r)\" in *WSL2*|*wsl2*) "
      L"printf '%s\\n' DESIGNPP_WSL2_READY ;; *) exit 78 ;; esac"};
  SetupStep verification{L"Design++ 전용 Ubuntu 실행 확인",
                         WslExecutor::BuildRequest(probe),
                         OutputEncoding::kUtf8, false};
  verification.required_output_marker = "DESIGNPP_WSL2_READY";
  steps.push_back(std::move(verification));

  ProcessRequest select;
  select.executable = L"wsl.exe";
  select.arguments = {L"--set-default", L"DesignPlusPlus"};
  steps.push_back({L"Design++ 전용 Ubuntu를 기본값으로 설정", std::move(select),
                   OutputEncoding::kUtf16LittleEndian, false});
  return steps;
}

std::vector<SetupStep> BuildCompleteToolSetupSteps(
    OrfsBuildPolicy orfs_build_policy) {
  const auto& openlane = *core::ToolchainCompatibilityCatalog::Find(
      "openlane2", "openlane2-2.3.10");
  const auto& orfs =
      *core::ToolchainCompatibilityCatalog::Find("orfs", "orfs-26Q2");
  std::vector<SetupStep> steps;
  steps.push_back({L"APT 패키지 인덱스 업데이트",
                   MakeRootWslRequest(L"/usr/bin/apt-get", {L"update"}),
                   OutputEncoding::kUtf8, false});
  steps.push_back(
      {L"기본 EDA 패키지 설치",
       MakeRootWslRequest(
           L"/usr/bin/apt-get",
           {L"install", L"-y", L"verilator", L"iverilog", L"yosys", L"gtkwave",
            L"magic", L"netgen-lvs", L"klayout", L"python3", L"python3-pip",
            L"python3-venv", L"python3-dev", L"curl", L"git",
            L"ca-certificates", L"xz-utils", L"make"}),
       OutputEncoding::kUtf8, false});

  WslCommand python_environment;
  python_environment.program = L"/bin/bash";
  python_environment.arguments = {
      L"-lc",
      L"python3 -m venv \"$HOME/.designpp/venv\" && "
      L"\"$HOME/.designpp/venv/bin/python\" -m pip install --upgrade pip "
      L"'cocotb>=1.9,<2'"};
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
  // Initialize Nix explicitly; login logout hooks must not replace our status.
  openlane_install.arguments = {
      L"-c",
      L"set -eu; . /nix/var/nix/profiles/default/etc/profile.d/nix-daemon.sh "
      L"2>/dev/null || true; toolchains=\"$HOME/.designpp/toolchains\"; "
      L"active=\"$toolchains/environments/" +
          CatalogText(openlane.bundle_id) +
          L"\"; mkdir -p \"$toolchains/environments\"; "
          L"exec 9>\"$toolchains/.openlane-install.lock\"; "
          L"flock -n 9 || { echo 'Another OpenLane install is running.' >&2; "
          L"exit 73; }; " +
          std::wstring(kCandidateCleanupScript) +
          L"cleanup_candidates openlane openlane2 "
          L"https://github.com/efabless/openlane2.git || "
          L"echo 'Some abandoned OpenLane candidates could not be "
          L"removed.' >&2; test ! -e \"$active\" || { "
          L"echo 'Environment already exists; recheck it instead of replacing "
          L"it.'; exit 0; }; "
          L"candidate=''; "
          L"cleanup() { status=$?; trap - EXIT; "
          L"if [ -n \"$candidate\" ] && [ -d \"$candidate\" ] && "
          L"[ ! -L \"$candidate\" ]; then "
          L"case \"$candidate\" in \"$toolchains\"/.openlane-candidate.*) "
          L"if rm -rf -- \"$candidate\"; then "
          L"printf 'Removed OpenLane candidate: %s\\n' \"$candidate\"; "
          L"else printf 'Could not remove OpenLane candidate: %s\\n' "
          L"\"$candidate\" >&2; fi;; esac; fi; return $status; }; "
          L"trap cleanup EXIT; trap 'exit 143' TERM; "
          L"trap 'exit 130' INT; trap 'exit 129' HUP; "
          L"candidate=$(mktemp -d \"$toolchains/.openlane-candidate.XXXXXX\"); "
          L"printf '%s\\n' 'owner=designpp' 'provider=openlane2' "
          L">\"$candidate/.designpp-candidate\"; "
          L"git -C \"$candidate\" init -q; "
          L"git -C \"$candidate\" remote add origin "
          L"https://github.com/efabless/openlane2.git; "
          L"git -C \"$candidate\" fetch --depth 1 origin " +
          CatalogText(openlane.revision) +
          L"; "
          L"git -C \"$candidate\" checkout --detach FETCH_HEAD; "
          L"test \"$(git -C \"$candidate\" rev-parse HEAD)\" = " +
          CatalogText(openlane.revision) +
          L"; "
          L"nix-shell \"$candidate/shell.nix\" --run 'openlane --smoke-test'; "
          L"printf '%s\\n' 'provider=openlane2' 'version=" +
          CatalogText(openlane.version) +
          L"' "
          L"'commit=" +
          CatalogText(openlane.revision) +
          L"' "
          L">\"$candidate/.designpp-environment\"; "
          L"rm -- \"$candidate/.designpp-candidate\"; "
          L"mv -- \"$candidate\" \"$active\"; candidate=''; "
          L"trap - EXIT; "
          L"echo 'OpenLane environment prepared; activation is a separate "
          L"action.'"};
  steps.push_back({L"OpenLane 2와 managed EDA toolchain 설치",
                   WslExecutor::BuildRequest(openlane_install),
                   OutputEncoding::kUtf8, false});

  const bool allow_local_build =
      orfs_build_policy == OrfsBuildPolicy::kAllowLocalBuild;
  const std::wstring build_budget_script =
      allow_local_build
          ? L"logical_cpus=$(nproc 2>/dev/null || printf '1'); "
            L"case \"$logical_cpus\" in ''|*[!0-9]*) logical_cpus=1;; "
            L"esac; build_cores=1; "
            L"if [ \"$logical_cpus\" -gt 1 ]; then "
            L"build_cores=$((logical_cpus - 1)); fi; "
          : L"";
  const std::wstring build_options =
      allow_local_build
          ? L"--max-jobs 1 --cores \"$build_cores\" --builders '' "
            L"--option fallback false "
          : L"--max-jobs 0 --builders '' --option fallback false ";
  const std::wstring failure_guidance =
      allow_local_build
          ? L"ORFS preparation failed. Inspect the Nix build log. Active "
            L"environment unchanged."
          : L"ORFS preparation failed. Source builds are disabled; inspect "
            L"the Nix log for missing cache entries or validation errors. "
            L"A source build requires separate approval. Active environment "
            L"unchanged.";
  WslCommand orfs_install;
  orfs_install.program = L"/bin/bash";
  orfs_install.arguments = {
      L"-c",
      L"set -eu; . /nix/var/nix/profiles/default/etc/profile.d/"
      L"nix-daemon.sh 2>/dev/null || true; "
      L"toolchains=\"$HOME/.designpp/toolchains\"; "
      L"active=\"$toolchains/environments/" +
          CatalogText(orfs.bundle_id) +
          L"\"; mkdir -p \"$toolchains/environments\"; "
          L"command -v flock >/dev/null 2>&1 || { "
          L"echo 'ORFS install requires flock.' >&2; exit 69; }; "
          L"exec 9>\"$toolchains/.orfs-install.lock\"; "
          L"flock -n 9 || { echo 'Another ORFS install is running.' >&2; "
          L"exit 73; }; " +
          std::wstring(kCandidateCleanupScript) +
          L"cleanup_candidates orfs orfs "
          L"https://github.com/The-OpenROAD-Project/"
          L"OpenROAD-flow-scripts || "
          L"echo 'Some abandoned ORFS candidates could not be removed.' "
          L">&2; "
          L"test ! -e \"$active\" || { "
          L"echo 'Environment already exists; recheck it instead of replacing "
          L"it.'; exit 0; }; "
          L"candidate=''; "
          L"cleanup() { status=$?; trap - EXIT; "
          L"if [ -n \"$candidate\" ] && [ -d \"$candidate\" ] && "
          L"[ ! -L \"$candidate\" ]; then "
          L"case \"$candidate\" in \"$toolchains\"/.orfs-candidate.*) "
          L"if rm -rf -- \"$candidate\"; then "
          L"printf 'Removed ORFS candidate: %s\\n' \"$candidate\"; "
          L"else printf 'Could not remove ORFS candidate: %s\\n' "
          L"\"$candidate\" >&2; fi;; esac; fi; return $status; }; "
          L"trap cleanup EXIT; trap 'exit 143' TERM; "
          L"trap 'exit 130' INT; trap 'exit 129' HUP; "
          L"candidate=$(mktemp -d \"$toolchains/.orfs-candidate.XXXXXX\"); "
          L"printf '%s\\n' 'owner=designpp' 'provider=orfs' "
          L">\"$candidate/.designpp-candidate\"; "
          L"git -C \"$candidate\" init -q; "
          L"git -C \"$candidate\" remote add origin "
          L"https://github.com/The-OpenROAD-Project/OpenROAD-flow-scripts; "
          L"git -C \"$candidate\" -c fetch.recurseSubmodules=false fetch "
          L"--depth 1 origin " +
          CatalogText(orfs.revision) +
          L"; "
          L"git -C \"$candidate\" checkout --detach FETCH_HEAD; "
          L"test \"$(git -C \"$candidate\" rev-parse HEAD)\" = " +
          CatalogText(orfs.revision) +
          L"; "
          L"test -f \"$candidate/flow/Makefile\"; "
          L"test -f \"$candidate/flake.nix\"; "
          L"git -C \"$candidate\" -c fetch.recurseSubmodules=false "
          L"submodule update --init --recursive --depth 1 -- tools/yosys "
          L"tools/OpenROAD; "
          L"test \"$(git -C \"$candidate/tools/OpenROAD\" rev-parse HEAD)\" "
          L"= " +
          CatalogText(orfs.dependencies[0].revision) +
          L"; "
          L"test \"$(git -C \"$candidate/tools/yosys\" rev-parse HEAD)\" = " +
          CatalogText(orfs.dependencies[1].revision) +
          L"; "
          L"test \"$(git -C \"$candidate/tools/yosys/abc\" rev-parse HEAD)\" "
          L"= " +
          CatalogText(orfs.dependencies[2].revision) +
          L"; "
          L"mkdir -p \"$candidate/tools/eqy\"; "
          L"git -C \"$candidate/tools/eqy\" init -q; "
          L"git -C \"$candidate/tools/eqy\" remote add origin "
          L"https://github.com/YosysHQ/eqy.git; "
          L"git -C \"$candidate/tools/eqy\" fetch --depth 1 origin " +
          CatalogText(orfs.dependencies[3].revision) +
          L"; "
          L"git -C \"$candidate/tools/eqy\" checkout --detach FETCH_HEAD; "
          L"test \"$(git -C \"$candidate/tools/eqy\" rev-parse HEAD)\" = " +
          CatalogText(orfs.dependencies[3].revision) +
          L"; "
          L"printf '%s' \"$1\" | git -C \"$candidate/tools/yosys\" "
          L"apply --unidiff-zero -; "
          L"printf '%s' \"$2\" | git -C \"$candidate/tools/yosys\" "
          L"apply --unidiff-zero -; "
          L"printf '%s' \"$3\" | git -C \"$candidate\" "
          L"apply --unidiff-zero -; " +
          build_budget_script +
          L"nix --extra-experimental-features 'nix-command flakes' develop "
          L"\"$candidate\" --no-write-lock-file --override-input yosys "
          L"\"path:$candidate/tools/yosys\" "
          L"--override-input openroad "
          L"\"path:$candidate/tools/OpenROAD\" "
          L"--override-input eqy-src "
          L"\"path:$candidate/tools/eqy\" " +
          build_options +
          L"--command /bin/bash -c "
          L"'yosys -p \"help read_liberty\" 2>/dev/null | "
          L"grep -Fq -- -unit_delay && "
          L"yosys -p \"help stat\" 2>/dev/null | grep -Fq -- -hierarchy && "
          L"yosys-abc -c \"read_lib -h\" 2>&1 | grep -q -- -m && "
          L"printf \"%s\\n\" \"help repair_timing\" \"exit\" | "
          L"openroad -no_init -exit /dev/stdin 2>&1 | grep -Fq -- -sequence && "
          L"openroad -version && eqy --version' || { "
          L"echo '" +
          failure_guidance +
          L"' "
          L">&2; exit 78; }; "
          L"printf '%s\\n' 'schema_version=2' 'input_mode=path' "
          L"'provider=orfs' 'version=" +
          CatalogText(orfs.version) +
          L"' "
          L"'commit=" +
          CatalogText(orfs.revision) +
          L"' "
          L">\"$candidate/.designpp-environment\"; "
          L"rm -f -- \"$candidate/.designpp-candidate\"; "
          L"mv -- \"$candidate\" \"$active\"; candidate=''; "
          L"trap - EXIT; "
          L"echo 'ORFS environment prepared; activation is a separate action.'",
      L"designpp-orfs-install",
      kOrfsYosysRecipePatch,
      kOrfsAbcTestRecipePatch,
      kOrfsEqyRecipePatch};
  steps.push_back({L"OpenROAD Flow Scripts 설치",
                   WslExecutor::BuildRequest(orfs_install),
                   OutputEncoding::kUtf8, false});
  steps.push_back(WebView2InstallStep());
  steps.push_back({L"ASAP7 CDL models", BuildAsap7ModelRequest(true),
                   OutputEncoding::kUtf8, false});
  return steps;
}

std::vector<SetupStep> BuildBuildCacheCleanupSteps() {
  WslCommand cleanup;
  cleanup.program = L"/bin/bash";
  cleanup.arguments = {
      L"-c",
      L"set -eu; toolchains=\"$HOME/.designpp/toolchains\"; "
      L"if [ ! -d \"$toolchains\" ]; then "
      L"echo 'No Design++ build candidates to remove.'; exit 0; fi; "
      L"command -v flock >/dev/null 2>&1 || { "
      L"echo 'Cache cleanup requires flock.' >&2; exit 69; }; "
      L"exec 8>\"$toolchains/.openlane-install.lock\"; "
      L"flock -n 8 || { "
      L"echo 'OpenLane installation is running; cleanup skipped.' >&2; "
      L"exit 73; }; "
      L"exec 9>\"$toolchains/.orfs-install.lock\"; "
      L"flock -n 9 || { "
      L"echo 'ORFS installation is running; cleanup skipped.' >&2; "
      L"exit 73; }; " +
          std::wstring(kCandidateCleanupScript) +
          L"failed=0; cleanup_candidates openlane openlane2 "
          L"https://github.com/efabless/openlane2.git || failed=1; "
          L"cleanup_candidates orfs orfs "
          L"https://github.com/The-OpenROAD-Project/"
          L"OpenROAD-flow-scripts || failed=1; "
          L"if [ \"$failed\" -eq 0 ]; then "
          L"echo 'Design++ abandoned build candidates cleaned.'; fi; "
          L"exit \"$failed\""};
  return {{L"Design++ 빌드 캐시 정리", WslExecutor::BuildRequest(cleanup),
           OutputEncoding::kUtf8, false}};
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

ProcessRequest BuildAsap7ModelRequest(bool prepare) {
  WslCommand command;
  command.program = L"/bin/bash";
  command.arguments = {L"-c",
                       LR"script(set -eu
revision=472f7b3f6680ffb1109d6792219e6ea5570c26a0
root="$HOME/.designpp/models/asap7sc7p5t_28"
destination="$root/$revision"
verify() ( cd "$1" && printf '%s\n' "$2" | sha256sum --check --status )
if [ "$1" = probe ]; then
  test -d "$destination" || { echo 'ASAP7 CDL models are not installed'; exit 44; }
  verify "$destination" "$2" || { echo 'ASAP7 CDL model hash mismatch'; exit 78; }
  echo 'ASAP7 CDL 28.2022'; exit 0
fi
mkdir -p "$root"
exec 9>"$root/.prepare.lock"
flock -x 9
if [ -e "$destination" ]; then
  verify "$destination" "$2" || { echo 'Existing model package is damaged; retained for diagnosis' >&2; exit 78; }
  echo 'ASAP7 CDL models already prepared'; exit 0
fi
candidate=$(mktemp -d "$root/.candidate.XXXXXXXX")
echo "Preparing ASAP7 CDL models: $candidate"
base="https://raw.githubusercontent.com/The-OpenROAD-Project/asap7sc7p5t_28/$revision"
for variant in R L SL SRAM; do
  file="asap7sc7p5t_28_$variant.cdl"
  curl --fail --location --proto '=https' --tlsv1.2 --connect-timeout 30 --max-time 180 "$base/CDL/LVS/$file" -o "$candidate/$file"
done
curl --fail --location --proto '=https' --tlsv1.2 --connect-timeout 30 --max-time 180 "$base/LICENSE" -o "$candidate/LICENSE"
verify "$candidate" "$2"
printf '%s\n' "$2" > "$candidate/SHA256SUMS"
printf 'schema_version=1\nrepository=https://github.com/The-OpenROAD-Project/asap7sc7p5t_28\ncommit=%s\nkind=cell-cdl\nlvs_recipe_validated=false\n' "$revision" > "$candidate/manifest.txt"
mv -T -- "$candidate" "$destination"
echo "ASAP7 CDL models prepared: $destination (LVS recipe not yet validated)"
)script",
                       L"designpp-asap7-models",
                       prepare ? L"prepare" : L"probe",
                       L"444a08c16f5a44b4ec6f64e5ad4112d0dd8ca5a3fe5da7a7644e63"
                       L"f538bf681e  asap7sc7p5t_28_R.cdl\n"
                       L"b600c4979ae42e0ec6b40a456f14a158229ec1f15da2585d1f6f93"
                       L"8190d13005  asap7sc7p5t_28_L.cdl\n"
                       L"d88ba74a3944e2708a26cf26a3c4d9ddd6babfc6e6f15b76f801ae"
                       L"936cb05b73  asap7sc7p5t_28_SL.cdl\n"
                       L"9a212b1a6084a0d8ad2b8b2d5a0a0cd0216bdfec7f5175e1e4f710"
                       L"17b758189c  asap7sc7p5t_28_SRAM.cdl\n"
                       L"6f6244fdd72b8650af453f5962d01dce947e6fcd74a391a40e2ef8"
                       L"63069a9a59  LICENSE"};
  return WslExecutor::BuildRequest(command);
}

std::vector<SetupStep> BuildToolInstallSteps(
    ToolId tool_id, OrfsBuildPolicy orfs_build_policy) {
  if (tool_id == ToolId::kAsap7Models) {
    return {{L"ASAP7 CDL models", BuildAsap7ModelRequest(true),
             OutputEncoding::kUtf8, false}};
  }
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
        L"pip 'cocotb>=1.9,<2'"};
    return {{L"cocotb 설치 / 업데이트", WslExecutor::BuildRequest(command),
             OutputEncoding::kUtf8, false}};
  }

  std::vector<SetupStep> complete =
      BuildCompleteToolSetupSteps(orfs_build_policy);
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
    std::vector<SetupStep> steps;
    steps.push_back(std::move(complete[3]));
    steps.push_back(std::move(complete[5]));
    steps.push_back({L"ASAP7 CDL models", BuildAsap7ModelRequest(true),
                     OutputEncoding::kUtf8, false});
    return steps;
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
    // Runs may still use this environment from another window or process.
    return {};
  }
  if (tool_id == ToolId::kOrfs) {
    return {};
  }
  return {};
}

}  // namespace designpp::runtime

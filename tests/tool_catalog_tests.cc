// Copyright 2026 The Design++ Authors

#include <CppUnitTest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "designpp/runtime/setup_catalog.h"
#include "designpp/runtime/tool_catalog.h"

using Microsoft::VisualStudio::CppUnitTestFramework::Assert;

namespace designpp::tests {
namespace {

std::wstring JoinArguments(const std::vector<std::wstring>& arguments) {
  std::wstring result;
  for (const std::wstring& argument : arguments) {
    result += argument;
    result.push_back(L' ');
  }
  return result;
}

}  // namespace

// clang-format cannot parse the CppUnitTest class and method declaration
// macros.
// clang-format off
TEST_CLASS(ToolCatalogTests) {
 public:
  TEST_METHOD(WslSetupInitializesAndSelectsUbuntuBeforeExplicitProbe) {
    const std::vector<runtime::SetupStep> setup =
        runtime::BuildWslSetupSteps();
    Assert::AreEqual<std::size_t>(2U, setup.size());
    const std::wstring configure = JoinArguments(setup[0].request.arguments);
    Assert::IsTrue(configure.find(L"-NonInteractive") ==
                   std::wstring::npos);
    Assert::IsTrue(configure.find(L"FailSetup 'set-default-version'") !=
                   std::wstring::npos);
    Assert::IsTrue(configure.find(L"Press Enter after recording the error") !=
                   std::wstring::npos);
    Assert::IsTrue(configure.find(L"--no-launch") != std::wstring::npos);
    Assert::IsTrue(configure.find(L"wsl.exe --update") != std::wstring::npos);
    Assert::IsTrue(setup[1].required_output_marker ==
                   "DESIGNPP_WSL2_READY");
    Assert::IsTrue(configure.find(
                       L"--distribution Ubuntu --user root --exec "
                       L"/usr/bin/true") != std::wstring::npos);
    Assert::IsTrue(configure.find(L"default=designpp") !=
                   std::wstring::npos);
    Assert::IsTrue(configure.find(L"--set-version Ubuntu 2") !=
                   std::wstring::npos);
    Assert::IsTrue(configure.find(L"--set-default Ubuntu") !=
                   std::wstring::npos);
    Assert::IsTrue(configure.find(L"Restart Windows") !=
                   std::wstring::npos);
    Assert::IsTrue(configure.find(L"ubuntu-bootstrap.pending") !=
                   std::wstring::npos);
    Assert::IsTrue(configure.find(L"$managedBootstrap") !=
                   std::wstring::npos);
    const std::size_t default_version =
        configure.find(L"--set-default-version 2");
    const std::size_t install = configure.find(L"--install --distribution");
    Assert::IsTrue(install < default_version);
    Assert::IsTrue(configure.find(L"if ($managedBootstrap)") !=
                   std::wstring::npos);
    Assert::IsTrue(configure.find(L"did not stop cleanly") !=
                   std::wstring::npos);
    Assert::IsTrue(setup[0].restart_required_exit_code.has_value());
    Assert::AreEqual<std::uint32_t>(
        3010, *setup[0].restart_required_exit_code);

    const std::wstring probe = JoinArguments(setup[1].request.arguments);
    Assert::IsTrue(probe.find(L"--distribution Ubuntu") !=
                   std::wstring::npos);
    Assert::IsTrue(probe.find(L"/bin/sh") != std::wstring::npos);
    Assert::IsTrue(probe.find(L"uname -r") != std::wstring::npos);
  }

  TEST_METHOD(ExistingWslSetupUsesOnlyChosenDistribution) {
    const auto steps = runtime::BuildExistingWslSetupSteps(L"Existing Ubuntu");
    Assert::AreEqual<std::size_t>(3U, steps.size());
    Assert::IsTrue(JoinArguments(steps[0].request.arguments).find(L"--update") !=
                   std::wstring::npos);
    Assert::IsTrue(JoinArguments(steps[1].request.arguments)
                       .find(L"--distribution Existing Ubuntu") !=
                   std::wstring::npos);
    Assert::IsTrue(steps[1].required_output_marker ==
                   "DESIGNPP_WSL2_READY");
    Assert::IsTrue(JoinArguments(steps[2].request.arguments)
                       .find(L"--set-default Existing Ubuntu") !=
                   std::wstring::npos);
    for (const auto& step : steps) {
      Assert::IsTrue(JoinArguments(step.request.arguments).find(L"--install") ==
                     std::wstring::npos);
      Assert::IsTrue(JoinArguments(step.request.arguments).find(L"--set-version") ==
                     std::wstring::npos);
    }
  }

  TEST_METHOD(DedicatedUbuntuSetupKeepsASeparateDistribution) {
    const auto steps = runtime::BuildDedicatedUbuntuSetupSteps();
    Assert::AreEqual<std::size_t>(3U, steps.size());
    const std::wstring setup = JoinArguments(steps[0].request.arguments);
    Assert::IsTrue(setup.find(L"--install --distribution Ubuntu --name "
                              L"$target --no-launch") != std::wstring::npos);
    Assert::IsTrue(setup.find(L"$target='DesignPlusPlus'") !=
                   std::wstring::npos);
    Assert::IsTrue(JoinArguments(steps[1].request.arguments)
                       .find(L"--distribution DesignPlusPlus") !=
                   std::wstring::npos);
    Assert::IsTrue(steps[1].required_output_marker ==
                   "DESIGNPP_WSL2_READY");
    Assert::IsTrue(JoinArguments(steps[2].request.arguments)
                       .find(L"--set-default DesignPlusPlus") !=
                   std::wstring::npos);
  }

  TEST_METHOD(ManagedInstallStatusDoesNotDependOnLoginLogoutHooks) {
    for (const auto tool : {runtime::ToolId::kOpenLane2,
                            runtime::ToolId::kOrfs}) {
      bool found = false;
      for (const auto& step : runtime::BuildToolInstallSteps(tool)) {
        const auto& args = step.request.arguments;
        const auto bash = std::find(args.begin(), args.end(), L"/bin/bash");
        if (JoinArguments(args).find(L"Environment already exists") ==
            std::wstring::npos) continue;
        Assert::IsTrue(bash != args.end() && bash + 1 != args.end());
        Assert::AreEqual(std::wstring(L"-c"), *(bash + 1));
        Assert::IsTrue(JoinArguments(args).find(L"nix-daemon.sh") !=
                       std::wstring::npos);
        found = true;
      }
      Assert::IsTrue(found);
    }
  }

  TEST_METHOD(OrfsInventoryAcceptsLegacyAndVersionedInputModes) {
    for (const auto& tool : runtime::BuildToolCatalog()) {
      if (tool.id != runtime::ToolId::kOrfs) continue;
      const std::wstring command = JoinArguments(tool.probe_request.arguments);
      Assert::IsTrue(command.find(L"schema_version=") !=
                     std::wstring::npos);
      Assert::IsTrue(command.find(L"input_mode=") != std::wstring::npos);
      Assert::IsTrue(command.find(L":|1:|2:path") != std::wstring::npos);
      return;
    }
    Assert::Fail(L"ORFS inventory is missing");
  }

  TEST_METHOD(CompleteSetupInstallsPythonHeadersForCocotbSourceBuilds) {
    bool found = false;
    for (const auto& step : runtime::BuildCompleteToolSetupSteps()) {
      const std::wstring command = JoinArguments(step.request.arguments);
      if (command.find(L"python3-venv") == std::wstring::npos) continue;
      Assert::IsTrue(command.find(L"python3-dev") != std::wstring::npos);
      found = true;
    }
    Assert::IsTrue(found);
  }

  TEST_METHOD(CalibreIsNotListedOrProbed) {
    const auto tools = runtime::BuildToolCatalog();
    for (const auto& tool : tools) {
      Assert::IsTrue(tool.display_name.find(L"Calibre") == std::wstring::npos);
      Assert::IsTrue(JoinArguments(tool.probe_request.arguments).find(L"calibre") == std::wstring::npos);
    }
  }
  TEST_METHOD(VersionParserSkipsWarningsAndRejectsProgress) {
    Assert::AreEqual(std::wstring(L"0.68+post"), runtime::ParseToolVersion(
        runtime::ToolId::kYosys, L"warning: not writing lock file\nYosys 0.68+post (git sha1 abc)\n"));
    Assert::IsTrue(runtime::ParseToolVersion(runtime::ToolId::kOpenRoad,
        L"GITDIR-NOTFOUND\ncopying path '/nix/store/package-1.2'\n").empty());
    Assert::IsTrue(runtime::ParseToolVersion(runtime::ToolId::kYosys,
        std::wstring(65537, L'x')).empty());
    Assert::AreEqual(std::wstring(L"2.34.8"), runtime::ParseToolVersion(
        runtime::ToolId::kNix,
        L"nix (Determinate Nix 3.21.9) 2.34.8\n"));
    Assert::AreEqual(std::wstring(L"26Q2"), runtime::ParseToolVersion(
        runtime::ToolId::kOrfs,
        L"DESIGNPP_MANAGED_VERSION=26Q2\n"));
  }

  TEST_METHOD(InventoryNeverRealizesNixOrUpdatesRepositories) {
    std::size_t openroad_rows = 0;
    for (const auto& tool : runtime::BuildToolCatalog()) {
      const auto command = JoinArguments(tool.probe_request.arguments);
      Assert::IsTrue(command.find(L"nix-shell") == std::wstring::npos);
      Assert::IsTrue(command.find(L"nix develop") == std::wstring::npos);
      Assert::IsTrue(command.find(L"git pull") == std::wstring::npos);
      if (tool.id == runtime::ToolId::kOpenRoad) {
        ++openroad_rows;
        Assert::IsTrue(command.find(L"DESIGNPP_ENVIRONMENT_MISSING") != std::wstring::npos);
        Assert::IsTrue(command.find(L".designpp-environment") !=
                       std::wstring::npos);
        Assert::IsTrue(command.find(L"toolchains/environments/") !=
                       std::wstring::npos);
        Assert::IsTrue(command.find(
                           L"0e2d771c5ec38f232493c2afea738ea0200cb972") ==
                       std::wstring::npos);
      }
    }
    Assert::AreEqual(std::size_t(2), openroad_rows);
  }
  TEST_METHOD(WebView2UsesWindowsProbeAndElevatedEvergreenInstaller) {
    const std::vector<runtime::ToolDefinition> tools =
        runtime::BuildToolCatalog();
    const auto webview = std::find_if(
        tools.begin(), tools.end(), [](const runtime::ToolDefinition& tool) {
          return tool.id == runtime::ToolId::kWebView2;
        });
    Assert::IsTrue(webview != tools.end());
    Assert::AreEqual(std::wstring(L"powershell.exe"),
                     webview->probe_request.executable.wstring());
    Assert::IsTrue(webview->install_method ==
                   runtime::InstallMethod::kWindowsRuntime);

    const std::vector<runtime::SetupStep> install =
        runtime::BuildToolInstallSteps(runtime::ToolId::kWebView2);
    Assert::AreEqual(static_cast<std::size_t>(1), install.size());
    Assert::IsTrue(install[0].requires_elevation);
    const std::wstring command = JoinArguments(install[0].request.arguments);
    Assert::IsTrue(command.find(L"LinkId=2124703") != std::wstring::npos);
    Assert::IsTrue(command.find(L"/silent") != std::wstring::npos);
    Assert::IsTrue(command.find(L"/install") != std::wstring::npos);
  }

  TEST_METHOD(WebView2RemovalIsProtectedBecauseRuntimeIsShared) {
    const std::vector<runtime::SetupStep> removal =
        runtime::BuildToolRemoveSteps(runtime::ToolId::kWebView2);
    Assert::IsTrue(removal.empty());
  }

  TEST_METHOD(OrfsInstallPreparesCheckoutCompatibleFlakeTools) {
    const std::vector<runtime::SetupStep> install =
        runtime::BuildToolInstallSteps(runtime::ToolId::kOrfs);
    Assert::AreEqual(static_cast<std::size_t>(3), install.size());
    const std::wstring nix_command =
        JoinArguments(install[0].request.arguments);
    Assert::IsTrue(nix_command.find(L"install.determinate.systems") !=
                   std::wstring::npos);
    const std::wstring command = JoinArguments(install[1].request.arguments);
    Assert::IsTrue(command.find(L"nix-command flakes") != std::wstring::npos);
    Assert::IsTrue(command.find(L"toolchains/environments/orfs-26Q2") !=
                   std::wstring::npos);
    Assert::IsTrue(command.find(L"--branch") == std::wstring::npos);
    Assert::IsTrue(command.find(L"036d106273e66855cd5214d49518fd0f0df7de61") != std::wstring::npos);
    Assert::IsTrue(command.find(L"0e2d771c5ec38f232493c2afea738ea0200cb972") != std::wstring::npos);
    Assert::IsTrue(command.find(L"d3e297fcd479247322f83d14f42b3556db7acdfb") != std::wstring::npos);
    Assert::IsTrue(command.find(L"--max-jobs 0 --builders '' --option fallback false") != std::wstring::npos);
    Assert::IsTrue(command.find(L"rm -rf -- \"$candidate\"") !=
                   std::wstring::npos);
    Assert::IsTrue(command.find(L".designpp-candidate") !=
                   std::wstring::npos);
    Assert::IsTrue(command.find(L"cleanup_candidates orfs orfs") !=
                   std::wstring::npos);
    Assert::IsTrue(command.find(L"Source builds are disabled") != std::wstring::npos);
    Assert::IsTrue(command.find(L".orfs-candidate.XXXXXX") !=
                   std::wstring::npos);
    Assert::IsTrue(command.find(L"'schema_version=2' 'input_mode=path'") !=
                   std::wstring::npos);
    Assert::IsTrue(command.find(L".orfs-backup.XXXXXX") ==
                   std::wstring::npos);
    Assert::IsTrue(command.find(L"activation is a separate action") !=
                   std::wstring::npos);
    Assert::IsTrue(command.find(L".orfs-install.lock") !=
                   std::wstring::npos);
    Assert::IsTrue(command.find(L"-unit_delay") != std::wstring::npos);
    Assert::IsTrue(command.find(L"openroad -version") != std::wstring::npos);
    Assert::IsTrue(command.find(L"pull --ff-only") == std::wstring::npos);
    Assert::IsTrue(command.find(L"submodule update --init --recursive --depth 1 -- tools/yosys") != std::wstring::npos);
    Assert::IsTrue(command.find(L"--no-write-lock-file --override-input yosys") != std::wstring::npos);
    Assert::IsTrue(command.find(L"-hierarchy") != std::wstring::npos);
    Assert::IsTrue(command.find(L"read_lib -h") != std::wstring::npos);
    Assert::IsTrue(command.find(L"src = ./abc;") != std::wstring::npos);
    Assert::IsTrue(command.find(L"FETCHCONTENT_SOURCE_DIR_GOOGLETEST") != std::wstring::npos);
    Assert::IsTrue(command.find(L"FETCHCONTENT_FULLY_DISCONNECTED=ON") != std::wstring::npos);
    Assert::IsTrue(command.find(L"eff96db01293848b993651caa52d747f191be02e") != std::wstring::npos);
    Assert::IsTrue(command.find(L"override-input eqy-src") != std::wstring::npos);
    Assert::IsTrue(command.find(L"--override-input yosys "
                                L"\"path:$candidate/tools/yosys\"") !=
                   std::wstring::npos);
    Assert::IsTrue(command.find(L"--override-input openroad "
                                L"\"path:$candidate/tools/OpenROAD\"") !=
                   std::wstring::npos);
    Assert::IsTrue(command.find(L"--override-input eqy-src "
                                L"\"path:$candidate/tools/eqy\"") !=
                   std::wstring::npos);
    Assert::IsTrue(command.find(L"eqy --version") != std::wstring::npos);
    Assert::IsTrue(command.find(L"make -j$NIX_BUILD_CORES") != std::wstring::npos);
    Assert::IsTrue(command.find(L"ABCEXTERNAL=yosys-abc PREFIX=$out") !=
                   std::wstring::npos);
    Assert::IsTrue(command.find(L"pkgs.clang pkgs.gnumake") !=
                   std::wstring::npos);
    Assert::IsTrue(command.find(L"pkgs.libffi pkgs.readline pkgs.tcl pkgs.zlib") !=
                   std::wstring::npos);
    Assert::IsTrue(command.find(L"8e401543d3ecf65e3a3631c7a271793a4d356cb0") != std::wstring::npos);
    Assert::IsTrue(command.find(L"apply --unidiff-zero -") != std::wstring::npos);
    Assert::IsTrue(command.find(L"--override-input openroad") != std::wstring::npos);
    Assert::IsTrue(command.find(L"tools/yosys tools/OpenROAD") != std::wstring::npos);
    Assert::IsTrue(command.find(L"help repair_timing") != std::wstring::npos);
    Assert::IsTrue(command.find(L"grep -Fq -- -sequence") != std::wstring::npos);
    Assert::IsTrue(command.find(L"openlane2/shell.nix") ==
                   std::wstring::npos);
  }

  TEST_METHOD(BuildCacheCleanupProtectsEnvironmentsAndSharedNixStore) {
    const auto steps = runtime::BuildBuildCacheCleanupSteps();
    Assert::AreEqual(static_cast<std::size_t>(1), steps.size());
    const std::wstring command = JoinArguments(steps[0].request.arguments);
    Assert::IsTrue(command.find(L".openlane-install.lock") !=
                   std::wstring::npos);
    Assert::IsTrue(command.find(L".orfs-install.lock") !=
                   std::wstring::npos);
    Assert::IsTrue(command.find(L"flock -n 8") != std::wstring::npos);
    Assert::IsTrue(command.find(L"flock -n 9") != std::wstring::npos);
    Assert::IsTrue(command.find(L"[ ! -L \"$stale\" ]") !=
                   std::wstring::npos);
    Assert::IsTrue(command.find(L"[ ! -e \"$stale/.designpp-environment\" ]") !=
                   std::wstring::npos);
    Assert::IsTrue(command.find(L"owner=designpp") != std::wstring::npos);
    Assert::IsTrue(command.find(L"provider=$candidate_provider") !=
                   std::wstring::npos);
    Assert::IsTrue(command.find(L"cleanup_candidates openlane openlane2") !=
                   std::wstring::npos);
    Assert::IsTrue(command.find(L"cleanup_candidates orfs orfs") !=
                   std::wstring::npos);
    Assert::IsTrue(command.find(L"rm -rf -- \"$stale\"") !=
                   std::wstring::npos);
    Assert::IsTrue(command.find(L"nix store gc") == std::wstring::npos);
    Assert::IsTrue(command.find(L"/nix/store") == std::wstring::npos);
  }

  TEST_METHOD(OrfsSourceBuildRequiresExplicitPolicyAndLimitsCpuUse) {
    const auto install = runtime::BuildToolInstallSteps(
        runtime::ToolId::kOrfs,
        runtime::OrfsBuildPolicy::kAllowLocalBuild);
    Assert::AreEqual(std::size_t(3), install.size());
    const std::wstring command = JoinArguments(install[1].request.arguments);
    Assert::IsTrue(command.find(L"logical_cpus=$(nproc") !=
                   std::wstring::npos);
    Assert::IsTrue(command.find(L"build_cores=$((logical_cpus - 1))") !=
                   std::wstring::npos);
    Assert::IsTrue(command.find(
                       L"--max-jobs 1 --cores \"$build_cores\" --builders ''") !=
                   std::wstring::npos);
    Assert::IsTrue(command.find(L"--cores 2") == std::wstring::npos);
    Assert::IsTrue(command.find(L"--max-jobs 0") == std::wstring::npos);
    Assert::IsTrue(command.find(L"Source builds are disabled") ==
                   std::wstring::npos);
    Assert::IsTrue(command.find(L"Active environment unchanged") !=
                   std::wstring::npos);

    const auto full = runtime::BuildCompleteToolSetupSteps(
        runtime::OrfsBuildPolicy::kAllowLocalBuild);
    Assert::IsTrue(JoinArguments(full[5].request.arguments)
                       .find(L"--max-jobs 1 --cores \"$build_cores\"") !=
                   std::wstring::npos);
    const auto openlane = runtime::BuildToolInstallSteps(
        runtime::ToolId::kOpenLane2,
        runtime::OrfsBuildPolicy::kAllowLocalBuild);
    Assert::IsTrue(JoinArguments(openlane[1].request.arguments)
                       .find(L"--max-jobs 1 --cores \"$build_cores\"") ==
                   std::wstring::npos);
  }

  TEST_METHOD(Asap7ModelsArePinnedAndSeparateFromFlowInstallation) {
    const auto install = runtime::BuildToolInstallSteps(runtime::ToolId::kAsap7Models);
    Assert::AreEqual(std::size_t(1), install.size());
    const auto command = JoinArguments(install.front().request.arguments);
    Assert::IsTrue(command.find(L"472f7b3f6680ffb1109d6792219e6ea5570c26a0") != std::wstring::npos);
    Assert::IsTrue(command.find(L"sha256sum --check --status") != std::wstring::npos);
    Assert::IsTrue(command.find(L"flock -x") != std::wstring::npos);
    Assert::IsTrue(command.find(L"nix develop") == std::wstring::npos);
    Assert::IsTrue(command.find(L"rm -rf") == std::wstring::npos);
    const auto probe = runtime::BuildAsap7ModelRequest(false);
    Assert::IsTrue(std::find(probe.arguments.begin(), probe.arguments.end(), L"probe") != probe.arguments.end());
    Assert::IsTrue(runtime::BuildToolRemoveSteps(runtime::ToolId::kAsap7Models).empty());
    Assert::AreEqual(std::wstring(L"28.2022"), runtime::ParseToolVersion(runtime::ToolId::kAsap7Models, L"ASAP7 CDL 28.2022\n"));
  }

  TEST_METHOD(OpenLaneInstallPinsValidatedReleaseAndWritesMarker) {
    const std::vector<runtime::SetupStep> install =
        runtime::BuildToolInstallSteps(runtime::ToolId::kOpenLane2);
    Assert::AreEqual(static_cast<std::size_t>(2), install.size());
    const std::wstring command = JoinArguments(install[1].request.arguments);
    Assert::IsTrue(command.find(
        L"a7b0e6dba75ee7e891ff3d7824b29473d9cad289") !=
                   std::wstring::npos);
    Assert::IsTrue(command.find(L"version=2.3.10") != std::wstring::npos);
    Assert::IsTrue(command.find(L".designpp-environment") !=
                   std::wstring::npos);
    Assert::IsTrue(command.find(L"pull --ff-only") == std::wstring::npos);
    Assert::IsTrue(command.find(L".openlane-candidate.XXXXXX") !=
                   std::wstring::npos);
    Assert::IsTrue(command.find(L"cleanup_candidates openlane openlane2") !=
                   std::wstring::npos);
    Assert::IsTrue(command.find(L"rm -rf -- \"$candidate\"") !=
                   std::wstring::npos);
    Assert::IsTrue(command.find(
                       L"toolchains/environments/openlane2-2.3.10") !=
                   std::wstring::npos);
    Assert::IsTrue(command.find(L".openlane-backup.XXXXXX") ==
                   std::wstring::npos);
    Assert::IsTrue(command.find(L"activation is a separate action") !=
                   std::wstring::npos);
    Assert::IsTrue(runtime::BuildToolRemoveSteps(
                       runtime::ToolId::kOpenLane2)
                       .empty());
  }
};
// clang-format on

}  // namespace designpp::tests

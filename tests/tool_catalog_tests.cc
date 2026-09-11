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
    Assert::AreEqual(static_cast<std::size_t>(2), install.size());
    const std::wstring nix_command =
        JoinArguments(install[0].request.arguments);
    Assert::IsTrue(nix_command.find(L"install.determinate.systems") !=
                   std::wstring::npos);
    const std::wstring command = JoinArguments(install[1].request.arguments);
    Assert::IsTrue(command.find(L"nix-command flakes") != std::wstring::npos);
    Assert::IsTrue(command.find(L"toolchains/orfs") != std::wstring::npos);
    Assert::IsTrue(command.find(L"--branch") == std::wstring::npos);
    Assert::IsTrue(command.find(L"036d106273e66855cd5214d49518fd0f0df7de61") != std::wstring::npos);
    Assert::IsTrue(command.find(L"0e2d771c5ec38f232493c2afea738ea0200cb972") != std::wstring::npos);
    Assert::IsTrue(command.find(L"d3e297fcd479247322f83d14f42b3556db7acdfb") != std::wstring::npos);
    Assert::IsTrue(command.find(L"--max-jobs 0 --builders '' --option fallback false") != std::wstring::npos);
    Assert::IsTrue(command.find(L"rm -rf") == std::wstring::npos);
    Assert::IsTrue(command.find(L"Source builds are disabled") != std::wstring::npos);
    Assert::IsTrue(command.find(L".orfs-candidate.XXXXXX") !=
                   std::wstring::npos);
    Assert::IsTrue(command.find(L".orfs-backup.XXXXXX") !=
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

  TEST_METHOD(OpenLaneInstallPinsValidatedReleaseAndWritesMarker) {
    const std::vector<runtime::SetupStep> install =
        runtime::BuildToolInstallSteps(runtime::ToolId::kOpenLane2);
    Assert::AreEqual(static_cast<std::size_t>(2), install.size());
    const std::wstring command = JoinArguments(install[1].request.arguments);
    Assert::IsTrue(command.find(
        L"b89f7866fd3d19da470220baf89d0e7804962941") !=
                   std::wstring::npos);
    Assert::IsTrue(command.find(L"version=2.3.10") != std::wstring::npos);
    Assert::IsTrue(command.find(L".designpp-environment") !=
                   std::wstring::npos);
    Assert::IsTrue(command.find(L"pull --ff-only") == std::wstring::npos);
    Assert::IsTrue(command.find(L".openlane-candidate.XXXXXX") !=
                   std::wstring::npos);
    Assert::IsTrue(command.find(L".openlane-backup.XXXXXX") !=
                   std::wstring::npos);
  }
};
// clang-format on

}  // namespace designpp::tests

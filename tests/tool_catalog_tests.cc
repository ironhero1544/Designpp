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
};
// clang-format on

}  // namespace designpp::tests

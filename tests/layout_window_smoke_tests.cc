// Copyright 2026 The Design++ Authors

// clang-format off
#include <windows.h>
#include <commctrl.h>
#include <CppUnitTest.h>
// clang-format on

#include <atomic>
#include <filesystem>

#include "designpp/gui/layout_window.h"

using Microsoft::VisualStudio::CppUnitTestFramework::Assert;

namespace designpp::tests {
namespace {

class TemporaryLayoutWindowWorkspace final {
 public:
  explicit TemporaryLayoutWindowWorkspace(
      core::ViewKind view_kind = core::ViewKind::kLayout) {
    static std::atomic_uint64_t sequence = 0;
    directory_ = std::filesystem::temp_directory_path() /
                 (L"designpp-layout-window-test-" +
                  std::to_wstring(GetCurrentProcessId()) + L"-" +
                  std::to_wstring(++sequence));
    std::filesystem::create_directories(directory_ / L"cells" /
                                        Utf8Path(kCellId) / L"views" /
                                        Utf8Path(kViewId));
    record_.directory = directory_;
    record_.status = core::LibraryStatus::kReady;
    record_.library.id = kLibraryId;
    record_.library.name = "Physical design smoke";
    record_.library.created_utc = "2026-08-24T00:00:00Z";
    record_.library.modified_utc = record_.library.created_utc;
    core::Cell cell;
    cell.id = kCellId;
    cell.name = "top";
    core::View view;
    view.id = kViewId;
    view.name = "physical";
    view.kind = view_kind;
    cell.views.push_back(std::move(view));
    record_.library.cells.push_back(std::move(cell));
  }

  ~TemporaryLayoutWindowWorkspace() {
    std::error_code error;
    std::filesystem::remove_all(directory_, error);
  }

  [[nodiscard]] application::WorkspaceOpenRequest Request() const {
    application::WorkspaceOpenRequest request;
    request.library_id = kLibraryId;
    request.cell_id = kCellId;
    request.view_id = kViewId;
    return request;
  }

  [[nodiscard]] const application::LibraryRecord& record() const {
    return record_;
  }

 private:
  static constexpr char kLibraryId[] = "11111111-1111-4111-8111-111111111111";
  static constexpr char kCellId[] = "22222222-2222-4222-8222-222222222222";
  static constexpr char kViewId[] = "33333333-3333-4333-8333-333333333333";

  static std::filesystem::path Utf8Path(std::string_view text) {
    std::u8string value;
    for (char character : text) {
      value.push_back(static_cast<char8_t>(character));
    }
    return std::filesystem::path(value);
  }

  std::filesystem::path directory_;
  application::LibraryRecord record_;
};

void PumpMessages(DWORD milliseconds) {
  const ULONGLONG deadline = GetTickCount64() + milliseconds;
  while (GetTickCount64() < deadline) {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT,
                                MWMO_INPUTAVAILABLE);
  }
}

}  // namespace

// clang-format off
TEST_CLASS(LayoutWindowSmokeTests) {
 public:
  TEST_METHOD(IndependentLayoutWindowLoadsAndRemainsResponsive) {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES};
    Assert::IsTrue(InitCommonControlsEx(&controls) != FALSE);
    TemporaryLayoutWindowWorkspace workspace;
    const application::WorkspaceOpenRequest request = workspace.Request();
    gui::LayoutWindow window;
    Assert::IsTrue(window.Create(GetModuleHandleW(nullptr), request,
                                 workspace.record(), {}, {}));
    Assert::IsTrue(window.Kind() == gui::ViewWindowKind::kLayout);
    Assert::IsTrue(
        window.CanActivate(request, core::ViewKind::kLayout));
    HWND physical = FindWindowW(L"DesignPlusPlus.LayoutWindow", nullptr);
    Assert::IsNotNull(physical);
    ShowWindow(physical, SW_HIDE);
    PumpMessages(300);
    DWORD_PTR response = 0;
    Assert::IsTrue(SendMessageTimeoutW(physical, WM_NULL, 0, 0,
                                       SMTO_ABORTIFHUNG, 1000, &response) != 0);
    Assert::IsNotNull(GetDlgItem(physical, 7601));
    Assert::IsNotNull(GetDlgItem(physical, 7602));
    Assert::IsNotNull(GetDlgItem(physical, 7603));
    window.Close();
    Assert::IsFalse(window.IsOpen());
  }

  TEST_METHOD(PhysicalDesignViewIsNotOpenedAsLayoutWindow) {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES};
    Assert::IsTrue(InitCommonControlsEx(&controls) != FALSE);
    TemporaryLayoutWindowWorkspace workspace(
        core::ViewKind::kPhysicalDesign);
    const application::WorkspaceOpenRequest request = workspace.Request();
    gui::LayoutWindow window;
    Assert::IsFalse(window.Create(GetModuleHandleW(nullptr), request,
                                  workspace.record(), {}, {}));
    Assert::IsFalse(window.IsOpen());
    Assert::IsFalse(window.IsOpen());
  }
};
// clang-format on

}  // namespace designpp::tests

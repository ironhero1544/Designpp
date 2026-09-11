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

HWND FindControlById(HWND parent, int control_id) {
  if (!parent) return nullptr;
  if (GetDlgCtrlID(parent) == control_id) return parent;
  for (HWND child = GetWindow(parent, GW_CHILD); child;
       child = GetWindow(child, GW_HWNDNEXT)) {
    if (HWND found = FindControlById(child, control_id)) return found;
  }
  return nullptr;
}

HWND FindTestLayoutWindow() {
  return FindWindowW(L"DesignPlusPlus.LayoutWindow", L"Design++ Layout — top");
}

struct OwnedWindowSearch {
  HWND owner = nullptr;
  HWND found = nullptr;
};

BOOL CALLBACK FindOwnedWindowProcedure(HWND window, LPARAM parameter) {
  auto* search = reinterpret_cast<OwnedWindowSearch*>(parameter);
  if (GetWindow(window, GW_OWNER) == search->owner) {
    search->found = window;
    return FALSE;
  }
  return TRUE;
}

HWND FindOwnedSetupWindow(HWND owner) {
  OwnedWindowSearch search{owner};
  EnumWindows(FindOwnedWindowProcedure, reinterpret_cast<LPARAM>(&search));
  return search.found;
}

HWND CreateTestOwnerWindow() {
  return CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                         CW_USEDEFAULT, 100, 100, nullptr, nullptr,
                         GetModuleHandleW(nullptr), nullptr);
}

}  // namespace

std::wstring Text(HWND window) {
  wchar_t value[1024]{};
  GetWindowTextW(window, value, 1024);
  return value;
}

template <typename Predicate>
bool WaitUntil(Predicate condition) {
  const auto deadline = GetTickCount64() + 5000;
  while (!condition()) {
    if (GetTickCount64() >= deadline) return false;
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT,
                                MWMO_INPUTAVAILABLE);
  }
  return true;
}

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
    HWND physical = FindTestLayoutWindow();
    Assert::IsNotNull(physical);
    ShowWindow(physical, SW_HIDE);
    PumpMessages(300);
    DWORD_PTR response = 0;
    Assert::IsTrue(SendMessageTimeoutW(physical, WM_NULL, 0, 0,
                                       SMTO_ABORTIFHUNG, 1000, &response) != 0);
    Assert::IsNotNull(GetDlgItem(physical, 7601));
    Assert::IsNotNull(GetDlgItem(physical, 7602));
    Assert::IsNotNull(GetDlgItem(physical, 7603));
    Assert::IsNotNull(GetDlgItem(physical, 7609));
    Assert::IsFalse(IsWindowEnabled(GetDlgItem(physical, 7609)) != FALSE);
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

  TEST_METHOD(SaveBlankDefaultsPersistsWithoutClosingSetup) {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES};
    Assert::IsTrue(InitCommonControlsEx(&controls) != FALSE);
    TemporaryLayoutWindowWorkspace workspace;
    gui::LayoutWindow window;
    Assert::IsTrue(window.Create(GetModuleHandleW(nullptr), workspace.Request(),
                                 workspace.record(), {}, {}));
    HWND layout = FindTestLayoutWindow();
    Assert::IsTrue(WaitUntil([&] { return IsWindowEnabled(GetDlgItem(layout, 7604)); }));
    SendMessageW(layout, WM_COMMAND, MAKEWPARAM(7604, BN_CLICKED), 0);
    HWND setup = FindWindowW(L"DesignPlusPlus.LayoutSetupDialog", nullptr);
    Assert::IsNotNull(setup);
    HWND utilization = FindControlById(setup, 7707);
    SetWindowTextW(utilization, L"65");
    SendMessageW(setup, WM_COMMAND, MAKEWPARAM(7701, BN_CLICKED), 0);
    Assert::IsTrue(WaitUntil([&] { return Text(FindControlById(setup, 7742)) ==
                                                   L"Saved to the selected Cell"; }));
    SetWindowTextW(utilization, L" ");
    SetWindowTextW(FindControlById(setup, 7706), L"");
    SetWindowTextW(FindControlById(setup, 7708), L"");
    SendMessageW(setup, WM_COMMAND, MAKEWPARAM(7701, BN_CLICKED), 0);
    Assert::IsTrue(WaitUntil([&] { return Text(FindControlById(setup, 7742)) ==
                                                   L"Saved to the selected Cell"; }));
    application::ProjectService service;
    auto document = service.OpenOrCreate(workspace.record(), workspace.Request().cell_id);
    Assert::IsTrue(document.Ok());
    Assert::AreEqual(std::uint32_t(40),
        document.Value().project.physical_implementation.core_utilization_percent);
    Assert::IsTrue(core::UsesAutomaticValue(
        document.Value().project.physical_implementation, "core_utilization_percent"));
    Assert::IsTrue(IsWindow(setup) && IsWindow(layout));
    Assert::AreEqual(std::wstring(), Text(utilization));
    SendMessageW(setup, WM_CLOSE, 0, 0);
    Assert::IsFalse(IsWindow(setup) != FALSE);
    window.Close();
  }

  TEST_METHOD(InvalidUtilizationRetainsDraftAndCanBeCorrected) {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES};
    InitCommonControlsEx(&controls);
    TemporaryLayoutWindowWorkspace workspace;
    gui::LayoutWindow window;
    Assert::IsTrue(window.Create(GetModuleHandleW(nullptr), workspace.Request(),
                                 workspace.record(), {}, {}));
    HWND layout = FindTestLayoutWindow();
    Assert::IsTrue(WaitUntil([&] { return IsWindowEnabled(GetDlgItem(layout, 7604)); }));
    SendMessageW(layout, WM_COMMAND, MAKEWPARAM(7604, BN_CLICKED), 0);
    HWND setup = FindWindowW(L"DesignPlusPlus.LayoutSetupDialog", nullptr);
    SetWindowTextW(FindControlById(setup, 7707), L"40oops");
    SendMessageW(setup, WM_COMMAND, MAKEWPARAM(7701, BN_CLICKED), 0);
    Assert::IsTrue(Text(FindControlById(setup, 7742)).find(L"Core utilization:") == 0);
    Assert::AreEqual(std::wstring(L"40oops"), Text(FindControlById(setup, 7707)));
    SetWindowTextW(FindControlById(setup, 7707), L"");
    SendMessageW(setup, WM_COMMAND, MAKEWPARAM(7701, BN_CLICKED), 0);
    Assert::IsTrue(WaitUntil([&] { return Text(FindControlById(setup, 7742)) ==
                                                   L"Saved to the selected Cell"; }));
    SendMessageW(setup, WM_CLOSE, 0, 0);
    window.Close();
  }

  TEST_METHOD(JsonSaveUsesTheSamePersistentSaveWithoutClosing) {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES};
    InitCommonControlsEx(&controls);
    TemporaryLayoutWindowWorkspace workspace;
    gui::LayoutWindow window;
    Assert::IsTrue(window.Create(GetModuleHandleW(nullptr), workspace.Request(),
        workspace.record(), {}, {},
        [](HWND, const core::PhysicalImplementationConfiguration& configuration,
           gui::LayoutJsonApplyCallback apply) -> std::unique_ptr<gui::LayoutJsonSession> {
          auto changed = configuration;
          changed.die_area = {"0", "0", "16", "16"};
          changed.core_area = {"1", "1", "15", "15"};
          apply(changed, true);
          return {};
        }));
    HWND layout = FindTestLayoutWindow();
    Assert::IsTrue(WaitUntil([&] { return IsWindowEnabled(GetDlgItem(layout, 7604)); }));
    SendMessageW(layout, WM_COMMAND, MAKEWPARAM(7604, BN_CLICKED), 0);
    HWND setup = FindWindowW(L"DesignPlusPlus.LayoutSetupDialog", nullptr);
    SendMessageW(setup, WM_COMMAND, MAKEWPARAM(7741, BN_CLICKED), 0);
    Assert::IsTrue(WaitUntil([&] { return Text(FindControlById(setup, 7742)) ==
                                                   L"Saved to the selected Cell"; }));
    application::ProjectService service;
    auto document = service.OpenOrCreate(workspace.record(), workspace.Request().cell_id);
    Assert::IsTrue(document.Ok());
    Assert::AreEqual(std::size_t(4), document.Value().project.physical_implementation.core_area.size());
    Assert::IsTrue(IsWindow(setup) && IsWindow(layout));
    SendMessageW(setup, WM_CLOSE, 0, 0);
    window.Close();
  }

  TEST_METHOD(SetupDefersCloseAndRejectsDuplicateSave) {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES};
    InitCommonControlsEx(&controls);
    gui::LayoutSetupController controller;
    int saves = 0;
    core::PhysicalImplementationConfiguration snapshot;
    Assert::IsTrue(controller.Open(nullptr, GetModuleHandleW(nullptr), {}, {}, {}, {},
        [&](const core::PhysicalImplementationConfiguration& value) {
          ++saves;
          snapshot = value;
        }));
    HWND setup = FindWindowW(L"DesignPlusPlus.LayoutSetupDialog", nullptr);
    SendMessageW(setup, WM_COMMAND, MAKEWPARAM(7701, BN_CLICKED), 0);
    SendMessageW(setup, WM_COMMAND, MAKEWPARAM(7701, BN_CLICKED), 0);
    Assert::AreEqual(1, saves);
    SendMessageW(setup, WM_CLOSE, 0, 0);
    Assert::IsTrue(IsWindow(setup) != FALSE);
    controller.Saved(core::Status::Success(), snapshot);
    Assert::IsTrue(WaitUntil([&] { return !IsWindow(setup); }));
  }

  TEST_METHOD(SetupPersistsSelectedOrfsPlatform) {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES};
    Assert::IsTrue(InitCommonControlsEx(&controls) != FALSE);
    gui::LayoutSetupController controller;
    core::PhysicalImplementationConfiguration configuration;
    configuration.backend_id = "orfs";
    configuration.orfs.platform = "asap7";
    std::vector<adapters::OrfsPlatformCandidate> platforms = {
        {"asap7", true, ""}, {"sky130hd", true, ""}};
    core::PhysicalImplementationConfiguration saved;
    HWND owner = CreateTestOwnerWindow();
    Assert::IsNotNull(owner);
    Assert::IsTrue(controller.Open(
        owner, GetModuleHandleW(nullptr), configuration, {}, platforms, {},
        [&](const core::PhysicalImplementationConfiguration& value) {
          saved = value;
        }));
    HWND setup = FindOwnedSetupWindow(owner);
    HWND platform = FindControlById(setup, 7795);
    Assert::IsNotNull(platform);
    Assert::IsTrue(SendMessageW(platform, CB_SELECTSTRING, 0,
                                reinterpret_cast<LPARAM>(L"sky130hd")) !=
                   CB_ERR);
    SendMessageW(setup, WM_COMMAND, MAKEWPARAM(7701, BN_CLICKED), 0);
    Assert::AreEqual(std::string("orfs"), saved.backend_id);
    Assert::AreEqual(std::string("sky130hd"), saved.orfs.platform);
    controller.Saved(core::Status::Success(), saved);
    SendMessageW(setup, WM_CLOSE, 0, 0);
    DestroyWindow(owner);
  }

  TEST_METHOD(SetupKeepsUnavailableSavedOrfsPlatformVisible) {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES};
    Assert::IsTrue(InitCommonControlsEx(&controls) != FALSE);
    gui::LayoutSetupController controller;
    core::PhysicalImplementationConfiguration configuration;
    configuration.backend_id = "orfs";
    configuration.orfs.platform = "sky130hd";
    std::vector<adapters::OrfsPlatformCandidate> platforms = {
        {"asap7", true, ""}};
    core::PhysicalImplementationConfiguration saved;
    HWND owner = CreateTestOwnerWindow();
    Assert::IsNotNull(owner);
    Assert::IsTrue(controller.Open(
        owner, GetModuleHandleW(nullptr), configuration, {}, platforms, {},
        [&](const core::PhysicalImplementationConfiguration& value) {
          saved = value;
        }));
    HWND setup = FindOwnedSetupWindow(owner);
    HWND platform = FindControlById(setup, 7795);
    Assert::AreEqual(static_cast<LRESULT>(2),
                     SendMessageW(platform, CB_GETCOUNT, 0, 0));
    Assert::AreEqual(static_cast<LRESULT>(1),
                     SendMessageW(platform, CB_GETCURSEL, 0, 0));
    wchar_t selected_platform[64]{};
    SendMessageW(platform, CB_GETLBTEXT, 1,
                 reinterpret_cast<LPARAM>(selected_platform));
    Assert::AreEqual(std::wstring(L"sky130hd"),
                     std::wstring(selected_platform));
    SendMessageW(setup, WM_COMMAND, MAKEWPARAM(7701, BN_CLICKED), 0);
    Assert::AreEqual(std::string("sky130hd"), saved.orfs.platform);
    controller.Saved(core::Status::Success(), saved);
    SendMessageW(setup, WM_CLOSE, 0, 0);
    DestroyWindow(owner);
  }

  TEST_METHOD(LateJsonCallbackDoesNotTouchDestroyedSetup) {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES};
    InitCommonControlsEx(&controls);
    gui::LayoutJsonApplyCallback late;
    int saves = 0;
    {
      gui::LayoutSetupController controller;
      Assert::IsTrue(controller.Open(nullptr, GetModuleHandleW(nullptr), {}, {}, {},
          [&](HWND, const core::PhysicalImplementationConfiguration&,
              gui::LayoutJsonApplyCallback callback) -> std::unique_ptr<gui::LayoutJsonSession> {
            late = std::move(callback);
            return {};
          }, [&](const core::PhysicalImplementationConfiguration&) { ++saves; }));
      HWND setup = FindWindowW(L"DesignPlusPlus.LayoutSetupDialog", nullptr);
      SendMessageW(setup, WM_COMMAND, MAKEWPARAM(7741, BN_CLICKED), 0);
      Assert::IsTrue(static_cast<bool>(late));
    }
    late({}, true);
    Assert::AreEqual(0, saves);
  }
};
// clang-format on

}  // namespace designpp::tests

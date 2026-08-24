// Copyright 2026 The Design++ Authors

// clang-format off
#include <windows.h>
#include <commctrl.h>
#include <CppUnitTest.h>
// clang-format on

#include <atomic>
#include <filesystem>
#include <fstream>

#include "designpp/application/run_store.h"
#include "designpp/gui/timing_window.h"

using Microsoft::VisualStudio::CppUnitTestFramework::Assert;

namespace designpp::tests {
namespace {

class TemporaryTimingWindowWorkspace final {
 public:
  TemporaryTimingWindowWorkspace() {
    static std::atomic_uint64_t sequence = 0;
    directory_ = std::filesystem::temp_directory_path() /
                 (L"designpp-timing-window-test-" +
                  std::to_wstring(GetCurrentProcessId()) + L"-" +
                  std::to_wstring(++sequence));
    std::filesystem::create_directories(directory_ / L"cells" /
                                        Utf8Path(kCellId) / L"views" /
                                        Utf8Path(kViewId));
    record_.directory = directory_;
    record_.status = core::LibraryStatus::kReady;
    record_.library.id = kLibraryId;
    record_.library.name = "Timing smoke";
    record_.library.created_utc = "2026-08-24T00:00:00Z";
    record_.library.modified_utc = record_.library.created_utc;
    core::Cell cell;
    cell.id = kCellId;
    cell.name = "top";
    core::View view;
    view.id = kViewId;
    view.name = "timing";
    view.kind = core::ViewKind::kTiming;
    cell.views.push_back(std::move(view));
    record_.library.cells.push_back(std::move(cell));

    core::Project project;
    project.id = "timing-smoke-project";
    project.top_module = "top";
    application::RunStore store;
    auto begun = store.Begin(CellDirectory(), project, "StaticTimingAnalysis",
                             "OpenSTA", "2.6.0");
    if (begun.Ok()) {
      application::RunRecord run = std::move(begun).Value();
      const auto summary = run.directory / L"reports" / L"timing-summary.json";
      std::ofstream(summary, std::ios::binary)
          << "{\"schema_version\":2,\"corner\":\"typical\","
             "\"setup_wns\":0.2,\"setup_tns\":0.0,"
             "\"hold_wns\":0.1,\"hold_tns\":0.0}";
      std::vector<application::RunArtifact> artifacts{
          {"summary",
           "json",
           "reports/timing-summary.json",
           std::filesystem::file_size(summary),
           {},
           false}};
      application::RunOutcome outcome;
      outcome.process_succeeded = true;
      outcome.result_succeeded = true;
      outcome.summary_relative_path = "reports/timing-summary.json";
      static_cast<void>(store.Complete(&run, application::RunStatus::kSucceeded,
                                       0, {}, std::move(artifacts),
                                       std::move(outcome)));
    }
  }

  ~TemporaryTimingWindowWorkspace() {
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

  [[nodiscard]] std::filesystem::path CellDirectory() const {
    return directory_ / L"cells" / Utf8Path(kCellId);
  }

 private:
  static constexpr char kLibraryId[] = "11111111-1111-4111-8111-111111111111";
  static constexpr char kCellId[] = "22222222-2222-4222-8222-222222222222";
  static constexpr char kViewId[] = "33333333-3333-4333-8333-333333333333";

  static std::filesystem::path Utf8Path(std::string_view text) {
    std::u8string value;
    for (char character : text)
      value.push_back(static_cast<char8_t>(character));
    return std::filesystem::path(value);
  }

  std::filesystem::path directory_;
  application::LibraryRecord record_;
};

void PumpWindowMessages(DWORD milliseconds) {
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

TEST_CLASS(TimingWindowSmokeTests){
  public : TEST_METHOD(IndependentWindowLoadsAndRemainsMessageResponsive){
      INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES};
Assert::IsTrue(InitCommonControlsEx(&controls) != FALSE);
TemporaryTimingWindowWorkspace workspace;
const application::WorkspaceOpenRequest request = workspace.Request();
gui::TimingWindow window;
Assert::IsTrue(window.Create(GetModuleHandleW(nullptr), request,
                             workspace.record(), {}, {}, {}));
Assert::IsTrue(window.Kind() == gui::ViewWindowKind::kTiming);
Assert::IsTrue(window.CanActivate(request, core::ViewKind::kTiming));
HWND timing = FindWindowW(L"DesignPlusPlus.TimingWindow", nullptr);
Assert::IsNotNull(timing);
RECT window_rectangle{};
Assert::IsTrue(GetWindowRect(timing, &window_rectangle) != FALSE);
const UINT dpi = gui::GetWindowDpi(timing);
Assert::AreEqual(static_cast<LONG>(gui::ScaleForDpi(1280, dpi)),
                 window_rectangle.right - window_rectangle.left);
Assert::AreEqual(static_cast<LONG>(gui::ScaleForDpi(820, dpi)),
                 window_rectangle.bottom - window_rectangle.top);
MINMAXINFO minimum_size{};
SendMessageW(timing, WM_GETMINMAXINFO, 0,
             reinterpret_cast<LPARAM>(&minimum_size));
Assert::AreEqual(static_cast<LONG>(gui::ScaleForDpi(900, dpi)),
                 minimum_size.ptMinTrackSize.x);
Assert::AreEqual(static_cast<LONG>(gui::ScaleForDpi(600, dpi)),
                 minimum_size.ptMinTrackSize.y);
ShowWindow(timing, SW_HIDE);
PumpWindowMessages(250);
DWORD_PTR response = 0;
Assert::IsTrue(SendMessageTimeoutW(timing, WM_NULL, 0, 0, SMTO_ABORTIFHUNG,
                                   1000, &response) != 0);
Assert::IsFalse(IsWindowEnabled(GetDlgItem(timing, 7101)) != FALSE);
Assert::IsFalse(IsWindowEnabled(GetDlgItem(timing, 7102)) != FALSE);
HWND runs = GetDlgItem(timing, 7110);
Assert::IsNotNull(runs);
Assert::AreEqual(1, ListView_GetItemCount(runs));
window.Close();
Assert::IsFalse(window.IsOpen());
}  // namespace designpp::tests
}
;

}  // namespace designpp::tests

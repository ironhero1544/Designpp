// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_GUI_TIMING_WINDOW_H_
#define DESIGNPP_GUI_TIMING_WINDOW_H_

#include <windows.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "designpp/application/project_service.h"
#include "designpp/application/timing_run_history.h"
#include "designpp/application/timing_run_service.h"
#include "designpp/gui/dpi.h"
#include "designpp/gui/view_window.h"
#include "designpp/runtime/execution_provider.h"
#include "designpp/runtime/task_scheduler.h"

namespace designpp::gui {

class TimingWindow final : public ViewWindow {
 public:
  TimingWindow() = default;
  TimingWindow(const TimingWindow&) = delete;
  TimingWindow& operator=(const TimingWindow&) = delete;
  ~TimingWindow() override;

  [[nodiscard]] bool Create(HINSTANCE instance,
                            const application::WorkspaceOpenRequest& request,
                            application::LibraryRecord library,
                            ViewWindowLogCallback central_log,
                            ViewWindowLibraryChangedCallback library_changed,
                            ViewWindowOpenCallback open_view);

  [[nodiscard]] ViewWindowKind Kind() const noexcept override;
  [[nodiscard]] bool CanActivate(
      const application::WorkspaceOpenRequest& request,
      core::ViewKind view_kind) const override;
  void Activate(const application::WorkspaceOpenRequest& request) override;
  [[nodiscard]] bool BelongsToLibrary(
      std::string_view library_id) const override;
  [[nodiscard]] bool MatchesCell(std::string_view library_id,
                                 std::string_view cell_id) const override;
  [[nodiscard]] bool IsOpen() const noexcept override;
  void RefreshLibrary(application::LibraryRecord library) override;
  [[nodiscard]] bool PrepareClose() override;
  void Close() override;
  [[nodiscard]] bool TranslateAccelerator(const MSG& message) override;

 private:
  struct EventChannel;
  static constexpr UINT kEventMessage = WM_APP + 71;

  static LRESULT CALLBACK WindowProcedure(HWND window, UINT message,
                                          WPARAM wparam, LPARAM lparam);
  LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);
  [[nodiscard]] bool CreateControls();
  void LayoutControls(int width, int height);
  void BeginLoad();
  void HandleEvents();
  void PopulateConfiguration();
  void StartTiming();
  void ApplyState(application::TimingRunState state);
  void AppendOutput(std::wstring_view text);
  void ShowProblems();
  void ShowRuns();
  void PopulateRunHistory();
  void SelectRun(std::size_t index);
  void ShowReports();
  void ShowArtifacts();
  void ShowScript();
  void OpenSynthesis();
  void ShutdownChannel();

  HINSTANCE instance_ = nullptr;
  HWND window_ = nullptr;
  HWND identity_ = nullptr;
  HWND run_button_ = nullptr;
  HWND cancel_button_ = nullptr;
  HWND reports_button_ = nullptr;
  HWND artifacts_button_ = nullptr;
  HWND script_button_ = nullptr;
  HWND synthesis_button_ = nullptr;
  HWND corner_label_ = nullptr;
  HWND sdc_label_ = nullptr;
  HWND liberty_label_ = nullptr;
  HWND corner_edit_ = nullptr;
  HWND sdc_combo_ = nullptr;
  HWND liberty_list_ = nullptr;
  HWND summary_ = nullptr;
  HWND violations_ = nullptr;
  HWND bottom_tabs_ = nullptr;
  HWND output_ = nullptr;
  HWND runs_list_ = nullptr;
  HWND status_ = nullptr;
  UINT dpi_ = kDefaultDpi;
  UniqueFont font_;

  application::WorkspaceOpenRequest request_;
  application::LibraryRecord library_;
  application::ProjectService project_service_;
  std::unique_ptr<application::ProjectDocument> document_;
  std::vector<application::ResolvedSource> sources_;
  std::vector<const application::ResolvedSource*> sdc_candidates_;
  std::vector<const application::ResolvedSource*> liberty_candidates_;
  std::vector<application::TimingRunSnapshot> run_snapshots_;
  std::shared_ptr<application::RunRecord> active_run_;
  std::vector<core::Diagnostic> diagnostics_;
  adapters::TimingMetrics metrics_;
  core::Status compatibility_status_;
  std::shared_ptr<application::RunRecord> compatible_synthesis_run_;
  std::string script_text_;
  runtime::WslExecutionProvider execution_provider_;
  application::TimingRunService timing_service_{&execution_provider_};
  runtime::TaskScheduler scheduler_{1};
  std::shared_ptr<EventChannel> event_channel_;
  ViewWindowLogCallback central_log_;
  ViewWindowLibraryChangedCallback library_changed_;
  ViewWindowOpenCallback open_view_;
  std::uint64_t generation_ = 1;
};

}  // namespace designpp::gui

#endif  // DESIGNPP_GUI_TIMING_WINDOW_H_

// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_GUI_LAYOUT_WINDOW_IMPL_H_
#define DESIGNPP_GUI_LAYOUT_WINDOW_IMPL_H_

#include <windows.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "designpp/application/layout_viewer_service.h"
#include "designpp/application/openlane_discovery_service.h"
#include "designpp/application/physical_implementation_service.h"
#include "designpp/application/project_service.h"
#include "designpp/core/toolchain_profile.h"
#include "designpp/gui/dpi.h"
#include "designpp/gui/view_window.h"
#include "designpp/runtime/execution_provider.h"
#include "designpp/runtime/task_scheduler.h"

namespace designpp::gui {

class LayoutWindowImplementation final : public ViewWindow {
 public:
  LayoutWindowImplementation() = default;
  LayoutWindowImplementation(const LayoutWindowImplementation&) = delete;
  LayoutWindowImplementation& operator=(const LayoutWindowImplementation&) =
      delete;
  ~LayoutWindowImplementation() override;

  [[nodiscard]] bool Create(HINSTANCE instance,
                            const application::WorkspaceOpenRequest& request,
                            application::LibraryRecord library,
                            ViewWindowLogCallback central_log,
                            ViewWindowLibraryChangedCallback library_changed);

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
  static constexpr UINT kEventMessage = WM_APP + 76;

  static LRESULT CALLBACK WindowProcedure(HWND window, UINT message,
                                          WPARAM wparam, LPARAM lparam);
  LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);
  [[nodiscard]] bool CreateControls();
  void LayoutControls(int width, int height);
  void BeginLoad();
  void HandleEvents();
  void PopulateConfiguration();
  void PopulateStandardCellLibraries();
  void EditSetup();
  void SaveSetup();
  void SaveAndStart(bool resume);
  void StartPreparedRun(bool resume);
  void ApplyState(application::ManagedFlowRunState state);
  void AppendOutput(std::wstring_view text);
  void ShowReports();
  void ShowArtifacts();
  void OpenLayout();
  void MergeCompletedRun(
      const std::shared_ptr<application::RunRecord>& completed_run);
  void PopulateRuns();
  void SelectRun(int index);
  void ShutdownChannel();

  HINSTANCE instance_ = nullptr;
  HWND window_ = nullptr;
  HWND identity_ = nullptr;
  HWND run_button_ = nullptr;
  HWND cancel_button_ = nullptr;
  HWND resume_button_ = nullptr;
  HWND config_button_ = nullptr;
  HWND reports_button_ = nullptr;
  HWND artifacts_button_ = nullptr;
  HWND pdk_edit_ = nullptr;
  HWND scl_edit_ = nullptr;
  HWND clocks_edit_ = nullptr;
  HWND period_edit_ = nullptr;
  HWND utilization_edit_ = nullptr;
  HWND density_edit_ = nullptr;
  HWND die_area_edit_ = nullptr;
  HWND pnr_sdc_combo_ = nullptr;
  HWND signoff_sdc_combo_ = nullptr;
  HWND advanced_edit_ = nullptr;
  HWND stages_list_ = nullptr;
  HWND summary_ = nullptr;
  HWND runs_list_ = nullptr;
  HWND output_ = nullptr;
  HWND status_ = nullptr;
  UINT dpi_ = kDefaultDpi;
  UniqueFont font_;

  application::WorkspaceOpenRequest request_;
  core::ViewKind view_kind_ = core::ViewKind::kLayout;
  application::LibraryRecord library_;
  std::shared_ptr<application::ProjectDocument> document_;
  std::vector<application::ResolvedSource> sources_;
  std::vector<const application::ResolvedSource*> sdc_candidates_;
  std::vector<application::OpenLanePdkCandidate> pdk_candidates_;
  core::ToolchainProfile profile_;
  std::vector<application::RunRecord> runs_;
  std::vector<adapters::ManagedFlowMetrics> run_metrics_;
  std::vector<std::optional<application::ManagedFlowResumeRequest>>
      resume_candidates_;
  std::shared_ptr<application::RunRecord> active_run_;
  std::optional<application::ManagedFlowResumeRequest> selected_resume_;
  adapters::ManagedFlowMetrics metrics_;
  runtime::WslExecutionProvider execution_provider_;
  application::PhysicalImplementationService flow_service_{
      &execution_provider_};
  application::LayoutViewerService viewer_service_{&execution_provider_};
  application::OpenLaneDiscoveryService discovery_service_{
      &execution_provider_};
  runtime::TaskScheduler scheduler_{1};
  std::shared_ptr<EventChannel> event_channel_;
  ViewWindowLogCallback central_log_;
  ViewWindowLibraryChangedCallback library_changed_;
  std::uint64_t generation_ = 1;
};

}  // namespace designpp::gui

#endif  // DESIGNPP_GUI_LAYOUT_WINDOW_IMPL_H_

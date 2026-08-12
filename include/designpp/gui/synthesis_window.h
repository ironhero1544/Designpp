// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_GUI_SYNTHESIS_WINDOW_H_
#define DESIGNPP_GUI_SYNTHESIS_WINDOW_H_

#include <windows.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "designpp/application/library_service.h"
#include "designpp/application/project_service.h"
#include "designpp/application/run_store.h"
#include "designpp/application/synthesis_run_service.h"
#include "designpp/application/workspace.h"
#include "designpp/core/diagnostic.h"
#include "designpp/gui/dpi.h"
#include "designpp/gui/gate_schematic_canvas.h"
#include "designpp/gui/view_window.h"
#include "designpp/runtime/execution_provider.h"
#include "designpp/runtime/task_scheduler.h"

namespace designpp::gui {

class SynthesisWindow final : public ViewWindow {
 public:
  SynthesisWindow() = default;
  SynthesisWindow(const SynthesisWindow&) = delete;
  SynthesisWindow& operator=(const SynthesisWindow&) = delete;
  ~SynthesisWindow() override;

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
  static constexpr UINT kEventMessage = WM_APP + 61;

  static LRESULT CALLBACK WindowProcedure(HWND window, UINT message,
                                          WPARAM wparam, LPARAM lparam);
  LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);
  [[nodiscard]] bool CreateControls();
  void LayoutControls(int width, int height);
  void BeginLoad();
  void HandleEvents();
  void StartSynthesis();
  void ApplyState(application::SynthesisRunState state);
  void AppendOutput(std::wstring_view text);
  void ShowProblems();
  void ShowRuns();
  void ShowReports();
  void ShowArtifacts();
  void ShowScript();
  void UpdateCanvas();
  void ShutdownChannel();
  void PopulateIdentity();

  HINSTANCE instance_ = nullptr;
  HWND window_ = nullptr;
  HWND identity_ = nullptr;
  HWND run_button_ = nullptr;
  HWND cancel_button_ = nullptr;
  HWND reports_button_ = nullptr;
  HWND artifacts_button_ = nullptr;
  HWND script_button_ = nullptr;
  HWND navigator_ = nullptr;
  GateSchematicCanvas canvas_;
  HWND properties_ = nullptr;
  HWND bottom_tabs_ = nullptr;
  HWND output_ = nullptr;
  HWND status_ = nullptr;
  UINT dpi_ = kDefaultDpi;
  UniqueFont font_;

  application::WorkspaceOpenRequest request_;
  application::LibraryRecord library_;
  application::ProjectService project_service_;
  std::unique_ptr<application::ProjectDocument> document_;
  std::vector<application::ResolvedSource> sources_;
  std::vector<application::RunRecord> runs_;
  std::vector<core::Diagnostic> diagnostics_;
  std::shared_ptr<application::RunRecord> active_run_;
  adapters::SynthesisMetrics metrics_;
  adapters::GateSchematic schematic_;
  std::string script_text_;
  runtime::WslExecutionProvider execution_provider_;
  application::SynthesisRunService synthesis_service_{&execution_provider_};
  runtime::TaskScheduler scheduler_{1};
  std::shared_ptr<EventChannel> event_channel_;
  ViewWindowLogCallback central_log_;
  ViewWindowLibraryChangedCallback library_changed_;
  std::uint64_t generation_ = 1;
};

}  // namespace designpp::gui

#endif  // DESIGNPP_GUI_SYNTHESIS_WINDOW_H_

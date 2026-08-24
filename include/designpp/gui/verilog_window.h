// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_GUI_VERILOG_WINDOW_H_
#define DESIGNPP_GUI_VERILOG_WINDOW_H_

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "designpp/adapters/cocotb_runner_adapter.h"
#include "designpp/adapters/icarus_debug_adapter.h"
#include "designpp/adapters/simulation_adapter.h"
#include "designpp/adapters/simulation_result_parser.h"
#include "designpp/application/debug_session_service.h"
#include "designpp/application/library_service.h"
#include "designpp/application/managed_source_service.h"
#include "designpp/application/project_service.h"
#include "designpp/application/run_store.h"
#include "designpp/application/testbench_execution_service.h"
#include "designpp/application/workspace.h"
#include "designpp/application/wsl_gui_execution_service.h"
#include "designpp/core/diagnostic.h"
#include "designpp/gui/dpi.h"
#include "designpp/gui/monaco_editor_host.h"
#include "designpp/gui/view_window.h"
#include "designpp/runtime/file_watch_service.h"
#include "designpp/runtime/process_runner.h"
#include "designpp/runtime/resource_coordinator.h"
#include "designpp/runtime/task_scheduler.h"

namespace designpp::gui {

class VerilogWindow final : public ViewWindow {
 public:
  using LogCallback = ViewWindowLogCallback;
  using LibraryChangedCallback = ViewWindowLibraryChangedCallback;

  VerilogWindow();
  VerilogWindow(const VerilogWindow&) = delete;
  VerilogWindow& operator=(const VerilogWindow&) = delete;
  ~VerilogWindow();

  [[nodiscard]] bool Create(
      HINSTANCE instance, const application::WorkspaceOpenRequest& request,
      application::LibraryRecord library,
      std::shared_ptr<MonacoEditorEnvironment> editor_environment,
      LogCallback central_log, LibraryChangedCallback library_changed);
  [[nodiscard]] bool Matches(std::string_view library_id,
                             std::string_view cell_id) const;
  [[nodiscard]] bool MatchesView(std::string_view library_id,
                                 std::string_view cell_id,
                                 std::string_view view_id) const;
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
  void ActivateView(std::string view_id);
  void RefreshLibrary(application::LibraryRecord library) override;
  [[nodiscard]] bool PrepareClose() override;
  void Close() override;
  [[nodiscard]] bool TranslateAccelerator(const MSG& message) override;

 private:
  struct EventChannel;
  struct SourceTreeNode;
  enum class SplitterDrag { kNone, kLeft, kRight, kBottom };
  enum class ActiveOperation { kNone, kLint, kSimulation, kDebug };
  enum class PendingRun { kNone, kLint, kSimulation, kDebug };

  static constexpr UINT kEventMessage = WM_APP + 41;
  static constexpr UINT kSourceTreeRebuildMessage = WM_APP + 42;
  static constexpr UINT kDebugViewRebuildMessage = WM_APP + 43;
  static LRESULT CALLBACK WindowProcedure(HWND window, UINT message,
                                          WPARAM wparam, LPARAM lparam);
  LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);

  [[nodiscard]] bool CreateControls();
  void LayoutControls(int width, int height);
  [[nodiscard]] SplitterDrag HitTestSplitter(POINT point) const;
  void UpdateSplitter(POINT point);
  void ApplyDpi(UINT dpi);
  void BeginLoad();
  void HandleEvents();
  void ApplyInferredProjectDefaults();
  void PopulateParameterDefaultsForSelection();
  void PopulateProject();
  void PopulateSources();
  void PopulateRuns();
  void PopulateProblems();
  void PopulateArtifacts(const application::RunRecord* run);
  void PopulateTestSummary();
  void UpdateInspector();
  void PopulateTestbenchInspector();
  void ReadTestbenchControls();
  void ShowBottomPage(int index) const;
  void MarkDirty();
  [[nodiscard]] bool ReadProjectControls();
  [[nodiscard]] bool SaveProject();
  void ToggleSource(std::size_t index, bool enabled);
  void AddSourceFiles();
  [[nodiscard]] std::string SelectedSourceViewId() const;
  void OpenSource(std::size_t index);
  void OpenSourceByDiagnostic(const core::Diagnostic& diagnostic);
  void HandleEditorMessage(application::EditorWebMessage message);
  void BeginSaveDocument(std::string document_id, std::string text,
                         bool overwrite_external);
  void DispatchDocumentSave(std::string document_id, std::string text,
                            bool overwrite_external);
  void DispatchNextDocumentSave();
  void RequestSaveAll();
  [[nodiscard]] application::EditorDocumentSnapshot* FindEditorDocument(
      std::string_view document_id);
  void StartLint();
  void HandleProbeComplete(const runtime::ProcessResult& result);
  void StartLintProcess(std::string tool_version);
  void StartSimulation();
  void HandleSimulationProbeComplete(const runtime::ProcessResult& result);
  void StartCocotbMakefilesProbe(std::string tool_version);
  void HandleCocotbMakefilesProbeComplete(const runtime::ProcessResult& result);
  void PrepareSimulation(std::string tool_version);
  void PrepareCocotb(std::string tool_version,
                     std::wstring makefiles_directory);
  void StartSimulationCompile();
  void StartSimulationExecute();
  void StartCocotbExecute();
  void FinishSimulation(const runtime::ProcessResult& result,
                        core::Status status,
                        std::vector<core::Diagnostic> diagnostics,
                        std::shared_ptr<application::RunRecord> run);
  void StartDebug();
  void HandleDebugProbeComplete(const runtime::ProcessResult& result);
  void PrepareDebug(std::string tool_version);
  void StartDebugCompile();
  void StartDebugExecute();
  void HandleDebugOutput(std::string_view output);
  void QueueDebugCommand(application::DebugCommandKind kind,
                         std::string command = {});
  void DispatchNextDebugCommand();
  void FinishDebug(const runtime::ProcessResult& result,
                   std::shared_ptr<application::RunRecord> run);
  void PopulateDebugView();
  void CancelActiveOperation();
  void OpenWaveform();
  void AppendOutput(std::wstring_view text);
  void SetStatus(std::wstring_view text) const;
  void UpdateTitle();
  void ShutdownChannel();

  HINSTANCE instance_ = nullptr;
  HWND window_ = nullptr;
  HWND add_source_button_ = nullptr;
  HWND source_tree_ = nullptr;
  HWND top_module_ = nullptr;
  HWND cpu_budget_ = nullptr;
  HWND include_directories_ = nullptr;
  HWND defines_ = nullptr;
  HWND parameters_ = nullptr;
  HWND constraint_path_ = nullptr;
  HWND active_view_ = nullptr;
  HWND path_preview_ = nullptr;
  HWND editor_container_ = nullptr;
  HWND save_button_ = nullptr;
  HWND lint_button_ = nullptr;
  HWND cancel_button_ = nullptr;
  HWND simulator_label_ = nullptr;
  HWND simulator_backend_ = nullptr;
  HWND dut_top_label_ = nullptr;
  HWND testbench_top_ = nullptr;
  HWND cocotb_module_ = nullptr;
  HWND cocotb_testcase_ = nullptr;
  HWND waveform_checkbox_ = nullptr;
  HWND waveform_format_ = nullptr;
  HWND simulation_run_button_ = nullptr;
  HWND simulation_cancel_button_ = nullptr;
  HWND open_waveform_button_ = nullptr;
  HWND simulation_status_ = nullptr;
  HWND debug_run_button_ = nullptr;
  HWND debug_continue_button_ = nullptr;
  HWND debug_step_button_ = nullptr;
  HWND debug_finish_button_ = nullptr;
  HWND bottom_tabs_ = nullptr;
  HWND problems_ = nullptr;
  HWND runs_ = nullptr;
  HWND artifacts_ = nullptr;
  HWND tests_ = nullptr;
  HWND output_ = nullptr;
  HWND debug_state_label_ = nullptr;
  HWND debug_scope_tree_ = nullptr;
  HWND debug_variables_ = nullptr;
  HWND debug_transcript_ = nullptr;
  HWND debug_command_ = nullptr;
  HWND debug_send_button_ = nullptr;
  HWND status_ = nullptr;
  UINT dpi_ = kDefaultDpi;
  int left_width_ = 0;
  int right_width_ = 0;
  int bottom_height_ = 0;
  SplitterDrag splitter_drag_ = SplitterDrag::kNone;
  UniqueFont font_;

  application::WorkspaceOpenRequest request_;
  application::LibraryRecord library_;
  application::ProjectService project_service_{};
  application::ManagedSourceService managed_source_service_{};
  application::RunStore run_store_{};
  std::unique_ptr<application::ProjectDocument> document_;
  std::vector<application::ResolvedSource> sources_;
  std::vector<std::unique_ptr<SourceTreeNode>> source_tree_nodes_;
  std::vector<std::string> module_candidates_;
  std::unordered_map<std::string, std::vector<std::string>>
      module_parameter_defaults_;
  std::vector<application::RunRecord> run_records_;
  std::unordered_map<std::string, adapters::SimulationTestSummary>
      run_test_summaries_;
  std::vector<core::Diagnostic> diagnostics_;
  std::vector<application::EditorDocumentSnapshot> editor_documents_;
  std::unordered_set<std::string> dirty_document_ids_;
  std::unordered_set<std::string> pending_close_document_ids_;
  std::unordered_set<std::string> saving_document_ids_;
  std::unordered_map<std::string, std::string> pending_save_text_;
  std::unordered_map<std::string, std::string> last_document_by_view_;
  std::unordered_map<std::string, std::vector<std::string>>
      document_module_candidates_;
  struct PendingDocumentSave {
    std::string document_id;
    std::string text;
    bool overwrite_external = false;
  };
  std::deque<PendingDocumentSave> document_save_queue_;
  std::optional<core::Diagnostic> pending_reveal_;
  std::shared_ptr<application::RunRecord> active_run_;
  std::optional<adapters::SimulationPlan> simulation_plan_;
  std::optional<adapters::CocotbPlan> cocotb_plan_;
  std::optional<adapters::SimulationTestSummary> simulation_summary_;
  std::optional<adapters::DebugPlan> debug_plan_;
  adapters::IcarusDebugProtocolParser debug_parser_;
  application::DebugSessionService debug_session_;
  runtime::WslExecutionProvider execution_provider_;
  application::TestbenchExecutionService testbench_execution_{
      &execution_provider_};
  application::WslGuiExecutionService viewer_execution_{&execution_provider_};
  struct PendingDebugCommand {
    application::DebugCommandKind kind =
        application::DebugCommandKind::kConsole;
    std::string command;
  };
  std::deque<PendingDebugCommand> debug_command_queue_;
  bool debug_command_pending_ = false;
  std::string debug_evaluation_target_;
  std::unordered_map<std::string, std::string> debug_values_;
  std::vector<std::string> debug_variable_names_;
  std::chrono::steady_clock::time_point simulation_started_{};
  std::string active_document_id_;
  std::string simulation_output_;
  std::filesystem::path last_waveform_;
  std::unique_ptr<runtime::ProcessSession> process_;
  runtime::ResourceCoordinator resource_coordinator_;
  std::unique_ptr<runtime::CpuTokenLease> cpu_lease_;
  std::string simulation_backend_id_ = "icarus";
  std::string simulation_runner_id_ = "hdl";
  std::string simulation_tool_version_;
  std::unique_ptr<runtime::CpuTokenLease> viewer_cpu_lease_;
  runtime::TaskScheduler scheduler_{2};
  runtime::FileWatchService file_watch_service_;
  MonacoEditorHost editor_host_;
  std::shared_ptr<MonacoEditorEnvironment> editor_environment_;
  std::shared_ptr<EventChannel> event_channel_;
  LogCallback central_log_;
  LibraryChangedCallback library_changed_;
  std::uint64_t generation_ = 1;
  bool applying_controls_ = false;
  bool source_tree_rebuild_pending_ = false;
  int common_control_notification_depth_ = 0;
  bool debug_view_rebuild_pending_ = false;
  bool dirty_ = false;
  bool close_prepared_ = false;
  ActiveOperation active_operation_ = ActiveOperation::kNone;
  PendingRun pending_run_ = PendingRun::kNone;
  bool save_project_after_documents_ = false;
  bool close_after_save_ = false;
};

}  // namespace designpp::gui

#endif  // DESIGNPP_GUI_VERILOG_WINDOW_H_

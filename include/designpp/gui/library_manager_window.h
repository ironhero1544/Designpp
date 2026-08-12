// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_GUI_LIBRARY_MANAGER_WINDOW_H_
#define DESIGNPP_GUI_LIBRARY_MANAGER_WINDOW_H_

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "designpp/application/library_browser_model.h"
#include "designpp/application/library_service.h"
#include "designpp/gui/dpi.h"
#include "designpp/gui/tool_check_window.h"
#include "designpp/gui/workspace_registry.h"
#include "designpp/runtime/process_runner.h"
#include "designpp/runtime/setup_catalog.h"
#include "designpp/runtime/task_scheduler.h"
#include "designpp/runtime/tool_catalog.h"

namespace designpp::gui {

// Owns the Library Manager main window and its global operation log.
class LibraryManagerWindow final {
 public:
  LibraryManagerWindow();
  LibraryManagerWindow(const LibraryManagerWindow&) = delete;
  LibraryManagerWindow& operator=(const LibraryManagerWindow&) = delete;
  ~LibraryManagerWindow();

  // Creates and shows the main Library Manager window.
  [[nodiscard]] bool Create(HINSTANCE instance, int show_command);

  // Runs the application message loop until the main window closes.
  [[nodiscard]] int RunMessageLoop() const;

 private:
  enum class Operation {
    kIdle,
    kCheckingTools,
    kSettingUpWsl,
    kInstallingToolchain,
    kRemovingToolchain,
    kInstallingTool,
    kRemovingTool,
    kCancelling,
  };

  struct EventChannel;
  struct NavigationTag;
  struct BrowserPane;

  struct ToolState {
    std::wstring status;
    std::wstring version;
  };

  struct ActiveTask {
    std::uint64_t id;
    std::optional<std::size_t> tool_index;
    std::wstring title;
    runtime::OutputEncoding output_encoding;
    std::unique_ptr<runtime::ProcessSession> session;
  };

  static constexpr UINT kEventsReadyMessage = WM_APP + 20;

  static LRESULT CALLBACK WindowProcedure(HWND window, UINT message,
                                          WPARAM wparam, LPARAM lparam);
  LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);

  [[nodiscard]] bool CreateControls();
  void ConfigureLibraryList();
  void LayoutControls(int width, int height) const;
  void ApplyDpi(UINT dpi);
  void OpenToolCheck();
  void OpenWorkspace(std::string_view library_id, std::string_view cell_id,
                     std::string_view view_id);
  void SelectLibraryRoot();
  void RefreshLibraries();
  void RebuildLibraryControls();
  void PopulateLibraryList();
  void HandleLibraryTreeSelection();
  void HandleLibraryListDoubleClick();
  void ShowLibraryContextMenu(POINT screen_point);
  void CreateLibraryItem(int command_id);
  void EditLibraryItem();
  void DeleteLibraryItem();
  void ImportLibraryFiles();
  void SubmitLibraryRefresh(std::wstring action);
  void SubmitLibraryOperation(std::wstring action,
                              std::function<core::Status()> operation);
  void UpdateLibraryCommandState() const;
  [[nodiscard]] bool CreateBrowserPanes();
  void LayoutBrowserPanes(int width, int height) const;
  [[nodiscard]] std::optional<std::size_t> HitTestBrowserSplitter(int x,
                                                                  int y) const;
  void ScheduleBrowserFilter();
  void SubmitBrowserFilter();
  void ApplyBrowserResults(std::uint64_t generation,
                           std::vector<application::BrowserResult> results);
  void HandleBrowserSelection(std::size_t pane_index);
  void ActivateBrowserPane(std::size_t pane_index, bool prefer_exact_match);
  void CreateFromBrowser(std::size_t pane_index, std::string name);
  void PopulateBrowserQuery(std::size_t pane_index);
  [[nodiscard]] std::optional<std::size_t> FindBrowserPane(HWND window) const;
  static LRESULT CALLBACK SearchEditProcedure(HWND window, UINT message,
                                              WPARAM wparam, LPARAM lparam,
                                              UINT_PTR subclass_id,
                                              DWORD_PTR reference_data);
  void StartToolCheck();
  void StartToolInstall(std::size_t tool_index);
  void StartToolRemove(std::size_t tool_index);
  void StartToolchainRemove();
  void StartWslSetup();
  void StartToolchainSetup();
  void StartPendingToolProbes();
  void StartNextSetupStep();
  void StartTask(runtime::ProcessRequest request, std::wstring title,
                 runtime::OutputEncoding output_encoding,
                 std::optional<std::size_t> tool_index,
                 bool requires_elevation = false);
  void CancelOperation();
  void HandleQueuedEvents();
  void HandleTaskCompletion(std::uint64_t task_id,
                            const runtime::ProcessResult& result);
  void FinishOperation(std::wstring status);
  void SetBusyControls(bool busy) const;
  void SetStatus(std::wstring_view status) const;
  void SetToolState(std::size_t index, std::wstring status,
                    std::wstring version);
  void AppendLog(std::wstring_view text) const;
  void ShutdownEventChannel();

  HINSTANCE instance_ = nullptr;
  HWND window_ = nullptr;
  HWND library_tree_ = nullptr;
  HWND library_list_ = nullptr;
  HWND progress_ = nullptr;
  HWND status_ = nullptr;
  HWND log_ = nullptr;
  UINT dpi_ = kDefaultDpi;
  UniqueFont font_;
  std::unique_ptr<ToolCheckWindow> tool_check_window_;
  std::unique_ptr<WorkspaceRegistry> workspace_registry_;
  application::LibraryService library_service_;
  runtime::TaskScheduler library_scheduler_{2};
  std::vector<application::LibraryRecord> libraries_;
  std::shared_ptr<const std::vector<application::LibraryRecord>>
      library_snapshot_;
  std::array<std::unique_ptr<BrowserPane>, 3> browser_panes_;
  std::array<double, 2> browser_split_ratios_{1.0 / 3.0, 2.0 / 3.0};
  std::optional<std::size_t> dragged_browser_splitter_;
  std::uint64_t browser_generation_ = 0;
  bool applying_browser_results_ = false;
  std::array<std::optional<std::string>, 3> pending_browser_selection_;
  std::string selected_library_id_;
  std::string selected_cell_id_;
  std::string selected_view_id_;
  std::string pending_create_name_;
  std::optional<core::ViewKind> pending_create_kind_;
  std::vector<std::unique_ptr<NavigationTag>> navigation_tags_;
  std::optional<std::size_t> selected_library_;
  std::optional<std::size_t> selected_cell_;

  Operation operation_ = Operation::kIdle;
  std::vector<runtime::ToolDefinition> tools_;
  std::vector<ToolState> tool_states_;
  std::vector<runtime::SetupStep> setup_steps_;
  std::vector<ActiveTask> active_tasks_;
  std::shared_ptr<EventChannel> event_channel_;
  std::unordered_map<std::uint64_t, std::string> decoder_remainders_;
  std::size_t next_tool_index_ = 0;
  std::size_t next_setup_step_ = 0;
  std::size_t completed_work_ = 0;
  std::size_t failed_work_ = 0;
  std::size_t maximum_parallel_probes_ = 1;
  std::uint64_t next_task_id_ = 1;
};

}  // namespace designpp::gui

#endif  // DESIGNPP_GUI_LIBRARY_MANAGER_WINDOW_H_

// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_GUI_CONSTRAINTS_WINDOW_H_
#define DESIGNPP_GUI_CONSTRAINTS_WINDOW_H_

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "designpp/application/library_service.h"
#include "designpp/application/managed_source_service.h"
#include "designpp/application/workspace.h"
#include "designpp/gui/dpi.h"
#include "designpp/gui/monaco_editor_host.h"
#include "designpp/gui/view_window.h"
#include "designpp/runtime/task_scheduler.h"

namespace designpp::gui {

// Owns the dedicated editor for Cell-scoped managed SDC files.
class ConstraintsWindow final : public ViewWindow {
 public:
  using LogCallback = ViewWindowLogCallback;
  using LibraryChangedCallback = ViewWindowLibraryChangedCallback;

  ConstraintsWindow();
  ConstraintsWindow(const ConstraintsWindow&) = delete;
  ConstraintsWindow& operator=(const ConstraintsWindow&) = delete;
  ~ConstraintsWindow();

  [[nodiscard]] bool Create(
      HINSTANCE instance, const application::WorkspaceOpenRequest& request,
      application::LibraryRecord library,
      std::shared_ptr<MonacoEditorEnvironment> editor_environment,
      LogCallback central_log, LibraryChangedCallback library_changed);

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
  struct FileEntry;

  static constexpr UINT kEventMessage = WM_APP + 71;
  static LRESULT CALLBACK WindowProcedure(HWND window, UINT message,
                                          WPARAM wparam, LPARAM lparam);
  LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);

  [[nodiscard]] bool CreateControls();
  void LayoutControls(int width, int height) const;
  void ApplyDpi(UINT dpi);
  void PopulateFiles();
  void OpenSelectedFile();
  void OpenFile(std::size_t index);
  void HandleEditorMessage(application::EditorWebMessage message);
  void SaveDocument(std::string document_id, std::string text,
                    bool overwrite_external);
  void HandleEvents();
  void UpdateDetails();
  void UpdateTitle();
  void SetStatus(std::wstring_view text) const;
  void ShutdownChannel();
  [[nodiscard]] application::EditorDocumentSnapshot* FindDocument(
      std::string_view document_id);

  HINSTANCE instance_ = nullptr;
  HWND window_ = nullptr;
  HWND identity_ = nullptr;
  HWND save_button_ = nullptr;
  HWND file_list_ = nullptr;
  HWND editor_container_ = nullptr;
  HWND details_ = nullptr;
  HWND status_ = nullptr;
  UINT dpi_ = kDefaultDpi;
  UniqueFont font_;

  application::WorkspaceOpenRequest request_;
  application::LibraryRecord library_;
  std::shared_ptr<MonacoEditorEnvironment> editor_environment_;
  LogCallback central_log_;
  LibraryChangedCallback library_changed_;
  MonacoEditorHost editor_host_;
  runtime::TaskScheduler scheduler_{2};
  std::shared_ptr<EventChannel> event_channel_;
  std::vector<FileEntry> files_;
  std::vector<application::EditorDocumentSnapshot> documents_;
  std::unordered_set<std::string> loading_paths_;
  std::unordered_set<std::string> saving_documents_;
  std::unordered_set<std::string> dirty_documents_;
  std::string active_document_id_;
  std::uint64_t generation_ = 1;
  bool editor_ready_ = false;
  bool close_after_save_ = false;
};

}  // namespace designpp::gui

#endif  // DESIGNPP_GUI_CONSTRAINTS_WINDOW_H_

// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_GUI_LIBERTY_WINDOW_H_
#define DESIGNPP_GUI_LIBERTY_WINDOW_H_

#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "designpp/application/library_service.h"
#include "designpp/application/managed_source_service.h"
#include "designpp/gui/dpi.h"
#include "designpp/gui/monaco_editor_host.h"
#include "designpp/runtime/task_scheduler.h"

namespace designpp::gui {

// Presents and manages Library-scoped Liberty technology files.
class LibertyWindow final {
 public:
  using LogCallback = std::function<void(std::wstring)>;
  using LibraryChangedCallback = std::function<void()>;

  LibertyWindow() = default;
  LibertyWindow(const LibertyWindow&) = delete;
  LibertyWindow& operator=(const LibertyWindow&) = delete;
  ~LibertyWindow();

  [[nodiscard]] bool Create(
      HINSTANCE instance, application::LibraryRecord library,
      std::shared_ptr<MonacoEditorEnvironment> editor_environment,
      LogCallback central_log, LibraryChangedCallback library_changed);
  [[nodiscard]] bool BelongsToLibrary(
      std::string_view library_id) const noexcept;
  [[nodiscard]] bool IsOpen() const noexcept;
  void Activate();
  void RefreshLibrary(application::LibraryRecord library);
  void Close();
  [[nodiscard]] bool TranslateAccelerator(const MSG& message);

 private:
  struct EventChannel;
  static constexpr UINT kEventMessage = WM_APP + 72;

  static LRESULT CALLBACK WindowProcedure(HWND window, UINT message,
                                          WPARAM wparam, LPARAM lparam);
  LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);
  [[nodiscard]] bool CreateControls();
  void LayoutControls(int width, int height) const;
  void ApplyDpi(UINT dpi);
  void PopulateFiles();
  void UpdateCommandState() const;
  [[nodiscard]] bool PrepareForLibraryMutation();
  void AddFiles();
  void ReplaceSelectedFile();
  void RemoveSelectedFile();
  void SubmitLibraryMutation(
      std::wstring action,
      std::function<core::Result<application::LibraryRecord>(
          application::LibraryService&)>
          mutation);
  void OpenSelectedFile();
  void OpenFile(std::size_t index);
  void HandleEditorMessage(application::EditorWebMessage message);
  void SaveDocument(std::string document_id, std::string text,
                    bool overwrite_external);
  void HandleEvents();
  void UpdateTitle();
  void SetStatus(std::wstring_view text) const;
  void ShutdownChannel();
  [[nodiscard]] application::EditorDocumentSnapshot* FindDocument(
      std::string_view document_id);
  [[nodiscard]] bool PrepareClose();

  HINSTANCE instance_ = nullptr;
  HWND window_ = nullptr;
  HWND identity_ = nullptr;
  HWND add_button_ = nullptr;
  HWND replace_button_ = nullptr;
  HWND remove_button_ = nullptr;
  HWND save_button_ = nullptr;
  HWND file_list_ = nullptr;
  HWND editor_container_ = nullptr;
  HWND status_ = nullptr;
  UINT dpi_ = kDefaultDpi;
  UniqueFont font_;

  application::LibraryRecord library_;
  std::shared_ptr<MonacoEditorEnvironment> editor_environment_;
  LogCallback central_log_;
  LibraryChangedCallback library_changed_;
  MonacoEditorHost editor_host_;
  // Document reads and Library mutations use independent bounded queues. A
  // large Liberty parse must never delay Add, Replace, Remove, or Save
  // operations.
  runtime::TaskScheduler document_scheduler_{1};
  runtime::TaskScheduler mutation_scheduler_{1};
  std::shared_ptr<EventChannel> event_channel_;
  std::vector<std::string> files_;
  std::vector<application::EditorDocumentSnapshot> documents_;
  std::unordered_set<std::string> loading_paths_;
  std::unordered_set<std::string> saving_documents_;
  std::unordered_set<std::string> dirty_documents_;
  std::string active_document_id_;
  std::uint64_t generation_ = 1;
  bool editor_ready_ = false;
  bool mutation_active_ = false;
  bool close_after_save_ = false;
};

}  // namespace designpp::gui

#endif  // DESIGNPP_GUI_LIBERTY_WINDOW_H_

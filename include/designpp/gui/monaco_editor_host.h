// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_GUI_MONACO_EDITOR_HOST_H_
#define DESIGNPP_GUI_MONACO_EDITOR_HOST_H_

#include <windows.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "designpp/application/editor_protocol.h"
#include "designpp/application/managed_source_service.h"
#include "designpp/core/diagnostic.h"
#include "designpp/core/status.h"

namespace designpp::gui {

class MonacoEditorEnvironment final {
 public:
  MonacoEditorEnvironment(const MonacoEditorEnvironment&) = delete;
  MonacoEditorEnvironment& operator=(const MonacoEditorEnvironment&) = delete;
  ~MonacoEditorEnvironment();

  [[nodiscard]] static std::shared_ptr<MonacoEditorEnvironment> Create();

 private:
  struct Implementation;
  explicit MonacoEditorEnvironment(
      std::unique_ptr<Implementation> implementation);
  std::unique_ptr<Implementation> implementation_;

  friend class MonacoEditorHost;
};

class MonacoEditorHost final {
 public:
  using MessageCallback = std::function<void(application::EditorWebMessage)>;
  using StatusCallback = std::function<void(core::Status)>;

  MonacoEditorHost();
  MonacoEditorHost(const MonacoEditorHost&) = delete;
  MonacoEditorHost& operator=(const MonacoEditorHost&) = delete;
  ~MonacoEditorHost();

  [[nodiscard]] bool Create(
      HWND parent, std::shared_ptr<MonacoEditorEnvironment> environment,
      std::string session_id, MessageCallback message_callback,
      StatusCallback status_callback);
  void Resize(const RECT& bounds) const;
  [[nodiscard]] bool Ready() const noexcept;
  [[nodiscard]] bool ContainsFocus() const noexcept;
  void OpenDocument(const application::EditorDocumentSnapshot& document,
                    std::string_view display_name, std::string_view language);
  void CloseDocument(std::string_view document_id);
  void SaveResult(std::string_view document_id, bool succeeded,
                  std::string_view message);
  void SetDiagnostics(
      const std::vector<core::Diagnostic>& diagnostics,
      const std::vector<application::EditorDocumentSnapshot>& documents);
  void RevealLocation(std::string_view document_id, std::uint32_t line,
                      std::uint32_t column);
  void SetReadOnly(std::string_view document_id, bool read_only);
  void RequestSaveAll();
  void Shutdown();

 private:
  struct Implementation;
  std::unique_ptr<Implementation> implementation_;
};

}  // namespace designpp::gui

#endif  // DESIGNPP_GUI_MONACO_EDITOR_HOST_H_

// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_GUI_WORKSPACE_REGISTRY_H_
#define DESIGNPP_GUI_WORKSPACE_REGISTRY_H_

#include <windows.h>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "designpp/application/library_service.h"
#include "designpp/application/workspace.h"
#include "designpp/gui/monaco_editor_host.h"
#include "designpp/gui/view_window.h"

namespace designpp::gui {

class WorkspaceRegistry final {
 public:
  using LogCallback = ViewWindowLogCallback;
  using LibraryChangedCallback = ViewWindowLibraryChangedCallback;

  WorkspaceRegistry(HINSTANCE instance, LogCallback central_log,
                    LibraryChangedCallback library_changed);
  WorkspaceRegistry(const WorkspaceRegistry&) = delete;
  WorkspaceRegistry& operator=(const WorkspaceRegistry&) = delete;
  ~WorkspaceRegistry();

  [[nodiscard]] bool Open(const application::WorkspaceOpenRequest& request,
                          const application::LibraryRecord& library);
  [[nodiscard]] bool PrepareCloseAll();
  void CloseAll();
  [[nodiscard]] bool TranslateAccelerator(const MSG& message) const;
  void RefreshLibrary(const application::LibraryRecord& library);
  [[nodiscard]] bool CloseFor(std::string_view library_id,
                              std::string_view cell_id);

 private:
  void RemoveClosed();

  HINSTANCE instance_ = nullptr;
  LogCallback central_log_;
  LibraryChangedCallback library_changed_;
  std::shared_ptr<MonacoEditorEnvironment> editor_environment_;
  std::vector<std::unique_ptr<ViewWindow>> windows_;
};

}  // namespace designpp::gui

#endif  // DESIGNPP_GUI_WORKSPACE_REGISTRY_H_

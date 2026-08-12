// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_GUI_VIEW_WINDOW_FACTORY_H_
#define DESIGNPP_GUI_VIEW_WINDOW_FACTORY_H_

#include <windows.h>

#include <memory>

#include "designpp/application/library_service.h"
#include "designpp/application/workspace.h"
#include "designpp/gui/monaco_editor_host.h"
#include "designpp/gui/view_window.h"

namespace designpp::gui {

struct ViewWindowDependencies {
  HINSTANCE instance = nullptr;
  std::shared_ptr<MonacoEditorEnvironment> editor_environment;
  ViewWindowLogCallback central_log;
  ViewWindowLibraryChangedCallback library_changed;
};

// Creates the concrete top-level tool window registered for view_kind.
[[nodiscard]] std::unique_ptr<ViewWindow> CreateViewWindow(
    const ViewWindowDependencies& dependencies,
    const application::WorkspaceOpenRequest& request,
    const application::LibraryRecord& library, core::ViewKind view_kind);

}  // namespace designpp::gui

#endif  // DESIGNPP_GUI_VIEW_WINDOW_FACTORY_H_

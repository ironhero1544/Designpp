// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_GUI_LAYOUT_JSON_EDITOR_H_
#define DESIGNPP_GUI_LAYOUT_JSON_EDITOR_H_

#include <windows.h>

#include <functional>
#include <memory>

#include "designpp/core/project.h"
#include "designpp/gui/layout_setup_controller.h"

namespace designpp::gui {

class MonacoEditorEnvironment;

// Opens a session-owned Monaco editor without a nested message loop for the
// typed physical implementation
// configuration. Apply updates the caller's setup draft; persistence remains
// owned by the parent Layout Setup dialog.
[[nodiscard]] std::unique_ptr<LayoutJsonSession> ShowLayoutJsonEditor(
    HWND owner, std::shared_ptr<MonacoEditorEnvironment> editor_environment,
    const core::PhysicalImplementationConfiguration& configuration,
    LayoutJsonApplyCallback apply_configuration);

}  // namespace designpp::gui

#endif  // DESIGNPP_GUI_LAYOUT_JSON_EDITOR_H_

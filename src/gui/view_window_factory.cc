// Copyright 2026 The Design++ Authors

#include "designpp/gui/view_window_factory.h"

#include <array>
#include <memory>

#include "designpp/gui/constraints_window.h"
#include "designpp/gui/layout_json_editor.h"
#include "designpp/gui/layout_window.h"
#include "designpp/gui/synthesis_window.h"
#include "designpp/gui/timing_window.h"
#include "designpp/gui/verilog_window.h"

namespace designpp::gui {
namespace {

using WindowCreator = std::unique_ptr<ViewWindow> (*)(
    const ViewWindowDependencies&, const application::WorkspaceOpenRequest&,
    const application::LibraryRecord&);

std::unique_ptr<ViewWindow> CreateVerilogWindow(
    const ViewWindowDependencies& dependencies,
    const application::WorkspaceOpenRequest& request,
    const application::LibraryRecord& library) {
  auto window = std::make_unique<VerilogWindow>();
  if (!window->Create(dependencies.instance, request, library,
                      dependencies.editor_environment, dependencies.central_log,
                      dependencies.library_changed)) {
    return nullptr;
  }
  return window;
}

std::unique_ptr<ViewWindow> CreateConstraintsWindow(
    const ViewWindowDependencies& dependencies,
    const application::WorkspaceOpenRequest& request,
    const application::LibraryRecord& library) {
  auto window = std::make_unique<ConstraintsWindow>();
  if (!window->Create(dependencies.instance, request, library,
                      dependencies.editor_environment, dependencies.central_log,
                      dependencies.library_changed)) {
    return nullptr;
  }
  return window;
}

std::unique_ptr<ViewWindow> CreateSynthesisWindow(
    const ViewWindowDependencies& dependencies,
    const application::WorkspaceOpenRequest& request,
    const application::LibraryRecord& library) {
  auto window = std::make_unique<SynthesisWindow>();
  if (!window->Create(dependencies.instance, request, library,
                      dependencies.central_log, dependencies.library_changed)) {
    return nullptr;
  }
  return window;
}

std::unique_ptr<ViewWindow> CreateTimingWindow(
    const ViewWindowDependencies& dependencies,
    const application::WorkspaceOpenRequest& request,
    const application::LibraryRecord& library) {
  auto window = std::make_unique<TimingWindow>();
  if (!window->Create(dependencies.instance, request, library,
                      dependencies.central_log, dependencies.library_changed,
                      dependencies.open_view)) {
    return nullptr;
  }
  return window;
}

std::unique_ptr<ViewWindow> CreateLayoutWindow(
    const ViewWindowDependencies& dependencies,
    const application::WorkspaceOpenRequest& request,
    const application::LibraryRecord& library) {
  auto window = std::make_unique<LayoutWindow>();
  if (!window->Create(
          dependencies.instance, request, library, dependencies.central_log,
          dependencies.library_changed,
          [environment = dependencies.editor_environment](
              HWND owner,
              const core::PhysicalImplementationConfiguration& configuration,
              LayoutJsonApplyCallback save_configuration) {
            return ShowLayoutJsonEditor(owner, environment, configuration,
                                        save_configuration);
          })) {
    return nullptr;
  }
  return window;
}

struct WindowRegistration {
  core::ViewKind view_kind;
  WindowCreator create;
};

constexpr std::array<WindowRegistration, 6> kWindowRegistrations{{
    {core::ViewKind::kVerilog, CreateVerilogWindow},
    {core::ViewKind::kTestbench, CreateVerilogWindow},
    {core::ViewKind::kConstraints, CreateConstraintsWindow},
    {core::ViewKind::kSynthesis, CreateSynthesisWindow},
    {core::ViewKind::kTiming, CreateTimingWindow},
    {core::ViewKind::kLayout, CreateLayoutWindow},
}};

}  // namespace

std::unique_ptr<ViewWindow> CreateViewWindow(
    const ViewWindowDependencies& dependencies,
    const application::WorkspaceOpenRequest& request,
    const application::LibraryRecord& library, core::ViewKind view_kind) {
  for (const WindowRegistration& registration : kWindowRegistrations) {
    if (registration.view_kind == view_kind) {
      return registration.create(dependencies, request, library);
    }
  }
  return nullptr;
}

}  // namespace designpp::gui

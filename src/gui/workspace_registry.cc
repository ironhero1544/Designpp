// Copyright 2026 The Design++ Authors

#include "designpp/gui/workspace_registry.h"

#include <algorithm>
#include <utility>

#include "designpp/gui/view_window_factory.h"

namespace designpp::gui {
namespace {

bool SameWorkspace(const application::WorkspaceOpenRequest& left,
                   const application::WorkspaceOpenRequest& right) {
  return left.library_id == right.library_id && left.cell_id == right.cell_id &&
         left.view_id == right.view_id;
}

}  // namespace

WorkspaceRegistry::WorkspaceRegistry(HINSTANCE instance,
                                     LogCallback central_log,
                                     LibraryChangedCallback library_changed)
    : instance_(instance),
      central_log_(std::move(central_log)),
      library_changed_(std::move(library_changed)),
      editor_environment_(MonacoEditorEnvironment::Create()) {}

WorkspaceRegistry::~WorkspaceRegistry() { CloseAll(); }

bool WorkspaceRegistry::Open(const application::WorkspaceOpenRequest& request,
                             const application::LibraryRecord& library) {
  RemoveClosed();
  // A callback may carry a stale LibraryRecord.  Never resolve a Cell ID from
  // a different Library snapshot: on disk the pair determines the project
  // directory and therefore the physical setup/run namespace.
  if (request.library_id != library.library.id) return false;
  const auto cell =
      std::find_if(library.library.cells.begin(), library.library.cells.end(),
                   [&request](const core::Cell& value) {
                     return value.id == request.cell_id;
                   });
  if (cell == library.library.cells.end()) {
    return false;
  }
  const auto view = std::find_if(cell->views.begin(), cell->views.end(),
                                 [&request](const core::View& value) {
                                   return value.id == request.view_id;
                                 });
  if (view == cell->views.end()) return false;
  const auto existing = std::find_if(
      windows_.begin(), windows_.end(), [&request](const WindowEntry& entry) {
        return SameWorkspace(entry.request, request);
      });
  if (existing != windows_.end()) {
    existing->window->Activate(request);
    return true;
  }
  ViewWindowDependencies dependencies{
      instance_, editor_environment_, central_log_, library_changed_,
      [this](const application::WorkspaceOpenRequest& sibling_request,
             const application::LibraryRecord& sibling_library) {
        return Open(sibling_request, sibling_library);
      }};
  std::unique_ptr<ViewWindow> window =
      CreateViewWindow(dependencies, request, library, view->kind);
  if (!window) return false;
  windows_.push_back({request, std::move(window)});
  return true;
}

bool WorkspaceRegistry::OpenLiberty(const application::LibraryRecord& library) {
  std::erase_if(liberty_windows_,
                [](const auto& window) { return !window->IsOpen(); });
  const auto existing =
      std::find_if(liberty_windows_.begin(), liberty_windows_.end(),
                   [&library](const auto& window) {
                     return window->BelongsToLibrary(library.library.id);
                   });
  if (existing != liberty_windows_.end()) {
    (*existing)->RefreshLibrary(library);
    (*existing)->Activate();
    return true;
  }
  auto window = std::make_unique<LibertyWindow>();
  if (!window->Create(instance_, library, editor_environment_, central_log_,
                      library_changed_)) {
    return false;
  }
  liberty_windows_.push_back(std::move(window));
  return true;
}

bool WorkspaceRegistry::PrepareCloseAll() {
  RemoveClosed();
  for (const WindowEntry& entry : windows_) {
    if (!entry.window->PrepareClose()) return false;
  }
  return true;
}

void WorkspaceRegistry::CloseAll() {
  for (const WindowEntry& entry : windows_) entry.window->Close();
  windows_.clear();
  for (const auto& window : liberty_windows_) window->Close();
  liberty_windows_.clear();
}

bool WorkspaceRegistry::TranslateAccelerator(const MSG& message) const {
  for (const WindowEntry& entry : windows_) {
    if (entry.window->TranslateAccelerator(message)) return true;
  }
  for (const auto& window : liberty_windows_) {
    if (window->TranslateAccelerator(message)) return true;
  }
  return false;
}

void WorkspaceRegistry::RefreshLibrary(
    const application::LibraryRecord& library) {
  RemoveClosed();
  for (const WindowEntry& entry : windows_) {
    entry.window->RefreshLibrary(library);
  }
  for (const auto& window : liberty_windows_) {
    window->RefreshLibrary(library);
  }
}

bool WorkspaceRegistry::CloseFor(std::string_view library_id,
                                 std::string_view cell_id) {
  RemoveClosed();
  std::vector<ViewWindow*> targets;
  for (const WindowEntry& entry : windows_) {
    const bool matches = cell_id.empty()
                             ? entry.request.library_id == library_id
                             : entry.request.library_id == library_id &&
                                   entry.request.cell_id == cell_id;
    if (matches) targets.push_back(entry.window.get());
  }
  for (ViewWindow* window : targets) {
    if (!window->PrepareClose()) return false;
  }
  for (ViewWindow* window : targets) window->Close();
  if (cell_id.empty()) {
    for (const auto& window : liberty_windows_) {
      if (window->BelongsToLibrary(library_id)) window->Close();
    }
  }
  RemoveClosed();
  return true;
}

void WorkspaceRegistry::RemoveClosed() {
  std::erase_if(windows_, [](const WindowEntry& entry) {
    return !entry.window->IsOpen();
  });
  std::erase_if(liberty_windows_,
                [](const auto& window) { return !window->IsOpen(); });
}

}  // namespace designpp::gui

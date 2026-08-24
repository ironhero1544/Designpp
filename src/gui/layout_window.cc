// Copyright 2026 The Design++ Authors

#include "designpp/gui/layout_window.h"

#include <utility>

namespace designpp::gui {

bool LayoutWindow::Create(HINSTANCE instance,
                          const application::WorkspaceOpenRequest& request,
                          application::LibraryRecord library,
                          ViewWindowLogCallback central_log,
                          ViewWindowLibraryChangedCallback library_changed) {
  return implementation_.Create(instance, request, std::move(library),
                                std::move(central_log),
                                std::move(library_changed));
}

ViewWindowKind LayoutWindow::Kind() const noexcept {
  return ViewWindowKind::kLayout;
}

bool LayoutWindow::CanActivate(const application::WorkspaceOpenRequest& request,
                               core::ViewKind view_kind) const {
  return implementation_.CanActivate(request, view_kind);
}

void LayoutWindow::Activate(const application::WorkspaceOpenRequest& request) {
  implementation_.Activate(request);
}

bool LayoutWindow::BelongsToLibrary(std::string_view library_id) const {
  return implementation_.BelongsToLibrary(library_id);
}

bool LayoutWindow::MatchesCell(std::string_view library_id,
                               std::string_view cell_id) const {
  return implementation_.MatchesCell(library_id, cell_id);
}

bool LayoutWindow::IsOpen() const noexcept { return implementation_.IsOpen(); }

void LayoutWindow::RefreshLibrary(application::LibraryRecord library) {
  implementation_.RefreshLibrary(std::move(library));
}

bool LayoutWindow::PrepareClose() { return implementation_.PrepareClose(); }

void LayoutWindow::Close() { implementation_.Close(); }

bool LayoutWindow::TranslateAccelerator(const MSG& message) {
  return implementation_.TranslateAccelerator(message);
}

}  // namespace designpp::gui

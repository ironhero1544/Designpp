// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_GUI_VIEW_WINDOW_H_
#define DESIGNPP_GUI_VIEW_WINDOW_H_

#include <windows.h>

#include <functional>
#include <string>
#include <string_view>

#include "designpp/application/library_service.h"
#include "designpp/application/workspace.h"
#include "designpp/core/library.h"

namespace designpp::gui {

enum class ViewWindowKind {
  kVerilog,
  kSynthesis,
  kTiming,
  kLayout,
  kReport,
};

using ViewWindowLogCallback = std::function<void(std::wstring)>;
using ViewWindowLibraryChangedCallback = std::function<void()>;

// Owns one top-level tool window. Implementations define which CellViews they
// can activate; the registry never switches on a concrete window class.
class ViewWindow {
 public:
  ViewWindow() = default;
  ViewWindow(const ViewWindow&) = delete;
  ViewWindow& operator=(const ViewWindow&) = delete;
  virtual ~ViewWindow() = default;

  [[nodiscard]] virtual ViewWindowKind Kind() const noexcept = 0;
  [[nodiscard]] virtual bool CanActivate(
      const application::WorkspaceOpenRequest& request,
      core::ViewKind view_kind) const = 0;
  virtual void Activate(const application::WorkspaceOpenRequest& request) = 0;
  [[nodiscard]] virtual bool BelongsToLibrary(
      std::string_view library_id) const = 0;
  [[nodiscard]] virtual bool MatchesCell(std::string_view library_id,
                                         std::string_view cell_id) const = 0;
  [[nodiscard]] virtual bool IsOpen() const noexcept = 0;
  virtual void RefreshLibrary(application::LibraryRecord library) = 0;
  [[nodiscard]] virtual bool PrepareClose() = 0;
  virtual void Close() = 0;
  [[nodiscard]] virtual bool TranslateAccelerator(const MSG& message) = 0;
};

}  // namespace designpp::gui

#endif  // DESIGNPP_GUI_VIEW_WINDOW_H_

// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_GUI_LAYOUT_WINDOW_H_
#define DESIGNPP_GUI_LAYOUT_WINDOW_H_

#include "designpp/gui/layout_window_impl.h"

namespace designpp::gui {

// User-facing Layout tool window. Physical implementation remains a backend
// concern and is exposed only through layout-oriented operations.
class LayoutWindow final : public ViewWindow {
 public:
  LayoutWindow() = default;
  LayoutWindow(const LayoutWindow&) = delete;
  LayoutWindow& operator=(const LayoutWindow&) = delete;
  ~LayoutWindow() override = default;

  [[nodiscard]] bool Create(HINSTANCE instance,
                            const application::WorkspaceOpenRequest& request,
                            application::LibraryRecord library,
                            ViewWindowLogCallback central_log,
                            ViewWindowLibraryChangedCallback library_changed);

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
  LayoutWindowImplementation implementation_;
};

}  // namespace designpp::gui

#endif  // DESIGNPP_GUI_LAYOUT_WINDOW_H_

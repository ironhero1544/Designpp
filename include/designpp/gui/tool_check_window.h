// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_GUI_TOOL_CHECK_WINDOW_H_
#define DESIGNPP_GUI_TOOL_CHECK_WINDOW_H_

#include <windows.h>

#include <cstddef>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

#include "designpp/gui/dpi.h"
#include "designpp/runtime/tool_catalog.h"

namespace designpp::gui {

// Displays tool availability in an independent window owned by Library Manager.
class ToolCheckWindow final {
 public:
  using StartCheckCallback = std::function<void()>;
  using ToolActionCallback = std::function<void(std::optional<std::size_t>)>;

  ToolCheckWindow() = default;
  ToolCheckWindow(const ToolCheckWindow&) = delete;
  ToolCheckWindow& operator=(const ToolCheckWindow&) = delete;
  ~ToolCheckWindow();

  // Creates the tool window or activates the existing instance.
  [[nodiscard]] bool CreateOrShow(
      HINSTANCE instance, HWND owner,
      const std::vector<runtime::ToolDefinition>& tools,
      StartCheckCallback start_check, ToolActionCallback install_tool,
      ToolActionCallback remove_tool, ToolActionCallback activate_tool,
      ToolActionCallback rollback_tool);

  // Updates one tool row.
  void SetToolState(std::size_t index, std::wstring_view status,
                    std::wstring_view version) const;

  // Enables or disables starting another check.
  void SetChecking(bool checking) const;

 private:
  static LRESULT CALLBACK WindowProcedure(HWND window, UINT message,
                                          WPARAM wparam, LPARAM lparam);
  LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);

  [[nodiscard]] bool CreateControls();
  void PopulateTools();
  void LayoutControls(int width, int height) const;
  void ApplyDpi(UINT dpi);
  void UpdateActionButtonLabels() const;
  [[nodiscard]] std::optional<std::size_t> SelectedToolIndex() const;

  HINSTANCE instance_ = nullptr;
  HWND owner_ = nullptr;
  HWND window_ = nullptr;
  HWND description_ = nullptr;
  HWND start_button_ = nullptr;
  HWND install_button_ = nullptr;
  HWND remove_button_ = nullptr;
  HWND activate_button_ = nullptr;
  HWND rollback_button_ = nullptr;
  HWND tool_list_ = nullptr;
  UINT dpi_ = kDefaultDpi;
  UniqueFont font_;
  std::vector<runtime::ToolDefinition> tools_;
  StartCheckCallback start_check_;
  ToolActionCallback install_tool_;
  ToolActionCallback remove_tool_;
  ToolActionCallback activate_tool_;
  ToolActionCallback rollback_tool_;
};

}  // namespace designpp::gui

#endif  // DESIGNPP_GUI_TOOL_CHECK_WINDOW_H_

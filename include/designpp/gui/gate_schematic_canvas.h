// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_GUI_GATE_SCHEMATIC_CANVAS_H_
#define DESIGNPP_GUI_GATE_SCHEMATIC_CANVAS_H_

#include <windows.h>

#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "designpp/gui/schematic_scene.h"

namespace designpp::gui {

// Owns the native child window used to inspect a synthesized gate netlist.
// All methods must be called on the thread that created the parent window.
class GateSchematicCanvas final {
 public:
  using ModuleOpenCallback = std::function<void(std::string_view)>;
  using BackCallback = std::function<void()>;

  GateSchematicCanvas() = default;
  GateSchematicCanvas(const GateSchematicCanvas&) = delete;
  GateSchematicCanvas& operator=(const GateSchematicCanvas&) = delete;
  ~GateSchematicCanvas();

  [[nodiscard]] bool Create(HINSTANCE instance, HWND parent);
  void Move(int x, int y, int width, int height);
  void SetScene(std::shared_ptr<const SchematicScene> scene);
  void SetMessage(std::wstring message);
  void SetNavigationCallbacks(ModuleOpenCallback open_module,
                              BackCallback go_back);
  [[nodiscard]] HWND Window() const noexcept;

 private:
  static LRESULT CALLBACK WindowProcedure(HWND window, UINT message,
                                          WPARAM wparam, LPARAM lparam);
  LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);
  void Paint();
  void UpdateScrollbars();
  void Scroll(bool horizontal, int command, int position);
  void ZoomAt(int wheel_delta, POINT cursor);
  void FitToWindow();
  void BeginPan(POINT cursor);
  void UpdatePan(POINT cursor);
  void EndPan();
  void OpenModuleAt(POINT cursor);

  HWND window_ = nullptr;
  std::shared_ptr<const SchematicScene> scene_;
  std::wstring message_ = L"No synthesized design.";
  bool has_schematic_ = false;
  int scroll_x_ = 0;
  int scroll_y_ = 0;
  int content_width_ = 800;
  int content_height_ = 500;
  double zoom_ = 1.0;
  bool panning_ = false;
  POINT pan_origin_{};
  int pan_scroll_x_ = 0;
  int pan_scroll_y_ = 0;
  ModuleOpenCallback open_module_;
  BackCallback go_back_;
};

}  // namespace designpp::gui

#endif  // DESIGNPP_GUI_GATE_SCHEMATIC_CANVAS_H_

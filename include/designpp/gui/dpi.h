// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_GUI_DPI_H_
#define DESIGNPP_GUI_DPI_H_

#include <windows.h>

namespace designpp::gui {

inline constexpr UINT kDefaultDpi = 96;

// Owns a Win32 font handle and deletes it at destruction.
class UniqueFont final {
 public:
  UniqueFont() = default;
  explicit UniqueFont(HFONT font) : font_(font) {}
  UniqueFont(const UniqueFont&) = delete;
  UniqueFont& operator=(const UniqueFont&) = delete;
  UniqueFont(UniqueFont&& other) noexcept;
  UniqueFont& operator=(UniqueFont&& other) noexcept;
  ~UniqueFont();

  // Returns the owned font handle, or nullptr if no font is owned.
  [[nodiscard]] HFONT Get() const { return font_; }

  // Replaces the owned font handle.
  void Reset(HFONT font = nullptr);

 private:
  HFONT font_ = nullptr;
};

// Enables the best process DPI-awareness mode supported by this Windows
// version.
[[nodiscard]] bool EnablePerMonitorDpiAwareness();

// Returns the DPI currently associated with a window.
[[nodiscard]] UINT GetWindowDpi(HWND window);

// Returns the system DPI before a top-level window is created.
[[nodiscard]] UINT GetSystemDpi();

// Scales a 96-DPI logical pixel value to the requested DPI.
[[nodiscard]] int ScaleForDpi(int value, UINT dpi);

// Creates a Segoe UI font sized for the requested DPI.
[[nodiscard]] UniqueFont CreateUiFont(UINT dpi);

// Applies a font to a window and all of its direct child controls.
void ApplyFontToWindowTree(HWND window, HFONT font);

}  // namespace designpp::gui

#endif  // DESIGNPP_GUI_DPI_H_

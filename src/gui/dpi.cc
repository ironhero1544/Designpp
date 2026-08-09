// Copyright 2026 The Design++ Authors

#include "designpp/gui/dpi.h"

#include <shellscalingapi.h>

#include <utility>

namespace designpp::gui {
namespace {

using SetProcessDpiAwarenessContextFunction =
    BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
using GetDpiForWindowFunction = UINT(WINAPI*)(HWND);
using GetDpiForSystemFunction = UINT(WINAPI*)();

BOOL CALLBACK ApplyFontCallback(HWND window, LPARAM font) {
  SendMessageW(window, WM_SETFONT, static_cast<WPARAM>(font), TRUE);
  return TRUE;
}

}  // namespace

UniqueFont::UniqueFont(UniqueFont&& other) noexcept
    : font_(std::exchange(other.font_, nullptr)) {}

UniqueFont& UniqueFont::operator=(UniqueFont&& other) noexcept {
  if (this != &other) {
    Reset(std::exchange(other.font_, nullptr));
  }
  return *this;
}

UniqueFont::~UniqueFont() { Reset(); }

void UniqueFont::Reset(HFONT font) {
  if (font_ != nullptr) {
    DeleteObject(font_);
  }
  font_ = font;
}

bool EnablePerMonitorDpiAwareness() {
  HMODULE user32 = GetModuleHandleW(L"user32.dll");
  if (user32 != nullptr) {
    const auto set_context =
        reinterpret_cast<SetProcessDpiAwarenessContextFunction>(
            GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
    if (set_context != nullptr &&
        set_context(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
      return true;
    }
  }

  HMODULE shcore = LoadLibraryW(L"shcore.dll");
  if (shcore != nullptr) {
    using SetProcessDpiAwarenessFunction =
        HRESULT(WINAPI*)(PROCESS_DPI_AWARENESS);
    const auto set_awareness = reinterpret_cast<SetProcessDpiAwarenessFunction>(
        GetProcAddress(shcore, "SetProcessDpiAwareness"));
    const bool succeeded =
        set_awareness != nullptr &&
        SUCCEEDED(set_awareness(PROCESS_PER_MONITOR_DPI_AWARE));
    FreeLibrary(shcore);
    if (succeeded) {
      return true;
    }
  }

  return SetProcessDPIAware() != FALSE;
}

UINT GetWindowDpi(HWND window) {
  HMODULE user32 = GetModuleHandleW(L"user32.dll");
  if (user32 != nullptr) {
    const auto get_dpi = reinterpret_cast<GetDpiForWindowFunction>(
        GetProcAddress(user32, "GetDpiForWindow"));
    if (get_dpi != nullptr) {
      const UINT dpi = get_dpi(window);
      if (dpi != 0) {
        return dpi;
      }
    }
  }
  return GetSystemDpi();
}

UINT GetSystemDpi() {
  HMODULE user32 = GetModuleHandleW(L"user32.dll");
  if (user32 != nullptr) {
    const auto get_dpi = reinterpret_cast<GetDpiForSystemFunction>(
        GetProcAddress(user32, "GetDpiForSystem"));
    if (get_dpi != nullptr) {
      const UINT dpi = get_dpi();
      if (dpi != 0) {
        return dpi;
      }
    }
  }

  HDC screen = GetDC(nullptr);
  if (screen == nullptr) {
    return kDefaultDpi;
  }
  const int dpi = GetDeviceCaps(screen, LOGPIXELSX);
  ReleaseDC(nullptr, screen);
  return dpi > 0 ? static_cast<UINT>(dpi) : kDefaultDpi;
}

int ScaleForDpi(int value, UINT dpi) {
  return MulDiv(value, static_cast<int>(dpi), static_cast<int>(kDefaultDpi));
}

UniqueFont CreateUiFont(UINT dpi) {
  const int height = -MulDiv(9, static_cast<int>(dpi), 72);
  return UniqueFont(CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI"));
}

void ApplyFontToWindowTree(HWND window, HFONT font) {
  if (window == nullptr || font == nullptr) {
    return;
  }
  SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
  EnumChildWindows(window, ApplyFontCallback, reinterpret_cast<LPARAM>(font));
}

}  // namespace designpp::gui

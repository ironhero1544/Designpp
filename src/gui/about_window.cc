// Copyright 2026 The Design++ Authors

#include "designpp/gui/about_window.h"

#include <shellapi.h>

#include <cwctype>
#include <string_view>

#include "Resource.h"
#include "designpp/gui/dpi.h"

namespace designpp::gui {
namespace {

constexpr wchar_t kAboutClassName[] = L"DesignPlusPlus.AboutWindow";
constexpr std::wstring_view kSecret = L"ironhero";
constexpr wchar_t kAuthorUrl[] = L"https://github.com/ironhero1544";

void AddLabel(HWND owner, const wchar_t* text, int y, int height, HFONT font,
              UINT dpi) {
  HWND label = CreateWindowExW(
      0, L"STATIC", text, WS_CHILD | WS_VISIBLE, ScaleForDpi(68, dpi),
      ScaleForDpi(y, dpi), ScaleForDpi(292, dpi), ScaleForDpi(height, dpi),
      owner, nullptr,
      reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE)),
      nullptr);
  if (label != nullptr) {
    SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
  }
}

LRESULT CALLBACK AboutWindowProcedure(HWND window, UINT message, WPARAM wparam,
                                      LPARAM lparam) {
  switch (message) {
    case WM_CREATE: {
      const HINSTANCE instance = reinterpret_cast<HINSTANCE>(
          GetWindowLongPtrW(window, GWLP_HINSTANCE));
      const UINT dpi = GetWindowDpi(window);
      const HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
      AddLabel(window, L"Design++ 1.0.0", 24, 22, font, dpi);
      AddLabel(window, L"Semiconductor IDE (Integrated Design Environment)", 54,
               38, font, dpi);
      AddLabel(window, L"Made by Ironhero", 108, 22, font, dpi);
      HWND icon = CreateWindowExW(
          0, L"STATIC", nullptr, WS_CHILD | WS_VISIBLE | SS_ICON,
          ScaleForDpi(24, dpi), ScaleForDpi(24, dpi), ScaleForDpi(32, dpi),
          ScaleForDpi(32, dpi), window, nullptr, instance, nullptr);
      if (icon != nullptr) {
        const HICON image = LoadIconW(instance, MAKEINTRESOURCEW(IDI_DESIGN));
        SendMessageW(icon, STM_SETICON, reinterpret_cast<WPARAM>(image), 0);
      }
      return 0;
    }
    case WM_KEYDOWN:
      if (wparam == VK_ESCAPE) {
        DestroyWindow(window);
        return 0;
      }
      return DefWindowProcW(window, message, wparam, lparam);
    case WM_CHAR: {
      const wchar_t character =
          static_cast<wchar_t>(std::towlower(static_cast<wint_t>(wparam)));
      const auto matched =
          static_cast<std::size_t>(GetWindowLongPtrW(window, GWLP_USERDATA));
      const std::size_t next =
          matched < kSecret.size() && character == kSecret[matched]
              ? matched + 1
              : (character == kSecret[0] ? 1 : 0);
      if (next == kSecret.size()) {
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        const HINSTANCE result = ShellExecuteW(window, L"open", kAuthorUrl,
                                               nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(result) > 32) DestroyWindow(window);
      } else {
        SetWindowLongPtrW(window, GWLP_USERDATA, static_cast<LONG_PTR>(next));
      }
      return 0;
    }
    case WM_CLOSE:
      DestroyWindow(window);
      return 0;
    default:
      return DefWindowProcW(window, message, wparam, lparam);
  }
}

}  // namespace

void ShowAboutWindow(HINSTANCE instance, HWND owner) {
  const UINT dpi = GetWindowDpi(owner);
  WNDCLASSEXW window_class = {};
  window_class.cbSize = sizeof(window_class);
  window_class.lpfnWndProc = AboutWindowProcedure;
  window_class.hInstance = instance;
  window_class.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_DESIGN));
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  window_class.lpszClassName = kAboutClassName;
  if (RegisterClassExW(&window_class) == 0 &&
      GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return;
  }

  HWND window =
      CreateWindowExW(0, kAboutClassName, L"Design++ 정보",
                      WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT,
                      CW_USEDEFAULT, ScaleForDpi(390, dpi),
                      ScaleForDpi(185, dpi), owner, nullptr, instance, nullptr);
  if (window != nullptr) {
    ShowWindow(window, SW_SHOWNORMAL);
    SetForegroundWindow(window);
    SetFocus(window);
  }
}

}  // namespace designpp::gui

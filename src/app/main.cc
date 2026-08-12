// Copyright 2026 The Design++ Authors

// Common Controls requires the base Windows declarations first.
// clang-format off
#include <windows.h>
#include <commctrl.h>
// clang-format on

#include "designpp/gui/dpi.h"
#include "designpp/gui/library_manager_window.h"

int APIENTRY wWinMain(_In_ HINSTANCE instance,
                      _In_opt_ HINSTANCE previous_instance,
                      _In_ LPWSTR command_line, _In_ int show_command) {
  UNREFERENCED_PARAMETER(previous_instance);

  UNREFERENCED_PARAMETER(command_line);

  const bool dpi_awareness_enabled =
      designpp::gui::EnablePerMonitorDpiAwareness();
  UNREFERENCED_PARAMETER(dpi_awareness_enabled);

  INITCOMMONCONTROLSEX controls{};
  controls.dwSize = sizeof(controls);
  controls.dwICC = ICC_BAR_CLASSES | ICC_TREEVIEW_CLASSES |
                   ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS;
  if (!InitCommonControlsEx(&controls)) {
    InitCommonControls();
  }

  designpp::gui::LibraryManagerWindow library_manager;
  if (!library_manager.Create(instance, show_command)) {
    MessageBoxW(nullptr, L"Library Manager를 만들 수 없습니다.", L"Design++",
                MB_OK | MB_ICONERROR);
    return 1;
  }
  return library_manager.RunMessageLoop();
}

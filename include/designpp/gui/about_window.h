// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_GUI_ABOUT_WINDOW_H_
#define DESIGNPP_GUI_ABOUT_WINDOW_H_

#include <windows.h>

namespace designpp::gui {

// Opens an independent, modeless About window owned by the caller.
void ShowAboutWindow(HINSTANCE instance, HWND owner);

}  // namespace designpp::gui

#endif  // DESIGNPP_GUI_ABOUT_WINDOW_H_

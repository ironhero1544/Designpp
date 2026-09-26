// Copyright 2026 The Design++ Authors

#include "designpp/gui/tool_check_window.h"

#include <commctrl.h>

#include <algorithm>
#include <utility>

namespace designpp::gui {
namespace {

constexpr wchar_t kWindowClassName[] = L"DesignPlusPlus.ToolCheckWindow";
constexpr int kStartCheckButtonId = 3001;
constexpr int kInstallToolButtonId = 3002;
constexpr int kRemoveToolButtonId = 3003;
constexpr int kActivateToolButtonId = 3004;
constexpr int kRollbackToolButtonId = 3005;
constexpr int kCleanBuildCacheButtonId = 3006;

std::wstring SelectedBundle(const runtime::ToolDefinition& tool) {
  if (tool.display_name.find(L"OpenLane") != std::wstring::npos) {
    return L"OpenLane 2.3.10 candidate";
  }
  if (tool.display_name.find(L"ORFS") != std::wstring::npos) {
    return L"ORFS 26Q2 (036d1062)";
  }
  return tool.install_method == runtime::InstallMethod::kManagedFlow
             ? L"Managed shared runtime"
             : L"System / profile range";
}

}  // namespace

ToolCheckWindow::~ToolCheckWindow() {
  if (window_ != nullptr) {
    DestroyWindow(window_);
  }
}

bool ToolCheckWindow::CreateOrShow(
    HINSTANCE instance, HWND owner,
    const std::vector<runtime::ToolDefinition>& tools,
    StartCheckCallback start_check, ToolActionCallback install_tool,
    ToolActionCallback remove_tool, ToolActionCallback activate_tool,
    ToolActionCallback rollback_tool, StartCheckCallback clean_build_cache) {
  start_check_ = std::move(start_check);
  install_tool_ = std::move(install_tool);
  remove_tool_ = std::move(remove_tool);
  activate_tool_ = std::move(activate_tool);
  rollback_tool_ = std::move(rollback_tool);
  clean_build_cache_ = std::move(clean_build_cache);
  if (window_ != nullptr) {
    ShowWindow(window_, SW_RESTORE);
    SetForegroundWindow(window_);
    return true;
  }

  instance_ = instance;
  owner_ = owner;
  tools_ = tools;

  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.style = CS_HREDRAW | CS_VREDRAW;
  window_class.lpfnWndProc = WindowProcedure;
  window_class.hInstance = instance;
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  window_class.lpszClassName = kWindowClassName;
  if (RegisterClassExW(&window_class) == 0 &&
      GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return false;
  }

  dpi_ = GetWindowDpi(owner);
  window_ =
      CreateWindowExW(0, kWindowClassName, L"Design++ Tool Check",
                      WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                      CW_USEDEFAULT, ScaleForDpi(1040, dpi_),
                      ScaleForDpi(620, dpi_), owner, nullptr, instance, this);
  if (window_ == nullptr) {
    return false;
  }
  ShowWindow(window_, SW_SHOW);
  UpdateWindow(window_);
  return true;
}

void ToolCheckWindow::SetToolState(std::size_t index, std::wstring_view status,
                                   std::wstring_view version) const {
  if (tool_list_ == nullptr || index >= tools_.size()) {
    return;
  }
  std::wstring status_text(status);
  std::wstring version_text(version);
  ListView_SetItemText(tool_list_, static_cast<int>(index), 2,
                       status_text.data());
  ListView_SetItemText(tool_list_, static_cast<int>(index), 3,
                       version_text.data());
  std::wstring summary(status);
  if (status.find(L"준비") != std::wstring_view::npos) {
    summary = L"환경 준비/복구를 실행하세요";
  } else if (status.find(L"실패") != std::wstring_view::npos) {
    summary = L"상세 로그와 선택 Profile을 확인하세요";
  }
  ListView_SetItemText(tool_list_, static_cast<int>(index), 6, summary.data());
}

void ToolCheckWindow::SetChecking(bool checking) const {
  if (start_button_ != nullptr) {
    EnableWindow(start_button_, !checking);
    SetWindowTextW(start_button_, checking ? L"검사 중..." : L"검사 시작");
  }
  EnableWindow(install_button_, !checking);
  EnableWindow(remove_button_, !checking);
  EnableWindow(activate_button_, !checking);
  EnableWindow(rollback_button_, !checking);
  EnableWindow(clean_build_cache_button_, !checking);
}

LRESULT CALLBACK ToolCheckWindow::WindowProcedure(HWND window, UINT message,
                                                  WPARAM wparam,
                                                  LPARAM lparam) {
  auto* self = reinterpret_cast<ToolCheckWindow*>(
      GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<ToolCheckWindow*>(create->lpCreateParams);
    self->window_ = window;
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  }
  return self != nullptr ? self->HandleMessage(message, wparam, lparam)
                         : DefWindowProcW(window, message, wparam, lparam);
}

LRESULT ToolCheckWindow::HandleMessage(UINT message, WPARAM wparam,
                                       LPARAM lparam) {
  switch (message) {
    case WM_CREATE:
      dpi_ = GetWindowDpi(window_);
      return CreateControls() ? 0 : -1;

    case WM_SIZE:
      LayoutControls(LOWORD(lparam), HIWORD(lparam));
      return 0;

    case WM_DPICHANGED: {
      const auto* suggested = reinterpret_cast<RECT*>(lparam);
      SetWindowPos(window_, nullptr, suggested->left, suggested->top,
                   suggested->right - suggested->left,
                   suggested->bottom - suggested->top,
                   SWP_NOACTIVATE | SWP_NOZORDER);
      ApplyDpi(HIWORD(wparam));
      return 0;
    }

    case WM_GETMINMAXINFO: {
      auto* information = reinterpret_cast<MINMAXINFO*>(lparam);
      information->ptMinTrackSize.x = ScaleForDpi(880, dpi_);
      information->ptMinTrackSize.y = ScaleForDpi(420, dpi_);
      return 0;
    }

    case WM_COMMAND:
      if (LOWORD(wparam) == kStartCheckButtonId && start_check_) {
        start_check_();
        return 0;
      }
      if (LOWORD(wparam) == kInstallToolButtonId && install_tool_) {
        install_tool_(SelectedToolIndex());
        return 0;
      }
      if (LOWORD(wparam) == kRemoveToolButtonId && remove_tool_) {
        remove_tool_(SelectedToolIndex());
        return 0;
      }
      if (LOWORD(wparam) == kActivateToolButtonId && activate_tool_) {
        activate_tool_(SelectedToolIndex());
        return 0;
      }
      if (LOWORD(wparam) == kRollbackToolButtonId && rollback_tool_) {
        rollback_tool_(SelectedToolIndex());
        return 0;
      }
      if (LOWORD(wparam) == kCleanBuildCacheButtonId && clean_build_cache_) {
        clean_build_cache_();
        return 0;
      }
      break;

    case WM_NOTIFY:
      if (reinterpret_cast<NMHDR*>(lparam)->hwndFrom == tool_list_ &&
          reinterpret_cast<NMHDR*>(lparam)->code == LVN_ITEMCHANGED) {
        UpdateActionButtonLabels();
      }
      break;

    case WM_CLOSE: {
      // An owned tool window should return focus to its owner when it closes.
      // Preserve an intentionally minimized owner instead of restoring it.
      const HWND owner = owner_;
      const bool reactivate_owner = owner != nullptr && IsWindow(owner) &&
                                    IsWindowVisible(owner) && !IsIconic(owner);
      DestroyWindow(window_);
      if (reactivate_owner && IsWindow(owner)) {
        ShowWindow(owner, SW_RESTORE);
        SetForegroundWindow(owner);
      }
      return 0;
    }

    case WM_DESTROY:
      window_ = nullptr;
      description_ = nullptr;
      start_button_ = nullptr;
      install_button_ = nullptr;
      remove_button_ = nullptr;
      activate_button_ = nullptr;
      rollback_button_ = nullptr;
      clean_build_cache_button_ = nullptr;
      tool_list_ = nullptr;
      return 0;

    default:
      break;
  }
  return DefWindowProcW(window_, message, wparam, lparam);
}

bool ToolCheckWindow::CreateControls() {
  description_ = CreateWindowExW(
      0, L"STATIC",
      L"선택 Profile의 실제 버전, 준비된 환경 및 검증 조합을 확인합니다. "
      L"검사는 다운로드나 빌드를 시작하지 않습니다. 미선택 시 전체 "
      L"작업입니다.",
      WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
  start_button_ = CreateWindowExW(
      0, L"BUTTON", L"검사 시작", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0,
      0, 0, window_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStartCheckButtonId)),
      instance_, nullptr);
  install_button_ = CreateWindowExW(
      0, L"BUTTON", L"환경 준비 / 복구", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
      0, 0, 0, 0, window_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kInstallToolButtonId)),
      instance_, nullptr);
  remove_button_ = CreateWindowExW(
      0, L"BUTTON", L"선택 삭제", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0,
      0, 0, window_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRemoveToolButtonId)),
      instance_, nullptr);
  activate_button_ = CreateWindowExW(
      0, L"BUTTON", L"선택 환경 활성화", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
      0, 0, 0, 0, window_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kActivateToolButtonId)),
      instance_, nullptr);
  rollback_button_ = CreateWindowExW(
      0, L"BUTTON", L"이전 환경 롤백", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0,
      0, 0, 0, window_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRollbackToolButtonId)),
      instance_, nullptr);
  clean_build_cache_button_ = CreateWindowExW(
      0, L"BUTTON", L"빌드 캐시 정리", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0,
      0, 0, 0, window_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCleanBuildCacheButtonId)),
      instance_, nullptr);
  tool_list_ = CreateWindowExW(
      WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"Tools",
      WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, 0,
      0, 0, 0, window_, nullptr, instance_, nullptr);
  if (description_ == nullptr || start_button_ == nullptr ||
      install_button_ == nullptr || remove_button_ == nullptr ||
      activate_button_ == nullptr || rollback_button_ == nullptr ||
      clean_build_cache_button_ == nullptr || tool_list_ == nullptr) {
    return false;
  }

  ListView_SetExtendedListViewStyle(tool_list_, LVS_EX_FULLROWSELECT |
                                                    LVS_EX_DOUBLEBUFFER |
                                                    LVS_EX_GRIDLINES);
  PopulateTools();
  UpdateActionButtonLabels();
  ApplyDpi(dpi_);
  return true;
}

void ToolCheckWindow::PopulateTools() {
  const wchar_t* headings[] = {L"도구 / 환경", L"용도",      L"상태",
                               L"실제 버전",   L"선택 조합", L"설치 방식",
                               L"후속 조치"};
  for (int column_index = 0; column_index < 7; ++column_index) {
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH;
    column.cx = 100;
    column.pszText = const_cast<wchar_t*>(headings[column_index]);
    ListView_InsertColumn(tool_list_, column_index, &column);
  }

  for (std::size_t index = 0; index < tools_.size(); ++index) {
    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = static_cast<int>(index);
    item.pszText = tools_[index].display_name.data();
    ListView_InsertItem(tool_list_, &item);
    ListView_SetItemText(tool_list_, static_cast<int>(index), 1,
                         tools_[index].purpose.data());
    SetToolState(index, L"확인 전", L"-");
    std::wstring bundle = SelectedBundle(tools_[index]);
    ListView_SetItemText(tool_list_, static_cast<int>(index), 4, bundle.data());
    std::wstring method =
        runtime::InstallMethodName(tools_[index].install_method);
    ListView_SetItemText(tool_list_, static_cast<int>(index), 5, method.data());
  }
}

void ToolCheckWindow::LayoutControls(int width, int height) const {
  if (description_ == nullptr) {
    return;
  }
  const int margin = ScaleForDpi(12, dpi_);
  const int gap = ScaleForDpi(8, dpi_);
  const int description_height = ScaleForDpi(42, dpi_);
  const int check_width = ScaleForDpi(110, dpi_);
  const int install_width = ScaleForDpi(180, dpi_);
  const int remove_width = ScaleForDpi(110, dpi_);
  const int activate_width = ScaleForDpi(135, dpi_);
  const int rollback_width = ScaleForDpi(125, dpi_);
  const int cache_width = ScaleForDpi(125, dpi_);
  const int button_height = ScaleForDpi(30, dpi_);
  const int buttons_width = check_width + install_width + activate_width +
                            rollback_width + remove_width + cache_width +
                            gap * 5;

  MoveWindow(description_, margin, margin, std::max(0, width - margin * 2),
             description_height, TRUE);
  const int button_y = margin + description_height + gap;
  int button_x = std::max(margin, width - margin - buttons_width);
  MoveWindow(start_button_, button_x, button_y, check_width, button_height,
             TRUE);
  button_x += check_width + gap;
  MoveWindow(install_button_, button_x, button_y, install_width, button_height,
             TRUE);
  button_x += install_width + gap;
  MoveWindow(activate_button_, button_x, button_y, activate_width,
             button_height, TRUE);
  button_x += activate_width + gap;
  MoveWindow(rollback_button_, button_x, button_y, rollback_width,
             button_height, TRUE);
  button_x += rollback_width + gap;
  MoveWindow(remove_button_, button_x, button_y, remove_width, button_height,
             TRUE);
  button_x += remove_width + gap;
  MoveWindow(clean_build_cache_button_, button_x, button_y, cache_width,
             button_height, TRUE);
  const int list_top = button_y + button_height + gap;
  MoveWindow(tool_list_, margin, list_top, std::max(0, width - margin * 2),
             std::max(0, height - list_top - margin), TRUE);
}

void ToolCheckWindow::UpdateActionButtonLabels() const {
  const bool has_selection = SelectedToolIndex().has_value();
  SetWindowTextW(install_button_, has_selection ? L"선택 환경 준비 / 복구"
                                                : L"전체 환경 준비 / 복구");
  SetWindowTextW(remove_button_, has_selection ? L"선택 삭제" : L"전체 삭제");
  const auto selected = SelectedToolIndex();
  const bool managed =
      selected && (tools_[*selected].id == runtime::ToolId::kOpenLane2 ||
                   tools_[*selected].id == runtime::ToolId::kOrfs);
  EnableWindow(activate_button_, managed);
  EnableWindow(rollback_button_, managed);
}

std::optional<std::size_t> ToolCheckWindow::SelectedToolIndex() const {
  if (tool_list_ == nullptr) {
    return std::nullopt;
  }
  const int selected = ListView_GetNextItem(tool_list_, -1, LVNI_SELECTED);
  if (selected < 0 || static_cast<std::size_t>(selected) >= tools_.size()) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(selected);
}

void ToolCheckWindow::ApplyDpi(UINT dpi) {
  dpi_ = dpi == 0 ? kDefaultDpi : dpi;
  font_ = CreateUiFont(dpi_);
  ApplyFontToWindowTree(window_, font_.Get());
  const int widths[] = {165, 190, 115, 190, 190, 120, 250};
  for (int index = 0; index < 7; ++index) {
    ListView_SetColumnWidth(tool_list_, index,
                            ScaleForDpi(widths[index], dpi_));
  }
  RECT client{};
  GetClientRect(window_, &client);
  LayoutControls(client.right - client.left, client.bottom - client.top);
}

}  // namespace designpp::gui

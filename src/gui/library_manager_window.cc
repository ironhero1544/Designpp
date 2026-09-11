// Copyright 2026 The Design++ Authors

#include "designpp/gui/library_manager_window.h"

#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <windowsx.h>

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <deque>
#include <functional>
#include <iterator>
#include <mutex>
#include <string_view>
#include <thread>
#include <utility>

#include "Resource.h"

namespace designpp::gui {
namespace {

constexpr wchar_t kWindowClassName[] = L"DesignPlusPlus.LibraryManagerWindow";
constexpr int kMaximumLogCharacters = 2'000'000;
constexpr UINT_PTR kBrowserFilterTimer = 51;
constexpr UINT kBrowserFilterDelayMilliseconds = 120;
constexpr std::size_t kBrowserPaneCount = 3U;

enum class UiEventKind {
  kOutput,
  kComplete,
  kLibraryRefresh,
  kRecentWorkspaces,
  kBrowserFilter,
};

struct UiEvent {
  UiEventKind kind;
  std::uint64_t task_id;
  runtime::OutputEncoding output_encoding;
  std::string output;
  runtime::ProcessResult result;
  std::vector<application::LibraryRecord> libraries;
  std::vector<application::RecentWorkspace> recent_workspaces;
  core::Status library_status;
  std::wstring action;
  std::uint64_t browser_generation = 0;
  std::vector<application::BrowserResult> browser_results;
};

struct ItemDialogData {
  std::wstring title;
  std::wstring name;
  std::wstring description;
  core::ViewKind view_kind = core::ViewKind::kVerilog;
  bool choose_view_kind = false;
};

std::wstring Utf8ToWide(std::string_view text) {
  if (text.empty()) return {};
  const int length =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), nullptr, 0);
  if (length <= 0) return L"[invalid UTF-8]";
  std::wstring result(static_cast<std::size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                      static_cast<int>(text.size()), result.data(), length);
  return result;
}

std::string WideToUtf8(std::wstring_view text) {
  if (text.empty()) return {};
  const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr,
                                         0, nullptr, nullptr);
  if (length <= 0) return {};
  std::string result(static_cast<std::size_t>(length), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                      result.data(), length, nullptr, nullptr);
  return result;
}

std::wstring DialogText(HWND dialog, int control_id) {
  const int length = GetWindowTextLengthW(GetDlgItem(dialog, control_id));
  std::wstring value(static_cast<std::size_t>(std::max(length, 0)) + 1, L'\0');
  GetDlgItemTextW(dialog, control_id, value.data(),
                  static_cast<int>(value.size()));
  value.resize(std::wcslen(value.c_str()));
  return value;
}

INT_PTR CALLBACK ItemDialogProcedure(HWND dialog, UINT message, WPARAM wparam,
                                     LPARAM lparam) {
  auto* data =
      reinterpret_cast<ItemDialogData*>(GetWindowLongPtrW(dialog, DWLP_USER));
  if (message == WM_INITDIALOG) {
    data = reinterpret_cast<ItemDialogData*>(lparam);
    SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(data));
    SetWindowTextW(dialog, data->title.c_str());
    SetDlgItemTextW(dialog, IDC_ITEM_NAME, data->name.c_str());
    SetDlgItemTextW(dialog, IDC_ITEM_DESCRIPTION, data->description.c_str());
    HWND combo = GetDlgItem(dialog, IDC_VIEW_KIND);
    for (core::ViewKind kind :
         {core::ViewKind::kVerilog, core::ViewKind::kTestbench,
          core::ViewKind::kConstraints, core::ViewKind::kSynthesis,
          core::ViewKind::kTiming, core::ViewKind::kLayout,
          core::ViewKind::kReport}) {
      const std::wstring name = Utf8ToWide(core::ViewKindName(kind));
      const LRESULT row = SendMessageW(combo, CB_ADDSTRING, 0,
                                       reinterpret_cast<LPARAM>(name.c_str()));
      SendMessageW(combo, CB_SETITEMDATA, row, static_cast<LPARAM>(kind));
      if (kind == data->view_kind) SendMessageW(combo, CB_SETCURSEL, row, 0);
    }
    EnableWindow(combo, data->choose_view_kind);
    SetFocus(GetDlgItem(dialog, IDC_ITEM_NAME));
    return FALSE;
  }
  if (message == WM_COMMAND && data != nullptr) {
    if (LOWORD(wparam) == IDOK) {
      data->name = DialogText(dialog, IDC_ITEM_NAME);
      data->description = DialogText(dialog, IDC_ITEM_DESCRIPTION);
      if (data->choose_view_kind) {
        const LRESULT selection =
            SendDlgItemMessageW(dialog, IDC_VIEW_KIND, CB_GETCURSEL, 0, 0);
        if (selection >= 0) {
          const LRESULT kind =
              SendDlgItemMessageW(dialog, IDC_VIEW_KIND, CB_GETITEMDATA,
                                  static_cast<WPARAM>(selection), 0);
          if (kind != CB_ERR)
            data->view_kind = static_cast<core::ViewKind>(kind);
        }
      }
      if (data->name.empty()) {
        MessageBoxW(dialog, L"이름을 입력하세요.", L"Design++",
                    MB_OK | MB_ICONWARNING);
        return TRUE;
      }
      EndDialog(dialog, IDOK);
      return TRUE;
    }
    if (LOWORD(wparam) == IDCANCEL) {
      EndDialog(dialog, IDCANCEL);
      return TRUE;
    }
  }
  return FALSE;
}

std::wstring NormalizeNewlines(std::wstring_view text) {
  std::wstring normalized;
  normalized.reserve(text.size() + 16);
  wchar_t previous = L'\0';
  for (wchar_t character : text) {
    if (character == L'\n' && previous != L'\r') {
      normalized.push_back(L'\r');
    }
    normalized.push_back(character);
    previous = character;
  }
  return normalized;
}

std::wstring DecodeUtf8(std::string_view bytes) {
  if (bytes.empty()) {
    return {};
  }
  const int length = MultiByteToWideChar(
      CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
  if (length <= 0) {
    return L"[UTF-8 decode error]";
  }
  std::wstring text(static_cast<std::size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()),
                      text.data(), length);
  return text;
}

std::size_t Utf8SequenceLength(unsigned char leading_byte) {
  if ((leading_byte & 0x80) == 0) {
    return 1;
  }
  if ((leading_byte & 0xE0) == 0xC0) {
    return 2;
  }
  if ((leading_byte & 0xF0) == 0xE0) {
    return 3;
  }
  if ((leading_byte & 0xF8) == 0xF0) {
    return 4;
  }
  return 1;
}

void PreserveIncompleteUtf8Suffix(std::string* bytes, std::string* remainder) {
  if (bytes->empty()) {
    return;
  }

  std::size_t leading_index = bytes->size() - 1;
  while (leading_index > 0 &&
         (static_cast<unsigned char>((*bytes)[leading_index]) & 0xC0) == 0x80 &&
         bytes->size() - leading_index < 4) {
    --leading_index;
  }
  const std::size_t available = bytes->size() - leading_index;
  const std::size_t expected =
      Utf8SequenceLength(static_cast<unsigned char>((*bytes)[leading_index]));
  if (expected > available) {
    *remainder = bytes->substr(leading_index);
    bytes->erase(leading_index);
  }
}

std::wstring DecodeOutput(std::string_view bytes,
                          runtime::OutputEncoding encoding,
                          std::string* remainder) {
  std::string combined = std::move(*remainder);
  combined.append(bytes);
  remainder->clear();

  if (encoding == runtime::OutputEncoding::kUtf8) {
    PreserveIncompleteUtf8Suffix(&combined, remainder);
    return DecodeUtf8(combined);
  }

  if (combined.size() % sizeof(wchar_t) != 0) {
    remainder->push_back(combined.back());
    combined.pop_back();
  }
  if (combined.empty()) {
    return {};
  }
  std::wstring text(combined.size() / sizeof(wchar_t), L'\0');
  std::memcpy(text.data(), combined.data(), combined.size());
  if (!text.empty() && text.front() == 0xFEFF) {
    text.erase(text.begin());
  }
  return text;
}

std::wstring FormatCommand(const runtime::ProcessRequest& request) {
  std::wstring command = L"> " + request.executable.wstring();
  for (const std::wstring& argument : request.arguments) {
    command.append(L" \"");
    command.append(argument);
    command.push_back(L'"');
  }
  command.append(L"\r\n");
  return command;
}

}  // namespace

struct LibraryManagerWindow::EventChannel {
  std::mutex mutex;
  std::deque<UiEvent> events;
  HWND window = nullptr;
};

struct LibraryManagerWindow::NavigationTag {
  enum class Kind { kRoot, kLibrary, kCell };
  Kind kind = Kind::kRoot;
  std::size_t library_index = 0;
  std::size_t cell_index = 0;
  HTREEITEM item = nullptr;
};

struct LibraryManagerWindow::BrowserPane {
  application::BrowserScope scope = application::BrowserScope::kLibrary;
  HWND title = nullptr;
  HWND search = nullptr;
  HWND kind_filter = nullptr;
  HWND status_filter = nullptr;
  HWND list = nullptr;
  application::BrowserQuery query;
  application::BrowserResult result;
};

LibraryManagerWindow::LibraryManagerWindow() = default;

LibraryManagerWindow::~LibraryManagerWindow() {
  library_scheduler_.RequestStop();
  if (window_ != nullptr) {
    DestroyWindow(window_);
  }
  ShutdownEventChannel();
  for (ActiveTask& task : active_tasks_) {
    task.session->Cancel();
  }
  active_tasks_.clear();
}

bool LibraryManagerWindow::Create(HINSTANCE instance, int show_command) {
  instance_ = instance;
  tools_ = runtime::BuildToolCatalog();
  tool_states_.assign(tools_.size(), {L"확인 전", L"-"});
  const unsigned int logical_cpus = std::thread::hardware_concurrency();
  const std::size_t usable_cpus = logical_cpus > 1 ? logical_cpus - 1 : 1;
  maximum_parallel_probes_ = std::clamp<std::size_t>(usable_cpus, 1, 2);

  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.style = CS_HREDRAW | CS_VREDRAW;
  window_class.lpfnWndProc = WindowProcedure;
  window_class.hInstance = instance;
  window_class.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_DESIGN));
  window_class.hIconSm = LoadIconW(instance, MAKEINTRESOURCEW(IDI_SMALL));
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  window_class.lpszMenuName = MAKEINTRESOURCEW(IDC_DESIGN);
  window_class.lpszClassName = kWindowClassName;
  if (RegisterClassExW(&window_class) == 0 &&
      GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return false;
  }

  dpi_ = GetSystemDpi();
  window_ =
      CreateWindowExW(0, kWindowClassName, L"Design++ Library Manager",
                      WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                      CW_USEDEFAULT, ScaleForDpi(1120, dpi_),
                      ScaleForDpi(760, dpi_), nullptr, nullptr, instance, this);
  if (window_ == nullptr) {
    return false;
  }

  ShowWindow(window_, show_command);
  UpdateWindow(window_);
  return true;
}

int LibraryManagerWindow::RunMessageLoop() const {
  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    if (workspace_registry_ &&
        workspace_registry_->TranslateAccelerator(message)) {
      continue;
    }
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  return static_cast<int>(message.wParam);
}

LRESULT CALLBACK LibraryManagerWindow::WindowProcedure(HWND window,
                                                       UINT message,
                                                       WPARAM wparam,
                                                       LPARAM lparam) {
  auto* self = reinterpret_cast<LibraryManagerWindow*>(
      GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<LibraryManagerWindow*>(create->lpCreateParams);
    self->window_ = window;
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  }
  return self != nullptr ? self->HandleMessage(message, wparam, lparam)
                         : DefWindowProcW(window, message, wparam, lparam);
}

LRESULT LibraryManagerWindow::HandleMessage(UINT message, WPARAM wparam,
                                            LPARAM lparam) {
  switch (message) {
    case WM_CREATE:
      event_channel_ = std::make_shared<EventChannel>();
      event_channel_->window = window_;
      tool_check_window_ = std::make_unique<ToolCheckWindow>();
      toolchain_doctor_window_ = std::make_unique<ToolchainDoctorWindow>();
      workspace_registry_ = std::make_unique<WorkspaceRegistry>(
          instance_, [this](std::wstring text) { AppendLog(text); },
          [this] { RefreshLibraries(); });
      dpi_ = GetWindowDpi(window_);
      if (!CreateControls()) return -1;
      LoadRecentWorkspaces();
      RefreshLibraries();
      return 0;

    case WM_SIZE:
      LayoutControls(LOWORD(lparam), HIWORD(lparam));
      return 0;

    case WM_SETCURSOR:
      if (LOWORD(lparam) == HTCLIENT) {
        POINT point{};
        GetCursorPos(&point);
        ScreenToClient(window_, &point);
        if (HitTestBrowserSplitter(point.x, point.y)) {
          SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
          return TRUE;
        }
      }
      break;

    case WM_LBUTTONDOWN: {
      const auto splitter =
          HitTestBrowserSplitter(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
      if (splitter) {
        dragged_browser_splitter_ = splitter;
        SetCapture(window_);
        return 0;
      }
      break;
    }

    case WM_MOUSEMOVE:
      if (dragged_browser_splitter_ && GetCapture() == window_) {
        RECT client{};
        GetClientRect(window_, &client);
        const int width = client.right - client.left;
        const int minimum = ScaleForDpi(180, dpi_);
        const int x = std::clamp(GET_X_LPARAM(lparam), minimum,
                                 std::max(minimum, width - minimum));
        if (*dragged_browser_splitter_ == 0) {
          const int maximum =
              static_cast<int>(browser_split_ratios_[1] * width) - minimum;
          browser_split_ratios_[0] =
              static_cast<double>(std::min(x, maximum)) / width;
        } else {
          const int minimum_x =
              static_cast<int>(browser_split_ratios_[0] * width) + minimum;
          browser_split_ratios_[1] =
              static_cast<double>(std::max(x, minimum_x)) / width;
        }
        LayoutControls(client.right, client.bottom);
        return 0;
      }
      break;

    case WM_LBUTTONUP:
      if (dragged_browser_splitter_) {
        dragged_browser_splitter_.reset();
        if (GetCapture() == window_) ReleaseCapture();
        return 0;
      }
      break;

    case WM_CAPTURECHANGED:
      dragged_browser_splitter_.reset();
      break;

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
      information->ptMinTrackSize.x = ScaleForDpi(780, dpi_);
      information->ptMinTrackSize.y = ScaleForDpi(560, dpi_);
      return 0;
    }

    case WM_COMMAND:
      if (HIWORD(wparam) == EN_CHANGE || HIWORD(wparam) == CBN_SELCHANGE) {
        const auto pane = FindBrowserPane(reinterpret_cast<HWND>(lparam));
        if (pane) {
          PopulateBrowserQuery(*pane);
          ScheduleBrowserFilter();
          return 0;
        }
      }
      if (LOWORD(wparam) >= IDM_RECENT_FIRST &&
          LOWORD(wparam) <= IDM_RECENT_LAST) {
        OpenRecentWorkspace(LOWORD(wparam) - IDM_RECENT_FIRST);
        return 0;
      }
      switch (LOWORD(wparam)) {
        case IDM_TOOL_CHECK:
          OpenToolCheck();
          return 0;
        case IDM_TOOLCHAIN_DOCTOR:
          OpenToolchainDoctor();
          return 0;
        case IDM_LIBRARY_ROOT:
          SelectLibraryRoot();
          return 0;
        case IDM_LIBRARY_REFRESH:
          RefreshLibraries();
          return 0;
        case IDM_RECENT_CLEAR:
          ClearRecentWorkspaces();
          return 0;
        case IDM_NEW_LIBRARY:
        case IDM_NEW_CELL:
        case IDM_NEW_VIEW:
          CreateLibraryItem(LOWORD(wparam));
          return 0;
        case IDM_LIBRARY_PROPERTIES:
          EditLibraryItem();
          return 0;
        case IDM_LIBRARY_DELETE:
          DeleteLibraryItem();
          return 0;
        case IDM_IMPORT_FILES:
          ImportLibraryFiles();
          return 0;
        case IDM_MANAGE_LIBERTY:
          if (selected_library_) {
            OpenLibertyFiles(libraries_[*selected_library_].library.id);
          }
          return 0;
        case IDM_WSL_SETUP:
          StartWslSetup();
          return 0;
        case IDM_INSTALL_BASE_TOOLS:
          StartToolchainSetup();
          return 0;
        case IDM_CANCEL_OPERATION:
          CancelOperation();
          return 0;
        case IDM_EXIT:
          SendMessageW(window_, WM_CLOSE, 0, 0);
          return 0;
        case IDM_ABOUT:
          MessageBoxW(window_,
                      L"Design++ Library Manager\nWin32 EDA Flow "
                      L"Orchestrator\nWSL2 Backend",
                      L"Design++ 정보", MB_OK | MB_ICONINFORMATION);
          return 0;
        default:
          break;
      }
      break;

    case WM_TIMER:
      if (wparam == kBrowserFilterTimer) {
        KillTimer(window_, kBrowserFilterTimer);
        SubmitBrowserFilter();
        return 0;
      }
      break;

    case WM_NOTIFY: {
      const auto* header = reinterpret_cast<NMHDR*>(lparam);
      const auto browser_pane = FindBrowserPane(header->hwndFrom);
      if (browser_pane) {
        if (header->code == LVN_GETDISPINFOW) {
          auto* display = reinterpret_cast<NMLVDISPINFOW*>(lparam);
          const BrowserPane& pane = *browser_panes_[*browser_pane];
          if (display->item.iItem >= 0 &&
              static_cast<std::size_t>(display->item.iItem) <
                  pane.result.items.size() &&
              (display->item.mask & LVIF_TEXT) != 0) {
            const application::BrowserItemRef& item =
                pane.result
                    .items[static_cast<std::size_t>(display->item.iItem)];
            std::wstring text;
            if (display->item.iSubItem == 0) {
              text = item.create_action
                         ? L"+ \"" + Utf8ToWide(item.name) + L"\" 만들기"
                         : Utf8ToWide(item.name);
            } else if (display->item.iSubItem == 1) {
              text = Utf8ToWide(item.type);
            } else if (display->item.iSubItem == 2) {
              text = Utf8ToWide(core::LibraryStatusName(item.status));
            } else {
              text = Utf8ToWide(item.details);
            }
            if (display->item.pszText != nullptr &&
                display->item.cchTextMax > 0) {
              wcsncpy_s(display->item.pszText, display->item.cchTextMax,
                        text.c_str(), _TRUNCATE);
            }
          }
          return 0;
        }
        if (header->code == LVN_ITEMCHANGED && !applying_browser_results_) {
          HandleBrowserSelection(*browser_pane);
          return 0;
        }
        if (header->code == NM_DBLCLK || header->code == NM_RETURN) {
          if (*browser_pane == 0 && header->code == NM_DBLCLK) {
            const auto* activation =
                reinterpret_cast<const NMITEMACTIVATE*>(lparam);
            if (activation->iSubItem == 3 && activation->iItem >= 0) {
              const BrowserPane& pane = *browser_panes_[0];
              const std::size_t row =
                  static_cast<std::size_t>(activation->iItem);
              if (row < pane.result.items.size() &&
                  !pane.result.items[row].create_action) {
                OpenLibertyFiles(pane.result.items[row].library_id);
                return 0;
              }
            }
          }
          ActivateBrowserPane(*browser_pane, false);
          return 0;
        }
        if (header->code == LVN_KEYDOWN) {
          const auto* key = reinterpret_cast<NMLVKEYDOWN*>(lparam);
          if (key->wVKey == VK_RETURN) {
            ActivateBrowserPane(*browser_pane, false);
            return 0;
          }
        }
      }
      if (header->hwndFrom == library_tree_ &&
          header->code == TVN_SELCHANGEDW) {
        HandleLibraryTreeSelection();
        return 0;
      }
      if (header->hwndFrom == library_list_ && header->code == NM_DBLCLK) {
        HandleLibraryListDoubleClick();
        return 0;
      }
      break;
    }

    case WM_CONTEXTMENU:
      if (FindBrowserPane(reinterpret_cast<HWND>(wparam)) ||
          reinterpret_cast<HWND>(wparam) == library_tree_ ||
          reinterpret_cast<HWND>(wparam) == library_list_) {
        POINT point{static_cast<short>(LOWORD(lparam)),
                    static_cast<short>(HIWORD(lparam))};
        ShowLibraryContextMenu(point);
        return 0;
      }
      break;

    case kEventsReadyMessage:
      HandleQueuedEvents();
      return 0;

    case WM_CLOSE:
      if (workspace_registry_ && !workspace_registry_->PrepareCloseAll()) {
        return 0;
      }
      if (workspace_registry_) workspace_registry_->CloseAll();
      DestroyWindow(window_);
      return 0;

    case WM_DESTROY:
      library_scheduler_.RequestStop();
      ShutdownEventChannel();
      for (ActiveTask& task : active_tasks_) {
        task.session->Cancel();
      }
      active_tasks_.clear();
      decoder_remainders_.clear();
      operation_ = Operation::kIdle;
      workspace_registry_.reset();
      tool_check_window_.reset();
      window_ = nullptr;
      PostQuitMessage(0);
      return 0;

    default:
      break;
  }
  return DefWindowProcW(window_, message, wparam, lparam);
}

bool LibraryManagerWindow::CreateControls() {
  library_tree_ = CreateWindowExW(
      WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"Library Explorer",
      WS_CHILD | TVS_HASLINES | TVS_LINESATROOT | TVS_HASBUTTONS, 0, 0, 0, 0,
      window_, nullptr, instance_, nullptr);
  library_list_ =
      CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"Libraries",
                      WS_CHILD | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                      0, 0, 0, 0, window_, nullptr, instance_, nullptr);
  log_ = CreateWindowExW(
      WS_EX_CLIENTEDGE, L"EDIT",
      L"Design++ Library Manager Output\r\n"
      L"도구 > Tool Check에서 WSL2 EDA 도구를 확인할 수 있습니다.\r\n",
      WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_LEFT | ES_MULTILINE |
          ES_AUTOVSCROLL | ES_READONLY,
      0, 0, 0, 0, window_, nullptr, instance_, nullptr);
  status_ = CreateWindowExW(0, STATUSCLASSNAMEW, nullptr,
                            WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0,
                            window_, nullptr, instance_, nullptr);
  progress_ =
      CreateWindowExW(0, PROGRESS_CLASSW, L"Progress", WS_CHILD | WS_VISIBLE, 0,
                      0, 0, 0, status_, nullptr, instance_, nullptr);

  if (library_tree_ == nullptr || library_list_ == nullptr || log_ == nullptr ||
      status_ == nullptr || progress_ == nullptr || !CreateBrowserPanes()) {
    return false;
  }

  SendMessageW(log_, EM_SETLIMITTEXT, kMaximumLogCharacters, 0);
  ListView_SetExtendedListViewStyle(library_list_,
                                    LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
  ConfigureLibraryList();
  ApplyDpi(dpi_);
  SetBusyControls(false);
  SetStatus(L"대기 중");
  return true;
}

void LibraryManagerWindow::ConfigureLibraryList() {
  const wchar_t* headings[] = {L"Library", L"Type", L"Status"};
  for (int index = 0; index < 3; ++index) {
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH;
    column.cx = 100;
    column.pszText = const_cast<wchar_t*>(headings[index]);
    ListView_InsertColumn(library_list_, index, &column);
  }
}

bool LibraryManagerWindow::CreateBrowserPanes() {
  const wchar_t* titles[] = {L"Library", L"Cell", L"View"};
  for (std::size_t index = 0; index < kBrowserPaneCount; ++index) {
    browser_panes_[index] = std::make_unique<BrowserPane>();
    BrowserPane& pane = *browser_panes_[index];
    pane.scope = static_cast<application::BrowserScope>(index);
    pane.title = CreateWindowExW(0, L"STATIC", titles[index],
                                 WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0,
                                 window_, nullptr, instance_, nullptr);
    pane.search = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        0, 0, 0, 0, window_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(4100 + index)), instance_,
        nullptr);
    pane.kind_filter = CreateWindowExW(
        0, WC_COMBOBOXW, L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, 0, 0,
        window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(4200 + index)),
        instance_, nullptr);
    pane.status_filter = CreateWindowExW(
        0, WC_COMBOBOXW, L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, 0, 0,
        window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(4300 + index)),
        instance_, nullptr);
    pane.list =
        CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_OWNERDATA |
                            LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                        0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    if (pane.title == nullptr || pane.search == nullptr ||
        pane.kind_filter == nullptr || pane.status_filter == nullptr ||
        pane.list == nullptr) {
      return false;
    }
    SendMessageW(pane.search, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(L"검색..."));
    SetWindowSubclass(pane.search, SearchEditProcedure,
                      static_cast<UINT_PTR>(index),
                      reinterpret_cast<DWORD_PTR>(this));
    ListView_SetExtendedListViewStyle(pane.list, LVS_EX_FULLROWSELECT |
                                                     LVS_EX_DOUBLEBUFFER |
                                                     LVS_EX_GRIDLINES);
    const wchar_t* columns[] = {L"Name", L"Type", L"Status", L"Managed Files"};
    for (int column_index = 0; column_index < 4; ++column_index) {
      LVCOLUMNW column{};
      column.mask = LVCF_TEXT | LVCF_WIDTH;
      column.cx = column_index == 0 ? 150 : column_index == 3 ? 220 : 85;
      column.pszText = const_cast<wchar_t*>(columns[column_index]);
      ListView_InsertColumn(pane.list, column_index, &column);
    }
    const wchar_t* statuses[] = {L"All",     L"Ready",   L"Empty",
                                 L"Missing", L"Invalid", L"Read-only"};
    for (const wchar_t* status : statuses) {
      SendMessageW(pane.status_filter, CB_ADDSTRING, 0,
                   reinterpret_cast<LPARAM>(status));
    }
    SendMessageW(pane.status_filter, CB_SETCURSEL, 0, 0);
    SendMessageW(pane.kind_filter, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"All kinds"));
    for (core::ViewKind kind :
         {core::ViewKind::kVerilog, core::ViewKind::kTestbench,
          core::ViewKind::kConstraints, core::ViewKind::kSynthesis,
          core::ViewKind::kTiming, core::ViewKind::kLayout,
          core::ViewKind::kReport}) {
      const std::wstring name = Utf8ToWide(core::ViewKindName(kind));
      const LRESULT row = SendMessageW(pane.kind_filter, CB_ADDSTRING, 0,
                                       reinterpret_cast<LPARAM>(name.c_str()));
      SendMessageW(pane.kind_filter, CB_SETITEMDATA, row,
                   static_cast<LPARAM>(kind));
    }
    SendMessageW(pane.kind_filter, CB_SETCURSEL, 0, 0);
    ShowWindow(pane.kind_filter, index == static_cast<std::size_t>(
                                              application::BrowserScope::kView)
                                     ? SW_SHOW
                                     : SW_HIDE);
  }
  return true;
}

std::optional<std::size_t> LibraryManagerWindow::FindBrowserPane(
    HWND control) const {
  for (std::size_t index = 0; index < browser_panes_.size(); ++index) {
    if (browser_panes_[index] == nullptr) continue;
    const BrowserPane& pane = *browser_panes_[index];
    if (control == pane.search || control == pane.kind_filter ||
        control == pane.status_filter || control == pane.list) {
      return index;
    }
  }
  return std::nullopt;
}

LRESULT CALLBACK LibraryManagerWindow::SearchEditProcedure(
    HWND window, UINT message, WPARAM wparam, LPARAM lparam,
    UINT_PTR subclass_id, DWORD_PTR reference_data) {
  auto* self = reinterpret_cast<LibraryManagerWindow*>(reference_data);
  if (message == WM_KEYDOWN && self != nullptr) {
    if (wparam == VK_RETURN) {
      self->ActivateBrowserPane(static_cast<std::size_t>(subclass_id), true);
      return 0;
    }
    if (wparam == VK_ESCAPE) {
      SetWindowTextW(window, L"");
      return 0;
    }
  }
  if (message == WM_NCDESTROY) {
    RemoveWindowSubclass(window, SearchEditProcedure, subclass_id);
  }
  return DefSubclassProc(window, message, wparam, lparam);
}

void LibraryManagerWindow::PopulateBrowserQuery(std::size_t pane_index) {
  if (pane_index >= browser_panes_.size() ||
      browser_panes_[pane_index] == nullptr) {
    return;
  }
  BrowserPane& pane = *browser_panes_[pane_index];
  pane.query.text =
      WideToUtf8(DialogText(window_, 4100 + static_cast<int>(pane_index)));
  const LRESULT status = SendMessageW(pane.status_filter, CB_GETCURSEL, 0, 0);
  pane.query.status.reset();
  if (status > 0 && status <= 5) {
    const core::LibraryStatus statuses[] = {
        core::LibraryStatus::kReady, core::LibraryStatus::kEmpty,
        core::LibraryStatus::kMissing, core::LibraryStatus::kInvalid,
        core::LibraryStatus::kReadOnly};
    pane.query.status = statuses[status - 1];
  }
  pane.query.view_kind.reset();
  if (pane.scope == application::BrowserScope::kView) {
    const LRESULT kind = SendMessageW(pane.kind_filter, CB_GETCURSEL, 0, 0);
    if (kind > 0) {
      const LRESULT value =
          SendMessageW(pane.kind_filter, CB_GETITEMDATA, kind, 0);
      if (value != CB_ERR) {
        pane.query.view_kind = static_cast<core::ViewKind>(value);
      }
    }
  }
}

void LibraryManagerWindow::ScheduleBrowserFilter() {
  SetTimer(window_, kBrowserFilterTimer, kBrowserFilterDelayMilliseconds,
           nullptr);
}

void LibraryManagerWindow::SubmitBrowserFilter() {
  if (std::any_of(browser_panes_.begin(), browser_panes_.end(),
                  [](const auto& pane) { return pane == nullptr; })) {
    return;
  }
  if (!library_snapshot_) {
    library_snapshot_ =
        std::make_shared<const std::vector<application::LibraryRecord>>(
            libraries_);
  }
  const auto snapshot = library_snapshot_;
  std::vector<application::BrowserQuery> queries;
  queries.reserve(browser_panes_.size());
  for (const auto& pane : browser_panes_) queries.push_back(pane->query);
  const std::string library_id = selected_library_id_;
  const std::string cell_id = selected_cell_id_;
  const std::uint64_t generation = ++browser_generation_;
  const std::shared_ptr<EventChannel> channel = event_channel_;
  const bool accepted = library_scheduler_.Submit(
      [snapshot, queries = std::move(queries), library_id, cell_id, generation,
       channel](std::stop_token stop_token) mutable {
        if (stop_token.stop_requested()) return;
        application::LibraryBrowserModel model(*snapshot);
        std::vector<application::BrowserResult> results;
        results.reserve(3);
        results.push_back(model.FilterLibraries(queries[0]));
        results.push_back(model.FilterCells(library_id, queries[1]));
        results.push_back(model.FilterViews(library_id, cell_id, queries[2]));
        HWND target = nullptr;
        {
          std::scoped_lock lock(channel->mutex);
          if (channel->window == nullptr) return;
          UiEvent event{};
          event.kind = UiEventKind::kBrowserFilter;
          event.browser_generation = generation;
          event.browser_results = std::move(results);
          channel->events.push_back(std::move(event));
          target = channel->window;
        }
        PostMessageW(target, kEventsReadyMessage, 0, 0);
      });
  if (!accepted) AppendLog(L"[Library] 검색 작업 대기열이 가득 찼습니다.\r\n");
}

void LibraryManagerWindow::ApplyBrowserResults(
    std::uint64_t generation, std::vector<application::BrowserResult> results) {
  if (generation != browser_generation_ || results.size() != 3 ||
      std::any_of(browser_panes_.begin(), browser_panes_.end(),
                  [](const auto& pane) { return pane == nullptr; })) {
    return;
  }
  applying_browser_results_ = true;
  std::optional<std::size_t> pending_activation;
  for (std::size_t index = 0; index < kBrowserPaneCount; ++index) {
    BrowserPane& pane = *browser_panes_[index];
    std::optional<std::string>& pending_selection =
        index == 0U   ? pending_browser_selection_[0]
        : index == 1U ? pending_browser_selection_[1]
                      : pending_browser_selection_[2];
    pane.result = std::move(results[index]);
    ListView_SetItemState(pane.list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_SetItemCountEx(pane.list,
                            static_cast<int>(pane.result.items.size()),
                            LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
    std::string desired;
    const bool activate_pending = pending_selection.has_value();
    if (activate_pending) {
      desired = *pending_selection;
    } else if (index == 0) {
      desired = selected_library_id_;
    } else if (index == 1) {
      desired = selected_cell_id_;
    } else {
      desired = selected_view_id_;
    }
    for (std::size_t row = 0; row < pane.result.items.size(); ++row) {
      const application::BrowserItemRef& item = pane.result.items[row];
      const std::string& id = index == 0   ? item.library_id
                              : index == 1 ? item.cell_id
                                           : item.view_id;
      if (!desired.empty() && id == desired) {
        ListView_SetItemState(pane.list, static_cast<int>(row),
                              LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(pane.list, static_cast<int>(row), FALSE);
        if (activate_pending) pending_activation = index;
        break;
      }
    }
    pending_selection.reset();
    InvalidateRect(pane.list, nullptr, TRUE);
  }
  applying_browser_results_ = false;
  if (pending_activation) HandleBrowserSelection(*pending_activation);
  UpdateLibraryCommandState();
}

void LibraryManagerWindow::HandleBrowserSelection(std::size_t pane_index) {
  if (pane_index >= browser_panes_.size() ||
      browser_panes_[pane_index] == nullptr) {
    return;
  }
  BrowserPane& pane = *browser_panes_[pane_index];
  const int row = ListView_GetNextItem(pane.list, -1, LVNI_SELECTED);
  if (row < 0 || static_cast<std::size_t>(row) >= pane.result.items.size())
    return;
  const application::BrowserItemRef& item =
      pane.result.items[static_cast<std::size_t>(row)];
  if (item.create_action) return;
  if (pane_index == 0) {
    if (selected_library_id_ == item.library_id && selected_cell_id_.empty()) {
      return;
    }
    selected_library_id_ = item.library_id;
    selected_cell_id_.clear();
    selected_view_id_.clear();
    selected_library_.reset();
    selected_cell_.reset();
    for (std::size_t index = 0; index < libraries_.size(); ++index) {
      if (libraries_[index].library.id == item.library_id) {
        selected_library_ = index;
        break;
      }
    }
  } else if (pane_index == 1) {
    if (selected_library_id_ == item.library_id &&
        selected_cell_id_ == item.cell_id && selected_view_id_.empty()) {
      return;
    }
    selected_library_id_ = item.library_id;
    selected_cell_id_ = item.cell_id;
    selected_view_id_.clear();
    selected_library_.reset();
    selected_cell_.reset();
    for (std::size_t library_index = 0; library_index < libraries_.size();
         ++library_index) {
      if (libraries_[library_index].library.id != selected_library_id_) {
        continue;
      }
      selected_library_ = library_index;
      const auto& cells = libraries_[library_index].library.cells;
      for (std::size_t index = 0; index < cells.size(); ++index) {
        if (cells[index].id == item.cell_id) {
          selected_cell_ = index;
          break;
        }
      }
      break;
    }
  } else {
    selected_library_id_ = item.library_id;
    selected_cell_id_ = item.cell_id;
    selected_view_id_ = item.view_id;
    selected_library_.reset();
    selected_cell_.reset();
    for (std::size_t library_index = 0; library_index < libraries_.size();
         ++library_index) {
      if (libraries_[library_index].library.id != selected_library_id_) {
        continue;
      }
      selected_library_ = library_index;
      const auto& cells = libraries_[library_index].library.cells;
      for (std::size_t cell_index = 0; cell_index < cells.size();
           ++cell_index) {
        if (cells[cell_index].id == selected_cell_id_) {
          selected_cell_ = cell_index;
          break;
        }
      }
      break;
    }
  }
  UpdateLibraryCommandState();
  ScheduleBrowserFilter();
}

void LibraryManagerWindow::ActivateBrowserPane(std::size_t pane_index,
                                               bool prefer_exact_match) {
  if (pane_index >= browser_panes_.size() ||
      browser_panes_[pane_index] == nullptr) {
    return;
  }
  BrowserPane& pane = *browser_panes_[pane_index];
  if (prefer_exact_match && pane.result.exact_match) {
    const application::BrowserItemRef& exact = *pane.result.exact_match;
    const std::string id = pane_index == 0   ? exact.library_id
                           : pane_index == 1 ? exact.cell_id
                                             : exact.view_id;
    pending_browser_selection_[pane_index] = id;
    if (!pane.result.exact_match_visible) {
      SendMessageW(pane.status_filter, CB_SETCURSEL, 0, 0);
      if (pane_index == 2) SendMessageW(pane.kind_filter, CB_SETCURSEL, 0, 0);
      PopulateBrowserQuery(pane_index);
      ScheduleBrowserFilter();
      return;
    }
    for (std::size_t row = 0; row < pane.result.items.size(); ++row) {
      const auto& item = pane.result.items[row];
      const std::string& row_id = pane_index == 0   ? item.library_id
                                  : pane_index == 1 ? item.cell_id
                                                    : item.view_id;
      if (row_id == id) {
        ListView_SetItemState(pane.list, static_cast<int>(row),
                              LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
        break;
      }
    }
  } else if (prefer_exact_match && pane.result.can_create) {
    CreateFromBrowser(pane_index, pane.query.text);
    return;
  }

  const int row = ListView_GetNextItem(pane.list, -1, LVNI_SELECTED);
  if (row < 0 || static_cast<std::size_t>(row) >= pane.result.items.size()) {
    if (pane.result.can_create) CreateFromBrowser(pane_index, pane.query.text);
    return;
  }
  const application::BrowserItemRef& item =
      pane.result.items[static_cast<std::size_t>(row)];
  if (item.create_action) {
    CreateFromBrowser(pane_index, item.name);
  } else if (pane_index < 2) {
    SetFocus(browser_panes_[pane_index + 1]->search);
  } else {
    OpenWorkspace(item.library_id, item.cell_id, item.view_id);
  }
}

void LibraryManagerWindow::CreateFromBrowser(std::size_t pane_index,
                                             std::string name) {
  if (pane_index >= browser_panes_.size() || browser_panes_[2] == nullptr) {
    return;
  }
  pending_create_name_ = std::move(name);
  pending_create_kind_ = browser_panes_[2]->query.view_kind;
  const int command = pane_index == 0   ? IDM_NEW_LIBRARY
                      : pane_index == 1 ? IDM_NEW_CELL
                                        : IDM_NEW_VIEW;
  CreateLibraryItem(command);
  pending_create_name_.clear();
  pending_create_kind_.reset();
}

void LibraryManagerWindow::OpenWorkspace(std::string_view library_id,
                                         std::string_view cell_id,
                                         std::string_view view_id) {
  const auto library = std::find_if(libraries_.begin(), libraries_.end(),
                                    [library_id](const auto& record) {
                                      return record.library.id == library_id;
                                    });
  if (library == libraries_.end() || workspace_registry_ == nullptr) {
    MessageBoxW(window_, L"Workspace 대상 Library를 찾을 수 없습니다.",
                L"Design++ Workspace", MB_OK | MB_ICONERROR);
    return;
  }
  application::WorkspaceOpenRequest request{
      std::string(library_id), std::string(cell_id), std::string(view_id)};
  if (!workspace_registry_->Open(request, *library)) {
    MessageBoxW(window_, L"Workspace 창을 만들 수 없습니다.",
                L"Design++ Workspace", MB_OK | MB_ICONERROR);
    return;
  }
  const auto cell =
      std::find_if(library->library.cells.begin(), library->library.cells.end(),
                   [cell_id](const core::Cell& candidate) {
                     return candidate.id == cell_id;
                   });
  const core::View* view = nullptr;
  if (cell != library->library.cells.end()) {
    const auto found_view =
        std::find_if(cell->views.begin(), cell->views.end(),
                     [view_id](const core::View& candidate) {
                       return candidate.id == view_id;
                     });
    if (found_view != cell->views.end()) view = &*found_view;
  }
  if (cell != library->library.cells.end() && view != nullptr) {
    SelectBrowserContext(library_id, cell_id, view_id);
    RecordRecentWorkspace(request, library->library.name + " / " + cell->name +
                                       " / " + view->name);
    AppendLog(L"[Workspace] " + Utf8ToWide(library->library.name) + L" / " +
              Utf8ToWide(cell->name) + L" / " + Utf8ToWide(view->name) +
              L" 열기\r\n");
    return;
  }
  AppendLog(L"[Workspace] " + Utf8ToWide(cell_id) + L" Cell 열기\r\n");
}

void LibraryManagerWindow::OpenLibertyFiles(std::string_view library_id) {
  const auto library = std::find_if(libraries_.begin(), libraries_.end(),
                                    [library_id](const auto& record) {
                                      return record.library.id == library_id;
                                    });
  if (library == libraries_.end() || workspace_registry_ == nullptr) {
    MessageBoxW(window_, L"Library를 찾을 수 없습니다.", L"Design++ Liberty",
                MB_OK | MB_ICONERROR);
    return;
  }
  if (!workspace_registry_->OpenLiberty(*library)) {
    MessageBoxW(window_, L"Liberty Viewer를 열 수 없습니다.",
                L"Design++ Liberty", MB_OK | MB_ICONERROR);
  }
}

void LibraryManagerWindow::LoadRecentWorkspaces() {
  const auto channel = event_channel_;
  const bool accepted =
      library_scheduler_.Submit([this, channel](std::stop_token token) {
        if (token.stop_requested()) return;
        auto loaded = recent_workspace_store_.Load();
        UiEvent event{};
        event.kind = UiEventKind::kRecentWorkspaces;
        event.library_status =
            loaded.Ok() ? core::Status::Success() : loaded.GetStatus();
        if (loaded.Ok()) event.recent_workspaces = std::move(loaded).Value();
        HWND target = nullptr;
        {
          std::scoped_lock lock(channel->mutex);
          if (channel->window == nullptr) return;
          channel->events.push_back(std::move(event));
          target = channel->window;
        }
        PostMessageW(target, kEventsReadyMessage, 0, 0);
      });
  if (!accepted)
    AppendLog(L"[Recent] 최근 Workspace를 불러올 수 없습니다.\r\n");
}

void LibraryManagerWindow::RecordRecentWorkspace(
    const application::WorkspaceOpenRequest& request,
    std::string display_name) {
  application::RecentWorkspace recent{request, std::move(display_name)};
  std::erase_if(recent_workspaces_, [&recent](const auto& candidate) {
    return candidate.request.library_id == recent.request.library_id &&
           candidate.request.cell_id == recent.request.cell_id &&
           candidate.request.view_id == recent.request.view_id;
  });
  recent_workspaces_.insert(recent_workspaces_.begin(), recent);
  if (recent_workspaces_.size() >
      application::RecentWorkspaceStore::kMaximumEntries) {
    recent_workspaces_.resize(
        application::RecentWorkspaceStore::kMaximumEntries);
  }
  RebuildRecentMenu();
  const bool accepted = library_scheduler_.Submit(
      [this, recent = std::move(recent)](std::stop_token token) {
        if (!token.stop_requested()) {
          static_cast<void>(recent_workspace_store_.Touch(recent));
        }
      });
  if (!accepted)
    AppendLog(L"[Recent] 최근 Workspace 저장이 지연되었습니다.\r\n");
}

void LibraryManagerWindow::RebuildRecentMenu() {
  if (window_ == nullptr) return;
  HMENU menu = GetMenu(window_);
  HMENU file_menu = menu != nullptr ? GetSubMenu(menu, 0) : nullptr;
  HMENU recent_menu = file_menu != nullptr ? GetSubMenu(file_menu, 3) : nullptr;
  if (recent_menu == nullptr) return;
  while (GetMenuItemCount(recent_menu) > 0) {
    DeleteMenu(recent_menu, 0, MF_BYPOSITION);
  }

  recent_menu_entries_.clear();
  std::vector<application::RecentWorkspace> valid;
  for (const application::RecentWorkspace& recent : recent_workspaces_) {
    bool exists = !libraries_loaded_;
    if (libraries_loaded_) {
      const auto library = std::find_if(
          libraries_.begin(), libraries_.end(), [&recent](const auto& record) {
            return record.library.id == recent.request.library_id;
          });
      if (library != libraries_.end()) {
        const auto cell = std::find_if(
            library->library.cells.begin(), library->library.cells.end(),
            [&recent](const core::Cell& candidate) {
              return candidate.id == recent.request.cell_id;
            });
        if (cell != library->library.cells.end()) {
          exists = std::any_of(cell->views.begin(), cell->views.end(),
                               [&recent](const core::View& candidate) {
                                 return candidate.id == recent.request.view_id;
                               });
        }
      }
    }
    if (!exists) continue;
    valid.push_back(recent);
    if (recent_menu_entries_.size() < 20) {
      recent_menu_entries_.push_back(recent);
    }
  }
  if (libraries_loaded_ && valid.size() != recent_workspaces_.size()) {
    recent_workspaces_ = valid;
    const auto snapshot = recent_workspaces_;
    static_cast<void>(
        library_scheduler_.Submit([this, snapshot](std::stop_token token) {
          if (!token.stop_requested()) {
            static_cast<void>(recent_workspace_store_.Replace(snapshot));
          }
        }));
  }

  if (recent_menu_entries_.empty()) {
    AppendMenuW(recent_menu, MF_STRING | MF_GRAYED, IDM_RECENT_EMPTY,
                L"(없음)");
  } else {
    for (std::size_t index = 0; index < recent_menu_entries_.size(); ++index) {
      std::wstring label = std::to_wstring(index + 1) + L"  " +
                           Utf8ToWide(recent_menu_entries_[index].display_name);
      AppendMenuW(recent_menu, MF_STRING,
                  IDM_RECENT_FIRST + static_cast<UINT>(index), label.c_str());
    }
  }
  AppendMenuW(recent_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(recent_menu,
              MF_STRING | (recent_workspaces_.empty() ? MF_GRAYED : 0),
              IDM_RECENT_CLEAR, L"목록 지우기");
  DrawMenuBar(window_);
}

void LibraryManagerWindow::OpenRecentWorkspace(std::size_t index) {
  if (index >= recent_menu_entries_.size()) return;
  const application::WorkspaceOpenRequest request =
      recent_menu_entries_[index].request;
  OpenWorkspace(request.library_id, request.cell_id, request.view_id);
}

void LibraryManagerWindow::ClearRecentWorkspaces() {
  recent_workspaces_.clear();
  recent_menu_entries_.clear();
  RebuildRecentMenu();
  const bool accepted =
      library_scheduler_.Submit([this](std::stop_token token) {
        if (!token.stop_requested()) {
          static_cast<void>(recent_workspace_store_.Clear());
        }
      });
  if (!accepted)
    AppendLog(L"[Recent] 최근 Workspace 목록을 지울 수 없습니다.\r\n");
}

void LibraryManagerWindow::SelectLibraryRoot() {
  BROWSEINFOW browse{};
  browse.hwndOwner = window_;
  browse.lpszTitle = L"Design++ 공용 Library Root를 선택하세요.";
  browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
  PIDLIST_ABSOLUTE item = SHBrowseForFolderW(&browse);
  if (item == nullptr) return;
  wchar_t path[MAX_PATH]{};
  const bool converted = SHGetPathFromIDListW(item, path) != FALSE;
  CoTaskMemFree(item);
  if (!converted) {
    MessageBoxW(window_, L"선택한 위치를 사용할 수 없습니다.", L"Design++",
                MB_OK | MB_ICONERROR);
    return;
  }
  const core::Status status = library_service_.SetLibraryRoot(path);
  if (!status.Ok()) {
    MessageBoxW(window_, Utf8ToWide(status.message).c_str(),
                L"Library Root 오류", MB_OK | MB_ICONERROR);
    return;
  }
  AppendLog(L"\r\n[Library] Root: " + std::wstring(path) + L"\r\n");
  RefreshLibraries();
}

void LibraryManagerWindow::RefreshLibraries() {
  SubmitLibraryRefresh(L"Library 새로 고침");
}

void LibraryManagerWindow::SubmitLibraryRefresh(std::wstring action) {
  SubmitLibraryOperation(std::move(action),
                         [] { return core::Status::Success(); });
}

void LibraryManagerWindow::SubmitLibraryOperation(
    std::wstring action, std::function<core::Status()> operation) {
  const std::shared_ptr<EventChannel> channel = event_channel_;
  SetStatus(action);
  const bool accepted = library_scheduler_.Submit(
      [this, channel, action = std::move(action),
       operation = std::move(operation)](std::stop_token stop_token) mutable {
        core::Status status = stop_token.stop_requested()
                                  ? core::Status{core::ErrorCode::kCancelled,
                                                 "Operation cancelled", 0}
                                  : operation();
        std::vector<application::LibraryRecord> records;
        if (status.Ok()) {
          auto refresh = library_service_.Refresh();
          if (refresh.Ok())
            records = std::move(refresh).Value();
          else
            status = refresh.GetStatus();
        }
        HWND target = nullptr;
        {
          std::scoped_lock lock(channel->mutex);
          if (channel->window == nullptr) return;
          UiEvent event{};
          event.kind = UiEventKind::kLibraryRefresh;
          event.libraries = std::move(records);
          event.library_status = std::move(status);
          event.action = std::move(action);
          channel->events.push_back(std::move(event));
          target = channel->window;
        }
        PostMessageW(target, kEventsReadyMessage, 0, 0);
      });
  if (!accepted) {
    SetStatus(L"Library 작업 대기열이 가득 찼습니다.");
    AppendLog(L"[Library] 작업 대기열이 가득 찼습니다.\r\n");
  }
}

void LibraryManagerWindow::RebuildLibraryControls() {
  TreeView_DeleteAllItems(library_tree_);
  navigation_tags_.clear();
  navigation_tags_.push_back(std::make_unique<NavigationTag>());
  NavigationTag* root_tag = navigation_tags_.back().get();
  auto root_path = library_service_.GetLibraryRoot();
  std::wstring root_label = L"Libraries";
  if (root_path.Ok()) root_label += L" — " + root_path.Value().wstring();
  TVINSERTSTRUCTW root_insert{};
  root_insert.hParent = TVI_ROOT;
  root_insert.hInsertAfter = TVI_LAST;
  root_insert.item.mask = TVIF_TEXT | TVIF_PARAM;
  root_insert.item.pszText = root_label.data();
  root_insert.item.lParam = reinterpret_cast<LPARAM>(root_tag);
  root_tag->item = TreeView_InsertItem(library_tree_, &root_insert);
  for (std::size_t library_index = 0; library_index < libraries_.size();
       ++library_index) {
    navigation_tags_.push_back(std::make_unique<NavigationTag>());
    NavigationTag* library_tag = navigation_tags_.back().get();
    library_tag->kind = NavigationTag::Kind::kLibrary;
    library_tag->library_index = library_index;
    std::wstring name = Utf8ToWide(libraries_[library_index].library.name);
    if (name.empty()) name = L"Invalid Library";
    TVINSERTSTRUCTW library_insert{};
    library_insert.hParent = root_tag->item;
    library_insert.hInsertAfter = TVI_LAST;
    library_insert.item.mask = TVIF_TEXT | TVIF_PARAM;
    library_insert.item.pszText = name.data();
    library_insert.item.lParam = reinterpret_cast<LPARAM>(library_tag);
    library_tag->item = TreeView_InsertItem(library_tree_, &library_insert);
    const auto& cells = libraries_[library_index].library.cells;
    for (std::size_t cell_index = 0; cell_index < cells.size(); ++cell_index) {
      navigation_tags_.push_back(std::make_unique<NavigationTag>());
      NavigationTag* cell_tag = navigation_tags_.back().get();
      cell_tag->kind = NavigationTag::Kind::kCell;
      cell_tag->library_index = library_index;
      cell_tag->cell_index = cell_index;
      std::wstring cell_name = Utf8ToWide(cells[cell_index].name);
      TVINSERTSTRUCTW cell_insert{};
      cell_insert.hParent = library_tag->item;
      cell_insert.hInsertAfter = TVI_LAST;
      cell_insert.item.mask = TVIF_TEXT | TVIF_PARAM;
      cell_insert.item.pszText = cell_name.data();
      cell_insert.item.lParam = reinterpret_cast<LPARAM>(cell_tag);
      cell_tag->item = TreeView_InsertItem(library_tree_, &cell_insert);
    }
  }
  TreeView_Expand(library_tree_, root_tag->item, TVE_EXPAND);
  TreeView_SelectItem(library_tree_, root_tag->item);
}

void LibraryManagerWindow::PopulateLibraryList() {
  ListView_DeleteAllItems(library_list_);
  std::wstring heading = selected_cell_      ? L"View"
                         : selected_library_ ? L"Cell"
                                             : L"Library";
  LVCOLUMNW column{};
  column.mask = LVCF_TEXT;
  column.pszText = heading.data();
  ListView_SetColumn(library_list_, 0, &column);
  auto add_row = [this](int row, std::wstring name, std::wstring type,
                        std::wstring status) {
    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = row;
    item.pszText = name.data();
    ListView_InsertItem(library_list_, &item);
    ListView_SetItemText(library_list_, row, 1, type.data());
    ListView_SetItemText(library_list_, row, 2, status.data());
  };
  if (!selected_library_) {
    for (std::size_t index = 0; index < libraries_.size(); ++index) {
      const auto& record = libraries_[index];
      add_row(static_cast<int>(index), Utf8ToWide(record.library.name),
              L"Library", Utf8ToWide(core::LibraryStatusName(record.status)));
    }
  } else if (!selected_cell_) {
    const auto& cells = libraries_[*selected_library_].library.cells;
    for (std::size_t index = 0; index < cells.size(); ++index) {
      add_row(static_cast<int>(index), Utf8ToWide(cells[index].name), L"Cell",
              cells[index].views.empty() ? L"Empty" : L"Ready");
    }
  } else {
    const auto& views =
        libraries_[*selected_library_].library.cells[*selected_cell_].views;
    for (std::size_t index = 0; index < views.size(); ++index) {
      const bool executable_view =
          views[index].kind == core::ViewKind::kSynthesis ||
          views[index].kind == core::ViewKind::kTiming ||
          views[index].kind == core::ViewKind::kLayout;
      add_row(
          static_cast<int>(index), Utf8ToWide(views[index].name),
          Utf8ToWide(core::ViewKindName(views[index].kind)),
          executable_view || !views[index].files.empty() ? L"Ready" : L"Empty");
    }
  }
  UpdateLibraryCommandState();
}

void LibraryManagerWindow::HandleLibraryTreeSelection() {
  TVITEMW item{};
  item.mask = TVIF_PARAM;
  item.hItem = TreeView_GetSelection(library_tree_);
  if (item.hItem == nullptr || !TreeView_GetItem(library_tree_, &item)) return;
  const auto* tag = reinterpret_cast<NavigationTag*>(item.lParam);
  selected_library_.reset();
  selected_cell_.reset();
  selected_library_id_.clear();
  selected_cell_id_.clear();
  selected_view_id_.clear();
  if (tag != nullptr && tag->kind != NavigationTag::Kind::kRoot) {
    if (tag->library_index >= libraries_.size()) return;
    selected_library_ = tag->library_index;
    selected_library_id_ = libraries_[*selected_library_].library.id;
    if (tag->kind == NavigationTag::Kind::kCell) {
      const auto& cells = libraries_[*selected_library_].library.cells;
      if (tag->cell_index >= cells.size()) return;
      selected_cell_ = tag->cell_index;
      selected_cell_id_ = cells[*selected_cell_].id;
    }
  }
  PopulateLibraryList();
  ScheduleBrowserFilter();
}

void LibraryManagerWindow::SelectBrowserContext(std::string_view library_id,
                                                std::string_view cell_id,
                                                std::string_view view_id) {
  selected_library_id_ = std::string(library_id);
  selected_cell_id_ = std::string(cell_id);
  selected_view_id_ = std::string(view_id);
  selected_library_.reset();
  selected_cell_.reset();
  for (std::size_t library_index = 0; library_index < libraries_.size();
       ++library_index) {
    if (libraries_[library_index].library.id != selected_library_id_) continue;
    selected_library_ = library_index;
    const auto& cells = libraries_[library_index].library.cells;
    for (std::size_t cell_index = 0; cell_index < cells.size(); ++cell_index) {
      if (cells[cell_index].id == selected_cell_id_) {
        selected_cell_ = cell_index;
        break;
      }
    }
    break;
  }
  UpdateLibraryCommandState();
  ScheduleBrowserFilter();
}

void LibraryManagerWindow::HandleLibraryListDoubleClick() {
  const int selected = ListView_GetNextItem(library_list_, -1, LVNI_SELECTED);
  if (selected < 0) return;
  if (selected_cell_) {
    const core::View& view = libraries_[*selected_library_]
                                 .library.cells[*selected_cell_]
                                 .views[static_cast<std::size_t>(selected)];
    OpenWorkspace(
        libraries_[*selected_library_].library.id,
        libraries_[*selected_library_].library.cells[*selected_cell_].id,
        view.id);
    return;
  }
  const NavigationTag::Kind kind = selected_library_
                                       ? NavigationTag::Kind::kCell
                                       : NavigationTag::Kind::kLibrary;
  for (const auto& tag : navigation_tags_) {
    if (tag->kind == kind &&
        tag->library_index == (selected_library_
                                   ? *selected_library_
                                   : static_cast<std::size_t>(selected)) &&
        (kind != NavigationTag::Kind::kCell ||
         tag->cell_index == static_cast<std::size_t>(selected))) {
      TreeView_SelectItem(library_tree_, tag->item);
      TreeView_EnsureVisible(library_tree_, tag->item);
      break;
    }
  }
}

void LibraryManagerWindow::ShowLibraryContextMenu(POINT screen_point) {
  if (screen_point.x == -1 && screen_point.y == -1) {
    GetCursorPos(&screen_point);
  }
  HMENU menu = CreatePopupMenu();
  AppendMenuW(menu, MF_STRING, IDM_NEW_LIBRARY, L"새 Library...");
  AppendMenuW(menu, MF_STRING, IDM_NEW_CELL, L"새 Cell...");
  AppendMenuW(menu, MF_STRING, IDM_NEW_VIEW, L"새 View...");
  AppendMenuW(menu, MF_STRING, IDM_IMPORT_FILES, L"파일 가져오기...");
  AppendMenuW(menu, MF_STRING, IDM_MANAGE_LIBERTY, L"Liberty 파일 관리...");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, IDM_LIBRARY_PROPERTIES, L"속성...");
  AppendMenuW(menu, MF_STRING, IDM_LIBRARY_DELETE, L"삭제");
  TrackPopupMenu(menu, TPM_RIGHTBUTTON, screen_point.x, screen_point.y, 0,
                 window_, nullptr);
  DestroyMenu(menu);
}

void LibraryManagerWindow::UpdateLibraryCommandState() const {
  HMENU menu = GetMenu(window_);
  const bool has_library = selected_library_.has_value();
  const bool has_cell = selected_cell_.has_value();
  const bool can_write =
      has_library &&
      (libraries_[*selected_library_].status == core::LibraryStatus::kReady ||
       libraries_[*selected_library_].status == core::LibraryStatus::kEmpty);
  EnableMenuItem(menu, IDM_NEW_CELL,
                 MF_BYCOMMAND | (can_write ? MF_ENABLED : MF_GRAYED));
  EnableMenuItem(
      menu, IDM_NEW_VIEW,
      MF_BYCOMMAND | (can_write && has_cell ? MF_ENABLED : MF_GRAYED));
  EnableMenuItem(
      menu, IDM_IMPORT_FILES,
      MF_BYCOMMAND |
          (can_write && (!has_cell || !selected_view_id_.empty()) ? MF_ENABLED
                                                                  : MF_GRAYED));
  EnableMenuItem(menu, IDM_MANAGE_LIBERTY,
                 MF_BYCOMMAND | (has_library ? MF_ENABLED : MF_GRAYED));
  EnableMenuItem(menu, IDM_LIBRARY_PROPERTIES,
                 MF_BYCOMMAND | (can_write ? MF_ENABLED : MF_GRAYED));
  EnableMenuItem(menu, IDM_LIBRARY_DELETE,
                 MF_BYCOMMAND | (has_library ? MF_ENABLED : MF_GRAYED));
  DrawMenuBar(window_);
}

void LibraryManagerWindow::CreateLibraryItem(int command_id) {
  if (command_id != IDM_NEW_LIBRARY && !selected_library_) return;
  if (command_id == IDM_NEW_VIEW && !selected_cell_) return;
  ItemDialogData data;
  data.name = Utf8ToWide(pending_create_name_);
  data.title = command_id == IDM_NEW_LIBRARY ? L"새 Library"
               : command_id == IDM_NEW_CELL  ? L"새 Cell"
                                             : L"새 View";
  data.choose_view_kind = command_id == IDM_NEW_VIEW;
  if (pending_create_kind_) data.view_kind = *pending_create_kind_;
  if (DialogBoxParamW(instance_, MAKEINTRESOURCEW(IDD_LIBRARY_ITEM), window_,
                      ItemDialogProcedure,
                      reinterpret_cast<LPARAM>(&data)) != IDOK) {
    return;
  }
  const std::string name = WideToUtf8(data.name);
  const std::string description = WideToUtf8(data.description);
  if (command_id == IDM_NEW_LIBRARY) {
    SubmitLibraryOperation(L"Library 생성", [this, name, description] {
      auto result = library_service_.CreateLibrary(name, description);
      return result.Ok() ? core::Status::Success() : result.GetStatus();
    });
    return;
  }
  const application::LibraryRecord record = libraries_[*selected_library_];
  if (command_id == IDM_NEW_CELL) {
    SubmitLibraryOperation(L"Cell 생성", [this, record, name, description] {
      auto result = library_service_.CreateCell(record, name, description);
      return result.Ok() ? core::Status::Success() : result.GetStatus();
    });
    return;
  }
  const std::string cell_id = record.library.cells[*selected_cell_].id;
  application::CreateViewRequest request{name, description, data.view_kind,
                                         ".sv"};
  SubmitLibraryOperation(L"View 생성", [this, record, cell_id, request] {
    auto result = library_service_.CreateView(record, cell_id, request);
    return result.Ok() ? core::Status::Success() : result.GetStatus();
  });
}

void LibraryManagerWindow::EditLibraryItem() {
  if (!selected_library_) return;
  const application::LibraryRecord record = libraries_[*selected_library_];
  std::string cell_id;
  std::string view_id;
  ItemDialogData data;
  data.title = L"속성";
  if (!selected_cell_) {
    data.name = Utf8ToWide(record.library.name);
    data.description = Utf8ToWide(record.library.description);
  } else {
    const core::Cell& cell = record.library.cells[*selected_cell_];
    cell_id = cell.id;
    const auto view_iterator = std::find_if(
        cell.views.begin(), cell.views.end(), [this](const core::View& view) {
          return view.id == selected_view_id_;
        });
    if (view_iterator != cell.views.end()) {
      const core::View& view = *view_iterator;
      view_id = view.id;
      data.name = Utf8ToWide(view.name);
      data.description = Utf8ToWide(view.description);
      data.view_kind = view.kind;
    } else {
      data.name = Utf8ToWide(cell.name);
      data.description = Utf8ToWide(cell.description);
    }
  }
  if (DialogBoxParamW(instance_, MAKEINTRESOURCEW(IDD_LIBRARY_ITEM), window_,
                      ItemDialogProcedure,
                      reinterpret_cast<LPARAM>(&data)) != IDOK) {
    return;
  }
  const std::string name = WideToUtf8(data.name);
  const std::string description = WideToUtf8(data.description);
  SubmitLibraryOperation(
      L"속성 저장", [this, record, cell_id, view_id, name, description] {
        auto result = library_service_.RenameItem(record, cell_id, view_id,
                                                  name, description);
        return result.Ok() ? core::Status::Success() : result.GetStatus();
      });
}

void LibraryManagerWindow::DeleteLibraryItem() {
  if (!selected_library_) return;
  const application::LibraryRecord record = libraries_[*selected_library_];
  std::string cell_id;
  std::string view_id;
  std::wstring label = Utf8ToWide(record.library.name);
  if (selected_cell_) {
    const core::Cell& cell = record.library.cells[*selected_cell_];
    cell_id = cell.id;
    label = Utf8ToWide(cell.name);
    const auto view_iterator = std::find_if(
        cell.views.begin(), cell.views.end(), [this](const core::View& view) {
          return view.id == selected_view_id_;
        });
    if (view_iterator != cell.views.end()) {
      const core::View& view = *view_iterator;
      view_id = view.id;
      label = Utf8ToWide(view.name);
    }
  }
  const std::wstring question =
      L"'" + label +
      L"' 항목을 삭제하시겠습니까? 로컬 항목은 휴지통으로 이동합니다.";
  if (MessageBoxW(window_, question.c_str(), L"Library 항목 삭제",
                  MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
    return;
  bool allow_unc_permanent = false;
  if (record.directory.is_absolute() &&
      record.directory.native().starts_with(L"\\\\")) {
    if (MessageBoxW(
            window_,
            L"네트워크 공유는 휴지통을 지원하지 않습니다. 이 항목을 영구 "
            L"삭제하시겠습니까? 이 작업은 복구할 수 없습니다.",
            L"네트워크 영구 삭제",
            MB_YESNO | MB_ICONSTOP | MB_DEFBUTTON2) != IDYES)
      return;
    allow_unc_permanent = true;
  }
  if (view_id.empty() && workspace_registry_ &&
      !workspace_registry_->CloseFor(record.library.id, cell_id)) {
    return;
  }
  SubmitLibraryOperation(L"Library 항목 삭제",
                         [this, record, cell_id, view_id, allow_unc_permanent] {
                           return library_service_.DeleteItem(
                               record, cell_id, view_id, allow_unc_permanent);
                         });
}

void LibraryManagerWindow::ImportLibraryFiles() {
  if (!selected_library_) return;
  const application::LibraryRecord record = libraries_[*selected_library_];
  std::string cell_id;
  std::string view_id;
  std::optional<core::ViewKind> target_view_kind;
  if (selected_cell_) {
    const core::Cell& cell = record.library.cells[*selected_cell_];
    const auto view_iterator = std::find_if(
        cell.views.begin(), cell.views.end(), [this](const core::View& view) {
          return view.id == selected_view_id_;
        });
    if (view_iterator == cell.views.end()) {
      MessageBoxW(window_, L"Library 또는 파일을 가져올 View를 선택하세요.",
                  L"Design++", MB_OK | MB_ICONINFORMATION);
      return;
    }
    cell_id = cell.id;
    view_id = view_iterator->id;
    target_view_kind = view_iterator->kind;
  }
  std::vector<wchar_t> buffer(65536, L'\0');
  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = window_;
  dialog.lpstrFile = buffer.data();
  dialog.nMaxFile = static_cast<DWORD>(buffer.size());
  if (cell_id.empty()) {
    dialog.lpstrFilter =
        L"Liberty files (*.lib;*.liberty)\0*.lib;*.liberty\0All files\0*.*\0\0";
  } else if (target_view_kind == core::ViewKind::kConstraints) {
    dialog.lpstrFilter = L"SDC files (*.sdc)\0*.sdc\0All files\0*.*\0\0";
  } else {
    dialog.lpstrFilter = L"모든 파일\0*.*\0\0";
  }
  dialog.Flags = OFN_FILEMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_EXPLORER;
  if (!GetOpenFileNameW(&dialog)) return;
  std::vector<std::filesystem::path> sources;
  const std::filesystem::path first(buffer.data());
  const wchar_t* cursor = buffer.data() + first.native().size() + 1;
  if (*cursor == L'\0') {
    sources.push_back(first);
  } else {
    while (*cursor != L'\0') {
      std::filesystem::path name(cursor);
      sources.push_back(first / name);
      cursor += name.native().size() + 1;
    }
  }
  const std::wstring action =
      cell_id.empty() ? L"Library 파일 가져오기" : L"View 파일 가져오기";
  SubmitLibraryOperation(
      action, [this, record, cell_id, view_id, sources = std::move(sources)] {
        auto result =
            library_service_.ImportFiles(record, cell_id, view_id, sources);
        return result.Ok() ? core::Status::Success() : result.GetStatus();
      });
}

void LibraryManagerWindow::LayoutControls(int width, int height) const {
  if (library_tree_ == nullptr) {
    return;
  }
  SendMessageW(status_, WM_SIZE, 0, 0);
  RECT status_bounds{};
  GetWindowRect(status_, &status_bounds);
  const int status_height = status_bounds.bottom - status_bounds.top;
  const int content_height = std::max(0, height - status_height);
  const int output_height = ScaleForDpi(190, dpi_);
  const int gap = ScaleForDpi(6, dpi_);
  const int upper_height = std::max(0, content_height - output_height - gap);
  LayoutBrowserPanes(width, upper_height);
  MoveWindow(log_, 0, upper_height + gap, width,
             std::max(0, content_height - upper_height - gap), TRUE);

  RECT status_client{};
  GetClientRect(status_, &status_client);
  const int first_part = ScaleForDpi(230, dpi_);
  const int parts[] = {first_part, -1};
  SendMessageW(status_, SB_SETPARTS, std::size(parts),
               reinterpret_cast<LPARAM>(parts));
  const int progress_margin = ScaleForDpi(4, dpi_);
  MoveWindow(
      progress_, progress_margin, progress_margin,
      std::max(0, first_part - progress_margin * 2),
      std::max(0, static_cast<int>(status_client.bottom) - progress_margin * 2),
      TRUE);
}

void LibraryManagerWindow::LayoutBrowserPanes(int width, int height) const {
  if (std::any_of(browser_panes_.begin(), browser_panes_.end(),
                  [](const auto& pane) { return pane == nullptr; })) {
    return;
  }
  const int gap = ScaleForDpi(6, dpi_);
  const int title_height = ScaleForDpi(22, dpi_);
  const int search_height = ScaleForDpi(25, dpi_);
  const int filter_height = ScaleForDpi(26, dpi_);
  const int inner_gap = ScaleForDpi(4, dpi_);
  const int split_positions[] = {
      static_cast<int>(browser_split_ratios_[0] * width),
      static_cast<int>(browser_split_ratios_[1] * width)};
  for (std::size_t index = 0; index < browser_panes_.size(); ++index) {
    const int left = index == 0 ? 0 : split_positions[index - 1] + gap / 2;
    const int right = index == 2 ? width : split_positions[index] - gap / 2;
    const int pane_width = std::max(0, right - left);
    const BrowserPane& pane = *browser_panes_[index];
    MoveWindow(pane.title, left, 0, pane_width, title_height, TRUE);
    MoveWindow(pane.search, left, title_height, pane_width, search_height,
               TRUE);
    const int filter_top = title_height + search_height + inner_gap;
    if (index == 2) {
      const int half = (pane_width - inner_gap) / 2;
      MoveWindow(pane.kind_filter, left, filter_top, half, filter_height, TRUE);
      MoveWindow(pane.status_filter, left + half + inner_gap, filter_top,
                 pane_width - half - inner_gap, filter_height, TRUE);
    } else {
      MoveWindow(pane.status_filter, left, filter_top, pane_width,
                 filter_height, TRUE);
    }
    const int list_top = filter_top + filter_height + inner_gap;
    MoveWindow(pane.list, left, list_top, pane_width,
               std::max(0, height - list_top), TRUE);
  }
}

std::optional<std::size_t> LibraryManagerWindow::HitTestBrowserSplitter(
    int x, int y) const {
  if (browser_panes_[0] == nullptr || y < 0 || log_ == nullptr) {
    return std::nullopt;
  }
  RECT log_bounds{};
  GetWindowRect(log_, &log_bounds);
  POINT log_top_left{log_bounds.left, log_bounds.top};
  ScreenToClient(window_, &log_top_left);
  if (y >= log_top_left.y) return std::nullopt;

  RECT client{};
  GetClientRect(window_, &client);
  const int width = client.right - client.left;
  const int hit_width = ScaleForDpi(5, dpi_);
  for (std::size_t index = 0; index < browser_split_ratios_.size(); ++index) {
    const int split = static_cast<int>(browser_split_ratios_[index] * width);
    if (x >= split - hit_width && x <= split + hit_width) return index;
  }
  return std::nullopt;
}

void LibraryManagerWindow::ApplyDpi(UINT dpi) {
  dpi_ = dpi == 0 ? kDefaultDpi : dpi;
  font_ = CreateUiFont(dpi_);
  ApplyFontToWindowTree(window_, font_.Get());
  ListView_SetColumnWidth(library_list_, 0, ScaleForDpi(300, dpi_));
  ListView_SetColumnWidth(library_list_, 1, ScaleForDpi(180, dpi_));
  ListView_SetColumnWidth(library_list_, 2, ScaleForDpi(180, dpi_));
  for (const auto& pane : browser_panes_) {
    if (pane == nullptr) continue;
    ListView_SetColumnWidth(pane->list, 0, ScaleForDpi(180, dpi_));
    ListView_SetColumnWidth(pane->list, 1, ScaleForDpi(120, dpi_));
    ListView_SetColumnWidth(pane->list, 2, ScaleForDpi(90, dpi_));
    ListView_SetColumnWidth(pane->list, 3, ScaleForDpi(240, dpi_));
  }
  RECT client{};
  GetClientRect(window_, &client);
  LayoutControls(client.right - client.left, client.bottom - client.top);
}

void LibraryManagerWindow::OpenToolCheck() {
  if (tool_check_window_ == nullptr) {
    tool_check_window_ = std::make_unique<ToolCheckWindow>();
  }
  if (!tool_check_window_->CreateOrShow(
          instance_, window_, tools_, [this]() { StartToolCheck(); },
          [this](std::optional<std::size_t> index) {
            index.has_value() ? StartToolInstall(*index)
                              : StartToolchainSetup();
          },
          [this](std::optional<std::size_t> index) {
            index.has_value() ? StartToolRemove(*index)
                              : StartToolchainRemove();
          })) {
    MessageBoxW(window_, L"Tool Check 창을 만들 수 없습니다.",
                L"Design++ Library Manager", MB_OK | MB_ICONERROR);
    return;
  }
  for (std::size_t index = 0; index < tool_states_.size(); ++index) {
    tool_check_window_->SetToolState(index, tool_states_[index].status,
                                     tool_states_[index].version);
  }
  tool_check_window_->SetChecking(operation_ != Operation::kIdle);
}

void LibraryManagerWindow::OpenToolchainDoctor() {
  if (toolchain_doctor_window_ == nullptr) {
    toolchain_doctor_window_ = std::make_unique<ToolchainDoctorWindow>();
  }
  if (!toolchain_doctor_window_->CreateOrShow(instance_, window_)) {
    MessageBoxW(window_, L"Toolchain Doctor 창을 만들 수 없습니다.",
                L"Design++ Library Manager", MB_OK | MB_ICONERROR);
  }
}

void LibraryManagerWindow::StartToolInstall(std::size_t tool_index) {
  if (operation_ != Operation::kIdle || tool_index >= tools_.size()) {
    return;
  }
  std::vector<runtime::SetupStep> steps =
      runtime::BuildToolInstallSteps(tools_[tool_index].id);
  if (steps.empty()) {
    MessageBoxW(window_,
                L"이 항목은 Design++가 직접 설치하지 않는 선택적 외부 "
                L"provider입니다.",
                L"개별 설치 지원 안 함", MB_OK | MB_ICONINFORMATION);
    return;
  }
  const std::wstring question = tools_[tool_index].display_name +
                                L"을(를) 설치하거나 업데이트하시겠습니까?\n\n" +
                                tools_[tool_index].install_hint;
  if (MessageBoxW(window_, question.c_str(), L"도구 설치 / 업데이트",
                  MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
    return;
  }
  operation_ = Operation::kInstallingTool;
  setup_steps_ = std::move(steps);
  next_setup_step_ = 0;
  completed_work_ = 0;
  failed_work_ = 0;
  SendMessageW(progress_, PBM_SETRANGE32, 0,
               static_cast<LPARAM>(setup_steps_.size()));
  SendMessageW(progress_, PBM_SETPOS, 0, 0);
  SetBusyControls(true);
  AppendLog(L"\r\n=== 개별 설치 / 업데이트: " +
            tools_[tool_index].display_name + L" ===\r\n");
  StartNextSetupStep();
}

void LibraryManagerWindow::StartToolRemove(std::size_t tool_index) {
  if (operation_ != Operation::kIdle || tool_index >= tools_.size()) {
    return;
  }
  std::vector<runtime::SetupStep> steps =
      runtime::BuildToolRemoveSteps(tools_[tool_index].id);
  if (steps.empty()) {
    if (tools_[tool_index].id == runtime::ToolId::kWebView2) {
      MessageBoxW(window_,
                  L"WebView2 Evergreen Runtime은 여러 Windows 앱이 공유하므로 "
                  L"Design++에서 삭제하지 않습니다. 문제가 있으면 선택 설치 "
                  L"/ 업데이트로 복구하세요.",
                  L"공유 Runtime 삭제 보호", MB_OK | MB_ICONINFORMATION);
      return;
    }
    MessageBoxW(window_,
                L"이 도구는 공유 managed environment 또는 외부 provider가 "
                L"소유하므로 개별 삭제할 수 없습니다. OpenROAD/OpenSTA는 "
                L"OpenLane 2와 함께 관리됩니다.",
                L"개별 삭제 지원 안 함", MB_OK | MB_ICONINFORMATION);
    return;
  }
  std::wstring question = tools_[tool_index].display_name +
                          L"을(를) 삭제하시겠습니까? 이 작업은 되돌리려면 "
                          L"다시 설치해야 합니다.";
  if (tools_[tool_index].id == runtime::ToolId::kOpenLane2) {
    question.append(
        L"\n\nOpenLane checkout을 삭제하면 managed OpenROAD/OpenSTA도 사용할 "
        L"수 없습니다.");
  }
  if (MessageBoxW(window_, question.c_str(), L"도구 삭제 확인",
                  MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
    return;
  }
  operation_ = Operation::kRemovingTool;
  setup_steps_ = std::move(steps);
  next_setup_step_ = 0;
  completed_work_ = 0;
  failed_work_ = 0;
  SendMessageW(progress_, PBM_SETRANGE32, 0,
               static_cast<LPARAM>(setup_steps_.size()));
  SendMessageW(progress_, PBM_SETPOS, 0, 0);
  SetBusyControls(true);
  AppendLog(L"\r\n=== 개별 삭제: " + tools_[tool_index].display_name +
            L" ===\r\n");
  StartNextSetupStep();
}

void LibraryManagerWindow::StartToolchainRemove() {
  if (operation_ != Operation::kIdle) {
    return;
  }
  const int answer = MessageBoxW(
      window_,
      L"Design++가 관리하는 APT EDA 패키지, cocotb 가상환경, OpenLane 2 "
      L"및 ORFS checkout을 모두 삭제합니다. 공유 Nix와 외부 Docker는 "
      L"유지됩니다. 다른 Windows 앱이 공유하는 WebView2 Runtime도 "
      L"삭제하지 않습니다. 계속하시겠습니까?",
      L"전체 EDA Toolchain 삭제 확인",
      MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
  if (answer != IDYES) {
    return;
  }
  operation_ = Operation::kRemovingToolchain;
  setup_steps_ = runtime::BuildCompleteToolRemoveSteps();
  next_setup_step_ = 0;
  completed_work_ = 0;
  failed_work_ = 0;
  SendMessageW(progress_, PBM_SETRANGE32, 0,
               static_cast<LPARAM>(setup_steps_.size()));
  SendMessageW(progress_, PBM_SETPOS, 0, 0);
  SetBusyControls(true);
  AppendLog(L"\r\n=== 전체 EDA Toolchain 삭제 시작 ===\r\n");
  StartNextSetupStep();
}

void LibraryManagerWindow::StartToolCheck() {
  if (operation_ != Operation::kIdle) {
    return;
  }
  operation_ = Operation::kCheckingTools;
  next_tool_index_ = 0;
  completed_work_ = 0;
  failed_work_ = 0;
  SendMessageW(progress_, PBM_SETRANGE32, 0,
               static_cast<LPARAM>(tools_.size()));
  SendMessageW(progress_, PBM_SETPOS, 0, 0);
  SetBusyControls(true);
  SetStatus(L"Design++ 실행 요구 사항을 병렬로 검사하고 있습니다...");
  AppendLog(L"\r\n=== Design++ 도구 및 Runtime 검사 시작 ===\r\n");
  for (std::size_t index = 0; index < tools_.size(); ++index) {
    SetToolState(index, L"대기", L"-");
  }
  StartPendingToolProbes();
}

void LibraryManagerWindow::StartWslSetup() {
  if (operation_ != Operation::kIdle) {
    return;
  }
  const int answer = MessageBoxW(
      window_,
      L"WSL과 Ubuntu 설치 단계만 관리자 권한으로 실행합니다. Library "
      L"Manager는 일반 권한으로 유지됩니다. 계속하시겠습니까?",
      L"WSL2 자동 설정", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
  if (answer != IDYES) {
    return;
  }
  operation_ = Operation::kSettingUpWsl;
  setup_steps_ = runtime::BuildWslSetupSteps();
  next_setup_step_ = 0;
  completed_work_ = 0;
  failed_work_ = 0;
  SendMessageW(progress_, PBM_SETRANGE32, 0,
               static_cast<LPARAM>(setup_steps_.size()));
  SendMessageW(progress_, PBM_SETPOS, 0, 0);
  SetBusyControls(true);
  AppendLog(L"\r\n=== WSL2 자동 설정 시작 ===\r\n");
  StartNextSetupStep();
}

void LibraryManagerWindow::StartToolchainSetup() {
  if (operation_ != Operation::kIdle) {
    return;
  }
  const int answer = MessageBoxW(
      window_,
      L"APT 도구, cocotb, Nix/OpenLane 2, ORFS 및 Microsoft WebView2 "
      L"Runtime을 설치하거나 업데이트합니다. 대용량 네트워크 다운로드가 "
      L"발생할 수 있습니다. "
      L"계속하시겠습니까?",
      L"EDA Toolchain 설치 / 업데이트",
      MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
  if (answer != IDYES) {
    return;
  }
  operation_ = Operation::kInstallingToolchain;
  setup_steps_ = runtime::BuildCompleteToolSetupSteps();
  next_setup_step_ = 0;
  completed_work_ = 0;
  failed_work_ = 0;
  SendMessageW(progress_, PBM_SETRANGE32, 0,
               static_cast<LPARAM>(setup_steps_.size()));
  SendMessageW(progress_, PBM_SETPOS, 0, 0);
  SetBusyControls(true);
  AppendLog(L"\r\n=== 전체 EDA Toolchain 설치 / 업데이트 시작 ===\r\n");
  StartNextSetupStep();
}

void LibraryManagerWindow::StartPendingToolProbes() {
  while (operation_ == Operation::kCheckingTools &&
         active_tasks_.size() < maximum_parallel_probes_ &&
         next_tool_index_ < tools_.size()) {
    const std::size_t index = next_tool_index_++;
    SetToolState(index, L"검사 중", L"-");
    StartTask(tools_[index].probe_request, tools_[index].display_name,
              runtime::OutputEncoding::kUtf8, index);
  }
  if (operation_ == Operation::kCheckingTools && active_tasks_.empty() &&
      next_tool_index_ >= tools_.size()) {
    FinishOperation(failed_work_ == 0 ? L"필수 도구 버전 검사가 통과했습니다. "
                                        L"Flow 호환성은 별도 검증이 필요합니다."
                                      : L"도구 검사가 완료되었습니다. 환경 "
                                        L"준비 및 오류 항목을 확인하세요.");
  }
}

void LibraryManagerWindow::StartNextSetupStep() {
  if (operation_ == Operation::kIdle || operation_ == Operation::kCancelling ||
      !active_tasks_.empty()) {
    return;
  }
  if (next_setup_step_ >= setup_steps_.size()) {
    const bool recheck_tools = operation_ == Operation::kInstallingToolchain ||
                               operation_ == Operation::kRemovingToolchain ||
                               operation_ == Operation::kInstallingTool ||
                               operation_ == Operation::kRemovingTool;
    FinishOperation(
        failed_work_ == 0
            ? L"설정 작업이 완료되었습니다."
            : L"설정 작업이 완료되었지만 일부 단계가 실패했습니다.");
    if (recheck_tools) {
      AppendLog(L"설치 결과를 자동으로 다시 검사합니다.\r\n");
      StartToolCheck();
    }
    return;
  }

  const runtime::SetupStep& step = setup_steps_[next_setup_step_++];
  SetStatus(step.title);
  AppendLog(L"\r\n--- " + step.title + L" ---\r\n");
  StartTask(step.request, step.title, step.output_encoding, std::nullopt,
            step.requires_elevation);
}

void LibraryManagerWindow::StartTask(runtime::ProcessRequest request,
                                     std::wstring title,
                                     runtime::OutputEncoding output_encoding,
                                     std::optional<std::size_t> tool_index,
                                     bool requires_elevation) {
  const std::uint64_t task_id = next_task_id_++;
  AppendLog(FormatCommand(request));
  const std::shared_ptr<EventChannel> channel = event_channel_;

  const runtime::OutputCallback on_output =
      [channel, task_id, output_encoding](std::string output) {
        HWND target = nullptr;
        {
          std::scoped_lock lock(channel->mutex);
          if (channel->window == nullptr) {
            return;
          }
          channel->events.push_back({UiEventKind::kOutput,
                                     task_id,
                                     output_encoding,
                                     std::move(output),
                                     {}});
          target = channel->window;
        }
        PostMessageW(target, kEventsReadyMessage, 0, 0);
      };
  const runtime::CompletionCallback on_complete =
      [channel, task_id, output_encoding](runtime::ProcessResult result) {
        HWND target = nullptr;
        {
          std::scoped_lock lock(channel->mutex);
          if (channel->window == nullptr) {
            return;
          }
          channel->events.push_back({UiEventKind::kComplete,
                                     task_id,
                                     output_encoding,
                                     {},
                                     std::move(result)});
          target = channel->window;
        }
        PostMessageW(target, kEventsReadyMessage, 0, 0);
      };
  runtime::ProcessLaunchResult launch =
      requires_elevation ? runtime::ProcessRunner::RunElevatedAsync(
                               std::move(request), on_complete)
                         : runtime::ProcessRunner::RunAsync(
                               std::move(request), on_output, on_complete);

  if (!launch.IsValid()) {
    AppendLog(L"[시작 오류] " + launch.error_message + L"\r\n");
    ++failed_work_;
    ++completed_work_;
    if (tool_index.has_value()) {
      const bool required = tools_[*tool_index].required;
      SetToolState(*tool_index, required ? L"실행 오류" : L"선택 사항 / 미설치",
                   required ? launch.error_message : L"-");
      if (!required) {
        --failed_work_;
      }
    }
    SendMessageW(progress_, PBM_SETPOS, static_cast<WPARAM>(completed_work_),
                 0);
    if (operation_ == Operation::kCheckingTools) {
      StartPendingToolProbes();
    } else {
      FinishOperation(L"프로세스를 시작할 수 없어 작업을 중단했습니다.");
    }
    return;
  }

  active_tasks_.push_back(
      {task_id, tool_index, std::move(title), output_encoding,
       std::make_unique<runtime::ProcessSession>(std::move(launch.session))});
}

void LibraryManagerWindow::CancelOperation() {
  if (operation_ == Operation::kIdle || operation_ == Operation::kCancelling) {
    return;
  }
  operation_ = Operation::kCancelling;
  SetStatus(L"실행 중인 작업을 취소하고 있습니다...");
  AppendLog(L"\r\n[취소 요청]\r\n");
  for (ActiveTask& task : active_tasks_) {
    task.session->Cancel();
  }
  if (active_tasks_.empty()) {
    FinishOperation(L"작업이 취소되었습니다.");
  }
}

void LibraryManagerWindow::HandleQueuedEvents() {
  std::deque<UiEvent> events;
  {
    std::scoped_lock lock(event_channel_->mutex);
    events.swap(event_channel_->events);
  }

  for (UiEvent& event : events) {
    if (event.kind == UiEventKind::kLibraryRefresh) {
      if (event.library_status.Ok()) {
        libraries_ = std::move(event.libraries);
        libraries_loaded_ = true;
        library_snapshot_ =
            std::make_shared<const std::vector<application::LibraryRecord>>(
                libraries_);
        if (workspace_registry_) {
          for (const auto& record : libraries_) {
            workspace_registry_->RefreshLibrary(record);
          }
        }
        selected_library_.reset();
        selected_cell_.reset();
        for (std::size_t library_index = 0; library_index < libraries_.size();
             ++library_index) {
          if (libraries_[library_index].library.id == selected_library_id_) {
            selected_library_ = library_index;
            const auto& cells = libraries_[library_index].library.cells;
            for (std::size_t cell_index = 0; cell_index < cells.size();
                 ++cell_index) {
              if (cells[cell_index].id == selected_cell_id_) {
                selected_cell_ = cell_index;
                break;
              }
            }
            break;
          }
        }
        if (!selected_library_) {
          selected_library_id_.clear();
          selected_cell_id_.clear();
          selected_view_id_.clear();
        } else if (!selected_cell_) {
          selected_cell_id_.clear();
          selected_view_id_.clear();
        }
        SubmitBrowserFilter();
        RebuildRecentMenu();
        AppendLog(L"[Library] " + event.action + L" 완료 (" +
                  std::to_wstring(libraries_.size()) + L"개 Library)\r\n");
        SetStatus(event.action + L" 완료");
      } else {
        const std::wstring message = Utf8ToWide(event.library_status.message);
        AppendLog(
            L"[Library 오류] " + event.action + L": " + message + L" (native=" +
            std::to_wstring(event.library_status.native_error) + L")\r\n");
        SetStatus(event.action + L" 실패");
        MessageBoxW(window_, message.c_str(), L"Library 작업 오류",
                    MB_OK | MB_ICONERROR);
      }
    } else if (event.kind == UiEventKind::kRecentWorkspaces) {
      if (event.library_status.Ok()) {
        recent_workspaces_ = std::move(event.recent_workspaces);
        RebuildRecentMenu();
      } else {
        AppendLog(L"[Recent 오류] " + Utf8ToWide(event.library_status.message) +
                  L"\r\n");
      }
    } else if (event.kind == UiEventKind::kBrowserFilter) {
      ApplyBrowserResults(event.browser_generation,
                          std::move(event.browser_results));
    } else if (event.kind == UiEventKind::kOutput) {
      std::string& remainder = decoder_remainders_[event.task_id];
      const std::wstring decoded =
          DecodeOutput(event.output, event.output_encoding, &remainder);
      const auto task = std::find_if(active_tasks_.begin(), active_tasks_.end(),
                                     [&event](const ActiveTask& active_task) {
                                       return active_task.id == event.task_id;
                                     });
      if (task != active_tasks_.end() && !decoded.empty()) {
        AppendLog(L"[" + task->title + L"] " + decoded);
      }
    } else {
      HandleTaskCompletion(event.task_id, event.result);
    }
  }
}

void LibraryManagerWindow::HandleTaskCompletion(
    std::uint64_t task_id, const runtime::ProcessResult& result) {
  const auto task_iterator = std::find_if(
      active_tasks_.begin(), active_tasks_.end(),
      [task_id](const ActiveTask& task) { return task.id == task_id; });
  if (task_iterator == active_tasks_.end()) {
    return;
  }

  const std::optional<std::size_t> tool_index = task_iterator->tool_index;
  const runtime::OutputEncoding encoding = task_iterator->output_encoding;
  const std::wstring title = task_iterator->title;
  active_tasks_.erase(task_iterator);
  decoder_remainders_.erase(task_id);

  const bool succeeded = result.started && !result.cancelled &&
                         result.error_message.empty() && result.exit_code == 0;
  if (!succeeded && !result.error_message.empty()) {
    AppendLog(L"[" + title + L"] 시작 오류: " + result.error_message + L"\r\n");
  }
  if (tool_index.has_value()) {
    std::string remainder;
    const std::wstring decoded =
        DecodeOutput(result.output, encoding, &remainder);
    const auto version =
        runtime::ParseToolVersion(tools_[*tool_index].id, decoded);
    if (decoded.find(L"DESIGNPP_ENVIRONMENT_MISSING") != std::wstring::npos) {
      SetToolState(*tool_index, L"미설치", L"-");
      if (tools_[*tool_index].required) ++failed_work_;
    } else if (decoded.find(L"DESIGNPP_ENVIRONMENT_PREPARATION_REQUIRED") !=
               std::wstring::npos) {
      SetToolState(*tool_index, L"환경 준비 필요", L"식별 불가");
      if (tools_[*tool_index].required) ++failed_work_;
    } else if (succeeded && !version.empty()) {
      SetToolState(*tool_index, L"버전 확인됨", version);
    } else if (succeeded) {
      SetToolState(*tool_index, L"검사 실패", L"식별 불가");
      if (tools_[*tool_index].required) ++failed_work_;
    } else {
      const bool required = tools_[*tool_index].required;
      SetToolState(*tool_index,
                   required ? L"미설치 / 오류" : L"선택 사항 / 미설치", L"-");
      if (required) {
        ++failed_work_;
      }
    }
  } else if (!succeeded) {
    ++failed_work_;
    if (!result.error_message.empty()) {
      AppendLog(L"[오류] " + result.error_message + L"\r\n");
    }
  }

  ++completed_work_;
  SendMessageW(progress_, PBM_SETPOS, static_cast<WPARAM>(completed_work_), 0);
  AppendLog(L"[" + title + L"] 종료 코드 " + std::to_wstring(result.exit_code) +
            (result.cancelled ? L" (취소됨)\r\n" : L"\r\n"));

  if (operation_ == Operation::kCancelling) {
    if (active_tasks_.empty()) {
      FinishOperation(L"작업이 취소되었습니다.");
    }
    return;
  }
  if (operation_ == Operation::kCheckingTools) {
    StartPendingToolProbes();
    return;
  }

  if (!succeeded && next_setup_step_ > 0 &&
      !setup_steps_[next_setup_step_ - 1].continue_after_failure) {
    FinishOperation(L"설정 단계가 실패하여 작업을 중단했습니다.");
    return;
  }
  StartNextSetupStep();
}

void LibraryManagerWindow::FinishOperation(std::wstring status) {
  operation_ = Operation::kIdle;
  setup_steps_.clear();
  SetBusyControls(false);
  SetStatus(status);
  AppendLog(L"=== " + status + L" ===\r\n");
}

void LibraryManagerWindow::SetBusyControls(bool busy) const {
  HMENU menu = GetMenu(window_);
  const UINT enabled = MF_BYCOMMAND | MF_ENABLED;
  const UINT disabled = MF_BYCOMMAND | MF_GRAYED;
  EnableMenuItem(menu, IDM_TOOL_CHECK, enabled);
  EnableMenuItem(menu, IDM_WSL_SETUP, busy ? disabled : enabled);
  EnableMenuItem(menu, IDM_INSTALL_BASE_TOOLS, busy ? disabled : enabled);
  EnableMenuItem(menu, IDM_CANCEL_OPERATION, busy ? enabled : disabled);
  DrawMenuBar(window_);
  if (tool_check_window_ != nullptr) {
    tool_check_window_->SetChecking(busy);
  }
}

void LibraryManagerWindow::SetStatus(std::wstring_view status) const {
  const std::wstring text(status);
  SendMessageW(status_, SB_SETTEXTW, 1, reinterpret_cast<LPARAM>(text.c_str()));
}

void LibraryManagerWindow::SetToolState(std::size_t index, std::wstring status,
                                        std::wstring version) {
  if (index >= tool_states_.size()) {
    return;
  }
  tool_states_[index] = {std::move(status), std::move(version)};
  if (tool_check_window_ != nullptr) {
    tool_check_window_->SetToolState(index, tool_states_[index].status,
                                     tool_states_[index].version);
  }
}

void LibraryManagerWindow::AppendLog(std::wstring_view text) const {
  if (log_ == nullptr || text.empty()) {
    return;
  }
  const std::wstring normalized = NormalizeNewlines(text);
  int length = GetWindowTextLengthW(log_);
  if (length + static_cast<int>(normalized.size()) > kMaximumLogCharacters) {
    const int remove_count = std::max(
        kMaximumLogCharacters / 10,
        length + static_cast<int>(normalized.size()) - kMaximumLogCharacters);
    SendMessageW(log_, EM_SETSEL, 0, std::min(remove_count, length));
    SendMessageW(log_, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(L""));
    length = GetWindowTextLengthW(log_);
  }
  SendMessageW(log_, EM_SETSEL, static_cast<WPARAM>(length),
               static_cast<LPARAM>(length));
  SendMessageW(log_, EM_REPLACESEL, FALSE,
               reinterpret_cast<LPARAM>(normalized.c_str()));
  SendMessageW(log_, EM_SCROLLCARET, 0, 0);
}

void LibraryManagerWindow::ShutdownEventChannel() {
  if (event_channel_ == nullptr) {
    return;
  }
  std::scoped_lock lock(event_channel_->mutex);
  event_channel_->window = nullptr;
  event_channel_->events.clear();
}

}  // namespace designpp::gui

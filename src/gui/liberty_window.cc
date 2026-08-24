// Copyright 2026 The Design++ Authors

#include "designpp/gui/liberty_window.h"

#include <combaseapi.h>
#include <commctrl.h>
#include <commdlg.h>

#include <algorithm>
#include <cwctype>
#include <deque>
#include <filesystem>
#include <mutex>
#include <utility>

namespace designpp::gui {
namespace {

constexpr wchar_t kWindowClassName[] = L"DesignPlusPlus.LibertyWindow";
constexpr int kFileListId = 7201;
constexpr int kAddButtonId = 7202;
constexpr int kReplaceButtonId = 7203;
constexpr int kRemoveButtonId = 7204;
constexpr int kSaveButtonId = 7205;

std::wstring Utf8ToWide(std::string_view text) {
  if (text.empty()) return {};
  const int size =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), nullptr, 0);
  if (size <= 0) return L"[invalid UTF-8]";
  std::wstring result(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                      static_cast<int>(text.size()), result.data(), size);
  return result;
}

std::string WideToUtf8(std::wstring_view text) {
  if (text.empty()) return {};
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                       static_cast<int>(text.size()), nullptr,
                                       0, nullptr, nullptr);
  if (size <= 0) return {};
  std::string result(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                      result.data(), size, nullptr, nullptr);
  return result;
}

std::wstring FileName(std::string_view relative_path) {
  return std::filesystem::path(Utf8ToWide(relative_path)).filename().wstring();
}

bool IsLibertyFile(std::string_view relative_path) {
  std::wstring extension =
      std::filesystem::path(Utf8ToWide(relative_path)).extension().wstring();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](wchar_t value) { return std::towlower(value); });
  return extension == L".lib" || extension == L".liberty";
}

std::string NewSessionId() {
  GUID guid{};
  if (CoCreateGuid(&guid) != S_OK) return "liberty-viewer";
  wchar_t buffer[40]{};
  if (StringFromGUID2(guid, buffer, static_cast<int>(std::size(buffer))) == 0) {
    return "liberty-viewer";
  }
  return WideToUtf8(buffer);
}

}  // namespace

struct LibertyWindow::EventChannel final {
  enum class EventKind {
    kDocumentLoaded,
    kDocumentSaved,
    kLibraryMutated,
  };

  struct Event {
    EventKind kind = EventKind::kDocumentLoaded;
    std::uint64_t generation = 0;
    std::string relative_path;
    std::string text;
    std::wstring action;
    core::Status status;
    application::EditorDocumentSnapshot document;
    application::LibraryRecord library;
  };

  std::mutex mutex;
  HWND window = nullptr;
  std::uint64_t generation = 0;
  std::deque<Event> events;
};

LibertyWindow::~LibertyWindow() {
  Close();
  document_scheduler_.RequestStop();
  mutation_scheduler_.RequestStop();
  editor_host_.Shutdown();
  ShutdownChannel();
}

bool LibertyWindow::Create(
    HINSTANCE instance, application::LibraryRecord library,
    std::shared_ptr<MonacoEditorEnvironment> editor_environment,
    LogCallback central_log, LibraryChangedCallback library_changed) {
  instance_ = instance;
  library_ = std::move(library);
  editor_environment_ = std::move(editor_environment);
  central_log_ = std::move(central_log);
  library_changed_ = std::move(library_changed);

  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.style = CS_HREDRAW | CS_VREDRAW;
  window_class.lpfnWndProc = WindowProcedure;
  window_class.hInstance = instance_;
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  window_class.lpszClassName = kWindowClassName;
  if (RegisterClassExW(&window_class) == 0 &&
      GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return false;
  }

  dpi_ = GetSystemDpi();
  window_ = CreateWindowExW(0, kWindowClassName, L"Design++ Liberty",
                            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                            ScaleForDpi(1120, dpi_), ScaleForDpi(720, dpi_),
                            nullptr, nullptr, instance_, this);
  if (window_ == nullptr) return false;
  ShowWindow(window_, SW_SHOW);
  UpdateWindow(window_);
  return true;
}

bool LibertyWindow::BelongsToLibrary(
    std::string_view library_id) const noexcept {
  return library_.library.id == library_id;
}

bool LibertyWindow::IsOpen() const noexcept { return window_ != nullptr; }

void LibertyWindow::Activate() {
  if (window_ == nullptr) return;
  ShowWindow(window_, SW_RESTORE);
  SetForegroundWindow(window_);
}

void LibertyWindow::RefreshLibrary(application::LibraryRecord library) {
  if (!BelongsToLibrary(library.library.id)) return;
  // A manager refresh can arrive while a mutation is still committing. Changing
  // the generation here would discard its terminal event and leave the buttons
  // permanently disabled. The successful mutation callback requests another
  // refresh after its own result has been applied.
  if (mutation_active_ || !dirty_documents_.empty() ||
      !saving_documents_.empty()) {
    return;
  }
  ++generation_;
  {
    std::scoped_lock lock(event_channel_->mutex);
    event_channel_->generation = generation_;
    event_channel_->events.clear();
  }
  for (const auto& document : documents_) {
    editor_host_.CloseDocument(document.id);
  }
  documents_.clear();
  loading_paths_.clear();
  active_document_id_.clear();
  library_ = std::move(library);
  PopulateFiles();
  UpdateTitle();
  if (editor_ready_) OpenSelectedFile();
}

void LibertyWindow::Close() {
  dirty_documents_.clear();
  close_after_save_ = false;
  if (window_ != nullptr) DestroyWindow(window_);
}

bool LibertyWindow::TranslateAccelerator(const MSG& message) {
  if (window_ == nullptr || message.message != WM_KEYDOWN) return false;
  if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 && message.wParam == 'S') {
    editor_host_.RequestSaveAll();
    return true;
  }
  return false;
}

LRESULT CALLBACK LibertyWindow::WindowProcedure(HWND window, UINT message,
                                                WPARAM wparam, LPARAM lparam) {
  auto* self = reinterpret_cast<LibertyWindow*>(
      GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<LibertyWindow*>(create->lpCreateParams);
    self->window_ = window;
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  }
  return self != nullptr ? self->HandleMessage(message, wparam, lparam)
                         : DefWindowProcW(window, message, wparam, lparam);
}

LRESULT LibertyWindow::HandleMessage(UINT message, WPARAM wparam,
                                     LPARAM lparam) {
  switch (message) {
    case WM_CREATE:
      event_channel_ = std::make_shared<EventChannel>();
      event_channel_->window = window_;
      event_channel_->generation = generation_;
      if (!CreateControls()) return -1;
      PopulateFiles();
      UpdateTitle();
      return 0;
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
      information->ptMinTrackSize = {ScaleForDpi(760, dpi_),
                                     ScaleForDpi(480, dpi_)};
      return 0;
    }
    case WM_COMMAND:
      if (LOWORD(wparam) == kFileListId && HIWORD(wparam) == LBN_DBLCLK) {
        OpenSelectedFile();
        return 0;
      }
      if (LOWORD(wparam) == kFileListId && HIWORD(wparam) == LBN_SELCHANGE) {
        UpdateCommandState();
        return 0;
      }
      if (LOWORD(wparam) == kAddButtonId && HIWORD(wparam) == BN_CLICKED) {
        AddFiles();
        return 0;
      }
      if (LOWORD(wparam) == kReplaceButtonId && HIWORD(wparam) == BN_CLICKED) {
        ReplaceSelectedFile();
        return 0;
      }
      if (LOWORD(wparam) == kRemoveButtonId && HIWORD(wparam) == BN_CLICKED) {
        RemoveSelectedFile();
        return 0;
      }
      if (LOWORD(wparam) == kSaveButtonId && HIWORD(wparam) == BN_CLICKED) {
        editor_host_.RequestSaveAll();
        return 0;
      }
      break;
    case kEventMessage:
      HandleEvents();
      return 0;
    case WM_CLOSE:
      if (PrepareClose()) DestroyWindow(window_);
      return 0;
    case WM_DESTROY:
      editor_host_.Shutdown();
      document_scheduler_.RequestStop();
      mutation_scheduler_.RequestStop();
      ShutdownChannel();
      window_ = nullptr;
      return 0;
    default:
      break;
  }
  return DefWindowProcW(window_, message, wparam, lparam);
}

bool LibertyWindow::CreateControls() {
  identity_ = CreateWindowExW(0, L"STATIC", L"Technology Liberty",
                              WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0,
                              window_, nullptr, instance_, nullptr);
  add_button_ = CreateWindowExW(
      0, L"BUTTON", L"Add...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0,
      0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddButtonId)),
      instance_, nullptr);
  replace_button_ = CreateWindowExW(
      0, L"BUTTON", L"Replace...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0,
      0, 0, window_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kReplaceButtonId)),
      instance_, nullptr);
  remove_button_ = CreateWindowExW(
      0, L"BUTTON", L"Remove", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0,
      0, window_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRemoveButtonId)), instance_,
      nullptr);
  file_list_ = CreateWindowExW(
      WS_EX_CLIENTEDGE, L"LISTBOX", L"",
      WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, 0,
      0, 0, 0, window_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kFileListId)), instance_,
      nullptr);
  editor_container_ =
      CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", L"Loading Liberty viewer...",
                      WS_CHILD | WS_VISIBLE | SS_CENTER, 0, 0, 0, 0, window_,
                      nullptr, instance_, nullptr);
  status_ = CreateWindowExW(0, STATUSCLASSNAMEW, L"Liberty viewer ready",
                            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr,
                            instance_, nullptr);
  save_button_ = CreateWindowExW(
      0, L"BUTTON", L"Save (Ctrl+S)",
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0, window_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSaveButtonId)), instance_,
      nullptr);
  if (identity_ == nullptr || add_button_ == nullptr ||
      replace_button_ == nullptr || remove_button_ == nullptr ||
      save_button_ == nullptr || file_list_ == nullptr ||
      editor_container_ == nullptr || status_ == nullptr) {
    return false;
  }
  ApplyDpi(dpi_);
  if (!editor_host_.Create(
          editor_container_, editor_environment_, NewSessionId(),
          [this](application::EditorWebMessage message) {
            HandleEditorMessage(std::move(message));
          },
          [this](core::Status status) {
            SetStatus(status.Ok() ? L"Liberty viewer ready"
                                  : Utf8ToWide(status.message));
          })) {
    SetStatus(L"Cannot create the Liberty viewer");
  }
  return true;
}

void LibertyWindow::LayoutControls(int width, int height) const {
  const int margin = ScaleForDpi(8, dpi_);
  const int header_height = ScaleForDpi(38, dpi_);
  const int status_height = ScaleForDpi(24, dpi_);
  const int list_width = ScaleForDpi(300, dpi_);
  const int button_width = ScaleForDpi(104, dpi_);
  const int button_gap = ScaleForDpi(6, dpi_);
  const int content_top = margin + header_height;
  const int content_height =
      std::max(0, height - content_top - status_height - margin);
  const int buttons_width = button_width * 4 + button_gap * 3;
  MoveWindow(identity_, margin, margin,
             std::max(0, width - margin * 3 - buttons_width),
             header_height - margin, TRUE);
  const int buttons_x = std::max(margin, width - margin - buttons_width);
  MoveWindow(add_button_, buttons_x, margin, button_width,
             header_height - margin, TRUE);
  MoveWindow(replace_button_, buttons_x + button_width + button_gap, margin,
             button_width, header_height - margin, TRUE);
  MoveWindow(remove_button_, buttons_x + (button_width + button_gap) * 2,
             margin, button_width, header_height - margin, TRUE);
  MoveWindow(save_button_, buttons_x + (button_width + button_gap) * 3, margin,
             button_width, header_height - margin, TRUE);
  MoveWindow(file_list_, margin, content_top, list_width, content_height, TRUE);
  const int editor_x = margin + list_width + margin;
  const int editor_width = std::max(0, width - editor_x - margin);
  MoveWindow(editor_container_, editor_x, content_top, editor_width,
             content_height, TRUE);
  editor_host_.Resize({0, 0, editor_width, content_height});
  MoveWindow(status_, 0, height - status_height, width, status_height, TRUE);
}

void LibertyWindow::ApplyDpi(UINT dpi) {
  dpi_ = dpi == 0 ? kDefaultDpi : dpi;
  font_ = CreateUiFont(dpi_);
  ApplyFontToWindowTree(window_, font_.Get());
  RECT client{};
  GetClientRect(window_, &client);
  LayoutControls(client.right, client.bottom);
}

void LibertyWindow::PopulateFiles() {
  const int previous =
      static_cast<int>(SendMessageW(file_list_, LB_GETCURSEL, 0, 0));
  std::string previous_path;
  if (previous >= 0 && static_cast<std::size_t>(previous) < files_.size()) {
    previous_path = files_[previous];
  }
  files_.clear();
  SendMessageW(file_list_, LB_RESETCONTENT, 0, 0);
  for (const core::ManagedFile& file : library_.library.files) {
    if (IsLibertyFile(file.relative_path)) files_.push_back(file.relative_path);
  }
  std::sort(files_.begin(), files_.end());
  int selected = files_.empty() ? -1 : 0;
  for (std::size_t index = 0; index < files_.size(); ++index) {
    const std::wstring label = FileName(files_[index]);
    SendMessageW(file_list_, LB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(label.c_str()));
    if (files_[index] == previous_path) selected = static_cast<int>(index);
  }
  if (selected >= 0) SendMessageW(file_list_, LB_SETCURSEL, selected, 0);
  UpdateCommandState();
  SetStatus(files_.empty() ? L"No shared .lib/.liberty file in this Library"
                           : L"Select a Liberty file to inspect or manage it");
}

void LibertyWindow::UpdateCommandState() const {
  const bool has_selection = file_list_ != nullptr &&
                             SendMessageW(file_list_, LB_GETCURSEL, 0, 0) >= 0;
  const bool can_mutate = !mutation_active_;
  EnableWindow(add_button_, can_mutate);
  EnableWindow(replace_button_, can_mutate && has_selection);
  EnableWindow(remove_button_, can_mutate && has_selection);
  EnableWindow(save_button_, !mutation_active_ && !dirty_documents_.empty());
}

bool LibertyWindow::PrepareForLibraryMutation() {
  if (dirty_documents_.empty() && saving_documents_.empty()) return true;
  const int choice = MessageBoxW(
      window_,
      L"Save the edited Liberty contents before changing the managed files?",
      L"Design++ Liberty", MB_YESNO | MB_ICONINFORMATION);
  if (choice == IDYES) editor_host_.RequestSaveAll();
  SetStatus(L"Finish saving before changing managed Liberty files");
  return false;
}

void LibertyWindow::AddFiles() {
  if (!PrepareForLibraryMutation()) return;
  std::vector<wchar_t> buffer(65536, L'\0');
  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = window_;
  dialog.lpstrFile = buffer.data();
  dialog.nMaxFile = static_cast<DWORD>(buffer.size());
  dialog.lpstrFilter = L"Liberty files (*.lib;*.liberty)\0*.lib;*.liberty\0\0";
  dialog.Flags = OFN_FILEMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_EXPLORER;
  if (!GetOpenFileNameW(&dialog)) {
    const DWORD error = CommDlgExtendedError();
    if (error == 0) {
      SetStatus(L"Add Liberty file cancelled");
    } else {
      const std::wstring message =
          L"Cannot open the Liberty file picker (error " +
          std::to_wstring(error) + L")";
      SetStatus(message);
      MessageBoxW(window_, message.c_str(), L"Add Liberty file",
                  MB_OK | MB_ICONERROR);
    }
    return;
  }
  std::vector<std::filesystem::path> sources;
  const std::filesystem::path first(buffer.data());
  const wchar_t* cursor = buffer.data() + first.native().size() + 1;
  if (*cursor == L'\0') {
    sources.push_back(first);
  } else {
    while (*cursor != L'\0') {
      const std::filesystem::path name(cursor);
      sources.push_back(first / name);
      cursor += name.native().size() + 1;
    }
  }
  const application::LibraryRecord library = library_;
  SubmitLibraryMutation(L"Add Liberty file",
                        [library, sources = std::move(sources)](
                            application::LibraryService& service) {
                          return service.ImportFiles(library, {}, {}, sources);
                        });
}

void LibertyWindow::ReplaceSelectedFile() {
  if (!PrepareForLibraryMutation()) return;
  const int selected =
      static_cast<int>(SendMessageW(file_list_, LB_GETCURSEL, 0, 0));
  if (selected < 0 || static_cast<std::size_t>(selected) >= files_.size()) {
    SetStatus(L"Select a managed Liberty file to replace");
    MessageBoxW(window_, L"Select a managed Liberty file first.",
                L"Replace Liberty file", MB_OK | MB_ICONINFORMATION);
    return;
  }
  wchar_t buffer[32768]{};
  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = window_;
  dialog.lpstrFile = buffer;
  dialog.nMaxFile = static_cast<DWORD>(std::size(buffer));
  dialog.lpstrFilter = L"Liberty files (*.lib;*.liberty)\0*.lib;*.liberty\0\0";
  dialog.Flags = OFN_FILEMUSTEXIST | OFN_EXPLORER;
  if (!GetOpenFileNameW(&dialog)) {
    const DWORD error = CommDlgExtendedError();
    if (error == 0) {
      SetStatus(L"Replace Liberty file cancelled");
    } else {
      const std::wstring message =
          L"Cannot open the Liberty file picker (error " +
          std::to_wstring(error) + L")";
      SetStatus(message);
      MessageBoxW(window_, message.c_str(), L"Replace Liberty file",
                  MB_OK | MB_ICONERROR);
    }
    return;
  }
  const application::LibraryRecord library = library_;
  const std::string relative_path = files_[selected];
  const std::filesystem::path source(buffer);
  SubmitLibraryMutation(
      L"Replace Liberty file",
      [library, relative_path, source](application::LibraryService& service) {
        return service.ReplaceLibraryFile(library, relative_path, source);
      });
}

void LibertyWindow::RemoveSelectedFile() {
  if (!PrepareForLibraryMutation()) return;
  const int selected =
      static_cast<int>(SendMessageW(file_list_, LB_GETCURSEL, 0, 0));
  if (selected < 0 || static_cast<std::size_t>(selected) >= files_.size()) {
    SetStatus(L"Select a managed Liberty file to remove");
    MessageBoxW(window_, L"Select a managed Liberty file first.",
                L"Remove Liberty file", MB_OK | MB_ICONINFORMATION);
    return;
  }
  const std::wstring question =
      L"Remove " + FileName(files_[selected]) +
      L" from this Library? Existing synthesis and timing settings may become "
      L"invalid.";
  if (MessageBoxW(window_, question.c_str(), L"Remove Liberty file",
                  MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
    return;
  }
  const application::LibraryRecord library = library_;
  const std::string relative_path = files_[selected];
  SubmitLibraryMutation(
      L"Remove Liberty file",
      [library, relative_path](application::LibraryService& service) {
        return service.RemoveLibraryFile(library, relative_path);
      });
}

void LibertyWindow::SubmitLibraryMutation(
    std::wstring action, std::function<core::Result<application::LibraryRecord>(
                             application::LibraryService&)>
                             mutation) {
  if (mutation_active_) return;
  mutation_active_ = true;
  UpdateCommandState();
  SetStatus(action + L"...");
  if (central_log_) central_log_(L"[Liberty] " + action + L" started\r\n");
  const auto channel = event_channel_;
  const std::uint64_t generation = generation_;
  const bool accepted = mutation_scheduler_.Submit(
      [channel, generation, action = std::move(action),
       mutation = std::move(mutation)](std::stop_token stop_token) mutable {
        if (stop_token.stop_requested()) return;
        EventChannel::Event event;
        event.kind = EventChannel::EventKind::kLibraryMutated;
        event.generation = generation;
        event.action = std::move(action);
        application::LibraryService service;
        auto result = mutation(service);
        if (result.Ok()) {
          event.library = std::move(result).Value();
          event.status = core::Status::Success();
        } else {
          event.status = result.GetStatus();
        }
        HWND target = nullptr;
        {
          std::scoped_lock lock(channel->mutex);
          if (channel->window == nullptr || channel->generation != generation) {
            return;
          }
          channel->events.push_back(std::move(event));
          target = channel->window;
        }
        if (!PostMessageW(target, kEventMessage, 0, 0)) {
          std::scoped_lock lock(channel->mutex);
          channel->events.clear();
        }
      });
  if (!accepted) {
    mutation_active_ = false;
    UpdateCommandState();
    SetStatus(L"Liberty management queue is full");
    if (central_log_) {
      central_log_(L"[Liberty] Management queue rejected the operation\r\n");
    }
  }
}

void LibertyWindow::OpenSelectedFile() {
  const int selected =
      static_cast<int>(SendMessageW(file_list_, LB_GETCURSEL, 0, 0));
  if (selected >= 0) OpenFile(static_cast<std::size_t>(selected));
}

void LibertyWindow::OpenFile(std::size_t index) {
  if (index >= files_.size()) return;
  const std::string relative_path = files_[index];
  const auto opened = std::find_if(
      documents_.begin(), documents_.end(), [&](const auto& document) {
        return document.relative_path == relative_path;
      });
  if (opened != documents_.end()) {
    editor_host_.OpenDocument(*opened, WideToUtf8(FileName(relative_path)),
                              "plaintext");
    return;
  }
  if (!loading_paths_.insert(relative_path).second) return;
  const application::LibraryRecord library = library_;
  const auto channel = event_channel_;
  const std::uint64_t generation = generation_;
  const bool accepted =
      document_scheduler_.Submit([library, relative_path, channel,
                                  generation](std::stop_token stop_token) {
        if (stop_token.stop_requested()) return;
        EventChannel::Event event;
        event.generation = generation;
        event.relative_path = relative_path;
        application::ManagedSourceService service;
        auto loaded =
            service.LoadDocument(library, {}, {}, relative_path, false);
        if (loaded.Ok()) {
          event.document = std::move(loaded).Value();
          event.status = core::Status::Success();
        } else {
          event.status = loaded.GetStatus();
        }
        HWND target = nullptr;
        {
          std::scoped_lock lock(channel->mutex);
          if (channel->window == nullptr || channel->generation != generation) {
            return;
          }
          channel->events.push_back(std::move(event));
          target = channel->window;
        }
        PostMessageW(target, kEventMessage, 0, 0);
      });
  if (!accepted) {
    loading_paths_.erase(relative_path);
    SetStatus(L"Liberty viewer load queue is full");
  }
}

void LibertyWindow::HandleEditorMessage(application::EditorWebMessage message) {
  if (message.type == "ready") {
    editor_ready_ = true;
    OpenSelectedFile();
  } else if (message.type == "active_document_changed") {
    active_document_id_ = message.document_id;
  } else if (message.type == "document_changed") {
    if (FindDocument(message.document_id) != nullptr) {
      dirty_documents_.insert(message.document_id);
      close_after_save_ = false;
      UpdateCommandState();
      UpdateTitle();
    }
  } else if (message.type == "save_document") {
    SaveDocument(std::move(message.document_id), std::move(message.text),
                 false);
  } else if (message.type == "fatal_error") {
    SetStatus(Utf8ToWide(message.text));
  }
}

void LibertyWindow::SaveDocument(std::string document_id, std::string text,
                                 bool overwrite_external) {
  application::EditorDocumentSnapshot* document = FindDocument(document_id);
  if (document == nullptr || document->read_only ||
      !saving_documents_.insert(document_id).second) {
    return;
  }
  const application::EditorDocumentSnapshot snapshot = *document;
  const application::LibraryRecord library = library_;
  const auto channel = event_channel_;
  const std::uint64_t generation = generation_;
  const bool accepted = mutation_scheduler_.Submit(
      [snapshot, library, document_id, text = std::move(text),
       overwrite_external, channel,
       generation](std::stop_token stop_token) mutable {
        if (stop_token.stop_requested()) return;
        EventChannel::Event event;
        event.kind = EventChannel::EventKind::kDocumentSaved;
        event.generation = generation;
        event.relative_path = document_id;
        event.text = text;
        application::ManagedSourceService service;
        auto saved =
            service.SaveDocument(library, snapshot, text, overwrite_external);
        if (saved.Ok()) {
          application::SaveDocumentResult result = std::move(saved).Value();
          event.library = std::move(result.library);
          event.document = std::move(result.document);
          event.status = core::Status::Success();
        } else {
          event.status = saved.GetStatus();
        }
        HWND target = nullptr;
        {
          std::scoped_lock lock(channel->mutex);
          if (channel->window == nullptr || channel->generation != generation) {
            return;
          }
          channel->events.push_back(std::move(event));
          target = channel->window;
        }
        PostMessageW(target, kEventMessage, 0, 0);
      });
  if (!accepted) {
    saving_documents_.erase(document_id);
    editor_host_.SaveResult(document_id, false, "Save queue is full");
  }
}

void LibertyWindow::HandleEvents() {
  std::deque<EventChannel::Event> events;
  {
    std::scoped_lock lock(event_channel_->mutex);
    events.swap(event_channel_->events);
  }
  for (auto& event : events) {
    if (event.generation != generation_) continue;
    if (event.kind == EventChannel::EventKind::kLibraryMutated) {
      mutation_active_ = false;
      UpdateCommandState();
      if (!event.status.Ok()) {
        SetStatus(Utf8ToWide(event.status.message));
        if (central_log_) {
          central_log_(L"[Liberty] " + event.action + L" failed: " +
                       Utf8ToWide(event.status.message) + L"\r\n");
        }
        MessageBoxW(window_, Utf8ToWide(event.status.message).c_str(),
                    event.action.c_str(), MB_OK | MB_ICONERROR);
        continue;
      }
      for (const auto& document : documents_) {
        editor_host_.CloseDocument(document.id);
      }
      documents_.clear();
      loading_paths_.clear();
      saving_documents_.clear();
      dirty_documents_.clear();
      active_document_id_.clear();
      library_ = std::move(event.library);
      PopulateFiles();
      UpdateTitle();
      if (editor_ready_) OpenSelectedFile();
      SetStatus(event.action + L" completed");
      if (central_log_) central_log_(L"[Liberty] " + event.action + L"\r\n");
      if (library_changed_) library_changed_();
      continue;
    }
    if (event.kind == EventChannel::EventKind::kDocumentSaved) {
      saving_documents_.erase(event.relative_path);
      if (!event.status.Ok()) {
        editor_host_.SaveResult(event.relative_path, false,
                                event.status.message);
        if (event.status.code == core::ErrorCode::kExternalModification &&
            MessageBoxW(window_,
                        L"The Liberty file changed outside Design++. "
                        L"Overwrite it with the editor contents?",
                        L"Design++ Liberty",
                        MB_YESNO | MB_ICONWARNING) == IDYES) {
          SaveDocument(event.relative_path, std::move(event.text), true);
        }
        SetStatus(Utf8ToWide(event.status.message));
        continue;
      }
      application::EditorDocumentSnapshot* document =
          FindDocument(event.relative_path);
      if (document != nullptr) *document = event.document;
      library_ = std::move(event.library);
      dirty_documents_.erase(event.relative_path);
      editor_host_.SaveResult(event.relative_path, true, "Saved");
      UpdateCommandState();
      UpdateTitle();
      SetStatus(L"Liberty file saved");
      if (central_log_) central_log_(L"[Liberty] Saved managed file\r\n");
      if (library_changed_) library_changed_();
      if (close_after_save_ && dirty_documents_.empty() &&
          saving_documents_.empty()) {
        close_after_save_ = false;
        DestroyWindow(window_);
        return;
      }
      continue;
    }
    loading_paths_.erase(event.relative_path);
    if (!event.status.Ok()) {
      SetStatus(Utf8ToWide(event.status.message));
      continue;
    }
    const std::string display_name =
        WideToUtf8(FileName(event.document.relative_path));
    documents_.push_back(std::move(event.document));
    editor_host_.OpenDocument(documents_.back(), display_name, "plaintext");
    SetStatus(L"Liberty ready for editing");
    if (central_log_) {
      central_log_(L"[Liberty] Opened " + Utf8ToWide(event.relative_path) +
                   L" for editing\r\n");
    }
  }
}

void LibertyWindow::UpdateTitle() {
  const std::wstring name = Utf8ToWide(library_.library.name);
  std::wstring title = L"Design++ Liberty — " + name;
  if (!dirty_documents_.empty()) title += L" *";
  SetWindowTextW(window_, title.c_str());
  SetWindowTextW(
      identity_,
      (L"Technology Liberty — " + name + L" (Library scope)").c_str());
}

void LibertyWindow::SetStatus(std::wstring_view text) const {
  if (status_ != nullptr) SetWindowTextW(status_, std::wstring(text).c_str());
}

void LibertyWindow::ShutdownChannel() {
  if (!event_channel_) return;
  std::scoped_lock lock(event_channel_->mutex);
  event_channel_->window = nullptr;
  ++event_channel_->generation;
  event_channel_->events.clear();
}

application::EditorDocumentSnapshot* LibertyWindow::FindDocument(
    std::string_view document_id) {
  const auto document = std::find_if(
      documents_.begin(), documents_.end(),
      [document_id](const auto& value) { return value.id == document_id; });
  return document == documents_.end() ? nullptr : &*document;
}

bool LibertyWindow::PrepareClose() {
  if (dirty_documents_.empty()) return true;
  const int choice =
      MessageBoxW(window_, L"Save the modified Liberty files before closing?",
                  L"Design++ Liberty", MB_YESNOCANCEL | MB_ICONQUESTION);
  if (choice == IDCANCEL) return false;
  if (choice == IDNO) {
    dirty_documents_.clear();
    return true;
  }
  close_after_save_ = true;
  editor_host_.RequestSaveAll();
  return false;
}

}  // namespace designpp::gui

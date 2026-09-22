// Copyright 2026 The Design++ Authors

#include "designpp/gui/constraints_window.h"

#include <combaseapi.h>
#include <commctrl.h>

#include <algorithm>
#include <cwctype>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <utility>

namespace designpp::gui {
namespace {

constexpr wchar_t kWindowClassName[] = L"DesignPlusPlus.ConstraintsWindow";
constexpr int kSaveButtonId = 7101;
constexpr int kFileListId = 7102;

std::wstring Utf8ToWide(std::string_view text) {
  if (text.empty()) return {};
  const int size =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), nullptr, 0);
  if (size <= 0) return {};
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

std::wstring LowercaseExtension(std::string_view relative_path) {
  std::wstring extension =
      std::filesystem::path(Utf8ToWide(relative_path)).extension().wstring();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](wchar_t value) { return std::towlower(value); });
  return extension;
}

std::string NewSessionId() {
  GUID guid{};
  if (CoCreateGuid(&guid) != S_OK) return "constraints-editor";
  wchar_t buffer[40]{};
  if (StringFromGUID2(guid, buffer, static_cast<int>(std::size(buffer))) == 0) {
    return "constraints-editor";
  }
  return WideToUtf8(buffer);
}

const core::Cell* FindCell(const application::LibraryRecord& library,
                           std::string_view cell_id) {
  const auto cell =
      std::find_if(library.library.cells.begin(), library.library.cells.end(),
                   [cell_id](const core::Cell& candidate) {
                     return candidate.id == cell_id;
                   });
  return cell == library.library.cells.end() ? nullptr : &*cell;
}

const core::View* FindView(const application::LibraryRecord& library,
                           std::string_view cell_id, std::string_view view_id) {
  const core::Cell* cell = FindCell(library, cell_id);
  if (cell == nullptr) return nullptr;
  const auto view = std::find_if(cell->views.begin(), cell->views.end(),
                                 [view_id](const core::View& candidate) {
                                   return candidate.id == view_id;
                                 });
  return view == cell->views.end() ? nullptr : &*view;
}

}  // namespace

struct ConstraintsWindow::FileEntry final {
  std::string cell_id;
  std::string view_id;
  std::string relative_path;
};

enum class ConstraintsEventKind { kLoaded, kSaved };

struct ConstraintsEvent final {
  ConstraintsEventKind kind = ConstraintsEventKind::kLoaded;
  std::uint64_t generation = 0;
  core::Status status;
  std::string key;
  std::string text;
  application::EditorDocumentSnapshot document;
  application::LibraryRecord library;
};

struct ConstraintsWindow::EventChannel final {
  std::mutex mutex;
  HWND window = nullptr;
  std::uint64_t generation = 0;
  std::deque<ConstraintsEvent> events;
};

ConstraintsWindow::ConstraintsWindow() = default;

ConstraintsWindow::~ConstraintsWindow() {
  if (window_ != nullptr) DestroyWindow(window_);
  scheduler_.RequestStop();
  editor_host_.Shutdown();
  ShutdownChannel();
}

bool ConstraintsWindow::Create(
    HINSTANCE instance, const application::WorkspaceOpenRequest& request,
    application::LibraryRecord library,
    std::shared_ptr<MonacoEditorEnvironment> editor_environment,
    LogCallback central_log, LibraryChangedCallback library_changed) {
  instance_ = instance;
  request_ = request;
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
  window_ = CreateWindowExW(0, kWindowClassName, L"Design++ Constraints",
                            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                            CW_USEDEFAULT, CW_USEDEFAULT,
                            ScaleForDpi(1100, dpi_), ScaleForDpi(720, dpi_),
                            nullptr, nullptr, instance_, this);
  if (window_ == nullptr) return false;
  ShowWindow(window_, SW_SHOW);
  UpdateWindow(window_);
  return true;
}

ViewWindowKind ConstraintsWindow::Kind() const noexcept {
  return ViewWindowKind::kConstraints;
}

bool ConstraintsWindow::CanActivate(
    const application::WorkspaceOpenRequest& request,
    core::ViewKind view_kind) const {
  return view_kind == core::ViewKind::kConstraints &&
         MatchesCell(request.library_id, request.cell_id);
}

void ConstraintsWindow::Activate(
    const application::WorkspaceOpenRequest& request) {
  request_.view_id = request.view_id;
  PopulateFiles();
  if (editor_ready_) OpenSelectedFile();
  ShowWindow(window_, SW_RESTORE);
  SetForegroundWindow(window_);
}

bool ConstraintsWindow::BelongsToLibrary(std::string_view library_id) const {
  return request_.library_id == library_id;
}

bool ConstraintsWindow::MatchesCell(std::string_view library_id,
                                    std::string_view cell_id) const {
  return request_.library_id == library_id && request_.cell_id == cell_id;
}

bool ConstraintsWindow::IsOpen() const noexcept { return window_ != nullptr; }

void ConstraintsWindow::RefreshLibrary(application::LibraryRecord library) {
  if (library.library.id != request_.library_id) return;
  library_ = std::move(library);
  PopulateFiles();
  UpdateTitle();
}

bool ConstraintsWindow::PrepareClose() {
  if (dirty_documents_.empty()) return true;
  const int choice =
      MessageBoxW(window_, L"변경된 Constraint 파일을 저장하시겠습니까?",
                  L"Design++ Constraints", MB_YESNOCANCEL | MB_ICONQUESTION);
  if (choice == IDCANCEL) return false;
  if (choice == IDNO) {
    dirty_documents_.clear();
    return true;
  }
  close_after_save_ = true;
  editor_host_.RequestSaveAll();
  return false;
}

void ConstraintsWindow::Close() {
  dirty_documents_.clear();
  close_after_save_ = false;
  if (window_ != nullptr) DestroyWindow(window_);
}

bool ConstraintsWindow::TranslateAccelerator(const MSG& message) {
  if (window_ == nullptr || message.message != WM_KEYDOWN) return false;
  if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 && message.wParam == 'S') {
    editor_host_.RequestSaveAll();
    return true;
  }
  return false;
}

LRESULT CALLBACK ConstraintsWindow::WindowProcedure(HWND window, UINT message,
                                                    WPARAM wparam,
                                                    LPARAM lparam) {
  auto* self = reinterpret_cast<ConstraintsWindow*>(
      GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<ConstraintsWindow*>(create->lpCreateParams);
    self->window_ = window;
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  }
  return self != nullptr ? self->HandleMessage(message, wparam, lparam)
                         : DefWindowProcW(window, message, wparam, lparam);
}

LRESULT ConstraintsWindow::HandleMessage(UINT message, WPARAM wparam,
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
      information->ptMinTrackSize.x = ScaleForDpi(760, dpi_);
      information->ptMinTrackSize.y = ScaleForDpi(480, dpi_);
      return 0;
    }
    case WM_COMMAND:
      if (LOWORD(wparam) == kSaveButtonId) {
        editor_host_.RequestSaveAll();
        return 0;
      }
      if (LOWORD(wparam) == kFileListId &&
          (HIWORD(wparam) == LBN_DBLCLK || HIWORD(wparam) == LBN_SELCHANGE)) {
        UpdateDetails();
        if (HIWORD(wparam) == LBN_DBLCLK) OpenSelectedFile();
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
      scheduler_.RequestStop();
      ShutdownChannel();
      window_ = nullptr;
      return 0;
    default:
      break;
  }
  return DefWindowProcW(window_, message, wparam, lparam);
}

bool ConstraintsWindow::CreateControls() {
  identity_ = CreateWindowExW(0, L"STATIC", L"Timing Constraints",
                              WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE, 0, 0, 0,
                              0, window_, nullptr, instance_, nullptr);
  save_button_ = CreateWindowExW(
      0, L"BUTTON", L"Save (Ctrl+S)", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0,
      0, 0, window_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSaveButtonId)), instance_,
      nullptr);
  file_list_ = CreateWindowExW(
      WS_EX_CLIENTEDGE, L"LISTBOX", L"",
      WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, 0,
      0, 0, 0, window_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kFileListId)), instance_,
      nullptr);
  editor_container_ = CreateWindowExW(
      WS_EX_CLIENTEDGE, L"STATIC", L"Loading constraint editor...",
      WS_CHILD | WS_VISIBLE | SS_CENTER, 0, 0, 0, 0, window_, nullptr,
      instance_, nullptr);
  details_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", L"Select an SDC file",
                             WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0,
                             window_, nullptr, instance_, nullptr);
  status_ = CreateWindowExW(0, STATUSCLASSNAMEW, L"Constraints ready",
                            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr,
                            instance_, nullptr);
  if (identity_ == nullptr || save_button_ == nullptr ||
      file_list_ == nullptr || editor_container_ == nullptr ||
      details_ == nullptr || status_ == nullptr) {
    return false;
  }
  ApplyDpi(dpi_);
  if (!editor_host_.Create(
          editor_container_, editor_environment_, NewSessionId(),
          [this](application::EditorWebMessage message) {
            HandleEditorMessage(std::move(message));
          },
          [this](core::Status status) {
            SetStatus(status.Ok() ? L"Constraint editor ready"
                                  : Utf8ToWide(status.message));
          })) {
    SetStatus(L"Cannot create the constraint editor");
  }
  return true;
}

void ConstraintsWindow::LayoutControls(int width, int height) const {
  if (window_ == nullptr) return;
  const int margin = ScaleForDpi(8, dpi_);
  const int header_height = ScaleForDpi(48, dpi_);
  const int status_height = ScaleForDpi(24, dpi_);
  const int left_width = ScaleForDpi(260, dpi_);
  const int right_width = ScaleForDpi(280, dpi_);
  const int content_top = margin + header_height;
  const int content_height =
      std::max(0, height - content_top - status_height - margin);
  MoveWindow(identity_, margin, margin,
             std::max(0, width - ScaleForDpi(180, dpi_) - margin * 3),
             header_height - margin, TRUE);
  MoveWindow(save_button_, width - ScaleForDpi(180, dpi_) - margin, margin,
             ScaleForDpi(180, dpi_), header_height - margin, TRUE);
  MoveWindow(file_list_, margin, content_top, left_width, content_height, TRUE);
  MoveWindow(details_, std::max(margin, width - right_width - margin),
             content_top, right_width, content_height, TRUE);
  RECT editor_bounds{
      margin + left_width + margin, content_top,
      std::max(margin + left_width + margin, width - right_width - margin * 2),
      content_top + content_height};
  const int editor_width =
      std::max(0, static_cast<int>(editor_bounds.right - editor_bounds.left));
  const int editor_height =
      std::max(0, static_cast<int>(editor_bounds.bottom - editor_bounds.top));
  MoveWindow(editor_container_, editor_bounds.left, editor_bounds.top,
             editor_width, editor_height, TRUE);
  RECT host_bounds{0, 0, editor_width, editor_height};
  editor_host_.Resize(host_bounds);
  MoveWindow(status_, 0, height - status_height, width, status_height, TRUE);
}

void ConstraintsWindow::ApplyDpi(UINT dpi) {
  dpi_ = dpi == 0 ? kDefaultDpi : dpi;
  font_ = CreateUiFont(dpi_);
  ApplyFontToWindowTree(window_, font_.Get());
  RECT client{};
  GetClientRect(window_, &client);
  LayoutControls(client.right, client.bottom);
}

void ConstraintsWindow::PopulateFiles() {
  const int previous =
      static_cast<int>(SendMessageW(file_list_, LB_GETCURSEL, 0, 0));
  std::string previous_path;
  if (previous >= 0 && static_cast<std::size_t>(previous) < files_.size()) {
    previous_path = files_[previous].relative_path;
  }
  files_.clear();
  SendMessageW(file_list_, LB_RESETCONTENT, 0, 0);
  const core::View* view =
      FindView(library_, request_.cell_id, request_.view_id);
  if (view != nullptr && view->kind == core::ViewKind::kConstraints) {
    for (const core::ManagedFile& file : view->files) {
      const std::wstring extension = LowercaseExtension(file.relative_path);
      if (extension != L".sdc") continue;
      files_.push_back({request_.cell_id, view->id, file.relative_path});
    }
  }
  std::sort(files_.begin(), files_.end(),
            [](const FileEntry& left, const FileEntry& right) {
              return left.relative_path < right.relative_path;
            });
  int selected = files_.empty() ? -1 : 0;
  for (std::size_t index = 0; index < files_.size(); ++index) {
    std::wstring label = FileName(files_[index].relative_path);
    SendMessageW(file_list_, LB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(label.c_str()));
    if (files_[index].relative_path == previous_path) {
      selected = static_cast<int>(index);
    }
  }
  if (selected >= 0) SendMessageW(file_list_, LB_SETCURSEL, selected, 0);
  UpdateDetails();
}

void ConstraintsWindow::OpenSelectedFile() {
  const int selected =
      static_cast<int>(SendMessageW(file_list_, LB_GETCURSEL, 0, 0));
  if (selected < 0) return;
  OpenFile(static_cast<std::size_t>(selected));
}

void ConstraintsWindow::OpenFile(std::size_t index) {
  if (index >= files_.size()) return;
  const FileEntry file = files_[index];
  const auto opened = std::find_if(
      documents_.begin(), documents_.end(), [&](const auto& document) {
        return document.relative_path == file.relative_path;
      });
  if (opened != documents_.end()) {
    editor_host_.OpenDocument(*opened, WideToUtf8(FileName(file.relative_path)),
                              "tcl");
    return;
  }
  if (!loading_paths_.insert(file.relative_path).second) return;
  const application::LibraryRecord library = library_;
  const auto channel = event_channel_;
  const std::uint64_t generation = generation_;
  if (!scheduler_.Submit([library, file, channel,
                          generation](std::stop_token stop_token) {
        if (stop_token.stop_requested()) return;
        ConstraintsEvent event;
        event.kind = ConstraintsEventKind::kLoaded;
        event.generation = generation;
        event.key = file.relative_path;
        application::ManagedSourceService service;
        auto loaded = service.LoadDocument(library, file.cell_id, file.view_id,
                                           file.relative_path, false);
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
      })) {
    loading_paths_.erase(file.relative_path);
    SetStatus(L"Constraint editor load queue is full");
  }
}

void ConstraintsWindow::HandleEditorMessage(
    application::EditorWebMessage message) {
  if (message.type == "ready") {
    editor_ready_ = true;
    OpenSelectedFile();
  } else if (message.type == "active_document_changed") {
    active_document_id_ = message.document_id;
    UpdateDetails();
  } else if (message.type == "document_changed") {
    if (FindDocument(message.document_id) != nullptr) {
      dirty_documents_.insert(message.document_id);
      close_after_save_ = false;
      UpdateTitle();
    }
  } else if (message.type == "save_document") {
    SaveDocument(std::move(message.document_id), std::move(message.text),
                 false);
  } else if (message.type == "fatal_error") {
    SetStatus(Utf8ToWide(message.text));
  }
}

void ConstraintsWindow::SaveDocument(std::string document_id, std::string text,
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
  if (!scheduler_.Submit([snapshot, library, document_id,
                          text = std::move(text), overwrite_external, channel,
                          generation](std::stop_token stop_token) mutable {
        if (stop_token.stop_requested()) return;
        ConstraintsEvent event;
        event.kind = ConstraintsEventKind::kSaved;
        event.generation = generation;
        event.key = document_id;
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
      })) {
    saving_documents_.erase(document_id);
    editor_host_.SaveResult(document_id, false, "Save queue is full");
  }
}

void ConstraintsWindow::HandleEvents() {
  std::deque<ConstraintsEvent> events;
  {
    std::scoped_lock lock(event_channel_->mutex);
    events.swap(event_channel_->events);
  }
  for (ConstraintsEvent& event : events) {
    if (event.generation != generation_) continue;
    if (event.kind == ConstraintsEventKind::kLoaded) {
      loading_paths_.erase(event.key);
      if (!event.status.Ok()) {
        SetStatus(Utf8ToWide(event.status.message));
        continue;
      }
      const std::string display_name =
          WideToUtf8(FileName(event.document.relative_path));
      documents_.push_back(std::move(event.document));
      application::EditorDocumentSnapshot& document = documents_.back();
      editor_host_.OpenDocument(document, display_name, "tcl");
      SetStatus(L"SDC ready for editing");
    } else {
      saving_documents_.erase(event.key);
      if (!event.status.Ok()) {
        editor_host_.SaveResult(event.key, false, event.status.message);
        if (event.status.code == core::ErrorCode::kExternalModification &&
            MessageBoxW(window_,
                        L"파일이 외부에서 변경되었습니다. 현재 편집 내용으로 "
                        L"덮어쓰시겠습니까?",
                        L"Design++ Constraints",
                        MB_YESNO | MB_ICONWARNING) == IDYES) {
          SaveDocument(event.key, std::move(event.text), true);
        }
        SetStatus(Utf8ToWide(event.status.message));
        continue;
      }
      application::EditorDocumentSnapshot* document = FindDocument(event.key);
      if (document != nullptr) *document = event.document;
      library_ = std::move(event.library);
      dirty_documents_.erase(event.key);
      editor_host_.SaveResult(event.key, true, "Saved");
      SetStatus(L"Constraint file saved");
      UpdateTitle();
      if (library_changed_) library_changed_();
      if (central_log_) {
        const core::Cell* cell = FindCell(library_, request_.cell_id);
        const std::wstring prefix =
            cell ? L"[Constraints " + Utf8ToWide(cell->name) + L"] "
                 : L"[Constraints] ";
        central_log_(prefix + L"Saved managed SDC\r\n");
      }
      if (close_after_save_ && dirty_documents_.empty() &&
          saving_documents_.empty()) {
        close_after_save_ = false;
        DestroyWindow(window_);
        return;
      }
    }
  }
}

void ConstraintsWindow::UpdateDetails() {
  const int selected =
      static_cast<int>(SendMessageW(file_list_, LB_GETCURSEL, 0, 0));
  if (selected < 0 || static_cast<std::size_t>(selected) >= files_.size()) {
    SetWindowTextW(details_,
                   L"Managed Constraints\r\n\r\nNo SDC file in "
                   L"this View.\r\n\r\nImport files from Library Manager.");
    return;
  }
  const FileEntry& file = files_[selected];
  std::wstring text =
      L"SDC Timing Constraints\r\n\r\nDouble-click to edit.\r\n"
      L"Define clocks, input/output delays, and exceptions.";
  text += L"\r\n\r\nManaged path:\r\n" + Utf8ToWide(file.relative_path);
  SetWindowTextW(details_, text.c_str());
}

void ConstraintsWindow::UpdateTitle() {
  std::wstring cell_name = Utf8ToWide(request_.cell_id);
  if (const core::Cell* cell = FindCell(library_, request_.cell_id);
      cell != nullptr) {
    cell_name = Utf8ToWide(cell->name);
  }
  std::wstring title = L"Design++ Constraints — " + cell_name;
  if (!dirty_documents_.empty()) title += L" *";
  SetWindowTextW(window_, title.c_str());
  SetWindowTextW(identity_, (L"Timing Constraints — " + cell_name).c_str());
}

void ConstraintsWindow::SetStatus(std::wstring_view text) const {
  if (status_ != nullptr) {
    SetWindowTextW(status_, std::wstring(text).c_str());
  }
}

void ConstraintsWindow::ShutdownChannel() {
  if (!event_channel_) return;
  std::scoped_lock lock(event_channel_->mutex);
  event_channel_->window = nullptr;
  ++event_channel_->generation;
  event_channel_->events.clear();
}

application::EditorDocumentSnapshot* ConstraintsWindow::FindDocument(
    std::string_view document_id) {
  const auto document = std::find_if(
      documents_.begin(), documents_.end(),
      [document_id](const auto& value) { return value.id == document_id; });
  return document == documents_.end() ? nullptr : &*document;
}

}  // namespace designpp::gui

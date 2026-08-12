// Copyright 2026 The Design++ Authors

#include "designpp/gui/library_manager_window.h"

#include <commctrl.h>

#include <algorithm>
#include <cstring>
#include <deque>
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

enum class UiEventKind {
  kOutput,
  kComplete,
};

struct UiEvent {
  UiEventKind kind;
  std::uint64_t task_id;
  runtime::OutputEncoding output_encoding;
  std::string output;
  runtime::ProcessResult result;
};

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

std::wstring FirstNonEmptyLine(std::wstring text) {
  std::size_t position = 0;
  while (position < text.size()) {
    const std::size_t end = text.find_first_of(L"\r\n", position);
    std::wstring line = text.substr(position, end - position);
    const std::size_t first = line.find_first_not_of(L" \t");
    if (first != std::wstring::npos) {
      const std::size_t last = line.find_last_not_of(L" \t");
      return line.substr(first, last - first + 1);
    }
    if (end == std::wstring::npos) {
      break;
    }
    position = text.find_first_not_of(L"\r\n", end);
    if (position == std::wstring::npos) {
      break;
    }
  }
  return {};
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

LibraryManagerWindow::~LibraryManagerWindow() {
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
  maximum_parallel_probes_ = std::clamp<std::size_t>(usable_cpus, 1, 4);

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
      information->ptMinTrackSize.x = ScaleForDpi(780, dpi_);
      information->ptMinTrackSize.y = ScaleForDpi(560, dpi_);
      return 0;
    }

    case WM_COMMAND:
      switch (LOWORD(wparam)) {
        case IDM_TOOL_CHECK:
          OpenToolCheck();
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
          DestroyWindow(window_);
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

    case kEventsReadyMessage:
      HandleQueuedEvents();
      return 0;

    case WM_CLOSE:
      DestroyWindow(window_);
      return 0;

    case WM_DESTROY:
      ShutdownEventChannel();
      for (ActiveTask& task : active_tasks_) {
        task.session->Cancel();
      }
      active_tasks_.clear();
      decoder_remainders_.clear();
      operation_ = Operation::kIdle;
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
      WS_CHILD | WS_VISIBLE | TVS_HASLINES | TVS_LINESATROOT | TVS_HASBUTTONS,
      0, 0, 0, 0, window_, nullptr, instance_, nullptr);
  library_list_ = CreateWindowExW(
      WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"Libraries",
      WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, 0,
      0, 0, 0, window_, nullptr, instance_, nullptr);
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
      status_ == nullptr || progress_ == nullptr) {
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

void LibraryManagerWindow::LayoutControls(int width, int height) const {
  if (library_tree_ == nullptr) {
    return;
  }
  SendMessageW(status_, WM_SIZE, 0, 0);
  RECT status_bounds{};
  GetWindowRect(status_, &status_bounds);
  const int status_height = status_bounds.bottom - status_bounds.top;
  const int content_height = std::max(0, height - status_height);
  const int left_width = ScaleForDpi(270, dpi_);
  const int output_height = ScaleForDpi(190, dpi_);
  const int gap = ScaleForDpi(6, dpi_);
  const int upper_height = std::max(0, content_height - output_height - gap);
  const int right_width = std::max(0, width - left_width - gap);

  MoveWindow(library_tree_, 0, 0, left_width, upper_height, TRUE);
  MoveWindow(library_list_, left_width + gap, 0, right_width, upper_height,
             TRUE);
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

void LibraryManagerWindow::ApplyDpi(UINT dpi) {
  dpi_ = dpi == 0 ? kDefaultDpi : dpi;
  font_ = CreateUiFont(dpi_);
  ApplyFontToWindowTree(window_, font_.Get());
  ListView_SetColumnWidth(library_list_, 0, ScaleForDpi(300, dpi_));
  ListView_SetColumnWidth(library_list_, 1, ScaleForDpi(180, dpi_));
  ListView_SetColumnWidth(library_list_, 2, ScaleForDpi(180, dpi_));
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
      L"유지됩니다. 계속하시겠습니까?",
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
  SetStatus(L"EDA 도구를 병렬로 검사하고 있습니다...");
  AppendLog(L"\r\n=== EDA 도구 검사 시작 ===\r\n");
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
      L"APT 도구, cocotb, Nix/OpenLane 2 및 ORFS를 설치하거나 "
      L"업데이트합니다. 대용량 네트워크 다운로드가 발생할 수 있습니다. "
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
    StartTask(runtime::WslExecutor::BuildRequest(tools_[index].probe_command),
              tools_[index].display_name, runtime::OutputEncoding::kUtf8,
              index);
  }
  if (operation_ == Operation::kCheckingTools && active_tasks_.empty() &&
      next_tool_index_ >= tools_.size()) {
    FinishOperation(
        failed_work_ == 0
            ? L"모든 도구가 설치되어 있습니다."
            : L"도구 검사가 완료되었습니다. 미설치 항목을 확인하세요.");
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
    if (event.kind == UiEventKind::kOutput) {
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
    if (succeeded) {
      SetToolState(*tool_index, L"설치됨", FirstNonEmptyLine(decoded));
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

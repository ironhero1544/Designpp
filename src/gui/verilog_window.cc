// Copyright 2026 The Design++ Authors

#include "designpp/gui/verilog_window.h"

#include <combaseapi.h>
#include <commctrl.h>
#include <commdlg.h>
#include <windowsx.h>

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <deque>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

#include "designpp/adapters/cocotb_runner_adapter.h"
#include "designpp/adapters/icarus_debug_adapter.h"
#include "designpp/adapters/icarus_simulation_adapter.h"
#include "designpp/adapters/simulation_adapter_factory.h"
#include "designpp/adapters/simulation_result_parser.h"
#include "designpp/adapters/verilator_adapter.h"
#include "designpp/application/cocotb_run_finalizer.h"
#include "designpp/application/module_scanner.h"
#include "designpp/runtime/path_mapper.h"
#include "designpp/runtime/wsl_executor.h"

namespace designpp::gui {
namespace {

constexpr wchar_t kVerilogClassName[] = L"DesignPlusPlus.VerilogWindow";
constexpr int kSaveButtonId = 5101;
constexpr int kLintButtonId = 5102;
constexpr int kCancelButtonId = 5103;
constexpr int kTopModuleId = 5104;
constexpr int kCpuBudgetId = 5105;
constexpr int kAddSourceButtonId = 5106;
constexpr int kSimulatorBackendId = 5109;
constexpr int kTestbenchTopId = 5110;
constexpr int kWaveformId = 5111;
constexpr int kSimulationRunId = 5112;
constexpr int kSimulationCancelId = 5113;
constexpr int kOpenWaveformId = 5114;
constexpr int kDebugRunId = 5115;
constexpr int kDebugContinueId = 5116;
constexpr int kDebugStepId = 5117;
constexpr int kDebugFinishId = 5118;
constexpr int kDebugSendId = 5119;
constexpr int kCocotbModuleId = 5120;
constexpr int kCocotbTestcaseId = 5121;
constexpr int kWaveformFormatId = 5122;
constexpr int kMaximumOutputCharacters = 2'000'000;

enum class WorkspaceEventKind {
  kLoaded,
  kSourcesRefreshed,
  kOutput,
  kProbeComplete,
  kLintComplete,
  kSimulationProbeComplete,
  kCocotbMakefilesProbeComplete,
  kSimulationPrepared,
  kSimulationCompileComplete,
  kSimulationComplete,
  kDebugProbeComplete,
  kDebugPrepared,
  kDebugCompileComplete,
  kDebugOutput,
  kDebugComplete,
  kViewerComplete,
  kDocumentLoaded,
  kDocumentSaved,
  kFilesImported,
  kExternalFilesChanged,
};

struct WorkspaceEvent {
  WorkspaceEventKind kind = WorkspaceEventKind::kOutput;
  std::uint64_t generation = 0;
  core::Status status;
  std::unique_ptr<application::ProjectDocument> document;
  std::vector<application::ResolvedSource> sources;
  std::vector<std::string> module_candidates;
  std::vector<std::string> document_module_candidates;
  std::unordered_map<std::string, std::vector<std::string>>
      module_parameter_defaults;
  std::unordered_map<std::string, std::vector<std::string>> testbench_modules;
  bool source_snapshot_available = false;
  std::vector<application::RunRecord> runs;
  std::unordered_map<std::string, adapters::SimulationTestSummary>
      run_test_summaries;
  std::string output;
  runtime::ProcessResult process_result;
  std::vector<core::Diagnostic> diagnostics;
  std::shared_ptr<application::RunRecord> run;
  std::optional<adapters::SimulationPlan> simulation_plan;
  std::optional<adapters::CocotbPlan> cocotb_plan;
  std::optional<adapters::SimulationTestSummary> simulation_summary;
  std::optional<adapters::DebugPlan> debug_plan;
  application::EditorDocumentSnapshot editor_document;
  application::LibraryRecord library;
  std::string document_id;
  std::string save_text;
  std::vector<std::filesystem::path> changed_paths;
};

void ScanModuleMetadata(const std::vector<std::filesystem::path>& source_files,
                        WorkspaceEvent* event) {
  application::SystemVerilogModuleScanner scanner;
  for (application::ModuleDeclaration& module :
       scanner.FindModules(source_files)) {
    event->module_candidates.push_back(module.name);
    event->module_parameter_defaults.emplace(
        std::move(module.name), std::move(module.parameter_defaults));
  }
}

void ScanTestbenchMetadata(
    const std::vector<application::ResolvedSource>& sources,
    WorkspaceEvent* event) {
  application::SystemVerilogModuleScanner scanner;
  for (const application::ResolvedSource& source : sources) {
    if (!source.enabled || !source.exists ||
        source.view_kind != core::ViewKind::kTestbench) {
      continue;
    }
    std::vector<std::string> names;
    for (const application::ModuleDeclaration& module :
         scanner.FindModules({source.windows_path})) {
      names.push_back(module.name);
    }
    event->testbench_modules.emplace(source.relative_path, std::move(names));
  }
}

std::string WideToUtf8(std::wstring_view text);

std::string NewUuid() {
  GUID guid{};
  if (CoCreateGuid(&guid) != S_OK) return {};
  wchar_t buffer[40]{};
  const int uuid_length =
      StringFromGUID2(guid, buffer, static_cast<int>(std::size(buffer)));
  if (uuid_length == 0) {
    return {};
  }
  std::string id = WideToUtf8(buffer);
  id.erase(std::remove(id.begin(), id.end(), '{'), id.end());
  id.erase(std::remove(id.begin(), id.end(), '}'), id.end());
  return id;
}

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

std::wstring WindowText(HWND window) {
  const int length = GetWindowTextLengthW(window);
  std::wstring text(static_cast<std::size_t>(std::max(length, 0)) + 1, L'\0');
  GetWindowTextW(window, text.data(), static_cast<int>(text.size()));
  text.resize(std::wcslen(text.c_str()));
  return text;
}

std::wstring JoinValues(const std::vector<std::string>& values) {
  std::wstring result;
  for (const std::string& value : values) {
    if (!result.empty()) result += L"; ";
    result += Utf8ToWide(value);
  }
  return result;
}

std::vector<std::string> SplitValues(std::wstring_view text) {
  std::vector<std::string> result;
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t end = text.find_first_of(L";\r\n", start);
    std::wstring value(text.substr(start, end == std::wstring_view::npos
                                              ? text.size() - start
                                              : end - start));
    const auto not_space = [](wchar_t character) {
      return std::iswspace(character) == 0;
    };
    value.erase(value.begin(),
                std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(),
                value.end());
    if (!value.empty()) result.push_back(WideToUtf8(value));
    if (end == std::wstring_view::npos) break;
    start = end + 1;
  }
  return result;
}

bool IsSimpleHdlIdentifier(std::string_view value) {
  if (value.empty() ||
      !(std::isalpha(static_cast<unsigned char>(value.front())) ||
        value.front() == '_' || value.front() == '$')) {
    return false;
  }
  return std::all_of(value.begin() + 1, value.end(), [](char character) {
    return std::isalnum(static_cast<unsigned char>(character)) ||
           character == '_' || character == '$';
  });
}

std::string ManagedParentPath(std::string_view relative_path) {
  const std::size_t separator = relative_path.find_last_of("/\\");
  return separator == std::string_view::npos
             ? std::string()
             : std::string(relative_path.substr(0, separator));
}

const core::Cell* FindCell(const application::LibraryRecord& library,
                           std::string_view cell_id) {
  const auto iterator = std::find_if(
      library.library.cells.begin(), library.library.cells.end(),
      [cell_id](const core::Cell& cell) { return cell.id == cell_id; });
  return iterator == library.library.cells.end() ? nullptr : &*iterator;
}

const core::View* FindView(const core::Cell* cell, std::string_view view_id) {
  if (cell == nullptr) return nullptr;
  const auto iterator = std::find_if(
      cell->views.begin(), cell->views.end(),
      [view_id](const core::View& view) { return view.id == view_id; });
  return iterator == cell->views.end() ? nullptr : &*iterator;
}

std::wstring SourceKindName(core::ViewKind kind) {
  return Utf8ToWide(core::ViewKindName(kind));
}

std::wstring DiagnosticSeverityName(core::DiagnosticSeverity severity) {
  switch (severity) {
    case core::DiagnosticSeverity::kError:
      return L"Error";
    case core::DiagnosticSeverity::kWarning:
      return L"Warning";
    case core::DiagnosticSeverity::kInfo:
      return L"Info";
  }
  return L"Info";
}

bool PathsEqual(const std::filesystem::path& left,
                const std::filesystem::path& right) {
  const std::wstring normalized_left = left.lexically_normal().wstring();
  const std::wstring normalized_right = right.lexically_normal().wstring();
  return _wcsicmp(normalized_left.c_str(), normalized_right.c_str()) == 0;
}

core::Result<std::filesystem::path> CreateVerilatorBuildDirectory(
    std::string_view run_id) {
  wchar_t temporary_root[MAX_PATH]{};
  const DWORD length = GetTempPathW(
      static_cast<DWORD>(std::size(temporary_root)), temporary_root);
  if (length == 0 || length >= std::size(temporary_root)) {
    return core::Status{core::ErrorCode::kIoError,
                        "Cannot resolve the Windows temporary directory",
                        GetLastError()};
  }
  const std::filesystem::path root(temporary_root);
  if (std::any_of(root.native().begin(), root.native().end(),
                  [](wchar_t character) { return character > 0x7f; })) {
    return core::Status{
        core::ErrorCode::kInvalidArgument,
        "Verilator requires an ASCII Windows temporary-directory path", 0};
  }
  const std::filesystem::path directory =
      root / L"DesignPlusPlus" / L"verilator" / Utf8ToWide(run_id);
  std::error_code error;
  if (!std::filesystem::create_directories(directory, error) || error) {
    return core::Status{core::ErrorCode::kIoError,
                        "Cannot create Verilator build directory",
                        static_cast<unsigned long>(error.value())};
  }
  return directory;
}

void RemoveVerilatorBuildDirectory(const adapters::SimulationPlan& plan,
                                   const application::RunRecord& run) {
  if (PathsEqual(plan.waveform_path.parent_path(),
                 run.directory / L"artifacts")) {
    return;
  }
  std::error_code error;
  std::filesystem::remove_all(plan.waveform_path.parent_path(), error);
}

core::Result<std::string> HashSimulationInputs(
    const adapters::SimulationRequest& request) {
  std::uint64_t hash = 1469598103934665603ULL;
  const auto append = [&hash](std::string_view bytes) {
    for (const unsigned char byte : bytes) {
      hash ^= byte;
      hash *= 1099511628211ULL;
    }
  };
  append(request.testbench_view_id);
  append(request.testbench_relative_path);
  append(request.testbench_top);
  append(request.waveform_format);
  for (const std::string& define : request.project.defines) append(define);
  for (const std::string& directory : request.project.include_directories) {
    append(directory);
  }
  for (const application::ResolvedSource& source : request.sources) {
    if (!source.enabled || (source.view_kind != core::ViewKind::kVerilog &&
                            !(source.view_kind == core::ViewKind::kTestbench &&
                              source.view_id == request.testbench_view_id))) {
      continue;
    }
    append(source.relative_path);
    std::ifstream input(source.windows_path, std::ios::binary);
    if (!input) {
      return core::Status{core::ErrorCode::kIoError,
                          "Cannot hash a simulation source", 0};
    }
    std::vector<char> buffer(64 * 1024);
    while (input.read(buffer.data(),
                      static_cast<std::streamsize>(buffer.size())) ||
           input.gcount() > 0) {
      append(std::string_view(buffer.data(),
                              static_cast<std::size_t>(input.gcount())));
    }
    if (!input.eof()) {
      return core::Status{core::ErrorCode::kIoError,
                          "Cannot finish hashing a simulation source", 0};
    }
  }
  std::ostringstream output;
  output << std::hex << hash;
  return output.str();
}

}  // namespace

struct VerilogWindow::EventChannel final {
  std::mutex mutex;
  HWND window = nullptr;
  std::uint64_t generation = 0;
  std::deque<WorkspaceEvent> events;
  bool external_change_pending = false;
};

struct VerilogWindow::SourceTreeNode final {
  enum class Kind { kCategory, kView, kFolder, kFile };

  Kind kind = Kind::kCategory;
  std::string stable_key;
  std::string view_id;
  std::size_t source_index = 0;
  HTREEITEM item = nullptr;
};

VerilogWindow::VerilogWindow() = default;

VerilogWindow::~VerilogWindow() {
  if (window_ != nullptr) DestroyWindow(window_);
  file_watch_service_.Stop();
  scheduler_.RequestStop();
  if (process_) process_->Cancel();
  viewer_execution_.Shutdown();
  process_.reset();
  ShutdownChannel();
}

bool VerilogWindow::Create(
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
  window_class.lpszClassName = kVerilogClassName;
  if (RegisterClassExW(&window_class) == 0 &&
      GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return false;
  }
  dpi_ = GetSystemDpi();
  window_ = CreateWindowExW(0, kVerilogClassName, L"Design++ Workspace",
                            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                            CW_USEDEFAULT, CW_USEDEFAULT,
                            ScaleForDpi(1120, dpi_), ScaleForDpi(760, dpi_),
                            nullptr, nullptr, instance_, this);
  if (window_ == nullptr) return false;
  ShowWindow(window_, SW_SHOW);
  UpdateWindow(window_);
  return true;
}

bool VerilogWindow::Matches(std::string_view library_id,
                            std::string_view cell_id) const {
  return request_.library_id == library_id && request_.cell_id == cell_id;
}

bool VerilogWindow::MatchesView(std::string_view library_id,
                                std::string_view cell_id,
                                std::string_view view_id) const {
  return Matches(library_id, cell_id) && request_.view_id == view_id;
}

ViewWindowKind VerilogWindow::Kind() const noexcept {
  return ViewWindowKind::kVerilog;
}

bool VerilogWindow::CanActivate(
    const application::WorkspaceOpenRequest& request,
    core::ViewKind view_kind) const {
  const bool source_view = view_kind == core::ViewKind::kVerilog ||
                           view_kind == core::ViewKind::kTestbench;
  return source_view && Matches(request.library_id, request.cell_id);
}

void VerilogWindow::Activate(const application::WorkspaceOpenRequest& request) {
  ActivateView(request.view_id);
}

bool VerilogWindow::BelongsToLibrary(std::string_view library_id) const {
  return request_.library_id == library_id;
}

bool VerilogWindow::MatchesCell(std::string_view library_id,
                                std::string_view cell_id) const {
  return Matches(library_id, cell_id);
}

bool VerilogWindow::IsOpen() const noexcept { return window_ != nullptr; }

void VerilogWindow::ActivateView(std::string view_id) {
  request_.view_id = std::move(view_id);
  if (window_ == nullptr) return;
  PopulateProject();
  UpdateInspector();
  UpdateTitle();
  if (editor_host_.Ready()) {
    const auto last_document = last_document_by_view_.find(request_.view_id);
    auto source = std::find_if(
        sources_.begin(), sources_.end(), [&](const auto& candidate) {
          return candidate.view_id == request_.view_id && candidate.exists &&
                 (last_document == last_document_by_view_.end() ||
                  candidate.relative_path == last_document->second);
        });
    if (source == sources_.end() &&
        last_document != last_document_by_view_.end()) {
      source = std::find_if(
          sources_.begin(), sources_.end(), [&](const auto& candidate) {
            return candidate.view_id == request_.view_id && candidate.exists;
          });
    }
    if (source != sources_.end()) {
      OpenSource(
          static_cast<std::size_t>(std::distance(sources_.begin(), source)));
    }
  }
  ShowWindow(window_, SW_RESTORE);
  SetForegroundWindow(window_);
}

void VerilogWindow::RefreshLibrary(application::LibraryRecord library) {
  if (library.library.id != request_.library_id || !document_ ||
      window_ == nullptr) {
    return;
  }
  library_ = std::move(library);
  const auto channel = event_channel_;
  const auto snapshot = library_;
  const auto project = document_->project;
  const std::string cell_id = request_.cell_id;
  const std::uint64_t generation = generation_;
  static_cast<void>(scheduler_.Submit([channel, snapshot, project, cell_id,
                                       generation](std::stop_token stop_token) {
    if (stop_token.stop_requested()) return;
    WorkspaceEvent event;
    event.kind = WorkspaceEventKind::kSourcesRefreshed;
    event.generation = generation;
    application::ProjectService service;
    auto resolved = service.ResolveSources(snapshot, cell_id, project);
    if (!resolved.Ok()) {
      event.status = resolved.GetStatus();
    } else {
      event.sources = std::move(resolved).Value();
      std::vector<std::filesystem::path> rtl_paths;
      for (const auto& source : event.sources) {
        if (source.enabled && source.exists &&
            source.view_kind == core::ViewKind::kVerilog) {
          rtl_paths.push_back(source.windows_path);
        }
      }
      ScanModuleMetadata(rtl_paths, &event);
      ScanTestbenchMetadata(event.sources, &event);
      event.status = core::Status::Success();
    }
    HWND target = nullptr;
    {
      std::scoped_lock lock(channel->mutex);
      if (!channel->window || channel->generation != generation) return;
      channel->events.push_back(std::move(event));
      target = channel->window;
    }
    PostMessageW(target, kEventMessage, 0, 0);
  }));
}

bool VerilogWindow::PrepareClose() {
  if (window_ == nullptr || close_prepared_) return true;
  if (active_operation_ != ActiveOperation::kNone) {
    const int choice = MessageBoxW(
        window_, L"작업이 실행 중입니다. 취소하고 Workspace를 닫으시겠습니까?",
        L"Design++ Workspace", MB_YESNO | MB_ICONWARNING);
    if (choice != IDYES) return false;
    CancelActiveOperation();
  }
  if (!dirty_document_ids_.empty() || dirty_) {
    const int choice = MessageBoxW(
        window_, L"열린 파일과 프로젝트 변경 사항을 저장하시겠습니까?",
        L"Design++ Workspace", MB_YESNOCANCEL | MB_ICONQUESTION);
    if (choice == IDCANCEL) return false;
    if (choice == IDYES) {
      if (!dirty_document_ids_.empty()) {
        save_project_after_documents_ = true;
        close_after_save_ = true;
        RequestSaveAll();
        SetStatus(L"Saving source files before closing Workspace");
        return false;
      }
      if (dirty_ && !SaveProject()) return false;
    }
    if (choice == IDNO) {
      dirty_ = false;
      dirty_document_ids_.clear();
    }
  }
  close_prepared_ = true;
  return true;
}

void VerilogWindow::Close() {
  if (window_ == nullptr) return;
  if (!close_prepared_ && !PrepareClose()) return;
  DestroyWindow(window_);
}

bool VerilogWindow::TranslateAccelerator(const MSG& message) {
  if (window_ == nullptr || message.message != WM_KEYDOWN ||
      message.wParam != 'S' || (GetKeyState(VK_CONTROL) & 0x8000) == 0 ||
      (message.hwnd != window_ && !IsChild(window_, message.hwnd))) {
    return false;
  }
  if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) {
    if (!dirty_document_ids_.empty()) {
      save_project_after_documents_ = true;
      RequestSaveAll();
    } else {
      static_cast<void>(SaveProject());
    }
  } else if (!editor_host_.ContainsFocus()) {
    static_cast<void>(SaveProject());
  } else {
    return false;
  }
  return true;
}

LRESULT CALLBACK VerilogWindow::WindowProcedure(HWND window, UINT message,
                                                WPARAM wparam, LPARAM lparam) {
  auto* self = reinterpret_cast<VerilogWindow*>(
      GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<VerilogWindow*>(create->lpCreateParams);
    self->window_ = window;
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  }
  return self ? self->HandleMessage(message, wparam, lparam)
              : DefWindowProcW(window, message, wparam, lparam);
}

LRESULT VerilogWindow::HandleMessage(UINT message, WPARAM wparam,
                                     LPARAM lparam) {
  switch (message) {
    case WM_CREATE:
      event_channel_ = std::make_shared<EventChannel>();
      event_channel_->window = window_;
      event_channel_->generation = generation_;
      if (!CreateControls()) return -1;
      BeginLoad();
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
      information->ptMinTrackSize.x = ScaleForDpi(820, dpi_);
      information->ptMinTrackSize.y = ScaleForDpi(580, dpi_);
      return 0;
    }

    case WM_COMMAND:
      if (LOWORD(wparam) == kAddSourceButtonId) {
        AddSourceFiles();
        return 0;
      }
      if (LOWORD(wparam) == kSaveButtonId) {
        static_cast<void>(SaveProject());
        return 0;
      }
      if (LOWORD(wparam) == kLintButtonId) {
        StartLint();
        return 0;
      }
      if (LOWORD(wparam) == kCancelButtonId) {
        CancelActiveOperation();
        return 0;
      }
      if (LOWORD(wparam) == kSimulationRunId) {
        StartSimulation();
        return 0;
      }
      if (LOWORD(wparam) == kSimulationCancelId) {
        CancelActiveOperation();
        return 0;
      }
      if (LOWORD(wparam) == kOpenWaveformId) {
        OpenWaveform();
        return 0;
      }
      if (LOWORD(wparam) == kDebugRunId) {
        StartDebug();
        return 0;
      }
      if (LOWORD(wparam) == kDebugContinueId) {
        QueueDebugCommand(application::DebugCommandKind::kContinue);
        return 0;
      }
      if (LOWORD(wparam) == kDebugStepId) {
        QueueDebugCommand(application::DebugCommandKind::kStep);
        return 0;
      }
      if (LOWORD(wparam) == kDebugFinishId) {
        QueueDebugCommand(application::DebugCommandKind::kFinish);
        return 0;
      }
      if (LOWORD(wparam) == kDebugSendId) {
        QueueDebugCommand(application::DebugCommandKind::kConsole,
                          WideToUtf8(WindowText(debug_command_)));
        return 0;
      }
      if (!applying_controls_ &&
          (LOWORD(wparam) == kTestbenchTopId ||
           LOWORD(wparam) == kSimulatorBackendId ||
           LOWORD(wparam) == kWaveformId || LOWORD(wparam) == kCocotbModuleId ||
           LOWORD(wparam) == kCocotbTestcaseId ||
           LOWORD(wparam) == kWaveformFormatId) &&
          (HIWORD(wparam) == CBN_SELCHANGE ||
           HIWORD(wparam) == CBN_EDITCHANGE || HIWORD(wparam) == BN_CLICKED ||
           HIWORD(wparam) == EN_CHANGE)) {
        ReadTestbenchControls();
        if (LOWORD(wparam) == kSimulatorBackendId) {
          const bool cocotb =
              SendMessageW(simulator_backend_, CB_GETCURSEL, 0, 0) >= 2;
          EnableWindow(cocotb_module_, cocotb);
          EnableWindow(cocotb_testcase_, cocotb);
          EnableWindow(debug_run_button_, !cocotb);
        }
        return 0;
      }
      if (!applying_controls_ &&
          (reinterpret_cast<HWND>(lparam) == top_module_ ||
           reinterpret_cast<HWND>(lparam) == cpu_budget_ ||
           reinterpret_cast<HWND>(lparam) == include_directories_ ||
           reinterpret_cast<HWND>(lparam) == defines_ ||
           reinterpret_cast<HWND>(lparam) == parameters_ ||
           reinterpret_cast<HWND>(lparam) == constraint_path_) &&
          (HIWORD(wparam) == EN_CHANGE || HIWORD(wparam) == CBN_EDITCHANGE ||
           HIWORD(wparam) == CBN_SELCHANGE)) {
        if (reinterpret_cast<HWND>(lparam) == top_module_ &&
            HIWORD(wparam) == CBN_SELCHANGE) {
          PopulateParameterDefaultsForSelection();
        }
        MarkDirty();
        return 0;
      }
      break;

    case WM_NOTIFY: {
      ++common_control_notification_depth_;
      struct NotificationDepthGuard final {
        int& depth;
        ~NotificationDepthGuard() { --depth; }
      } notification_depth_guard{common_control_notification_depth_};
      const auto* header = reinterpret_cast<NMHDR*>(lparam);
      if (header->hwndFrom == source_tree_ &&
          header->code == TVN_ITEMCHANGEDW && !applying_controls_) {
        const auto* changed = reinterpret_cast<NMTVITEMCHANGE*>(lparam);
        if ((changed->uChanged & TVIF_STATE) == 0 ||
            ((changed->uStateOld ^ changed->uStateNew) & TVIS_STATEIMAGEMASK) ==
                0) {
          return 0;
        }
        auto* node = reinterpret_cast<SourceTreeNode*>(changed->lParam);
        if (node == nullptr) return 0;
        const bool enabled =
            ((changed->uStateNew & TVIS_STATEIMAGEMASK) >> 12) == 2;
        applying_controls_ = true;
        if (node->kind == SourceTreeNode::Kind::kFile) {
          ToggleSource(node->source_index, enabled);
        } else {
          std::function<void(HTREEITEM)> toggle_descendants =
              [&](HTREEITEM parent) {
                for (HTREEITEM child = TreeView_GetChild(source_tree_, parent);
                     child != nullptr;
                     child = TreeView_GetNextSibling(source_tree_, child)) {
                  TVITEMW item{};
                  item.mask = TVIF_PARAM;
                  item.hItem = child;
                  if (TreeView_GetItem(source_tree_, &item)) {
                    auto* child_node =
                        reinterpret_cast<SourceTreeNode*>(item.lParam);
                    if (child_node != nullptr &&
                        child_node->kind == SourceTreeNode::Kind::kFile) {
                      ToggleSource(child_node->source_index, enabled);
                    } else {
                      toggle_descendants(child);
                    }
                  }
                }
              };
          toggle_descendants(changed->hItem);
        }
        applying_controls_ = false;
        // The TreeView continues using the notified item after this callback
        // returns. Rebuilding here would delete that item reentrantly and leave
        // comctl32 traversing freed internal nodes.
        if (!source_tree_rebuild_pending_) {
          source_tree_rebuild_pending_ = true;
          if (!PostMessageW(window_, kSourceTreeRebuildMessage, 0, 0)) {
            source_tree_rebuild_pending_ = false;
          }
        }
        return 0;
      }
      if (header->hwndFrom == source_tree_ && header->code == NM_DBLCLK) {
        const HTREEITEM selected = TreeView_GetSelection(source_tree_);
        TVITEMW item{};
        item.mask = TVIF_PARAM;
        item.hItem = selected;
        if (selected != nullptr && TreeView_GetItem(source_tree_, &item)) {
          auto* node = reinterpret_cast<SourceTreeNode*>(item.lParam);
          if (node != nullptr && node->kind == SourceTreeNode::Kind::kFile) {
            OpenSource(node->source_index);
          }
        }
        return 0;
      }
      if (header->hwndFrom == problems_ && header->code == NM_DBLCLK) {
        const auto* activation = reinterpret_cast<NMITEMACTIVATE*>(lparam);
        if (activation->iItem >= 0 &&
            static_cast<std::size_t>(activation->iItem) < diagnostics_.size()) {
          OpenSourceByDiagnostic(diagnostics_[activation->iItem]);
        }
        return 0;
      }
      if (header->hwndFrom == runs_ &&
          (header->code == LVN_ITEMCHANGED || header->code == NM_CLICK)) {
        const int selected = ListView_GetNextItem(runs_, -1, LVNI_SELECTED);
        const application::RunRecord* selected_run =
            selected >= 0 &&
                    static_cast<std::size_t>(selected) < run_records_.size()
                ? &run_records_[selected]
                : nullptr;
        PopulateArtifacts(selected_run);
        simulation_summary_.reset();
        if (selected_run != nullptr) {
          const auto summary = run_test_summaries_.find(selected_run->id);
          if (summary != run_test_summaries_.end()) {
            simulation_summary_ = summary->second;
          }
        }
        PopulateTestSummary();
      }
      if (header->hwndFrom == artifacts_ && header->code == NM_DBLCLK) {
        OpenWaveform();
        return 0;
      }
      if (header->hwndFrom == debug_variables_ && header->code == NM_DBLCLK) {
        const int selected =
            ListView_GetNextItem(debug_variables_, -1, LVNI_SELECTED);
        if (selected >= 0 &&
            static_cast<std::size_t>(selected) < debug_variable_names_.size()) {
          QueueDebugCommand(application::DebugCommandKind::kConsole,
                            "$display " + debug_variable_names_[selected]);
        }
        return 0;
      }
      if (header->hwndFrom == debug_scope_tree_ && header->code == NM_DBLCLK) {
        const HTREEITEM selected = TreeView_GetSelection(debug_scope_tree_);
        wchar_t name[256]{};
        TVITEMW item{};
        item.mask = TVIF_TEXT;
        item.hItem = selected;
        item.pszText = name;
        item.cchTextMax = static_cast<int>(std::size(name));
        if (selected != nullptr && TreeView_GetItem(debug_scope_tree_, &item)) {
          const std::string scope = WideToUtf8(name);
          if (scope == "..") {
            QueueDebugCommand(application::DebugCommandKind::kConsole, "pop");
          } else if (scope != debug_session_.Snapshot().scope) {
            QueueDebugCommand(application::DebugCommandKind::kConsole,
                              "push " + scope);
          }
          QueueDebugCommand(application::DebugCommandKind::kConsole, "where");
          QueueDebugCommand(application::DebugCommandKind::kConsole, "list");
        }
        return 0;
      }
      if (header->hwndFrom == bottom_tabs_ && header->code == TCN_SELCHANGE) {
        ShowBottomPage(TabCtrl_GetCurSel(bottom_tabs_));
        return 0;
      }
      break;
    }

    case WM_KEYDOWN:
      if (wparam == VK_INSERT && GetFocus() == source_tree_) {
        AddSourceFiles();
        return 0;
      }
      if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 && wparam == 'S') {
        static_cast<void>(SaveProject());
        return 0;
      }
      break;

    case WM_SETCURSOR:
      if (LOWORD(lparam) == HTCLIENT) {
        POINT point{};
        GetCursorPos(&point);
        ScreenToClient(window_, &point);
        const SplitterDrag splitter = HitTestSplitter(point);
        if (splitter != SplitterDrag::kNone) {
          SetCursor(LoadCursorW(nullptr, splitter == SplitterDrag::kBottom
                                             ? IDC_SIZENS
                                             : IDC_SIZEWE));
          return TRUE;
        }
      }
      break;

    case WM_LBUTTONDOWN: {
      const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      splitter_drag_ = HitTestSplitter(point);
      if (splitter_drag_ != SplitterDrag::kNone) {
        SetCapture(window_);
        return 0;
      }
      break;
    }

    case WM_MOUSEMOVE:
      if (splitter_drag_ != SplitterDrag::kNone && GetCapture() == window_) {
        UpdateSplitter({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
        return 0;
      }
      break;

    case WM_LBUTTONUP:
      if (splitter_drag_ != SplitterDrag::kNone) {
        splitter_drag_ = SplitterDrag::kNone;
        ReleaseCapture();
        return 0;
      }
      break;

    case WM_CAPTURECHANGED:
      splitter_drag_ = SplitterDrag::kNone;
      break;

    case kEventMessage:
      HandleEvents();
      return 0;

    case kSourceTreeRebuildMessage:
      source_tree_rebuild_pending_ = false;
      if (window_ != nullptr && source_tree_ != nullptr) PopulateSources();
      return 0;

    case kDebugViewRebuildMessage:
      debug_view_rebuild_pending_ = false;
      if (window_ != nullptr) PopulateDebugView();
      return 0;

    case WM_CLOSE:
      if (PrepareClose()) Close();
      return 0;

    case WM_DESTROY:
      ++generation_;
      source_tree_rebuild_pending_ = false;
      debug_view_rebuild_pending_ = false;
      file_watch_service_.Stop();
      scheduler_.RequestStop();
      testbench_execution_.Shutdown();
      if (process_) process_->Cancel();
      viewer_execution_.Shutdown();
      process_.reset();
      editor_host_.Shutdown();
      ShutdownChannel();
      window_ = nullptr;
      return 0;

    default:
      break;
  }
  return DefWindowProcW(window_, message, wparam, lparam);
}

bool VerilogWindow::CreateControls() {
  add_source_button_ = CreateWindowExW(
      0, L"BUTTON", L"Add Files...",
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0, window_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddSourceButtonId)),
      instance_, nullptr);
  source_tree_ = CreateWindowExW(
      WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASBUTTONS | TVS_HASLINES |
          TVS_LINESATROOT | TVS_SHOWSELALWAYS | TVS_CHECKBOXES,
      0, 0, 0, 0, window_, nullptr, instance_, nullptr);
  top_module_ = CreateWindowExW(
      WS_EX_CLIENTEDGE, WC_COMBOBOXW, L"",
      WS_CHILD | WS_VISIBLE | CBS_DROPDOWN | WS_VSCROLL, 0, 0, 0, 0, window_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTopModuleId)), instance_,
      nullptr);
  cpu_budget_ = CreateWindowExW(
      WS_EX_CLIENTEDGE, L"EDIT", L"1",
      WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_AUTOHSCROLL, 0, 0, 0, 0, window_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCpuBudgetId)), instance_,
      nullptr);
  include_directories_ = CreateWindowExW(
      WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0,
      0, 0, 0, window_, nullptr, instance_, nullptr);
  defines_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                             WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0,
                             window_, nullptr, instance_, nullptr);
  parameters_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0,
                                0, window_, nullptr, instance_, nullptr);
  constraint_path_ = CreateWindowExW(
      WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0,
      0, 0, 0, window_, nullptr, instance_, nullptr);
  active_view_ =
      CreateWindowExW(0, L"STATIC", L"View: loading...", WS_CHILD | WS_VISIBLE,
                      0, 0, 0, 0, window_, nullptr, instance_, nullptr);
  editor_container_ =
      CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", L"Loading Monaco editor...",
                      WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 0, 0,
                      window_, nullptr, instance_, nullptr);
  save_button_ = CreateWindowExW(
      0, L"BUTTON", L"Save (Ctrl+S)", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0,
      0, 0, 0, window_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSaveButtonId)), instance_,
      nullptr);
  lint_button_ = CreateWindowExW(
      0, L"BUTTON", L"Verilator Lint", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0,
      0, 0, 0, window_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kLintButtonId)), instance_,
      nullptr);
  cancel_button_ = CreateWindowExW(
      0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0,
      0, window_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCancelButtonId)), instance_,
      nullptr);
  simulator_label_ =
      CreateWindowExW(0, L"STATIC", L"Simulator", WS_CHILD, 0, 0, 0, 0, window_,
                      nullptr, instance_, nullptr);
  simulator_backend_ = CreateWindowExW(
      WS_EX_CLIENTEDGE, WC_COMBOBOXW, L"",
      WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, 0, 0, window_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSimulatorBackendId)),
      instance_, nullptr);
  dut_top_label_ =
      CreateWindowExW(0, L"STATIC", L"DUT Top: not configured", WS_CHILD, 0, 0,
                      0, 0, window_, nullptr, instance_, nullptr);
  testbench_top_ = CreateWindowExW(
      WS_EX_CLIENTEDGE, WC_COMBOBOXW, L"", WS_CHILD | CBS_DROPDOWN | WS_VSCROLL,
      0, 0, 0, 0, window_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTestbenchTopId)), instance_,
      nullptr);
  cocotb_module_ = CreateWindowExW(
      WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 0, 0,
      window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCocotbModuleId)),
      instance_, nullptr);
  cocotb_testcase_ = CreateWindowExW(
      WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 0, 0,
      window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCocotbTestcaseId)),
      instance_, nullptr);
  waveform_checkbox_ = CreateWindowExW(
      0, L"BUTTON", L"Generate VCD", WS_CHILD | BS_AUTOCHECKBOX, 0, 0, 0, 0,
      window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kWaveformId)),
      instance_, nullptr);
  waveform_format_ = CreateWindowExW(
      WS_EX_CLIENTEDGE, WC_COMBOBOXW, L"",
      WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, 0, 0, window_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kWaveformFormatId)),
      instance_, nullptr);
  simulation_run_button_ = CreateWindowExW(
      0, L"BUTTON", L"Run Testbench", WS_CHILD | BS_PUSHBUTTON, 0, 0, 0, 0,
      window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSimulationRunId)),
      instance_, nullptr);
  simulation_cancel_button_ = CreateWindowExW(
      0, L"BUTTON", L"Cancel", WS_CHILD | BS_PUSHBUTTON, 0, 0, 0, 0, window_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSimulationCancelId)),
      instance_, nullptr);
  open_waveform_button_ = CreateWindowExW(
      0, L"BUTTON", L"Open Waveform", WS_CHILD | BS_PUSHBUTTON, 0, 0, 0, 0,
      window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kOpenWaveformId)),
      instance_, nullptr);
  simulation_status_ =
      CreateWindowExW(0, L"STATIC", L"Testbench ready", WS_CHILD, 0, 0, 0, 0,
                      window_, nullptr, instance_, nullptr);
  debug_run_button_ = CreateWindowExW(
      0, L"BUTTON", L"Debug Testbench", WS_CHILD | BS_PUSHBUTTON, 0, 0, 0, 0,
      window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDebugRunId)),
      instance_, nullptr);
  debug_continue_button_ = CreateWindowExW(
      0, L"BUTTON", L"Continue", WS_CHILD | BS_PUSHBUTTON, 0, 0, 0, 0, window_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDebugContinueId)),
      instance_, nullptr);
  debug_step_button_ = CreateWindowExW(
      0, L"BUTTON", L"Step", WS_CHILD | BS_PUSHBUTTON, 0, 0, 0, 0, window_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDebugStepId)), instance_,
      nullptr);
  debug_finish_button_ = CreateWindowExW(
      0, L"BUTTON", L"Finish", WS_CHILD | BS_PUSHBUTTON, 0, 0, 0, 0, window_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDebugFinishId)), instance_,
      nullptr);
  bottom_tabs_ =
      CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0,
                      window_, nullptr, instance_, nullptr);
  problems_ =
      CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                      WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS, 0,
                      0, 0, 0, window_, nullptr, instance_, nullptr);
  runs_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                          WS_CHILD | LVS_REPORT | LVS_SHOWSELALWAYS, 0, 0, 0, 0,
                          window_, nullptr, instance_, nullptr);
  artifacts_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                               WS_CHILD | LVS_REPORT, 0, 0, 0, 0, window_,
                               nullptr, instance_, nullptr);
  tests_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                           WS_CHILD | LVS_REPORT | LVS_SHOWSELALWAYS, 0, 0, 0,
                           0, window_, nullptr, instance_, nullptr);
  output_ = CreateWindowExW(
      WS_EX_CLIENTEDGE, L"EDIT", L"Design++ Workspace Output\r\n",
      WS_CHILD | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY, 0, 0,
      0, 0, window_, nullptr, instance_, nullptr);
  debug_state_label_ =
      CreateWindowExW(0, L"STATIC", L"Debugger: Idle", WS_CHILD, 0, 0, 0, 0,
                      window_, nullptr, instance_, nullptr);
  debug_scope_tree_ = CreateWindowExW(
      WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
      WS_CHILD | TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT, 0, 0, 0, 0,
      window_, nullptr, instance_, nullptr);
  debug_variables_ =
      CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                      WS_CHILD | LVS_REPORT | LVS_SHOWSELALWAYS, 0, 0, 0, 0,
                      window_, nullptr, instance_, nullptr);
  debug_transcript_ = CreateWindowExW(
      WS_EX_CLIENTEDGE, L"EDIT", L"",
      WS_CHILD | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY, 0, 0,
      0, 0, window_, nullptr, instance_, nullptr);
  debug_command_ =
      CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL,
                      0, 0, 0, 0, window_, nullptr, instance_, nullptr);
  debug_send_button_ = CreateWindowExW(
      0, L"BUTTON", L"Send", WS_CHILD | BS_PUSHBUTTON, 0, 0, 0, 0, window_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDebugSendId)), instance_,
      nullptr);
  status_ = CreateWindowExW(0, STATUSCLASSNAMEW, L"Loading project...",
                            WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0,
                            window_, nullptr, instance_, nullptr);
  if (!add_source_button_ || !source_tree_ || !top_module_ || !cpu_budget_ ||
      !include_directories_ || !defines_ || !parameters_ || !constraint_path_ ||
      !active_view_ || !editor_container_ || !save_button_ || !lint_button_ ||
      !cancel_button_ || !simulator_label_ || !simulator_backend_ ||
      !dut_top_label_ || !testbench_top_ || !cocotb_module_ ||
      !cocotb_testcase_ || !waveform_checkbox_ || !waveform_format_ ||
      !simulation_run_button_ || !simulation_cancel_button_ ||
      !open_waveform_button_ || !simulation_status_ || !debug_run_button_ ||
      !debug_continue_button_ || !debug_step_button_ || !debug_finish_button_ ||
      !bottom_tabs_ || !problems_ || !runs_ || !artifacts_ || !tests_ ||
      !output_ || !debug_state_label_ || !debug_scope_tree_ ||
      !debug_variables_ || !debug_transcript_ || !debug_command_ ||
      !debug_send_button_ || !status_) {
    return false;
  }
  TreeView_SetExtendedStyle(source_tree_, TVS_EX_DOUBLEBUFFER,
                            TVS_EX_DOUBLEBUFFER);
  ListView_SetExtendedListViewStyle(problems_,
                                    LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
  const wchar_t* problem_columns[] = {L"Severity", L"Code", L"File", L"Line",
                                      L"Message"};
  const int problem_widths[] = {80, 90, 240, 55, 360};
  for (int index = 0; index < 5; ++index) {
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH;
    column.cx = problem_widths[index];
    column.pszText = const_cast<wchar_t*>(problem_columns[index]);
    ListView_InsertColumn(problems_, index, &column);
  }
  ListView_SetExtendedListViewStyle(runs_,
                                    LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
  const wchar_t* run_columns[] = {L"Started", L"Stage", L"Tool", L"Status"};
  for (int index = 0; index < 4; ++index) {
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH;
    column.cx = index == 0 ? 180 : 120;
    column.pszText = const_cast<wchar_t*>(run_columns[index]);
    ListView_InsertColumn(runs_, index, &column);
  }
  LVCOLUMNW artifact_column{};
  artifact_column.mask = LVCF_TEXT | LVCF_WIDTH;
  artifact_column.cx = 500;
  artifact_column.pszText = const_cast<wchar_t*>(L"Simulation Artifacts");
  ListView_InsertColumn(artifacts_, 0, &artifact_column);
  ListView_SetExtendedListViewStyle(tests_,
                                    LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
  const wchar_t* test_columns[] = {L"Status", L"Suite", L"Test", L"Time",
                                   L"Detail"};
  const int test_widths[] = {80, 150, 220, 90, 500};
  for (int index = 0; index < 5; ++index) {
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH;
    column.cx = test_widths[index];
    column.pszText = const_cast<wchar_t*>(test_columns[index]);
    ListView_InsertColumn(tests_, index, &column);
  }
  ListView_SetExtendedListViewStyle(debug_variables_,
                                    LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
  const wchar_t* debug_columns[] = {L"Name", L"Kind", L"Value"};
  const int debug_widths[] = {190, 90, 180};
  for (int index = 0; index < 3; ++index) {
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH;
    column.cx = debug_widths[index];
    column.pszText = const_cast<wchar_t*>(debug_columns[index]);
    ListView_InsertColumn(debug_variables_, index, &column);
  }
  for (const wchar_t* title :
       {L"Problems", L"Runs", L"Artifacts", L"Tests", L"Output", L"Debug"}) {
    TCITEMW item{};
    item.mask = TCIF_TEXT;
    item.pszText = const_cast<wchar_t*>(title);
    TabCtrl_InsertItem(bottom_tabs_, TabCtrl_GetItemCount(bottom_tabs_), &item);
  }
  SendMessageW(output_, EM_SETLIMITTEXT, kMaximumOutputCharacters, 0);
  SendMessageW(debug_transcript_, EM_SETLIMITTEXT, kMaximumOutputCharacters, 0);
  SendMessageW(include_directories_, EM_SETCUEBANNER, TRUE,
               reinterpret_cast<LPARAM>(L"Include directories (; separated)"));
  SendMessageW(defines_, EM_SETCUEBANNER, TRUE,
               reinterpret_cast<LPARAM>(L"Defines (; separated)"));
  SendMessageW(parameters_, EM_SETCUEBANNER, TRUE,
               reinterpret_cast<LPARAM>(L"Parameters (; separated)"));
  SendMessageW(constraint_path_, EM_SETCUEBANNER, TRUE,
               reinterpret_cast<LPARAM>(L"Constraint path (Library relative)"));
  EnableWindow(save_button_, FALSE);
  EnableWindow(lint_button_, FALSE);
  EnableWindow(cancel_button_, FALSE);
  SendMessageW(waveform_checkbox_, BM_SETCHECK, BST_CHECKED, 0);
  SendMessageW(cocotb_module_, EM_SETCUEBANNER, TRUE,
               reinterpret_cast<LPARAM>(L"cocotb Python module"));
  SendMessageW(cocotb_testcase_, EM_SETCUEBANNER, TRUE,
               reinterpret_cast<LPARAM>(L"Testcase filter (optional)"));
  SendMessageW(simulator_backend_, CB_ADDSTRING, 0,
               reinterpret_cast<LPARAM>(L"Icarus Verilog"));
  SendMessageW(simulator_backend_, CB_ADDSTRING, 0,
               reinterpret_cast<LPARAM>(L"Verilator"));
  SendMessageW(simulator_backend_, CB_ADDSTRING, 0,
               reinterpret_cast<LPARAM>(L"cocotb + Icarus"));
  SendMessageW(simulator_backend_, CB_ADDSTRING, 0,
               reinterpret_cast<LPARAM>(L"cocotb + Verilator"));
  SendMessageW(simulator_backend_, CB_SETCURSEL, 0, 0);
  SendMessageW(waveform_format_, CB_ADDSTRING, 0,
               reinterpret_cast<LPARAM>(L"VCD"));
  SendMessageW(waveform_format_, CB_ADDSTRING, 0,
               reinterpret_cast<LPARAM>(L"FST"));
  SendMessageW(waveform_format_, CB_SETCURSEL, 0, 0);
  EnableWindow(simulation_cancel_button_, FALSE);
  EnableWindow(open_waveform_button_, FALSE);
  EnableWindow(debug_continue_button_, FALSE);
  EnableWindow(debug_step_button_, FALSE);
  EnableWindow(debug_finish_button_, FALSE);
  EnableWindow(debug_send_button_, FALSE);
  if (!editor_host_.Create(
          editor_container_, editor_environment_, NewUuid(),
          [this](application::EditorWebMessage message) {
            HandleEditorMessage(std::move(message));
          },
          [this](core::Status status) {
            SetStatus(Utf8ToWide(status.message));
            SetWindowTextW(
                editor_container_,
                (L"Monaco editor unavailable\r\n" + Utf8ToWide(status.message) +
                 L"\r\nInstall or repair Microsoft Edge WebView2 Runtime.")
                    .c_str());
            AppendOutput(L"[Editor] " + Utf8ToWide(status.message) + L"\r\n");
          })) {
    SetWindowTextW(editor_container_, L"Monaco editor initialization failed");
  }
  ApplyDpi(dpi_);
  ShowBottomPage(0);
  return true;
}

void VerilogWindow::LayoutControls(int width, int height) {
  if (!source_tree_) return;
  SendMessageW(status_, WM_SIZE, 0, 0);
  RECT status_bounds{};
  GetWindowRect(status_, &status_bounds);
  const int status_height = status_bounds.bottom - status_bounds.top;
  const int content_height = std::max(0, height - status_height);
  const int gap = ScaleForDpi(6, dpi_);
  if (left_width_ == 0) left_width_ = ScaleForDpi(300, dpi_);
  if (right_width_ == 0) right_width_ = ScaleForDpi(320, dpi_);
  if (bottom_height_ == 0) {
    bottom_height_ = std::max(ScaleForDpi(220, dpi_), content_height / 3);
  }
  const int minimum_editor_width = ScaleForDpi(240, dpi_);
  left_width_ = std::clamp(
      left_width_, ScaleForDpi(160, dpi_),
      std::max(ScaleForDpi(160, dpi_),
               width - right_width_ - gap * 2 - minimum_editor_width));
  right_width_ = std::clamp(
      right_width_, ScaleForDpi(220, dpi_),
      std::max(ScaleForDpi(220, dpi_),
               width - left_width_ - gap * 2 - minimum_editor_width));
  bottom_height_ =
      std::clamp(bottom_height_, ScaleForDpi(130, dpi_),
                 std::max(ScaleForDpi(130, dpi_),
                          content_height - ScaleForDpi(160, dpi_)));
  const int row_height = ScaleForDpi(25, dpi_);
  const int upper_height = std::max(0, content_height - bottom_height_ - gap);
  MoveWindow(add_source_button_, 0, 0, left_width_, row_height, TRUE);
  MoveWindow(source_tree_, 0, row_height + gap, left_width_,
             std::max(0, upper_height - row_height - gap), TRUE);
  const int editor_left = left_width_ + gap;
  const int settings_left = std::max(editor_left, width - right_width_);
  const int editor_width = std::max(0, settings_left - editor_left - gap);
  MoveWindow(editor_container_, editor_left, 0, editor_width, upper_height,
             TRUE);
  RECT editor_bounds{0, 0, editor_width, upper_height};
  editor_host_.Resize(editor_bounds);
  MoveWindow(active_view_, settings_left, 0, right_width_, row_height, TRUE);
  MoveWindow(top_module_, settings_left, row_height + gap, right_width_,
             ScaleForDpi(200, dpi_), TRUE);
  MoveWindow(cpu_budget_, settings_left, row_height * 2 + gap * 2, right_width_,
             row_height, TRUE);
  MoveWindow(include_directories_, settings_left, row_height * 3 + gap * 3,
             right_width_, row_height, TRUE);
  MoveWindow(defines_, settings_left, row_height * 4 + gap * 4, right_width_,
             row_height, TRUE);
  MoveWindow(parameters_, settings_left, row_height * 5 + gap * 5, right_width_,
             row_height, TRUE);
  MoveWindow(constraint_path_, settings_left, row_height * 6 + gap * 6,
             right_width_, row_height, TRUE);
  const int button_top = row_height * 7 + gap * 7;
  const int compact_button_width = std::max(1, (right_width_ - gap * 2) / 3);
  MoveWindow(save_button_, settings_left, button_top, compact_button_width,
             row_height, TRUE);
  MoveWindow(lint_button_, settings_left + compact_button_width + gap,
             button_top, compact_button_width, row_height, TRUE);
  MoveWindow(cancel_button_, settings_left + (compact_button_width + gap) * 2,
             button_top, compact_button_width, row_height, TRUE);
  MoveWindow(simulator_label_, settings_left, 0, right_width_, row_height,
             TRUE);
  MoveWindow(simulator_backend_, settings_left, row_height + gap, right_width_,
             row_height, TRUE);
  MoveWindow(dut_top_label_, settings_left, row_height * 2 + gap * 2,
             right_width_, row_height, TRUE);
  MoveWindow(testbench_top_, settings_left, row_height * 3 + gap * 3,
             right_width_, ScaleForDpi(200, dpi_), TRUE);
  MoveWindow(cocotb_module_, settings_left, row_height * 4 + gap * 4,
             right_width_, row_height, TRUE);
  MoveWindow(cocotb_testcase_, settings_left, row_height * 5 + gap * 5,
             right_width_, row_height, TRUE);
  const int waveform_width = std::max(1, (right_width_ - gap) * 2 / 3);
  MoveWindow(waveform_checkbox_, settings_left, row_height * 6 + gap * 6,
             waveform_width, row_height, TRUE);
  MoveWindow(waveform_format_, settings_left + waveform_width + gap,
             row_height * 6 + gap * 6,
             std::max(1, right_width_ - waveform_width - gap), row_height,
             TRUE);
  MoveWindow(simulation_run_button_, settings_left, row_height * 7 + gap * 7,
             compact_button_width, row_height, TRUE);
  MoveWindow(simulation_cancel_button_,
             settings_left + compact_button_width + gap,
             row_height * 7 + gap * 7, compact_button_width, row_height, TRUE);
  MoveWindow(open_waveform_button_,
             settings_left + (compact_button_width + gap) * 2,
             row_height * 7 + gap * 7, compact_button_width, row_height, TRUE);
  MoveWindow(debug_run_button_, settings_left, row_height * 8 + gap * 8,
             right_width_, row_height, TRUE);
  const int debug_button_width = std::max(1, (right_width_ - gap * 2) / 3);
  MoveWindow(debug_continue_button_, settings_left, row_height * 9 + gap * 9,
             debug_button_width, row_height, TRUE);
  MoveWindow(debug_step_button_, settings_left + debug_button_width + gap,
             row_height * 9 + gap * 9, debug_button_width, row_height, TRUE);
  MoveWindow(debug_finish_button_,
             settings_left + (debug_button_width + gap) * 2,
             row_height * 9 + gap * 9, debug_button_width, row_height, TRUE);
  MoveWindow(simulation_status_, settings_left, row_height * 10 + gap * 10,
             right_width_, row_height * 2, TRUE);
  const int tab_top = content_height - bottom_height_;
  MoveWindow(bottom_tabs_, 0, tab_top, width, bottom_height_, TRUE);
  RECT page{0, 0, width, bottom_height_};
  TabCtrl_AdjustRect(bottom_tabs_, FALSE, &page);
  const int page_x = page.left;
  const int page_y = tab_top + page.top;
  const int page_width = std::max(0, static_cast<int>(page.right - page.left));
  const int page_height = std::max(0, static_cast<int>(page.bottom - page.top));
  for (HWND child : {problems_, runs_, artifacts_, tests_, output_}) {
    MoveWindow(child, page_x, page_y, page_width, page_height, TRUE);
  }
  const int debug_header = row_height;
  const int debug_command_width =
      std::max(0, page_width - ScaleForDpi(80, dpi_));
  const int debug_body_height = std::max(0, page_height - row_height * 2 - gap);
  const int debug_tree_width = std::max(ScaleForDpi(120, dpi_), page_width / 5);
  const int debug_variable_width =
      std::max(ScaleForDpi(180, dpi_), page_width / 3);
  MoveWindow(debug_state_label_, page_x, page_y, page_width, debug_header,
             TRUE);
  MoveWindow(debug_scope_tree_, page_x, page_y + debug_header, debug_tree_width,
             debug_body_height, TRUE);
  MoveWindow(debug_variables_, page_x + debug_tree_width + gap,
             page_y + debug_header, debug_variable_width, debug_body_height,
             TRUE);
  MoveWindow(debug_transcript_,
             page_x + debug_tree_width + debug_variable_width + gap * 2,
             page_y + debug_header,
             std::max(0, page_width - debug_tree_width - debug_variable_width -
                             gap * 2),
             debug_body_height, TRUE);
  MoveWindow(debug_command_, page_x, page_y + page_height - row_height,
             debug_command_width, row_height, TRUE);
  MoveWindow(debug_send_button_, page_x + debug_command_width + gap,
             page_y + page_height - row_height,
             std::max(0, page_width - debug_command_width - gap), row_height,
             TRUE);
}

VerilogWindow::SplitterDrag VerilogWindow::HitTestSplitter(POINT point) const {
  if (window_ == nullptr || status_ == nullptr) return SplitterDrag::kNone;
  RECT client{};
  GetClientRect(window_, &client);
  RECT status_bounds{};
  GetWindowRect(status_, &status_bounds);
  const int content_height =
      client.bottom - (status_bounds.bottom - status_bounds.top);
  const int gap = ScaleForDpi(6, dpi_);
  if (point.x >= left_width_ && point.x < left_width_ + gap &&
      point.y < content_height - bottom_height_) {
    return SplitterDrag::kLeft;
  }
  const int right_splitter = client.right - right_width_ - gap;
  if (point.x >= right_splitter && point.x < right_splitter + gap &&
      point.y < content_height - bottom_height_) {
    return SplitterDrag::kRight;
  }
  const int bottom_splitter = content_height - bottom_height_ - gap;
  if (point.y >= bottom_splitter && point.y < bottom_splitter + gap) {
    return SplitterDrag::kBottom;
  }
  return SplitterDrag::kNone;
}

void VerilogWindow::UpdateSplitter(POINT point) {
  RECT client{};
  GetClientRect(window_, &client);
  RECT status_bounds{};
  GetWindowRect(status_, &status_bounds);
  const int content_height =
      client.bottom - (status_bounds.bottom - status_bounds.top);
  if (splitter_drag_ == SplitterDrag::kLeft) {
    left_width_ = point.x;
  } else if (splitter_drag_ == SplitterDrag::kRight) {
    right_width_ = client.right - point.x;
  } else if (splitter_drag_ == SplitterDrag::kBottom) {
    bottom_height_ = content_height - point.y;
  }
  LayoutControls(client.right, client.bottom);
}

void VerilogWindow::ApplyDpi(UINT dpi) {
  const UINT previous_dpi = dpi_ == 0 ? kDefaultDpi : dpi_;
  const UINT next_dpi = dpi == 0 ? kDefaultDpi : dpi;
  if (left_width_ != 0) {
    left_width_ = MulDiv(left_width_, next_dpi, previous_dpi);
    right_width_ = MulDiv(right_width_, next_dpi, previous_dpi);
    bottom_height_ = MulDiv(bottom_height_, next_dpi, previous_dpi);
  }
  dpi_ = next_dpi;
  font_ = CreateUiFont(dpi_);
  ApplyFontToWindowTree(window_, font_.Get());
  RECT client{};
  GetClientRect(window_, &client);
  LayoutControls(client.right, client.bottom);
}

void VerilogWindow::BeginLoad() {
  const auto channel = event_channel_;
  const auto library = library_;
  const auto request = request_;
  const std::uint64_t generation = generation_;
  const bool accepted = scheduler_.Submit(
      [channel, library, request, generation](std::stop_token stop_token) {
        if (stop_token.stop_requested()) return;
        WorkspaceEvent event;
        event.kind = WorkspaceEventKind::kLoaded;
        event.generation = generation;
        application::ProjectService service;
        auto opened = service.OpenOrCreate(library, request.cell_id);
        if (!opened.Ok()) {
          event.status = opened.GetStatus();
        } else {
          event.document = std::make_unique<application::ProjectDocument>(
              std::move(opened).Value());
          auto resolved = service.ResolveSources(library, request.cell_id,
                                                 event.document->project);
          if (!resolved.Ok()) {
            event.status = resolved.GetStatus();
          } else {
            event.sources = std::move(resolved).Value();
            std::vector<std::filesystem::path> rtl_paths;
            for (const auto& source : event.sources) {
              if (source.enabled && source.exists &&
                  source.view_kind == core::ViewKind::kVerilog) {
                rtl_paths.push_back(source.windows_path);
              }
            }
            ScanModuleMetadata(rtl_paths, &event);
            ScanTestbenchMetadata(event.sources, &event);
            application::RunStore run_store;
            const std::filesystem::path cell_directory =
                library.directory / L"cells" / Utf8ToWide(request.cell_id);
            static_cast<void>(run_store.RecoverInterrupted(cell_directory));
            auto runs = run_store.List(cell_directory);
            if (runs.Ok()) {
              event.runs = std::move(runs).Value();
              for (const application::RunRecord& run : event.runs) {
                if (run.stage != "Simulation" || run.tool != "cocotb") {
                  continue;
                }
                auto summary = application::LoadCocotbRunSummary(run);
                if (summary.Ok()) {
                  event.run_test_summaries.emplace(run.id,
                                                   std::move(summary).Value());
                }
              }
            }
            event.status = core::Status::Success();
          }
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
  if (!accepted) SetStatus(L"Workspace load queue is full");
}

void VerilogWindow::HandleEvents() {
  std::deque<WorkspaceEvent> events;
  {
    std::scoped_lock lock(event_channel_->mutex);
    events.swap(event_channel_->events);
    event_channel_->external_change_pending = false;
  }
  for (WorkspaceEvent& event : events) {
    if (event.generation != generation_) continue;
    if (event.kind == WorkspaceEventKind::kLoaded) {
      if (!event.status.Ok()) {
        SetStatus(Utf8ToWide(event.status.message));
        AppendOutput(L"[Project error] " + Utf8ToWide(event.status.message) +
                     L"\r\n");
        continue;
      }
      document_ = std::move(event.document);
      sources_ = std::move(event.sources);
      module_candidates_ = std::move(event.module_candidates);
      module_parameter_defaults_ = std::move(event.module_parameter_defaults);
      document_module_candidates_ = std::move(event.testbench_modules);
      run_records_ = std::move(event.runs);
      run_test_summaries_ = std::move(event.run_test_summaries);
      ApplyInferredProjectDefaults();
      PopulateProject();
      PopulateSources();
      PopulateRuns();
      const std::filesystem::path watched_directory =
          library_.directory / L"cells" / Utf8ToWide(request_.cell_id) /
          L"views";
      const std::shared_ptr<EventChannel> channel = event_channel_;
      const std::uint64_t generation = generation_;
      const core::Status watch_status = file_watch_service_.Start(
          watched_directory,
          [channel, generation](std::vector<std::filesystem::path> paths) {
            HWND target = nullptr;
            {
              std::scoped_lock lock(channel->mutex);
              if (channel->window == nullptr ||
                  channel->generation != generation) {
                return;
              }
              if (channel->external_change_pending) {
                const auto pending = std::find_if(
                    channel->events.rbegin(), channel->events.rend(),
                    [](const WorkspaceEvent& event) {
                      return event.kind ==
                             WorkspaceEventKind::kExternalFilesChanged;
                    });
                if (pending != channel->events.rend()) {
                  pending->changed_paths.insert(pending->changed_paths.end(),
                                                paths.begin(), paths.end());
                }
                return;
              }
              WorkspaceEvent event;
              event.kind = WorkspaceEventKind::kExternalFilesChanged;
              event.generation = generation;
              event.changed_paths = std::move(paths);
              channel->events.push_back(std::move(event));
              channel->external_change_pending = true;
              target = channel->window;
            }
            if (!PostMessageW(target, kEventMessage, 0, 0)) {
              std::scoped_lock lock(channel->mutex);
              channel->external_change_pending = false;
            }
          });
      if (!watch_status.Ok()) {
        AppendOutput(L"[Editor] File watching unavailable: " +
                     Utf8ToWide(watch_status.message) + L"\r\n");
      }
      EnableWindow(save_button_, document_ && !document_->read_only);
      EnableWindow(lint_button_, document_ && document_->valid_configuration);
      SetStatus(document_ && document_->read_only ? L"Project opened read-only"
                                                  : L"Project ready");
      if (document_ && !document_->diagnostic.empty()) {
        AppendOutput(Utf8ToWide(document_->diagnostic) + L"\r\n");
      }
      if (editor_host_.Ready()) {
        const auto source = std::find_if(
            sources_.begin(), sources_.end(), [&](const auto& candidate) {
              return candidate.view_id == request_.view_id && candidate.exists;
            });
        if (source != sources_.end()) {
          OpenSource(static_cast<std::size_t>(
              std::distance(sources_.begin(), source)));
        }
      }
      UpdateTitle();
      UpdateInspector();
    } else if (event.kind == WorkspaceEventKind::kSourcesRefreshed) {
      if (event.status.Ok()) {
        sources_ = std::move(event.sources);
        module_candidates_ = std::move(event.module_candidates);
        module_parameter_defaults_ = std::move(event.module_parameter_defaults);
        document_module_candidates_ = std::move(event.testbench_modules);
        ApplyInferredProjectDefaults();
        PopulateProject();
        PopulateSources();
        AppendOutput(L"[Project] Source Set synchronized\r\n");
      } else {
        AppendOutput(L"[Project] Source Set refresh failed: " +
                     Utf8ToWide(event.status.message) + L"\r\n");
      }
    } else if (event.kind == WorkspaceEventKind::kFilesImported) {
      if (!event.status.Ok()) {
        const std::wstring message = Utf8ToWide(event.status.message);
        SetStatus(L"Source file import failed");
        AppendOutput(L"[Source import error] " + message + L"\r\n");
        MessageBoxW(window_, message.c_str(), L"Add Source Files",
                    MB_OK | MB_ICONERROR);
        continue;
      }
      library_ = std::move(event.library);
      sources_ = std::move(event.sources);
      module_candidates_ = std::move(event.module_candidates);
      module_parameter_defaults_ = std::move(event.module_parameter_defaults);
      document_module_candidates_ = std::move(event.testbench_modules);
      ApplyInferredProjectDefaults();
      PopulateProject();
      PopulateSources();
      SetStatus(L"Source files added");
      AppendOutput(
          L"[Source] Managed files added and Source Set synchronized\r\n");
      if (library_changed_) library_changed_();
    } else if (event.kind == WorkspaceEventKind::kOutput) {
      AppendOutput(Utf8ToWide(event.output));
    } else if (event.kind == WorkspaceEventKind::kProbeComplete) {
      process_.reset();
      HandleProbeComplete(event.process_result);
    } else if (event.kind == WorkspaceEventKind::kLintComplete) {
      process_.reset();
      active_operation_ = ActiveOperation::kNone;
      cpu_lease_.reset();
      EnableWindow(lint_button_, TRUE);
      EnableWindow(cancel_button_, FALSE);
      diagnostics_ = std::move(event.diagnostics);
      if (event.run) {
        active_run_ = std::move(event.run);
        run_records_.insert(run_records_.begin(), *active_run_);
      }
      PopulateProblems();
      PopulateRuns();
      TabCtrl_SetCurSel(bottom_tabs_, diagnostics_.empty() ? 1 : 0);
      ShowBottomPage(diagnostics_.empty() ? 1 : 0);
      SetStatus(event.process_result.cancelled        ? L"Lint cancelled"
                : event.process_result.exit_code == 0 ? L"Lint succeeded"
                                                      : L"Lint failed");
      editor_host_.SetDiagnostics(diagnostics_, editor_documents_);
      UpdateInspector();
    } else if (event.kind == WorkspaceEventKind::kSimulationProbeComplete) {
      process_.reset();
      HandleSimulationProbeComplete(event.process_result);
    } else if (event.kind ==
               WorkspaceEventKind::kCocotbMakefilesProbeComplete) {
      process_.reset();
      HandleCocotbMakefilesProbeComplete(event.process_result);
    } else if (event.kind == WorkspaceEventKind::kSimulationPrepared) {
      const bool has_plan = simulation_runner_id_ == "cocotb"
                                ? event.cocotb_plan.has_value()
                                : event.simulation_plan.has_value();
      if (!event.status.Ok() || !has_plan || !event.run) {
        active_operation_ = ActiveOperation::kNone;
        cpu_lease_.reset();
        if (event.run) {
          run_records_.insert(run_records_.begin(), *event.run);
          PopulateRuns();
        }
        SetStatus(Utf8ToWide(event.status.message));
        MessageBoxW(window_, Utf8ToWide(event.status.message).c_str(),
                    L"Icarus Simulation", MB_OK | MB_ICONERROR);
        UpdateInspector();
      } else {
        simulation_plan_ = std::move(event.simulation_plan);
        cocotb_plan_ = std::move(event.cocotb_plan);
        active_run_ = std::move(event.run);
        if (simulation_runner_id_ == "cocotb") {
          StartCocotbExecute();
        } else {
          StartSimulationCompile();
        }
      }
    } else if (event.kind == WorkspaceEventKind::kSimulationCompileComplete) {
      process_.reset();
      simulation_output_ = event.process_result.output;
      StartSimulationExecute();
    } else if (event.kind == WorkspaceEventKind::kSimulationComplete) {
      process_.reset();
      simulation_summary_ = std::move(event.simulation_summary);
      FinishSimulation(event.process_result, std::move(event.status),
                       std::move(event.diagnostics), std::move(event.run));
    } else if (event.kind == WorkspaceEventKind::kDebugProbeComplete) {
      process_.reset();
      HandleDebugProbeComplete(event.process_result);
    } else if (event.kind == WorkspaceEventKind::kDebugPrepared) {
      if (!event.status.Ok() || !event.debug_plan || !event.run) {
        active_operation_ = ActiveOperation::kNone;
        cpu_lease_.reset();
        debug_session_.Complete(false, false);
        if (event.run) {
          run_records_.insert(run_records_.begin(), *event.run);
          PopulateRuns();
        }
        SetStatus(Utf8ToWide(event.status.message));
        UpdateInspector();
        PopulateDebugView();
      } else {
        debug_plan_ = std::move(event.debug_plan);
        active_run_ = std::move(event.run);
        StartDebugCompile();
      }
    } else if (event.kind == WorkspaceEventKind::kDebugCompileComplete) {
      process_.reset();
      simulation_output_ = event.process_result.output;
      StartDebugExecute();
    } else if (event.kind == WorkspaceEventKind::kDebugOutput) {
      AppendOutput(Utf8ToWide(event.output));
      HandleDebugOutput(event.output);
    } else if (event.kind == WorkspaceEventKind::kDebugComplete) {
      process_.reset();
      diagnostics_ = std::move(event.diagnostics);
      FinishDebug(event.process_result, std::move(event.run));
    } else if (event.kind == WorkspaceEventKind::kViewerComplete) {
      viewer_cpu_lease_.reset();
      if (!event.status.Ok() && !event.status.message.empty()) {
        AppendOutput(L"[WSLg] " + Utf8ToWide(event.status.message) + L"\r\n");
      }
      if (!event.process_result.error_message.empty()) {
        AppendOutput(L"[GTKWave] Process initialization failed: " +
                     event.process_result.error_message + L"\r\n");
      }
      if (!event.process_result.cancelled &&
          (!event.process_result.started ||
           !event.process_result.error_message.empty() ||
           event.process_result.exit_code != 0)) {
        SetStatus(L"GTKWave exited with an error");
      }
    } else if (event.kind == WorkspaceEventKind::kDocumentLoaded) {
      if (!event.status.Ok()) {
        const auto existing =
            std::find_if(editor_documents_.begin(), editor_documents_.end(),
                         [&](const auto& document) {
                           return document.relative_path == event.document_id;
                         });
        if (existing != editor_documents_.end() &&
            event.status.code == core::ErrorCode::kNotFound) {
          existing->missing = true;
          existing->read_only = true;
          editor_host_.SetReadOnly(existing->id, true);
        }
        SetStatus(Utf8ToWide(event.status.message));
        AppendOutput(L"[Editor] " + Utf8ToWide(event.status.message) + L"\r\n");
        continue;
      }
      const auto existing =
          std::find_if(editor_documents_.begin(), editor_documents_.end(),
                       [&](const auto& document) {
                         return document.relative_path ==
                                event.editor_document.relative_path;
                       });
      application::EditorDocumentSnapshot* opened = nullptr;
      if (existing == editor_documents_.end()) {
        editor_documents_.push_back(std::move(event.editor_document));
        opened = &editor_documents_.back();
      } else {
        dirty_document_ids_.erase(existing->id);
        editor_host_.CloseDocument(existing->id);
        *existing = std::move(event.editor_document);
        opened = &*existing;
      }
      document_module_candidates_[opened->relative_path] =
          std::move(event.document_module_candidates);
      const std::filesystem::path path = Utf8ToWide(opened->relative_path);
      const std::string extension = WideToUtf8(path.extension().wstring());
      const std::string language = extension == ".sv" || extension == ".svh"
                                       ? "systemverilog"
                                       : "verilog";
      editor_host_.OpenDocument(*opened, WideToUtf8(path.filename().wstring()),
                                language);
      if (!opened->diagnostic.empty()) {
        AppendOutput(L"[Editor] " + Utf8ToWide(opened->diagnostic) + L"\r\n");
      }
      if (pending_reveal_ &&
          (pending_reveal_->file == opened->relative_path ||
           pending_reveal_->file == WideToUtf8(path.filename().wstring()))) {
        editor_host_.RevealLocation(opened->id, pending_reveal_->line,
                                    pending_reveal_->column);
        pending_reveal_.reset();
      }
      SetStatus(L"Managed source opened");
    } else if (event.kind == WorkspaceEventKind::kDocumentSaved) {
      saving_document_ids_.erase(event.document_id);
      application::EditorDocumentSnapshot* document =
          FindEditorDocument(event.document_id);
      if (!event.status.Ok()) {
        if (event.status.code == core::ErrorCode::kExternalModification &&
            document != nullptr) {
          const int choice = MessageBoxW(
              window_,
              L"파일이 외부에서 변경되었습니다.\n예: 편집본으로 덮어쓰기\n"
              L"아니요: 디스크 내용 다시 읽기\n취소: 현재 편집 유지",
              L"Design++ Editor conflict", MB_YESNOCANCEL | MB_ICONWARNING);
          if (choice == IDYES) {
            BeginSaveDocument(event.document_id, event.save_text, true);
          } else if (choice == IDNO) {
            const auto source = std::find_if(
                sources_.begin(), sources_.end(), [&](const auto& candidate) {
                  return candidate.relative_path == document->relative_path;
                });
            if (source != sources_.end()) {
              OpenSource(static_cast<std::size_t>(
                  std::distance(sources_.begin(), source)));
            }
          }
        } else {
          editor_host_.SaveResult(event.document_id, false,
                                  event.status.message);
          SetStatus(Utf8ToWide(event.status.message));
        }
        document_save_queue_.clear();
        save_project_after_documents_ = false;
        close_after_save_ = false;
        pending_run_ = PendingRun::kNone;
        continue;
      }
      const std::string previous_path =
          document == nullptr ? std::string() : document->relative_path;
      library_ = std::move(event.library);
      if (document != nullptr) *document = std::move(event.editor_document);
      const bool renamed = document != nullptr && !previous_path.empty() &&
                           previous_path != document->relative_path;
      if (document != nullptr && !renamed) {
        document_module_candidates_[document->relative_path] =
            event.document_module_candidates;
        if (event.document_module_candidates.size() == 1) {
          for (core::TestbenchConfiguration& configuration :
               document_->project.testbench_configurations) {
            if (configuration.view_id == document->view_id &&
                configuration.relative_path == document->relative_path &&
                configuration.top_module !=
                    event.document_module_candidates.front()) {
              configuration.top_module =
                  event.document_module_candidates.front();
              dirty_ = true;
            }
          }
        }
      }
      if (event.source_snapshot_available) {
        sources_ = std::move(event.sources);
        module_candidates_ = std::move(event.module_candidates);
        module_parameter_defaults_ = std::move(event.module_parameter_defaults);
        document_module_candidates_ = std::move(event.testbench_modules);
      }
      if (renamed) {
        document_module_candidates_.erase(previous_path);
        document_module_candidates_[document->relative_path] =
            event.document_module_candidates;
        const auto source = std::find_if(
            sources_.begin(), sources_.end(), [&](const auto& candidate) {
              return candidate.relative_path == previous_path;
            });
        if (source != sources_.end()) {
          source->relative_path = document->relative_path;
          source->windows_path =
              library_.directory / Utf8ToWide(document->relative_path);
          source->exists = true;
        }
        bool override_updated = false;
        for (core::SourceOverride& override_value :
             document_->project.source_overrides) {
          if (override_value.view_id == document->view_id &&
              override_value.relative_path == previous_path) {
            override_value.relative_path = document->relative_path;
            const auto renamed_source = std::find_if(
                sources_.begin(), sources_.end(), [&](const auto& source) {
                  return source.view_id == override_value.view_id &&
                         source.relative_path == override_value.relative_path;
                });
            if (renamed_source != sources_.end()) {
              renamed_source->enabled = override_value.enabled;
              if (!override_value.role.empty()) {
                renamed_source->role = override_value.role;
              }
            }
            override_updated = true;
          }
        }
        for (core::TestbenchConfiguration& configuration :
             document_->project.testbench_configurations) {
          if (configuration.view_id == document->view_id &&
              configuration.relative_path == previous_path) {
            configuration.relative_path = document->relative_path;
            if (event.document_module_candidates.size() == 1 &&
                configuration.top_module !=
                    event.document_module_candidates.front()) {
              configuration.top_module =
                  event.document_module_candidates.front();
            }
            override_updated = true;
          }
        }
        if (override_updated) {
          dirty_ = true;
          UpdateTitle();
        }
        if (event.source_snapshot_available) {
          ApplyInferredProjectDefaults();
          PopulateProject();
          PopulateSources();
        }
      }
      dirty_document_ids_.erase(event.document_id);
      pending_save_text_.erase(event.document_id);
      editor_host_.SaveResult(event.document_id, true, "Saved");
      const bool close_requested =
          pending_close_document_ids_.erase(event.document_id) > 0;
      if (close_requested) {
        editor_host_.CloseDocument(event.document_id);
        std::erase_if(editor_documents_, [&](const auto& candidate) {
          return candidate.id == event.document_id;
        });
      } else if (renamed) {
        editor_host_.CloseDocument(event.document_id);
        const std::filesystem::path path = Utf8ToWide(document->relative_path);
        const std::string extension = WideToUtf8(path.extension().wstring());
        const std::string language = extension == ".sv" || extension == ".svh"
                                         ? "systemverilog"
                                         : "verilog";
        editor_host_.OpenDocument(
            *document, WideToUtf8(path.filename().wstring()), language);
        last_document_by_view_[document->view_id] = document->relative_path;
        AppendOutput(L"[Editor] Top module에 맞춰 파일명을 " +
                     path.filename().wstring() + L"(으)로 변경했습니다.\r\n");
      }
      DispatchNextDocumentSave();
      if (dirty_document_ids_.empty() && saving_document_ids_.empty() &&
          document_save_queue_.empty()) {
        if (save_project_after_documents_) {
          save_project_after_documents_ = false;
          if (dirty_ && !SaveProject()) {
            close_after_save_ = false;
            continue;
          }
        }
        if (close_after_save_) {
          close_after_save_ = false;
          close_prepared_ = true;
          PostMessageW(window_, WM_CLOSE, 0, 0);
        } else if (pending_run_ != PendingRun::kNone) {
          const PendingRun pending = pending_run_;
          pending_run_ = PendingRun::kNone;
          if (pending == PendingRun::kLint) {
            StartLint();
          } else if (pending == PendingRun::kSimulation) {
            StartSimulation();
          } else {
            StartDebug();
          }
        }
      }
      SetStatus(L"Managed source saved");
      UpdateInspector();
    } else if (event.kind == WorkspaceEventKind::kExternalFilesChanged) {
      for (application::EditorDocumentSnapshot& editor_document :
           editor_documents_) {
        const bool affected = std::any_of(
            event.changed_paths.begin(), event.changed_paths.end(),
            [&](const std::filesystem::path& changed_path) {
              return PathsEqual(changed_path,
                                library_.directory /
                                    Utf8ToWide(editor_document.relative_path));
            });
        if (!affected || saving_document_ids_.contains(editor_document.id)) {
          continue;
        }
        if (dirty_document_ids_.contains(editor_document.id)) {
          editor_document.externally_modified = true;
          SetStatus(L"A dirty source was modified outside Design++");
          continue;
        }
        const auto source = std::find_if(
            sources_.begin(), sources_.end(), [&](const auto& candidate) {
              return candidate.relative_path == editor_document.relative_path;
            });
        if (source != sources_.end()) {
          OpenSource(static_cast<std::size_t>(
              std::distance(sources_.begin(), source)));
        } else {
          editor_document.missing = true;
          editor_document.read_only = true;
          editor_host_.SetReadOnly(editor_document.id, true);
          SetStatus(L"An open source file was removed externally");
        }
      }
    }
  }
}

void VerilogWindow::ApplyInferredProjectDefaults() {
  if (!document_ || document_->read_only) return;
  core::Project& project = document_->project;
  bool changed = false;
  if (project.top_module.empty()) {
    if (module_candidates_.size() == 1) {
      project.top_module = module_candidates_.front();
    } else {
      const core::Cell* cell = FindCell(library_, request_.cell_id);
      if (cell != nullptr) {
        const auto candidate = std::find_if(
            module_candidates_.begin(), module_candidates_.end(),
            [&cell](std::string_view name) {
              if (name.size() != cell->name.size()) return false;
              return std::equal(
                  name.begin(), name.end(), cell->name.begin(),
                  [](char left, char right) {
                    return std::tolower(static_cast<unsigned char>(left)) ==
                           std::tolower(static_cast<unsigned char>(right));
                  });
            });
        if (candidate != module_candidates_.end()) {
          project.top_module = *candidate;
        } else if (module_candidates_.empty() &&
                   IsSimpleHdlIdentifier(cell->name)) {
          project.top_module = cell->name;
        }
      }
    }
    changed = !project.top_module.empty();
  }

  if (project.parameters.empty() && !project.top_module.empty()) {
    const auto defaults = module_parameter_defaults_.find(project.top_module);
    if (defaults != module_parameter_defaults_.end() &&
        !defaults->second.empty()) {
      project.parameters = defaults->second;
      changed = true;
    }
  }

  if (project.include_directories.empty()) {
    std::set<std::string> directories;
    for (const application::ResolvedSource& source : sources_) {
      const std::size_t dot = source.relative_path.find_last_of('.');
      std::string extension = dot == std::string::npos
                                  ? std::string()
                                  : source.relative_path.substr(dot);
      std::transform(extension.begin(), extension.end(), extension.begin(),
                     [](char character) {
                       return static_cast<char>(
                           std::tolower(static_cast<unsigned char>(character)));
                     });
      if (source.enabled && source.exists &&
          (extension == ".vh" || extension == ".svh")) {
        const std::string directory = ManagedParentPath(source.relative_path);
        if (!directory.empty()) directories.insert(directory);
      }
    }
    if (!directories.empty()) {
      project.include_directories.assign(directories.begin(),
                                         directories.end());
      changed = true;
    }
  }

  if (changed) {
    dirty_ = true;
    AppendOutput(
        L"[Project] 기본 설정을 Source Set에서 자동 추론했습니다.\r\n");
  }
}

void VerilogWindow::PopulateParameterDefaultsForSelection() {
  if (!document_ || document_->read_only || !WindowText(parameters_).empty()) {
    return;
  }
  const LRESULT selection = SendMessageW(top_module_, CB_GETCURSEL, 0, 0);
  if (selection == CB_ERR) return;
  const LRESULT length =
      SendMessageW(top_module_, CB_GETLBTEXTLEN, selection, 0);
  if (length == CB_ERR) return;
  std::wstring value(static_cast<std::size_t>(length) + 1, L'\0');
  SendMessageW(top_module_, CB_GETLBTEXT, selection,
               reinterpret_cast<LPARAM>(value.data()));
  value.resize(static_cast<std::size_t>(length));
  const auto defaults = module_parameter_defaults_.find(WideToUtf8(value));
  if (defaults == module_parameter_defaults_.end() ||
      defaults->second.empty()) {
    return;
  }
  applying_controls_ = true;
  SetWindowTextW(parameters_, JoinValues(defaults->second).c_str());
  applying_controls_ = false;
}

void VerilogWindow::PopulateProject() {
  if (!document_) return;
  applying_controls_ = true;
  SendMessageW(top_module_, CB_RESETCONTENT, 0, 0);
  for (const std::string& candidate : module_candidates_) {
    const std::wstring value = Utf8ToWide(candidate);
    SendMessageW(top_module_, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(value.c_str()));
  }
  SetWindowTextW(top_module_,
                 Utf8ToWide(document_->project.top_module).c_str());
  SetWindowTextW(cpu_budget_,
                 std::to_wstring(document_->project.cpu_budget).c_str());
  SetWindowTextW(include_directories_,
                 JoinValues(document_->project.include_directories).c_str());
  SetWindowTextW(defines_, JoinValues(document_->project.defines).c_str());
  SetWindowTextW(parameters_,
                 JoinValues(document_->project.parameters).c_str());
  SetWindowTextW(
      constraint_path_,
      Utf8ToWide(document_->project.constraint_path.value_or("")).c_str());
  const core::Cell* cell = FindCell(library_, request_.cell_id);
  const core::View* view = FindView(cell, request_.view_id);
  const std::wstring view_text = view ? L"Active View: " +
                                            Utf8ToWide(view->name) + L" (" +
                                            SourceKindName(view->kind) + L")"
                                      : L"Active View: unavailable";
  SetWindowTextW(active_view_, view_text.c_str());
  applying_controls_ = false;
}

void VerilogWindow::UpdateInspector() {
  const core::View* view =
      FindView(FindCell(library_, request_.cell_id), request_.view_id);
  const bool testbench =
      view != nullptr && view->kind == core::ViewKind::kTestbench;
  ShowWindow(add_source_button_, SW_SHOW);
  ShowWindow(source_tree_, SW_SHOW);
  for (HWND control :
       {active_view_, top_module_, cpu_budget_, include_directories_, defines_,
        parameters_, constraint_path_, save_button_, lint_button_,
        cancel_button_}) {
    ShowWindow(control, testbench ? SW_HIDE : SW_SHOW);
  }
  for (HWND control :
       {simulator_label_, simulator_backend_, dut_top_label_, testbench_top_,
        cocotb_module_, cocotb_testcase_, waveform_checkbox_, waveform_format_,
        simulation_run_button_, simulation_cancel_button_,
        open_waveform_button_, simulation_status_, debug_run_button_,
        debug_continue_button_, debug_step_button_, debug_finish_button_}) {
    ShowWindow(control, testbench ? SW_SHOW : SW_HIDE);
  }
  ShowWindow(editor_container_, SW_SHOW);
  if (testbench) PopulateTestbenchInspector();
}

void VerilogWindow::PopulateTestbenchInspector() {
  if (!document_) return;
  application::EditorDocumentSnapshot* active =
      FindEditorDocument(active_document_id_);
  if (active == nullptr) return;
  const auto saved =
      std::find_if(document_->project.testbench_configurations.begin(),
                   document_->project.testbench_configurations.end(),
                   [&](const auto& value) {
                     return value.view_id == active->view_id &&
                            value.relative_path == active->relative_path;
                   });
  std::vector<std::string> candidates =
      document_module_candidates_[active->relative_path];
  std::string selected =
      saved == document_->project.testbench_configurations.end()
          ? std::string()
          : saved->top_module;
  if (selected.empty() && candidates.size() == 1) {
    selected = candidates.front();
  }
  if (selected.empty()) {
    const std::string stem =
        WideToUtf8(std::filesystem::path(Utf8ToWide(active->relative_path))
                       .stem()
                       .wstring());
    const auto match = std::find(candidates.begin(), candidates.end(), stem);
    if (match != candidates.end()) selected = *match;
  }
  if (selected.empty()) {
    std::vector<std::string> view_candidates;
    for (const auto& [path, values] : document_module_candidates_) {
      const auto source = std::find_if(
          sources_.begin(), sources_.end(), [&](const auto& candidate) {
            return candidate.relative_path == path &&
                   candidate.view_id == active->view_id;
          });
      if (source != sources_.end()) {
        view_candidates.insert(view_candidates.end(), values.begin(),
                               values.end());
      }
    }
    std::sort(view_candidates.begin(), view_candidates.end());
    view_candidates.erase(
        std::unique(view_candidates.begin(), view_candidates.end()),
        view_candidates.end());
    if (view_candidates.size() == 1) selected = view_candidates.front();
    candidates.insert(candidates.end(), view_candidates.begin(),
                      view_candidates.end());
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()),
                     candidates.end());
  }
  applying_controls_ = true;
  SendMessageW(testbench_top_, CB_RESETCONTENT, 0, 0);
  for (const std::string& candidate : candidates) {
    const std::wstring value = Utf8ToWide(candidate);
    SendMessageW(testbench_top_, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(value.c_str()));
  }
  SetWindowTextW(testbench_top_, Utf8ToWide(selected).c_str());
  core::TestbenchConfiguration* resolved_configuration =
      saved == document_->project.testbench_configurations.end() ? nullptr
                                                                 : &*saved;
  if (resolved_configuration == nullptr && !selected.empty()) {
    core::TestbenchConfiguration* stale_configuration = nullptr;
    for (core::TestbenchConfiguration& configuration :
         document_->project.testbench_configurations) {
      const bool path_exists = std::any_of(
          sources_.begin(), sources_.end(), [&](const auto& source) {
            return source.view_id == configuration.view_id &&
                   source.relative_path == configuration.relative_path;
          });
      if (configuration.view_id == active->view_id &&
          configuration.top_module == selected && !path_exists) {
        if (stale_configuration != nullptr) {
          stale_configuration = nullptr;
          break;
        }
        stale_configuration = &configuration;
      }
    }
    if (stale_configuration != nullptr) {
      stale_configuration->relative_path = active->relative_path;
      resolved_configuration = stale_configuration;
      if (!document_->read_only) MarkDirty();
      AppendOutput(
          L"[Project] 이전 Testbench 파일 경로를 현재 관리 파일로 "
          L"복구했습니다.\r\n");
    }
  }
  const bool waveform = resolved_configuration == nullptr
                            ? true
                            : resolved_configuration->waveform_enabled;
  const std::string backend = resolved_configuration == nullptr
                                  ? "icarus"
                                  : resolved_configuration->backend;
  const bool cocotb = resolved_configuration != nullptr &&
                      resolved_configuration->runner == "cocotb";
  const int simulator_selection =
      (cocotb ? 2 : 0) + (backend == "verilator" ? 1 : 0);
  SendMessageW(simulator_backend_, CB_SETCURSEL, simulator_selection, 0);
  SetWindowTextW(
      cocotb_module_,
      resolved_configuration == nullptr
          ? L""
          : Utf8ToWide(resolved_configuration->cocotb_module).c_str());
  SetWindowTextW(
      cocotb_testcase_,
      resolved_configuration == nullptr
          ? L""
          : Utf8ToWide(resolved_configuration->cocotb_testcase).c_str());
  SendMessageW(waveform_checkbox_, BM_SETCHECK,
               waveform ? BST_CHECKED : BST_UNCHECKED, 0);
  SetWindowTextW(waveform_checkbox_, L"Generate waveform");
  SendMessageW(waveform_format_, CB_SETCURSEL,
               resolved_configuration != nullptr &&
                       resolved_configuration->waveform_format == "fst"
                   ? 1
                   : 0,
               0);
  EnableWindow(cocotb_module_, cocotb);
  EnableWindow(cocotb_testcase_, cocotb);
  SetWindowTextW(
      dut_top_label_,
      (L"DUT Top: " + Utf8ToWide(document_->project.top_module)).c_str());
  applying_controls_ = false;
  EnableWindow(simulation_run_button_,
               !selected.empty() && !active->missing &&
                   active_operation_ == ActiveOperation::kNone);
  EnableWindow(simulation_cancel_button_,
               active_operation_ == ActiveOperation::kSimulation ||
                   active_operation_ == ActiveOperation::kDebug);
  SetWindowTextW(
      simulation_cancel_button_,
      active_operation_ == ActiveOperation::kDebug ? L"Stop" : L"Cancel");
  EnableWindow(open_waveform_button_, !last_waveform_.empty());
  const application::DebugSnapshot& debug = debug_session_.Snapshot();
  const bool paused = active_operation_ == ActiveOperation::kDebug &&
                      debug.state == application::DebugState::kPaused &&
                      debug.prompt_ready && !debug_command_pending_ &&
                      debug_command_queue_.empty();
  EnableWindow(debug_run_button_,
               !selected.empty() && !active->missing &&
                   active_operation_ == ActiveOperation::kNone &&
                   backend == "icarus" && !cocotb);
  EnableWindow(debug_continue_button_,
               paused && backend == "icarus" && !cocotb);
  EnableWindow(debug_step_button_, paused && backend == "icarus" && !cocotb);
  EnableWindow(debug_finish_button_, paused && backend == "icarus" && !cocotb);
}

void VerilogWindow::ReadTestbenchControls() {
  if (!document_) return;
  application::EditorDocumentSnapshot* active =
      FindEditorDocument(active_document_id_);
  if (active == nullptr) return;
  const int simulator_selection =
      static_cast<int>(SendMessageW(simulator_backend_, CB_GETCURSEL, 0, 0));
  simulation_runner_id_ = simulator_selection >= 2 ? "cocotb" : "hdl";
  simulation_backend_id_ =
      simulator_selection % 2 == 1 ? "verilator" : "icarus";
  const std::string top = simulation_runner_id_ == "cocotb"
                              ? document_->project.top_module
                              : WideToUtf8(WindowText(testbench_top_));
  if (top.empty()) return;
  const bool waveform =
      SendMessageW(waveform_checkbox_, BM_GETCHECK, 0, 0) == BST_CHECKED;
  const std::string backend = simulation_backend_id_;
  const std::string runner = simulation_runner_id_;
  const std::string cocotb_module = WideToUtf8(WindowText(cocotb_module_));
  const std::string cocotb_testcase = WideToUtf8(WindowText(cocotb_testcase_));
  const std::string waveform_format =
      waveform
          ? (SendMessageW(waveform_format_, CB_GETCURSEL, 0, 0) == 1 ? "fst"
                                                                     : "vcd")
          : "none";
  auto configuration =
      std::find_if(document_->project.testbench_configurations.begin(),
                   document_->project.testbench_configurations.end(),
                   [&](const auto& value) {
                     return value.view_id == active->view_id &&
                            value.relative_path == active->relative_path;
                   });
  if (configuration == document_->project.testbench_configurations.end()) {
    if (!document_->read_only) {
      document_->project.testbench_configurations.push_back(
          {active->view_id, active->relative_path, top, backend, waveform,
           runner, cocotb_module, cocotb_testcase, waveform_format});
      MarkDirty();
    }
  } else if (configuration->top_module != top ||
             configuration->backend != backend ||
             configuration->waveform_enabled != waveform ||
             configuration->runner != runner ||
             configuration->cocotb_module != cocotb_module ||
             configuration->cocotb_testcase != cocotb_testcase ||
             configuration->waveform_format != waveform_format) {
    if (!document_->read_only) {
      configuration->top_module = top;
      configuration->backend = backend;
      configuration->waveform_enabled = waveform;
      configuration->runner = runner;
      configuration->cocotb_module = cocotb_module;
      configuration->cocotb_testcase = cocotb_testcase;
      configuration->waveform_format = waveform_format;
      MarkDirty();
    }
  }
}

void VerilogWindow::PopulateSources() {
  if (source_tree_ == nullptr) return;
  std::string selected_key;
  const HTREEITEM previous_selection = TreeView_GetSelection(source_tree_);
  if (previous_selection != nullptr) {
    TVITEMW selected{};
    selected.mask = TVIF_PARAM;
    selected.hItem = previous_selection;
    if (TreeView_GetItem(source_tree_, &selected)) {
      const auto* node =
          reinterpret_cast<const SourceTreeNode*>(selected.lParam);
      if (node != nullptr) selected_key = node->stable_key;
    }
  }
  std::unordered_set<std::string> expanded_keys;
  for (const auto& node : source_tree_nodes_) {
    if (node->item != nullptr &&
        (TreeView_GetItemState(source_tree_, node->item, TVIS_EXPANDED) &
         TVIS_EXPANDED) != 0) {
      expanded_keys.insert(node->stable_key);
    }
  }
  const bool first_population = source_tree_nodes_.empty();
  applying_controls_ = true;
  TreeView_DeleteAllItems(source_tree_);
  source_tree_nodes_.clear();

  auto insert_node = [&](SourceTreeNode::Kind kind, std::string stable_key,
                         std::string view_id, std::size_t source_index,
                         std::wstring text, HTREEITEM parent) {
    auto node = std::make_unique<SourceTreeNode>();
    node->kind = kind;
    node->stable_key = std::move(stable_key);
    node->view_id = std::move(view_id);
    node->source_index = source_index;
    TVINSERTSTRUCTW insertion{};
    insertion.hParent = parent;
    insertion.hInsertAfter = TVI_LAST;
    insertion.item.mask = TVIF_TEXT | TVIF_PARAM;
    insertion.item.pszText = text.data();
    insertion.item.lParam = reinterpret_cast<LPARAM>(node.get());
    node->item = TreeView_InsertItem(source_tree_, &insertion);
    SourceTreeNode* result = node.get();
    source_tree_nodes_.push_back(std::move(node));
    return result;
  };

  const core::Cell* cell = FindCell(library_, request_.cell_id);
  std::unordered_map<int, HTREEITEM> categories;
  const auto category_name = [](core::ViewKind kind) -> std::wstring {
    switch (kind) {
      case core::ViewKind::kVerilog:
        return L"RTL Sources";
      case core::ViewKind::kTestbench:
        return L"Testbench";
      default:
        return L"Other Sources";
    }
  };
  if (cell != nullptr) {
    for (const core::View& view : cell->views) {
      if (view.kind != core::ViewKind::kVerilog &&
          view.kind != core::ViewKind::kTestbench) {
        continue;
      }
      const int category_id = static_cast<int>(view.kind);
      HTREEITEM category = nullptr;
      const auto category_iterator = categories.find(category_id);
      if (category_iterator == categories.end()) {
        SourceTreeNode* category_node =
            insert_node(SourceTreeNode::Kind::kCategory,
                        "category:" + std::to_string(category_id), "", 0,
                        category_name(view.kind), TVI_ROOT);
        category = category_node->item;
        categories.emplace(category_id, category);
      } else {
        category = category_iterator->second;
      }
      SourceTreeNode* view_node =
          insert_node(SourceTreeNode::Kind::kView, "view:" + view.id, view.id,
                      0, Utf8ToWide(view.name), category);
      std::unordered_map<std::string, HTREEITEM> folders;
      for (std::size_t index = 0; index < sources_.size(); ++index) {
        const application::ResolvedSource& source = sources_[index];
        if (source.view_id != view.id) continue;
        std::string displayed_path = source.relative_path;
        const std::size_t files = displayed_path.find("/files/");
        if (files != std::string::npos) displayed_path.erase(0, files + 7);
        std::filesystem::path relative(Utf8ToWide(displayed_path));
        HTREEITEM parent = view_node->item;
        std::string folder_key;
        for (const auto& part : relative.parent_path()) {
          if (!folder_key.empty()) folder_key += '/';
          folder_key += WideToUtf8(part.wstring());
          auto folder = folders.find(folder_key);
          if (folder == folders.end()) {
            SourceTreeNode* folder_node =
                insert_node(SourceTreeNode::Kind::kFolder,
                            "folder:" + view.id + ":" + folder_key, view.id, 0,
                            part.wstring(), parent);
            parent = folder_node->item;
            folders.emplace(folder_key, parent);
          } else {
            parent = folder->second;
          }
        }
        std::wstring label = relative.filename().wstring();
        if (!source.exists) label += L" (Missing)";
        SourceTreeNode* file_node =
            insert_node(SourceTreeNode::Kind::kFile,
                        "file:" + source.view_id + ":" + source.relative_path,
                        source.view_id, index, std::move(label), parent);
        TreeView_SetCheckState(source_tree_, file_node->item,
                               source.enabled ? TRUE : FALSE);
      }
    }
  }

  std::function<bool(HTREEITEM)> update_parent_check = [&](HTREEITEM item) {
    TVITEMW tree_item{};
    tree_item.mask = TVIF_PARAM;
    tree_item.hItem = item;
    if (!TreeView_GetItem(source_tree_, &tree_item)) return false;
    const auto* node =
        reinterpret_cast<const SourceTreeNode*>(tree_item.lParam);
    if (node != nullptr && node->kind == SourceTreeNode::Kind::kFile) {
      return node->source_index < sources_.size() &&
             sources_[node->source_index].enabled;
    }
    bool has_child = false;
    bool all_checked = true;
    for (HTREEITEM child = TreeView_GetChild(source_tree_, item);
         child != nullptr;
         child = TreeView_GetNextSibling(source_tree_, child)) {
      has_child = true;
      all_checked = update_parent_check(child) && all_checked;
    }
    TreeView_SetCheckState(source_tree_, item,
                           has_child && all_checked ? TRUE : FALSE);
    return has_child && all_checked;
  };
  for (HTREEITEM root = TreeView_GetRoot(source_tree_); root != nullptr;
       root = TreeView_GetNextSibling(source_tree_, root)) {
    static_cast<void>(update_parent_check(root));
  }
  for (const auto& node : source_tree_nodes_) {
    if (first_population || expanded_keys.contains(node->stable_key)) {
      if (node->kind != SourceTreeNode::Kind::kFile) {
        TreeView_Expand(source_tree_, node->item, TVE_EXPAND);
      }
    }
    if (!selected_key.empty() && node->stable_key == selected_key) {
      TreeView_SelectItem(source_tree_, node->item);
    }
  }
  EnableWindow(add_source_button_, document_ != nullptr &&
                                       !document_->read_only &&
                                       cell != nullptr);
  applying_controls_ = false;
}

void VerilogWindow::PopulateRuns() {
  ListView_DeleteAllItems(runs_);
  for (std::size_t index = 0; index < run_records_.size(); ++index) {
    const auto& run = run_records_[index];
    std::wstring started = Utf8ToWide(run.started_utc);
    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = static_cast<int>(index);
    item.pszText = started.data();
    ListView_InsertItem(runs_, &item);
    std::wstring stage = Utf8ToWide(run.stage);
    std::wstring tool = Utf8ToWide(run.tool);
    std::wstring status = Utf8ToWide(application::RunStatusName(run.status));
    ListView_SetItemText(runs_, static_cast<int>(index), 1, stage.data());
    ListView_SetItemText(runs_, static_cast<int>(index), 2, tool.data());
    ListView_SetItemText(runs_, static_cast<int>(index), 3, status.data());
  }
}

void VerilogWindow::PopulateArtifacts(const application::RunRecord* run) {
  ListView_DeleteAllItems(artifacts_);
  last_waveform_.clear();
  if (run == nullptr) {
    EnableWindow(open_waveform_button_, FALSE);
    return;
  }
  for (std::size_t index = 0; index < run->artifacts.size(); ++index) {
    const application::RunArtifact& artifact = run->artifacts[index];
    std::wstring label = Utf8ToWide(artifact.kind + " (" + artifact.format +
                                    ") — " + artifact.relative_path);
    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = static_cast<int>(index);
    item.pszText = label.data();
    ListView_InsertItem(artifacts_, &item);
    if (last_waveform_.empty() && artifact.kind == "waveform" &&
        (artifact.format == "vcd" || artifact.format == "fst") &&
        core::IsSafeRelativePath(artifact.relative_path)) {
      last_waveform_ = run->directory / Utf8ToWide(artifact.relative_path);
    }
  }
  EnableWindow(open_waveform_button_, !last_waveform_.empty());
}

void VerilogWindow::PopulateTestSummary() {
  ListView_DeleteAllItems(tests_);
  if (!simulation_summary_) return;
  const auto status_name = [](adapters::SimulationTestStatus status) {
    switch (status) {
      case adapters::SimulationTestStatus::kPassed:
        return L"Passed";
      case adapters::SimulationTestStatus::kFailed:
        return L"Failed";
      case adapters::SimulationTestStatus::kError:
        return L"Error";
      case adapters::SimulationTestStatus::kSkipped:
        return L"Skipped";
    }
    return L"Error";
  };
  for (std::size_t index = 0; index < simulation_summary_->cases.size();
       ++index) {
    const adapters::SimulationTestCaseResult& test_case =
        simulation_summary_->cases[index];
    std::wstring status = status_name(test_case.status);
    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = static_cast<int>(index);
    item.pszText = status.data();
    ListView_InsertItem(tests_, &item);
    std::wstring suite = Utf8ToWide(test_case.suite);
    std::wstring name = Utf8ToWide(test_case.name);
    std::wstring duration = std::to_wstring(test_case.duration_seconds);
    std::wstring detail = Utf8ToWide(test_case.detail);
    ListView_SetItemText(tests_, static_cast<int>(index), 1, suite.data());
    ListView_SetItemText(tests_, static_cast<int>(index), 2, name.data());
    ListView_SetItemText(tests_, static_cast<int>(index), 3, duration.data());
    ListView_SetItemText(tests_, static_cast<int>(index), 4, detail.data());
  }
  const std::wstring aggregate =
      L"Tests: " + std::to_wstring(simulation_summary_->total) + L" total, " +
      std::to_wstring(simulation_summary_->passed) + L" passed, " +
      std::to_wstring(simulation_summary_->failed) + L" failed, " +
      std::to_wstring(simulation_summary_->errors) + L" errors, " +
      std::to_wstring(simulation_summary_->skipped) + L" skipped";
  SetWindowTextW(simulation_status_, aggregate.c_str());
}

void VerilogWindow::PopulateProblems() {
  ListView_DeleteAllItems(problems_);
  for (std::size_t index = 0; index < diagnostics_.size(); ++index) {
    const auto& diagnostic = diagnostics_[index];
    std::wstring severity = DiagnosticSeverityName(diagnostic.severity);
    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = static_cast<int>(index);
    item.pszText = severity.data();
    ListView_InsertItem(problems_, &item);
    std::wstring code = Utf8ToWide(diagnostic.code);
    std::wstring file = Utf8ToWide(diagnostic.file);
    std::wstring line = std::to_wstring(diagnostic.line);
    std::wstring message = Utf8ToWide(diagnostic.message);
    ListView_SetItemText(problems_, static_cast<int>(index), 1, code.data());
    ListView_SetItemText(problems_, static_cast<int>(index), 2, file.data());
    ListView_SetItemText(problems_, static_cast<int>(index), 3, line.data());
    ListView_SetItemText(problems_, static_cast<int>(index), 4, message.data());
  }
}

void VerilogWindow::ShowBottomPage(int index) const {
  const HWND pages[] = {problems_, runs_, artifacts_, tests_, output_};
  for (int page = 0; page < 5; ++page) {
    ShowWindow(pages[page], page == index ? SW_SHOW : SW_HIDE);
  }
  for (HWND control : {debug_state_label_, debug_scope_tree_, debug_variables_,
                       debug_transcript_, debug_command_, debug_send_button_}) {
    ShowWindow(control, index == 5 ? SW_SHOW : SW_HIDE);
  }
}

void VerilogWindow::MarkDirty() {
  if (!document_ || document_->read_only) return;
  dirty_ = true;
  close_prepared_ = false;
  UpdateTitle();
}

bool VerilogWindow::ReadProjectControls() {
  if (!document_) return false;
  document_->project.top_module = WideToUtf8(WindowText(top_module_));
  const std::wstring cpu_text = WindowText(cpu_budget_);
  wchar_t* end = nullptr;
  const unsigned long value = std::wcstoul(cpu_text.c_str(), &end, 10);
  const unsigned int logical = std::thread::hardware_concurrency();
  const unsigned long maximum = logical > 1 ? logical - 1 : 1;
  if (cpu_text.empty() || end == cpu_text.c_str() || *end != L'\0' ||
      value == 0 || value > maximum) {
    MessageBoxW(window_, L"CPU budget이 유효하지 않습니다.",
                L"Design++ Workspace", MB_OK | MB_ICONERROR);
    return false;
  }
  document_->project.cpu_budget = static_cast<std::uint32_t>(value);
  document_->project.include_directories =
      SplitValues(WindowText(include_directories_));
  document_->project.defines = SplitValues(WindowText(defines_));
  document_->project.parameters = SplitValues(WindowText(parameters_));
  const std::string constraint = WideToUtf8(WindowText(constraint_path_));
  document_->project.constraint_path =
      constraint.empty() ? std::nullopt
                         : std::optional<std::string>(constraint);
  const core::Status validation = core::ValidateProject(document_->project);
  if (!validation.Ok()) {
    MessageBoxW(window_, Utf8ToWide(validation.message).c_str(),
                L"Project Validation", MB_OK | MB_ICONERROR);
    return false;
  }
  return true;
}

bool VerilogWindow::SaveProject() {
  if (!document_ || document_->read_only) return false;
  if (!ReadProjectControls()) return false;
  const core::Status status = project_service_.Save(document_.get());
  if (!status.Ok()) {
    MessageBoxW(window_, Utf8ToWide(status.message).c_str(),
                L"Project Save Error", MB_OK | MB_ICONERROR);
    AppendOutput(L"[Save error] " + Utf8ToWide(status.message) + L"\r\n");
    return false;
  }
  dirty_ = false;
  SetStatus(L"Project saved");
  UpdateTitle();
  return true;
}

void VerilogWindow::ToggleSource(std::size_t index, bool enabled) {
  if (!document_ || document_->read_only || index >= sources_.size()) return;
  sources_[index].enabled = enabled;
  const auto& source = sources_[index];
  auto& overrides = document_->project.source_overrides;
  const auto existing = std::find_if(
      overrides.begin(), overrides.end(), [&source](const auto& value) {
        return value.view_id == source.view_id &&
               value.relative_path == source.relative_path;
      });
  if (enabled) {
    if (existing != overrides.end()) overrides.erase(existing);
  } else if (existing == overrides.end()) {
    overrides.push_back(
        {source.view_id, source.relative_path, false, source.role});
  } else {
    existing->enabled = false;
  }
  MarkDirty();
}

std::string VerilogWindow::SelectedSourceViewId() const {
  if (source_tree_ != nullptr) {
    HTREEITEM item = TreeView_GetSelection(source_tree_);
    while (item != nullptr) {
      TVITEMW selected{};
      selected.mask = TVIF_PARAM;
      selected.hItem = item;
      if (!TreeView_GetItem(source_tree_, &selected)) break;
      const auto* node =
          reinterpret_cast<const SourceTreeNode*>(selected.lParam);
      if (node != nullptr && !node->view_id.empty()) return node->view_id;
      item = TreeView_GetParent(source_tree_, item);
    }
  }
  return request_.view_id;
}

void VerilogWindow::AddSourceFiles() {
  if (!document_ || document_->read_only) {
    MessageBoxW(window_, L"읽기 전용 Workspace에는 파일을 추가할 수 없습니다.",
                L"Add Source Files", MB_OK | MB_ICONINFORMATION);
    return;
  }
  const std::string view_id = SelectedSourceViewId();
  const core::View* view =
      FindView(FindCell(library_, request_.cell_id), view_id);
  if (view == nullptr || (view->kind != core::ViewKind::kVerilog &&
                          view->kind != core::ViewKind::kTestbench)) {
    MessageBoxW(window_, L"파일을 추가할 RTL 또는 Testbench View를 선택하세요.",
                L"Add Source Files", MB_OK | MB_ICONINFORMATION);
    return;
  }

  std::vector<wchar_t> buffer(65536, L'\0');
  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = window_;
  dialog.lpstrFile = buffer.data();
  dialog.nMaxFile = static_cast<DWORD>(buffer.size());
  dialog.lpstrFilter =
      L"Verilog files (*.v;*.vh;*.sv;*.svh)\0*.v;*.vh;*.sv;*.svh\0All "
      L"files\0*.*\0\0";
  dialog.Flags = OFN_FILEMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_EXPLORER |
                 OFN_PATHMUSTEXIST;
  if (!GetOpenFileNameW(&dialog)) return;

  std::vector<std::filesystem::path> paths;
  const std::filesystem::path first(buffer.data());
  const wchar_t* cursor = buffer.data() + first.native().size() + 1;
  if (*cursor == L'\0') {
    paths.push_back(first);
  } else {
    while (*cursor != L'\0') {
      const std::filesystem::path name(cursor);
      paths.push_back(first / name);
      cursor += name.native().size() + 1;
    }
  }

  const application::LibraryRecord library = library_;
  const core::Project project = document_->project;
  const std::string cell_id = request_.cell_id;
  const auto channel = event_channel_;
  const std::uint64_t generation = generation_;
  const bool accepted = scheduler_.Submit(
      [library, project, cell_id, view_id, paths = std::move(paths), channel,
       generation](std::stop_token stop_token) {
        if (stop_token.stop_requested()) return;
        WorkspaceEvent event;
        event.kind = WorkspaceEventKind::kFilesImported;
        event.generation = generation;
        application::LibraryService library_service;
        auto imported =
            library_service.ImportFiles(library, cell_id, view_id, paths);
        if (!imported.Ok()) {
          event.status = imported.GetStatus();
        } else {
          event.library = std::move(imported).Value();
          application::ProjectService project_service;
          auto resolved =
              project_service.ResolveSources(event.library, cell_id, project);
          if (!resolved.Ok()) {
            event.status = resolved.GetStatus();
          } else {
            event.sources = std::move(resolved).Value();
            std::vector<std::filesystem::path> rtl_paths;
            for (const auto& source : event.sources) {
              if (source.enabled && source.exists &&
                  source.view_kind == core::ViewKind::kVerilog) {
                rtl_paths.push_back(source.windows_path);
              }
            }
            ScanModuleMetadata(rtl_paths, &event);
            ScanTestbenchMetadata(event.sources, &event);
            event.status = core::Status::Success();
          }
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
    SetStatus(L"Source import queue is full");
  } else {
    SetStatus(L"Adding managed source files...");
  }
}

void VerilogWindow::OpenSource(std::size_t index) {
  if (index >= sources_.size() || !sources_[index].exists) return;
  const application::ResolvedSource source = sources_[index];
  const application::LibraryRecord library = library_;
  const std::string cell_id = request_.cell_id;
  const bool read_only = !document_ || document_->read_only;
  const auto channel = event_channel_;
  const std::uint64_t generation = generation_;
  const bool accepted =
      scheduler_.Submit([source, library, cell_id, read_only, channel,
                         generation](std::stop_token stop_token) {
        if (stop_token.stop_requested()) return;
        WorkspaceEvent event;
        event.kind = WorkspaceEventKind::kDocumentLoaded;
        event.generation = generation;
        event.document_id = source.relative_path;
        application::ManagedSourceService service;
        auto loaded = service.LoadDocument(library, cell_id, source.view_id,
                                           source.relative_path, read_only);
        if (loaded.Ok()) {
          event.editor_document = std::move(loaded).Value();
          application::SystemVerilogModuleScanner scanner;
          for (const application::ModuleDeclaration& module :
               scanner.FindModulesInText(event.editor_document.text)) {
            event.document_module_candidates.push_back(module.name);
          }
          event.status = core::Status::Success();
        } else {
          event.status = loaded.GetStatus();
        }
        HWND target = nullptr;
        {
          std::scoped_lock lock(channel->mutex);
          if (!channel->window || channel->generation != generation) return;
          channel->events.push_back(std::move(event));
          target = channel->window;
        }
        PostMessageW(target, kEventMessage, 0, 0);
      });
  if (!accepted) SetStatus(L"Editor load queue is full");
}

void VerilogWindow::OpenSourceByDiagnostic(const core::Diagnostic& diagnostic) {
  const std::string diagnostic_filename = WideToUtf8(
      std::filesystem::path(Utf8ToWide(diagnostic.file)).filename().wstring());
  const auto source = std::find_if(
      sources_.begin(), sources_.end(), [&](const auto& candidate) {
        const std::string filename =
            WideToUtf8(candidate.windows_path.filename().wstring());
        return candidate.relative_path == diagnostic.file ||
               filename == diagnostic.file || filename == diagnostic_filename;
      });
  if (source == sources_.end()) {
    SetStatus(L"Diagnostic source is not managed by this Cell");
    return;
  }
  const std::size_t index =
      static_cast<std::size_t>(std::distance(sources_.begin(), source));
  const auto opened =
      std::find_if(editor_documents_.begin(), editor_documents_.end(),
                   [&](const auto& document) {
                     return document.relative_path == source->relative_path;
                   });
  if (opened == editor_documents_.end()) {
    pending_reveal_ = diagnostic;
    OpenSource(index);
    return;
  }
  editor_host_.RevealLocation(opened->id, diagnostic.line, diagnostic.column);
}

void VerilogWindow::HandleEditorMessage(application::EditorWebMessage message) {
  if (message.type == "ready") {
    const auto source = std::find_if(
        sources_.begin(), sources_.end(), [&](const auto& candidate) {
          return candidate.view_id == request_.view_id && candidate.exists;
        });
    if (source != sources_.end()) {
      OpenSource(
          static_cast<std::size_t>(std::distance(sources_.begin(), source)));
    }
  } else if (message.type == "active_document_changed") {
    application::EditorDocumentSnapshot* document =
        FindEditorDocument(message.document_id);
    if (document != nullptr) {
      active_document_id_ = document->id;
      request_.view_id = document->view_id;
      last_document_by_view_[document->view_id] = document->relative_path;
    } else {
      active_document_id_.clear();
    }
    UpdateInspector();
  } else if (message.type == "document_changed") {
    if (FindEditorDocument(message.document_id) != nullptr) {
      dirty_document_ids_.insert(message.document_id);
      close_prepared_ = false;
      UpdateTitle();
    }
  } else if (message.type == "save_document") {
    BeginSaveDocument(std::move(message.document_id), std::move(message.text),
                      false);
  } else if (message.type == "close_document_requested") {
    application::EditorDocumentSnapshot* document =
        FindEditorDocument(message.document_id);
    if (document == nullptr) return;
    if (dirty_document_ids_.contains(message.document_id)) {
      const int choice =
          MessageBoxW(window_, L"이 파일의 변경 사항을 저장하시겠습니까?",
                      L"Design++ Editor", MB_YESNOCANCEL | MB_ICONQUESTION);
      if (choice == IDCANCEL) return;
      if (choice == IDYES) {
        pending_close_document_ids_.insert(message.document_id);
        editor_host_.RequestSaveAll();
        return;
      }
      dirty_document_ids_.erase(message.document_id);
    }
    editor_host_.CloseDocument(message.document_id);
    std::erase_if(editor_documents_, [&](const auto& candidate) {
      return candidate.id == message.document_id;
    });
    if (active_document_id_ == message.document_id) {
      active_document_id_.clear();
      UpdateInspector();
    }
    UpdateTitle();
  } else if (message.type == "editor_command" &&
             message.command == "save_all") {
    if (!dirty_document_ids_.empty()) {
      save_project_after_documents_ = true;
      RequestSaveAll();
    } else {
      static_cast<void>(SaveProject());
    }
  } else if (message.type == "fatal_error") {
    AppendOutput(L"[Editor fatal] " + Utf8ToWide(message.text) + L"\r\n");
  }
}

void VerilogWindow::BeginSaveDocument(std::string document_id, std::string text,
                                      bool overwrite_external) {
  application::EditorDocumentSnapshot* document =
      FindEditorDocument(document_id);
  if (document == nullptr || saving_document_ids_.contains(document_id)) return;
  pending_save_text_[document_id] = text;
  if (!saving_document_ids_.empty()) {
    const auto queued =
        std::find_if(document_save_queue_.begin(), document_save_queue_.end(),
                     [&](const PendingDocumentSave& pending) {
                       return pending.document_id == document_id;
                     });
    if (queued == document_save_queue_.end()) {
      document_save_queue_.push_back(
          {std::move(document_id), std::move(text), overwrite_external});
    } else {
      queued->text = std::move(text);
      queued->overwrite_external = overwrite_external;
    }
    return;
  }
  DispatchDocumentSave(std::move(document_id), std::move(text),
                       overwrite_external);
}

void VerilogWindow::DispatchDocumentSave(std::string document_id,
                                         std::string text,
                                         bool overwrite_external) {
  application::EditorDocumentSnapshot* document =
      FindEditorDocument(document_id);
  if (document == nullptr) return;
  saving_document_ids_.insert(document_id);
  const application::EditorDocumentSnapshot snapshot = *document;
  const application::LibraryRecord library = library_;
  const core::Project project = document_->project;
  pending_save_text_[document_id] = text;
  const auto channel = event_channel_;
  const std::uint64_t generation = generation_;
  const bool accepted =
      scheduler_.Submit([snapshot, library, project, document_id,
                         text = std::move(text), overwrite_external, channel,
                         generation](std::stop_token stop_token) mutable {
        if (stop_token.stop_requested()) return;
        WorkspaceEvent event;
        event.kind = WorkspaceEventKind::kDocumentSaved;
        event.generation = generation;
        event.document_id = document_id;
        event.save_text = text;
        application::ManagedSourceService service;
        auto saved =
            service.SaveDocument(library, snapshot, text, overwrite_external);
        if (saved.Ok()) {
          application::SaveDocumentResult result = std::move(saved).Value();
          application::ProjectService project_service;
          auto sources = project_service.ResolveSources(
              result.library, snapshot.cell_id, project);
          if (sources.Ok()) {
            event.sources = std::move(sources).Value();
            std::vector<std::filesystem::path> rtl_paths;
            for (const auto& source : event.sources) {
              if (source.enabled && source.exists &&
                  source.view_kind == core::ViewKind::kVerilog) {
                rtl_paths.push_back(source.windows_path);
              }
            }
            ScanModuleMetadata(rtl_paths, &event);
            ScanTestbenchMetadata(event.sources, &event);
            event.source_snapshot_available = true;
          }
          event.library = std::move(result.library);
          event.editor_document = std::move(result.document);
          application::SystemVerilogModuleScanner scanner;
          for (const application::ModuleDeclaration& module :
               scanner.FindModulesInText(event.editor_document.text)) {
            event.document_module_candidates.push_back(module.name);
          }
          event.status = core::Status::Success();
        } else {
          event.status = saved.GetStatus();
        }
        HWND target = nullptr;
        {
          std::scoped_lock lock(channel->mutex);
          if (!channel->window || channel->generation != generation) return;
          channel->events.push_back(std::move(event));
          target = channel->window;
        }
        PostMessageW(target, kEventMessage, 0, 0);
      });
  if (!accepted) {
    saving_document_ids_.erase(document_id);
    pending_save_text_.erase(document_id);
    editor_host_.SaveResult(document_id, false, "Save queue is full");
  }
}

void VerilogWindow::DispatchNextDocumentSave() {
  if (!saving_document_ids_.empty() || document_save_queue_.empty()) return;
  PendingDocumentSave pending = std::move(document_save_queue_.front());
  document_save_queue_.pop_front();
  DispatchDocumentSave(std::move(pending.document_id), std::move(pending.text),
                       pending.overwrite_external);
}

void VerilogWindow::RequestSaveAll() { editor_host_.RequestSaveAll(); }

application::EditorDocumentSnapshot* VerilogWindow::FindEditorDocument(
    std::string_view document_id) {
  const auto document =
      std::find_if(editor_documents_.begin(), editor_documents_.end(),
                   [document_id](const auto& candidate) {
                     return candidate.id == document_id;
                   });
  return document == editor_documents_.end() ? nullptr : &*document;
}

void VerilogWindow::StartLint() {
  if (!dirty_document_ids_.empty() || dirty_) {
    const int choice = MessageBoxW(
        window_, L"Lint 전에 모든 파일과 프로젝트 설정을 저장하시겠습니까?",
        L"Design++ Workspace", MB_OKCANCEL | MB_ICONQUESTION);
    if (choice != IDOK) return;
    if (!dirty_document_ids_.empty()) {
      pending_run_ = PendingRun::kLint;
      save_project_after_documents_ = true;
      RequestSaveAll();
      return;
    }
    if (dirty_ && !SaveProject()) return;
  }
  if (!document_ || active_operation_ != ActiveOperation::kNone) return;
  if (!ReadProjectControls()) return;
  if (dirty_) {
    MessageBoxW(window_, L"Lint 실행 전에 프로젝트를 저장하세요.",
                L"Design++ Workspace", MB_OK | MB_ICONINFORMATION);
    return;
  }
  adapters::VerilatorAdapter adapter;
  const core::Status validation =
      adapter.Validate(document_->project, sources_);
  if (!validation.Ok()) {
    MessageBoxW(window_, Utf8ToWide(validation.message).c_str(),
                L"Verilator Lint", MB_OK | MB_ICONERROR);
    return;
  }
  active_operation_ = ActiveOperation::kLint;
  EnableWindow(lint_button_, FALSE);
  EnableWindow(cancel_button_, TRUE);
  SetStatus(L"Probing Verilator...");
  AppendOutput(L"[Verilator] version probe\r\n");
  const auto channel = event_channel_;
  const std::uint64_t generation = generation_;
  auto launch = runtime::WslExecutor::RunAsync(
      adapter.BuildProbeCommand(),
      [channel, generation](std::string output) {
        WorkspaceEvent event;
        event.kind = WorkspaceEventKind::kOutput;
        event.generation = generation;
        event.output = std::move(output);
        HWND target = nullptr;
        {
          std::scoped_lock lock(channel->mutex);
          if (!channel->window || channel->generation != generation) return;
          channel->events.push_back(std::move(event));
          target = channel->window;
        }
        PostMessageW(target, kEventMessage, 0, 0);
      },
      [channel, generation](runtime::ProcessResult result) {
        WorkspaceEvent event;
        event.kind = WorkspaceEventKind::kProbeComplete;
        event.generation = generation;
        event.process_result = std::move(result);
        HWND target = nullptr;
        {
          std::scoped_lock lock(channel->mutex);
          if (!channel->window || channel->generation != generation) return;
          channel->events.push_back(std::move(event));
          target = channel->window;
        }
        PostMessageW(target, kEventMessage, 0, 0);
      });
  if (!launch.IsValid()) {
    active_operation_ = ActiveOperation::kNone;
    EnableWindow(lint_button_, TRUE);
    EnableWindow(cancel_button_, FALSE);
    MessageBoxW(window_, launch.error_message.c_str(), L"Verilator Probe",
                MB_OK | MB_ICONERROR);
    return;
  }
  process_ =
      std::make_unique<runtime::ProcessSession>(std::move(launch.session));
}

void VerilogWindow::HandleProbeComplete(const runtime::ProcessResult& result) {
  if (!result.started || result.cancelled || result.exit_code != 0) {
    active_operation_ = ActiveOperation::kNone;
    EnableWindow(lint_button_, TRUE);
    EnableWindow(cancel_button_, FALSE);
    SetStatus(result.cancelled ? L"Probe cancelled" : L"Verilator unavailable");
    return;
  }
  std::string version = result.output;
  const std::size_t newline = version.find_first_of("\r\n");
  if (newline != std::string::npos) version.resize(newline);
  StartLintProcess(std::move(version));
}

void VerilogWindow::StartLintProcess(std::string tool_version) {
  auto lease =
      resource_coordinator_.TryAcquireCpu(document_->project.cpu_budget);
  if (!lease.Ok()) {
    active_operation_ = ActiveOperation::kNone;
    EnableWindow(lint_button_, TRUE);
    EnableWindow(cancel_button_, FALSE);
    MessageBoxW(window_, Utf8ToWide(lease.GetStatus().message).c_str(),
                L"Design++ CPU Quota", MB_OK | MB_ICONINFORMATION);
    return;
  }
  cpu_lease_ =
      std::make_unique<runtime::CpuTokenLease>(std::move(lease).Value());
  adapters::VerilatorAdapter adapter;
  runtime::PathMapper mapper;
  auto command = adapter.BuildCommand(
      document_->project, sources_, mapper,
      adapters::ToolCapabilities{"Verilator", tool_version, true});
  if (!command.Ok()) {
    active_operation_ = ActiveOperation::kNone;
    cpu_lease_.reset();
    EnableWindow(lint_button_, TRUE);
    EnableWindow(cancel_button_, FALSE);
    MessageBoxW(window_, Utf8ToWide(command.GetStatus().message).c_str(),
                L"Verilator Lint", MB_OK | MB_ICONERROR);
    return;
  }
  const std::filesystem::path cell_directory =
      library_.directory / L"cells" / Utf8ToWide(request_.cell_id);
  auto begun = run_store_.Begin(cell_directory, document_->project, "Lint",
                                "Verilator", tool_version);
  if (!begun.Ok()) {
    active_operation_ = ActiveOperation::kNone;
    cpu_lease_.reset();
    EnableWindow(lint_button_, TRUE);
    EnableWindow(cancel_button_, FALSE);
    MessageBoxW(window_, Utf8ToWide(begun.GetStatus().message).c_str(),
                L"Run Store", MB_OK | MB_ICONERROR);
    return;
  }
  active_run_ =
      std::make_shared<application::RunRecord>(std::move(begun).Value());
  const auto run = active_run_;
  const auto channel = event_channel_;
  const std::uint64_t generation = generation_;
  SetStatus(L"Verilator Lint running...");
  AppendOutput(L"[Verilator Lint] started\r\n");
  auto launch = runtime::WslExecutor::RunAsync(
      command.Value(),
      [channel, generation, run](std::string output) {
        application::RunStore store;
        static_cast<void>(store.AppendLog(*run, output));
        WorkspaceEvent event;
        event.kind = WorkspaceEventKind::kOutput;
        event.generation = generation;
        event.output = std::move(output);
        HWND target = nullptr;
        {
          std::scoped_lock lock(channel->mutex);
          if (!channel->window || channel->generation != generation) return;
          channel->events.push_back(std::move(event));
          target = channel->window;
        }
        PostMessageW(target, kEventMessage, 0, 0);
      },
      [channel, generation, run](runtime::ProcessResult result) {
        adapters::VerilatorAdapter adapter;
        std::vector<core::Diagnostic> diagnostics =
            adapter.ParseDiagnostics(result.output);
        const application::RunStatus status =
            result.cancelled ? application::RunStatus::kCancelled
            : result.started && result.exit_code == 0
                ? application::RunStatus::kSucceeded
                : application::RunStatus::kFailed;
        application::RunStore store;
        static_cast<void>(
            store.Complete(run.get(), status, result.exit_code, diagnostics));
        WorkspaceEvent event;
        event.kind = WorkspaceEventKind::kLintComplete;
        event.generation = generation;
        event.process_result = std::move(result);
        event.diagnostics = std::move(diagnostics);
        event.run = run;
        HWND target = nullptr;
        {
          std::scoped_lock lock(channel->mutex);
          if (!channel->window || channel->generation != generation) return;
          channel->events.push_back(std::move(event));
          target = channel->window;
        }
        PostMessageW(target, kEventMessage, 0, 0);
      });
  if (!launch.IsValid()) {
    std::vector<core::Diagnostic> diagnostics;
    static_cast<void>(run_store_.Complete(
        active_run_.get(), application::RunStatus::kFailed, 0, diagnostics));
    run_records_.insert(run_records_.begin(), *active_run_);
    PopulateRuns();
    active_operation_ = ActiveOperation::kNone;
    cpu_lease_.reset();
    EnableWindow(lint_button_, TRUE);
    EnableWindow(cancel_button_, FALSE);
    MessageBoxW(window_, launch.error_message.c_str(), L"Verilator Lint",
                MB_OK | MB_ICONERROR);
    return;
  }
  process_ =
      std::make_unique<runtime::ProcessSession>(std::move(launch.session));
}

void VerilogWindow::StartSimulation() {
  if (!document_ || active_operation_ != ActiveOperation::kNone) return;
  application::EditorDocumentSnapshot* active =
      FindEditorDocument(active_document_id_);
  const core::View* view =
      active == nullptr
          ? nullptr
          : FindView(FindCell(library_, request_.cell_id), active->view_id);
  if (active == nullptr || view == nullptr ||
      view->kind != core::ViewKind::kTestbench || active->missing) {
    MessageBoxW(window_, L"실행할 Testbench 파일을 활성화하세요.",
                L"Icarus Simulation", MB_OK | MB_ICONINFORMATION);
    return;
  }
  ReadTestbenchControls();
  if (!dirty_document_ids_.empty() || dirty_) {
    const int choice = MessageBoxW(
        window_, L"Testbench 실행 전에 모든 변경 사항을 저장하시겠습니까?",
        L"Design++ Workspace", MB_OKCANCEL | MB_ICONQUESTION);
    if (choice != IDOK) return;
    if (!dirty_document_ids_.empty()) {
      pending_run_ = PendingRun::kSimulation;
      save_project_after_documents_ = true;
      RequestSaveAll();
      return;
    }
    if (dirty_ && !SaveProject()) return;
  }
  const std::string top = simulation_runner_id_ == "cocotb"
                              ? document_->project.top_module
                              : WideToUtf8(WindowText(testbench_top_));
  if (top.empty()) {
    MessageBoxW(window_, L"Testbench Top module을 선택하세요.",
                L"Icarus Simulation", MB_OK | MB_ICONERROR);
    return;
  }
  adapters::SimulationRequest request;
  request.project = document_->project;
  request.sources = sources_;
  request.testbench_view_id = active->view_id;
  request.testbench_relative_path = active->relative_path;
  request.testbench_top = top;
  request.waveform_enabled =
      SendMessageW(waveform_checkbox_, BM_GETCHECK, 0, 0) == BST_CHECKED;
  request.waveform_format =
      request.waveform_enabled
          ? (SendMessageW(waveform_format_, CB_GETCURSEL, 0, 0) == 1 ? "fst"
                                                                     : "vcd")
          : "none";
  request.artifact_directory = library_.directory / L"cells" /
                               Utf8ToWide(request_.cell_id) / L".designpp" /
                               L"runs" / L"pending" / L"artifacts";
  std::unique_ptr<adapters::SimulationAdapter> adapter;
  core::Status validation = core::Status::Success();
  runtime::WslCommand probe;
  if (simulation_runner_id_ == "cocotb") {
    adapters::CocotbRequest cocotb_request;
    cocotb_request.simulation = request;
    cocotb_request.module = WideToUtf8(WindowText(cocotb_module_));
    cocotb_request.testcase_filter = WideToUtf8(WindowText(cocotb_testcase_));
    cocotb_request.backend = simulation_backend_id_;
    if (cocotb_request.module.empty()) {
      validation = {core::ErrorCode::kInvalidArgument,
                    "Select a cocotb Python module", 0};
    }
    probe = adapters::CocotbRunnerAdapter().BuildProbeCommand();
  } else {
    adapter = adapters::CreateSimulationAdapter(simulation_backend_id_);
    if (!adapter) {
      validation = {core::ErrorCode::kInvalidArgument,
                    "The selected simulator is unsupported", 0};
    } else {
      validation = adapter->Validate(request);
      probe = adapter->BuildProbeCommand();
    }
  }
  if (!validation.Ok()) {
    MessageBoxW(window_, Utf8ToWide(validation.message).c_str(), L"Simulation",
                MB_OK | MB_ICONERROR);
    return;
  }

  active_operation_ = ActiveOperation::kSimulation;
  simulation_summary_.reset();
  cocotb_plan_.reset();
  simulation_plan_.reset();
  simulation_started_ = std::chrono::steady_clock::now();
  EnableWindow(simulation_run_button_, FALSE);
  EnableWindow(simulation_cancel_button_, TRUE);
  const std::wstring display_name = Utf8ToWide(
      adapters::SimulationBackendDisplayName(simulation_backend_id_));
  SetWindowTextW(simulation_status_,
                 (L"Probing " + display_name + L"...").c_str());
  SetStatus((L"Probing " + display_name + L"...").c_str());
  const auto channel = event_channel_;
  const std::uint64_t generation = generation_;
  auto launch = runtime::WslExecutor::RunAsync(
      probe,
      [channel, generation](std::string output) {
        WorkspaceEvent event;
        event.kind = WorkspaceEventKind::kOutput;
        event.generation = generation;
        event.output = std::move(output);
        HWND target = nullptr;
        {
          std::scoped_lock lock(channel->mutex);
          if (!channel->window || channel->generation != generation) return;
          channel->events.push_back(std::move(event));
          target = channel->window;
        }
        PostMessageW(target, kEventMessage, 0, 0);
      },
      [channel, generation](runtime::ProcessResult result) {
        WorkspaceEvent event;
        event.kind = WorkspaceEventKind::kSimulationProbeComplete;
        event.generation = generation;
        event.process_result = std::move(result);
        HWND target = nullptr;
        {
          std::scoped_lock lock(channel->mutex);
          if (!channel->window || channel->generation != generation) return;
          channel->events.push_back(std::move(event));
          target = channel->window;
        }
        PostMessageW(target, kEventMessage, 0, 0);
      });
  if (!launch.IsValid()) {
    active_operation_ = ActiveOperation::kNone;
    EnableWindow(simulation_run_button_, TRUE);
    EnableWindow(simulation_cancel_button_, FALSE);
    MessageBoxW(window_, launch.error_message.c_str(), L"Simulation Probe",
                MB_OK | MB_ICONERROR);
    return;
  }
  process_ =
      std::make_unique<runtime::ProcessSession>(std::move(launch.session));
}

void VerilogWindow::HandleSimulationProbeComplete(
    const runtime::ProcessResult& result) {
  if (!result.started || result.cancelled || result.exit_code != 0) {
    active_operation_ = ActiveOperation::kNone;
    EnableWindow(simulation_run_button_, TRUE);
    EnableWindow(simulation_cancel_button_, FALSE);
    SetWindowTextW(simulation_status_, result.cancelled
                                           ? L"Simulation cancelled"
                                           : L"Icarus Verilog unavailable");
    SetStatus(result.cancelled ? L"Simulation cancelled"
                               : L"Icarus Verilog unavailable");
    return;
  }
  std::string version = result.output;
  const std::size_t newline = version.find_first_of("\r\n");
  if (newline != std::string::npos) version.resize(newline);
  if (simulation_runner_id_ == "cocotb") {
    StartCocotbMakefilesProbe(std::move(version));
  } else {
    PrepareSimulation(std::move(version));
  }
}

void VerilogWindow::StartCocotbMakefilesProbe(std::string tool_version) {
  simulation_tool_version_ = std::move(tool_version);
  const auto channel = event_channel_;
  const std::uint64_t generation = generation_;
  auto launch = runtime::WslExecutor::RunAsync(
      adapters::CocotbRunnerAdapter().BuildMakefilesProbeCommand(),
      [channel, generation](std::string output) {
        WorkspaceEvent event;
        event.kind = WorkspaceEventKind::kOutput;
        event.generation = generation;
        event.output = std::move(output);
        HWND target = nullptr;
        {
          std::scoped_lock lock(channel->mutex);
          if (!channel->window || channel->generation != generation) return;
          channel->events.push_back(std::move(event));
          target = channel->window;
        }
        PostMessageW(target, kEventMessage, 0, 0);
      },
      [channel, generation](runtime::ProcessResult result) {
        WorkspaceEvent event;
        event.kind = WorkspaceEventKind::kCocotbMakefilesProbeComplete;
        event.generation = generation;
        event.process_result = std::move(result);
        HWND target = nullptr;
        {
          std::scoped_lock lock(channel->mutex);
          if (!channel->window || channel->generation != generation) return;
          channel->events.push_back(std::move(event));
          target = channel->window;
        }
        PostMessageW(target, kEventMessage, 0, 0);
      });
  if (!launch.IsValid()) {
    active_operation_ = ActiveOperation::kNone;
    EnableWindow(simulation_run_button_, TRUE);
    EnableWindow(simulation_cancel_button_, FALSE);
    MessageBoxW(window_, launch.error_message.c_str(), L"cocotb Probe",
                MB_OK | MB_ICONERROR);
    return;
  }
  process_ =
      std::make_unique<runtime::ProcessSession>(std::move(launch.session));
}

void VerilogWindow::HandleCocotbMakefilesProbeComplete(
    const runtime::ProcessResult& result) {
  if (!result.started || result.cancelled || result.exit_code != 0) {
    active_operation_ = ActiveOperation::kNone;
    EnableWindow(simulation_run_button_, TRUE);
    EnableWindow(simulation_cancel_button_, FALSE);
    SetStatus(result.cancelled ? L"cocotb run cancelled"
                               : L"cocotb makefiles unavailable");
    return;
  }
  std::string path = result.output;
  const std::size_t first = path.find_first_not_of(" \t\r\n");
  const std::size_t last = path.find_last_not_of(" \t\r\n");
  if (first == std::string::npos) {
    active_operation_ = ActiveOperation::kNone;
    SetStatus(L"cocotb makefiles path is empty");
    UpdateInspector();
    return;
  }
  path = path.substr(first, last - first + 1);
  PrepareCocotb(std::move(simulation_tool_version_), Utf8ToWide(path));
}

void VerilogWindow::PrepareSimulation(std::string tool_version) {
  auto lease = resource_coordinator_.TryAcquireCpu(1);
  if (!lease.Ok()) {
    active_operation_ = ActiveOperation::kNone;
    MessageBoxW(window_, Utf8ToWide(lease.GetStatus().message).c_str(),
                L"Design++ CPU Quota", MB_OK | MB_ICONINFORMATION);
    UpdateInspector();
    return;
  }
  cpu_lease_ =
      std::make_unique<runtime::CpuTokenLease>(std::move(lease).Value());
  application::EditorDocumentSnapshot* active =
      FindEditorDocument(active_document_id_);
  if (active == nullptr) {
    active_operation_ = ActiveOperation::kNone;
    cpu_lease_.reset();
    return;
  }
  adapters::SimulationRequest request;
  request.project = document_->project;
  request.sources = sources_;
  request.testbench_view_id = active->view_id;
  request.testbench_relative_path = active->relative_path;
  request.testbench_top = WideToUtf8(WindowText(testbench_top_));
  request.waveform_enabled =
      SendMessageW(waveform_checkbox_, BM_GETCHECK, 0, 0) == BST_CHECKED;
  request.waveform_format =
      request.waveform_enabled
          ? (SendMessageW(waveform_format_, CB_GETCURSEL, 0, 0) == 1 ? "fst"
                                                                     : "vcd")
          : "none";
  const std::string backend = simulation_backend_id_;
  const std::filesystem::path cell_directory =
      library_.directory / L"cells" / Utf8ToWide(request_.cell_id);
  const auto channel = event_channel_;
  const std::uint64_t generation = generation_;
  const bool accepted = scheduler_.Submit(
      [request = std::move(request), cell_directory,
       tool_version = std::move(tool_version), backend, channel,
       generation](std::stop_token stop_token) mutable {
        if (stop_token.stop_requested()) return;
        WorkspaceEvent event;
        event.kind = WorkspaceEventKind::kSimulationPrepared;
        event.generation = generation;
        application::RunStore store;
        auto begun = store.Begin(
            cell_directory, request.project, "Simulation",
            std::string(adapters::SimulationBackendDisplayName(backend)),
            tool_version);
        if (!begun.Ok()) {
          event.status = begun.GetStatus();
        } else {
          event.run = std::make_shared<application::RunRecord>(
              std::move(begun).Value());
          request.artifact_directory = event.run->directory / L"artifacts";
          if (backend == "verilator") {
            auto build_directory = CreateVerilatorBuildDirectory(event.run->id);
            if (!build_directory.Ok()) {
              event.status = build_directory.GetStatus();
            } else {
              request.build_directory = std::move(build_directory).Value();
            }
          }
          std::unique_ptr<adapters::SimulationAdapter> adapter =
              adapters::CreateSimulationAdapter(backend);
          if (event.status.message.empty() && !adapter) {
            event.status = {core::ErrorCode::kInvalidArgument,
                            "Simulation backend is unsupported", 0};
          }
          runtime::PathMapper mapper;
          auto plan =
              adapter && event.status.message.empty()
                  ? adapter->BuildPlan(request, mapper)
                  : core::Result<adapters::SimulationPlan>(event.status);
          if (!plan.Ok()) {
            event.status = plan.GetStatus();
          } else {
            event.simulation_plan = std::move(plan).Value();
            auto input_hash = HashSimulationInputs(request);
            if (!input_hash.Ok()) {
              event.status = input_hash.GetStatus();
            } else {
              event.simulation_plan->input_hash = std::move(input_hash).Value();
            }
            if (event.status.message.empty() &&
                !event.simulation_plan->wrapper_path.empty()) {
              std::ofstream wrapper(event.simulation_plan->wrapper_path,
                                    std::ios::binary | std::ios::trunc);
              wrapper << event.simulation_plan->wrapper_text;
              wrapper.flush();
              if (!wrapper) {
                event.status = {core::ErrorCode::kIoError,
                                "Cannot create waveform wrapper", 0};
              }
            }
            if (event.status.message.empty()) {
              event.status = core::Status::Success();
            }
          }
          if (!event.status.Ok()) {
            std::vector<core::Diagnostic> diagnostics;
            static_cast<void>(store.Complete(event.run.get(),
                                             application::RunStatus::kFailed, 0,
                                             diagnostics));
          }
        }
        HWND target = nullptr;
        {
          std::scoped_lock lock(channel->mutex);
          if (!channel->window || channel->generation != generation) return;
          channel->events.push_back(std::move(event));
          target = channel->window;
        }
        PostMessageW(target, kEventMessage, 0, 0);
      });
  if (!accepted) {
    active_operation_ = ActiveOperation::kNone;
    cpu_lease_.reset();
    SetStatus(L"Simulation preparation queue is full");
  }
}

void VerilogWindow::PrepareCocotb(std::string tool_version,
                                  std::wstring makefiles_directory) {
  auto lease = resource_coordinator_.TryAcquireCpu(1);
  if (!lease.Ok()) {
    active_operation_ = ActiveOperation::kNone;
    MessageBoxW(window_, Utf8ToWide(lease.GetStatus().message).c_str(),
                L"Design++ CPU Quota", MB_OK | MB_ICONINFORMATION);
    UpdateInspector();
    return;
  }
  cpu_lease_ =
      std::make_unique<runtime::CpuTokenLease>(std::move(lease).Value());
  application::EditorDocumentSnapshot* active =
      FindEditorDocument(active_document_id_);
  if (active == nullptr) {
    active_operation_ = ActiveOperation::kNone;
    cpu_lease_.reset();
    return;
  }
  adapters::CocotbRequest request;
  request.simulation.project = document_->project;
  request.simulation.sources = sources_;
  request.simulation.testbench_view_id = active->view_id;
  request.simulation.testbench_relative_path = active->relative_path;
  request.simulation.testbench_top = document_->project.top_module;
  request.simulation.waveform_enabled =
      SendMessageW(waveform_checkbox_, BM_GETCHECK, 0, 0) == BST_CHECKED;
  request.simulation.waveform_format =
      request.simulation.waveform_enabled
          ? (SendMessageW(waveform_format_, CB_GETCURSEL, 0, 0) == 1 ? "fst"
                                                                     : "vcd")
          : "none";
  request.module = WideToUtf8(WindowText(cocotb_module_));
  request.testcase_filter = WideToUtf8(WindowText(cocotb_testcase_));
  request.makefiles_directory = std::move(makefiles_directory);
  request.backend = simulation_backend_id_;
  const std::filesystem::path cell_directory =
      library_.directory / L"cells" / Utf8ToWide(request_.cell_id);
  const auto channel = event_channel_;
  const std::uint64_t generation = generation_;
  const bool accepted =
      scheduler_.Submit([request = std::move(request), cell_directory,
                         tool_version = std::move(tool_version), channel,
                         generation](std::stop_token stop_token) mutable {
        if (stop_token.stop_requested()) return;
        WorkspaceEvent event;
        event.kind = WorkspaceEventKind::kSimulationPrepared;
        event.generation = generation;
        application::RunStore store;
        auto begun = store.Begin(cell_directory, request.simulation.project,
                                 "Simulation", "cocotb", tool_version);
        if (!begun.Ok()) {
          event.status = begun.GetStatus();
        } else {
          event.run = std::make_shared<application::RunRecord>(
              std::move(begun).Value());
          request.simulation.artifact_directory =
              event.run->directory / L"artifacts";
          runtime::PathMapper mapper;
          auto plan =
              adapters::CocotbRunnerAdapter().BuildPlan(request, mapper);
          if (!plan.Ok()) {
            event.status = plan.GetStatus();
          } else {
            event.cocotb_plan = std::move(plan).Value();
            auto input_hash = HashSimulationInputs(request.simulation);
            if (!input_hash.Ok()) {
              event.status = input_hash.GetStatus();
            } else {
              event.cocotb_plan->input_hash = std::move(input_hash).Value();
              event.status = core::Status::Success();
            }
          }
          if (!event.status.Ok()) {
            std::vector<core::Diagnostic> diagnostics;
            static_cast<void>(store.Complete(event.run.get(),
                                             application::RunStatus::kFailed, 0,
                                             diagnostics));
          }
        }
        HWND target = nullptr;
        {
          std::scoped_lock lock(channel->mutex);
          if (!channel->window || channel->generation != generation) return;
          channel->events.push_back(std::move(event));
          target = channel->window;
        }
        PostMessageW(target, kEventMessage, 0, 0);
      });
  if (!accepted) {
    active_operation_ = ActiveOperation::kNone;
    cpu_lease_.reset();
    SetStatus(L"cocotb preparation queue is full");
  }
}

void VerilogWindow::StartSimulationCompile() {
  if (!simulation_plan_ || !active_run_) return;
  const auto channel = event_channel_;
  const auto run = active_run_;
  const adapters::SimulationPlan plan = *simulation_plan_;
  const std::string backend = simulation_backend_id_;
  const std::uint64_t generation = generation_;
  SetWindowTextW(simulation_status_, L"Compiling Testbench...");
  AppendOutput(L"[Simulation] compile started\r\n");
  auto launch = runtime::WslExecutor::RunAsync(
      simulation_plan_->compile,
      [channel, generation, run](std::string output) {
        application::RunStore store;
        static_cast<void>(store.AppendLog(*run, output));
        WorkspaceEvent event;
        event.kind = WorkspaceEventKind::kOutput;
        event.generation = generation;
        event.output = std::move(output);
        HWND target = nullptr;
        {
          std::scoped_lock lock(channel->mutex);
          if (!channel->window || channel->generation != generation) return;
          channel->events.push_back(std::move(event));
          target = channel->window;
        }
        PostMessageW(target, kEventMessage, 0, 0);
      },
      [channel, generation, run, plan, backend](runtime::ProcessResult result) {
        WorkspaceEvent event;
        event.generation = generation;
        event.process_result = result;
        if (result.started && !result.cancelled && result.exit_code == 0) {
          event.kind = WorkspaceEventKind::kSimulationCompileComplete;
          event.status = core::Status::Success();
        } else {
          event.kind = WorkspaceEventKind::kSimulationComplete;
          std::unique_ptr<adapters::SimulationAdapter> adapter =
              adapters::CreateSimulationAdapter(backend);
          if (adapter)
            event.diagnostics = adapter->ParseDiagnostics(result.output);
          const application::RunStatus status =
              result.cancelled ? application::RunStatus::kCancelled
                               : application::RunStatus::kFailed;
          application::RunStore store;
          static_cast<void>(store.Complete(run.get(), status, result.exit_code,
                                           event.diagnostics));
          RemoveVerilatorBuildDirectory(plan, *run);
          event.run = run;
          event.status = {result.cancelled ? core::ErrorCode::kCancelled
                                           : core::ErrorCode::kIoError,
                          result.cancelled ? "Simulation cancelled"
                                           : "Simulation compilation failed",
                          0};
        }
        HWND target = nullptr;
        {
          std::scoped_lock lock(channel->mutex);
          if (!channel->window || channel->generation != generation) return;
          channel->events.push_back(std::move(event));
          target = channel->window;
        }
        PostMessageW(target, kEventMessage, 0, 0);
      });
  if (!launch.IsValid()) {
    std::vector<core::Diagnostic> diagnostics;
    static_cast<void>(run_store_.Complete(
        active_run_.get(), application::RunStatus::kFailed, 0, diagnostics));
    run_records_.insert(run_records_.begin(), *active_run_);
    PopulateRuns();
    active_operation_ = ActiveOperation::kNone;
    cpu_lease_.reset();
    MessageBoxW(window_, launch.error_message.c_str(), L"Simulation Compile",
                MB_OK | MB_ICONERROR);
    UpdateInspector();
    return;
  }
  process_ =
      std::make_unique<runtime::ProcessSession>(std::move(launch.session));
}

void VerilogWindow::StartSimulationExecute() {
  if (!simulation_plan_ || !active_run_) return;
  const auto channel = event_channel_;
  const auto run = active_run_;
  const adapters::SimulationPlan plan = *simulation_plan_;
  const std::string compile_output = simulation_output_;
  const std::string backend = simulation_backend_id_;
  const std::uint64_t generation = generation_;
  SetWindowTextW(simulation_status_, L"Running Testbench...");
  AppendOutput(L"[Simulation] execution started\r\n");
  const core::Status launch_status = testbench_execution_.Start(
      application::TestbenchOperation::kSimulation,
      application::TestbenchExecutionState::kRunning, generation, plan.execute,
      [channel, generation, run, plan, compile_output,
       backend](application::TestbenchExecutionEvent execution) {
        if (execution.kind ==
            application::TestbenchExecutionEventKind::kOutput) {
          application::RunStore store;
          static_cast<void>(store.AppendLog(*run, execution.output));
          WorkspaceEvent event;
          event.kind = WorkspaceEventKind::kOutput;
          event.generation = generation;
          event.output = std::move(execution.output);
          HWND target = nullptr;
          {
            std::scoped_lock lock(channel->mutex);
            if (!channel->window || channel->generation != generation) return;
            channel->events.push_back(std::move(event));
            target = channel->window;
          }
          PostMessageW(target, kEventMessage, 0, 0);
          return;
        }
        runtime::ProcessResult result = std::move(execution.result);
        application::RunStore store;
        WorkspaceEvent event;
        event.kind = WorkspaceEventKind::kSimulationComplete;
        event.generation = generation;
        event.process_result = result;
        std::unique_ptr<adapters::SimulationAdapter> adapter =
            adapters::CreateSimulationAdapter(backend);
        if (adapter) {
          event.diagnostics =
              adapter->ParseDiagnostics(compile_output + result.output);
        }
        std::vector<application::RunArtifact> artifacts;
        std::error_code error;
        for (std::filesystem::directory_iterator
                 iterator(plan.waveform_path.parent_path(), error),
             end;
             !error && iterator != end; iterator.increment(error)) {
          if (!iterator->is_regular_file(error) ||
              iterator->path().extension() != plan.waveform_path.extension() ||
              iterator->file_size(error) == 0) {
            continue;
          }
          const DWORD attributes = GetFileAttributesW(iterator->path().c_str());
          if (attributes == INVALID_FILE_ATTRIBUTES ||
              (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            continue;
          }
          const std::filesystem::path destination =
              run->directory / L"artifacts" / iterator->path().filename();
          std::error_code copy_error;
          if (!PathsEqual(iterator->path(), destination)) {
            std::filesystem::copy_file(
                iterator->path(), destination,
                std::filesystem::copy_options::overwrite_existing, copy_error);
          }
          if (copy_error) {
            event.diagnostics.push_back(
                {core::DiagnosticSeverity::kError, "WAVEFORM_COPY", "", 0, 0,
                 "Cannot preserve the generated waveform in the run"});
            continue;
          }
          std::string waveform_format =
              WideToUtf8(plan.waveform_path.extension().wstring());
          if (!waveform_format.empty() && waveform_format.front() == '.') {
            waveform_format.erase(waveform_format.begin());
          }
          artifacts.push_back(
              {"waveform", std::move(waveform_format),
               WideToUtf8(std::filesystem::relative(destination, run->directory)
                              .generic_wstring()),
               std::filesystem::file_size(destination, error), plan.input_hash,
               result.cancelled});
        }
        RemoveVerilatorBuildDirectory(plan, *run);
        const bool waveform_required = !plan.wrapper_path.empty();
        const bool succeeded = result.started && !result.cancelled &&
                               result.exit_code == 0 &&
                               (!waveform_required || !artifacts.empty());
        const application::RunStatus run_status =
            result.cancelled ? application::RunStatus::kCancelled
            : succeeded      ? application::RunStatus::kSucceeded
                             : application::RunStatus::kFailed;
        const core::Status complete =
            store.Complete(run.get(), run_status, result.exit_code,
                           event.diagnostics, std::move(artifacts));
        event.run = run;
        event.status =
            !complete.Ok() ? complete
            : succeeded
                ? core::Status::Success()
                : core::Status{result.cancelled    ? core::ErrorCode::kCancelled
                               : waveform_required ? core::ErrorCode::kNotFound
                                                   : core::ErrorCode::kIoError,
                               result.cancelled ? "Simulation cancelled"
                               : waveform_required
                                   ? "Simulation did not produce its waveform"
                                   : "Testbench execution failed",
                               0};
        HWND target = nullptr;
        {
          std::scoped_lock lock(channel->mutex);
          if (!channel->window || channel->generation != generation) return;
          channel->events.push_back(std::move(event));
          target = channel->window;
        }
        PostMessageW(target, kEventMessage, 0, 0);
      });
  if (!launch_status.Ok()) {
    std::vector<core::Diagnostic> diagnostics;
    static_cast<void>(run_store_.Complete(
        active_run_.get(), application::RunStatus::kFailed, 0, diagnostics));
    run_records_.insert(run_records_.begin(), *active_run_);
    PopulateRuns();
    active_operation_ = ActiveOperation::kNone;
    cpu_lease_.reset();
    MessageBoxW(window_, Utf8ToWide(launch_status.message).c_str(),
                L"Simulation Runtime", MB_OK | MB_ICONERROR);
    UpdateInspector();
    return;
  }
}

void VerilogWindow::StartCocotbExecute() {
  if (!cocotb_plan_ || !active_run_) return;
  const auto channel = event_channel_;
  const auto run = active_run_;
  const adapters::CocotbPlan plan = *cocotb_plan_;
  const std::uint64_t generation = generation_;
  SetWindowTextW(simulation_status_, L"Running cocotb tests...");
  AppendOutput(L"[cocotb] execution started\r\n");
  const core::Status launch_status = testbench_execution_.Start(
      application::TestbenchOperation::kSimulation,
      application::TestbenchExecutionState::kRunning, generation, plan.execute,
      [channel, generation, run,
       plan](application::TestbenchExecutionEvent execution) {
        if (execution.kind ==
            application::TestbenchExecutionEventKind::kOutput) {
          application::RunStore store;
          static_cast<void>(store.AppendLog(*run, execution.output));
          WorkspaceEvent event;
          event.kind = WorkspaceEventKind::kOutput;
          event.generation = generation;
          event.output = std::move(execution.output);
          HWND target = nullptr;
          {
            std::scoped_lock lock(channel->mutex);
            if (!channel->window || channel->generation != generation) return;
            channel->events.push_back(std::move(event));
            target = channel->window;
          }
          PostMessageW(target, kEventMessage, 0, 0);
          return;
        }
        runtime::ProcessResult result = std::move(execution.result);
        WorkspaceEvent event;
        event.kind = WorkspaceEventKind::kSimulationComplete;
        event.generation = generation;
        event.process_result = result;
        application::CocotbRunFinalization finalization =
            application::FinalizeCocotbRun(run.get(), plan, result);
        event.simulation_summary = std::move(finalization.summary);
        event.diagnostics = std::move(finalization.diagnostics);
        event.run = run;
        event.status = std::move(finalization.status);
        HWND target = nullptr;
        {
          std::scoped_lock lock(channel->mutex);
          if (!channel->window || channel->generation != generation) return;
          channel->events.push_back(std::move(event));
          target = channel->window;
        }
        PostMessageW(target, kEventMessage, 0, 0);
      });
  if (!launch_status.Ok()) {
    std::vector<core::Diagnostic> diagnostics;
    static_cast<void>(run_store_.Complete(
        active_run_.get(), application::RunStatus::kFailed, 0, diagnostics));
    run_records_.insert(run_records_.begin(), *active_run_);
    PopulateRuns();
    active_operation_ = ActiveOperation::kNone;
    cpu_lease_.reset();
    MessageBoxW(window_, Utf8ToWide(launch_status.message).c_str(),
                L"cocotb Runtime", MB_OK | MB_ICONERROR);
    UpdateInspector();
  }
}

void VerilogWindow::FinishSimulation(
    const runtime::ProcessResult& result, core::Status status,
    std::vector<core::Diagnostic> diagnostics,
    std::shared_ptr<application::RunRecord> run) {
  active_operation_ = ActiveOperation::kNone;
  cpu_lease_.reset();
  simulation_plan_.reset();
  cocotb_plan_.reset();
  diagnostics_ = std::move(diagnostics);
  if (run) {
    active_run_ = std::move(run);
    if (simulation_summary_) {
      run_test_summaries_[active_run_->id] = *simulation_summary_;
    }
    run_records_.insert(run_records_.begin(), *active_run_);
    PopulateArtifacts(active_run_.get());
  }
  PopulateTestSummary();
  PopulateProblems();
  PopulateRuns();
  editor_host_.SetDiagnostics(diagnostics_, editor_documents_);
  const auto elapsed = std::chrono::steady_clock::now() - simulation_started_;
  const auto elapsed_milliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
  const std::wstring elapsed_text =
      std::to_wstring(elapsed_milliseconds / 1000.0) + L" s";
  const std::wstring message =
      status.Ok() ? L"Simulation succeeded (" + elapsed_text + L")"
                  : Utf8ToWide(status.message) + L" (" + elapsed_text + L")";
  SetWindowTextW(simulation_status_, message.c_str());
  SetStatus(message);
  if (!status.Ok() && !result.cancelled) {
    AppendOutput(L"[Simulation] " + message + L"\r\n");
  }
  if (simulation_summary_) {
    const auto& summary = *simulation_summary_;
    AppendOutput(L"[cocotb] tests=" + std::to_wstring(summary.total) +
                 L" passed=" + std::to_wstring(summary.passed) + L" failed=" +
                 std::to_wstring(summary.failed) + L" errors=" +
                 std::to_wstring(summary.errors) + L" skipped=" +
                 std::to_wstring(summary.skipped) + L"\r\n");
    for (const auto& test_case : summary.cases) {
      const wchar_t* state =
          test_case.status == adapters::SimulationTestStatus::kPassed ? L"PASS"
          : test_case.status == adapters::SimulationTestStatus::kSkipped
              ? L"SKIP"
              : L"FAIL";
      AppendOutput(L"[cocotb] " + std::wstring(state) + L" " +
                   Utf8ToWide(test_case.suite) + L"." +
                   Utf8ToWide(test_case.name) + L"\r\n");
    }
  }
  UpdateInspector();
}

void VerilogWindow::StartDebug() {
  if (!document_ || active_operation_ != ActiveOperation::kNone) return;
  application::EditorDocumentSnapshot* active =
      FindEditorDocument(active_document_id_);
  const core::View* view =
      active == nullptr
          ? nullptr
          : FindView(FindCell(library_, request_.cell_id), active->view_id);
  if (active == nullptr || view == nullptr ||
      view->kind != core::ViewKind::kTestbench || active->missing) {
    MessageBoxW(window_, L"디버깅할 Testbench 파일을 활성화하세요.",
                L"Icarus Debugger", MB_OK | MB_ICONINFORMATION);
    return;
  }
  ReadTestbenchControls();
  if (!dirty_document_ids_.empty() || dirty_) {
    const int choice =
        MessageBoxW(window_, L"디버깅 전에 모든 변경 사항을 저장하시겠습니까?",
                    L"Design++ Workspace", MB_OKCANCEL | MB_ICONQUESTION);
    if (choice != IDOK) return;
    if (!dirty_document_ids_.empty()) {
      pending_run_ = PendingRun::kDebug;
      save_project_after_documents_ = true;
      RequestSaveAll();
      return;
    }
    if (dirty_ && !SaveProject()) return;
  }
  adapters::SimulationRequest request;
  request.project = document_->project;
  request.sources = sources_;
  request.testbench_view_id = active->view_id;
  request.testbench_relative_path = active->relative_path;
  request.testbench_top = WideToUtf8(WindowText(testbench_top_));
  request.waveform_enabled =
      SendMessageW(waveform_checkbox_, BM_GETCHECK, 0, 0) == BST_CHECKED;
  request.waveform_format = request.waveform_enabled ? "vcd" : "none";
  request.artifact_directory = library_.directory / L"cells" /
                               Utf8ToWide(request_.cell_id) / L".designpp" /
                               L"runs" / L"pending" / L"artifacts";
  adapters::IcarusSimulationAdapter simulation_adapter;
  const core::Status validation = simulation_adapter.Validate(request);
  if (!validation.Ok()) {
    MessageBoxW(window_, Utf8ToWide(validation.message).c_str(),
                L"Icarus Debugger", MB_OK | MB_ICONERROR);
    return;
  }

  active_operation_ = ActiveOperation::kDebug;
  debug_session_.BeginProbe();
  debug_command_queue_.clear();
  debug_command_pending_ = false;
  debug_values_.clear();
  SetWindowTextW(debug_transcript_, L"");
  TabCtrl_SetCurSel(bottom_tabs_, 5);
  ShowBottomPage(5);
  PopulateDebugView();
  UpdateInspector();
  const auto channel = event_channel_;
  const std::uint64_t generation = generation_;
  auto launch = runtime::WslExecutor::RunAsync(
      simulation_adapter.BuildProbeCommand(),
      [channel, generation](std::string output) {
        WorkspaceEvent event;
        event.kind = WorkspaceEventKind::kOutput;
        event.generation = generation;
        event.output = std::move(output);
        HWND target = nullptr;
        {
          std::scoped_lock lock(channel->mutex);
          if (!channel->window || channel->generation != generation) return;
          channel->events.push_back(std::move(event));
          target = channel->window;
        }
        PostMessageW(target, kEventMessage, 0, 0);
      },
      [channel, generation](runtime::ProcessResult result) {
        WorkspaceEvent event;
        event.kind = WorkspaceEventKind::kDebugProbeComplete;
        event.generation = generation;
        event.process_result = std::move(result);
        HWND target = nullptr;
        {
          std::scoped_lock lock(channel->mutex);
          if (!channel->window || channel->generation != generation) return;
          channel->events.push_back(std::move(event));
          target = channel->window;
        }
        PostMessageW(target, kEventMessage, 0, 0);
      });
  if (!launch.IsValid()) {
    active_operation_ = ActiveOperation::kNone;
    debug_session_.Complete(false, false);
    MessageBoxW(window_, launch.error_message.c_str(), L"Icarus Debug Probe",
                MB_OK | MB_ICONERROR);
    UpdateInspector();
    PopulateDebugView();
    return;
  }
  process_ =
      std::make_unique<runtime::ProcessSession>(std::move(launch.session));
}

void VerilogWindow::HandleDebugProbeComplete(
    const runtime::ProcessResult& result) {
  if (!result.started || result.cancelled || result.exit_code != 0) {
    active_operation_ = ActiveOperation::kNone;
    debug_session_.Complete(result.cancelled, false);
    SetStatus(result.cancelled ? L"Debug cancelled"
                               : L"Icarus Verilog unavailable");
    UpdateInspector();
    PopulateDebugView();
    return;
  }
  std::string version = result.output;
  const std::size_t newline = version.find_first_of("\r\n");
  if (newline != std::string::npos) version.resize(newline);
  PrepareDebug(std::move(version));
}

void VerilogWindow::PrepareDebug(std::string tool_version) {
  auto lease = resource_coordinator_.TryAcquireCpu(1);
  if (!lease.Ok()) {
    active_operation_ = ActiveOperation::kNone;
    debug_session_.Complete(false, false);
    SetStatus(Utf8ToWide(lease.GetStatus().message));
    UpdateInspector();
    PopulateDebugView();
    return;
  }
  cpu_lease_ =
      std::make_unique<runtime::CpuTokenLease>(std::move(lease).Value());
  application::EditorDocumentSnapshot* active =
      FindEditorDocument(active_document_id_);
  if (active == nullptr) {
    active_operation_ = ActiveOperation::kNone;
    cpu_lease_.reset();
    return;
  }
  adapters::SimulationRequest request;
  request.project = document_->project;
  request.sources = sources_;
  request.testbench_view_id = active->view_id;
  request.testbench_relative_path = active->relative_path;
  request.testbench_top = WideToUtf8(WindowText(testbench_top_));
  request.waveform_enabled =
      SendMessageW(waveform_checkbox_, BM_GETCHECK, 0, 0) == BST_CHECKED;
  request.waveform_format = request.waveform_enabled ? "vcd" : "none";
  const std::filesystem::path cell_directory =
      library_.directory / L"cells" / Utf8ToWide(request_.cell_id);
  const auto channel = event_channel_;
  const std::uint64_t generation = generation_;
  debug_session_.MarkCompiling();
  const bool accepted =
      scheduler_.Submit([request = std::move(request), cell_directory,
                         tool_version = std::move(tool_version), channel,
                         generation](std::stop_token stop_token) mutable {
        if (stop_token.stop_requested()) return;
        WorkspaceEvent event;
        event.kind = WorkspaceEventKind::kDebugPrepared;
        event.generation = generation;
        application::RunStore store;
        auto begun = store.Begin(cell_directory, request.project, "Debug",
                                 "Icarus Verilog", tool_version);
        if (!begun.Ok()) {
          event.status = begun.GetStatus();
        } else {
          event.run = std::make_shared<application::RunRecord>(
              std::move(begun).Value());
          request.artifact_directory = event.run->directory / L"artifacts";
          adapters::IcarusDebugAdapter adapter;
          runtime::PathMapper mapper;
          auto plan = adapter.BuildDebugPlan(request, mapper);
          if (!plan.Ok()) {
            event.status = plan.GetStatus();
          } else {
            event.debug_plan = std::move(plan).Value();
            auto input_hash = HashSimulationInputs(request);
            if (!input_hash.Ok()) {
              event.status = input_hash.GetStatus();
            } else {
              event.debug_plan->simulation.input_hash =
                  std::move(input_hash).Value();
            }
            if (event.status.message.empty() &&
                !event.debug_plan->simulation.wrapper_path.empty()) {
              std::ofstream wrapper(event.debug_plan->simulation.wrapper_path,
                                    std::ios::binary | std::ios::trunc);
              wrapper << event.debug_plan->simulation.wrapper_text;
              wrapper.flush();
              if (!wrapper) {
                event.status = {core::ErrorCode::kIoError,
                                "Cannot create debug waveform wrapper", 0};
              }
            }
            if (event.status.message.empty()) {
              event.status = core::Status::Success();
            }
          }
          if (!event.status.Ok()) {
            std::vector<core::Diagnostic> diagnostics;
            static_cast<void>(store.Complete(event.run.get(),
                                             application::RunStatus::kFailed, 0,
                                             diagnostics));
          }
        }
        HWND target = nullptr;
        {
          std::scoped_lock lock(channel->mutex);
          if (!channel->window || channel->generation != generation) return;
          channel->events.push_back(std::move(event));
          target = channel->window;
        }
        PostMessageW(target, kEventMessage, 0, 0);
      });
  if (!accepted) {
    active_operation_ = ActiveOperation::kNone;
    cpu_lease_.reset();
    debug_session_.Complete(false, false);
    SetStatus(L"Debug preparation queue is full");
  }
  PopulateDebugView();
}

void VerilogWindow::StartDebugCompile() {
  if (!debug_plan_ || !active_run_) return;
  debug_session_.MarkCompiling();
  PopulateDebugView();
  const auto channel = event_channel_;
  const auto run = active_run_;
  const std::uint64_t generation = generation_;
  auto launch = runtime::WslExecutor::RunAsync(
      debug_plan_->simulation.compile,
      [channel, generation, run](std::string output) {
        application::RunStore store;
        static_cast<void>(store.AppendLog(*run, output));
        WorkspaceEvent event;
        event.kind = WorkspaceEventKind::kOutput;
        event.generation = generation;
        event.output = std::move(output);
        HWND target = nullptr;
        {
          std::scoped_lock lock(channel->mutex);
          if (!channel->window || channel->generation != generation) return;
          channel->events.push_back(std::move(event));
          target = channel->window;
        }
        PostMessageW(target, kEventMessage, 0, 0);
      },
      [channel, generation, run](runtime::ProcessResult result) {
        WorkspaceEvent event;
        event.generation = generation;
        event.process_result = result;
        if (result.started && !result.cancelled && result.exit_code == 0) {
          event.kind = WorkspaceEventKind::kDebugCompileComplete;
          event.status = core::Status::Success();
        } else {
          event.kind = WorkspaceEventKind::kDebugComplete;
          adapters::IcarusSimulationAdapter adapter;
          event.diagnostics = adapter.ParseDiagnostics(result.output);
          application::RunStore store;
          static_cast<void>(store.Complete(
              run.get(),
              result.cancelled ? application::RunStatus::kCancelled
                               : application::RunStatus::kFailed,
              result.exit_code, event.diagnostics));
          event.run = run;
        }
        HWND target = nullptr;
        {
          std::scoped_lock lock(channel->mutex);
          if (!channel->window || channel->generation != generation) return;
          channel->events.push_back(std::move(event));
          target = channel->window;
        }
        PostMessageW(target, kEventMessage, 0, 0);
      });
  if (!launch.IsValid()) {
    std::vector<core::Diagnostic> diagnostics;
    static_cast<void>(run_store_.Complete(
        active_run_.get(), application::RunStatus::kFailed, 0, diagnostics));
    runtime::ProcessResult result;
    FinishDebug(result, active_run_);
    return;
  }
  process_ =
      std::make_unique<runtime::ProcessSession>(std::move(launch.session));
}

void VerilogWindow::StartDebugExecute() {
  if (!debug_plan_ || !active_run_) return;
  debug_parser_ = adapters::IcarusDebugProtocolParser();
  debug_session_.MarkStarting();
  PopulateDebugView();
  const auto channel = event_channel_;
  const auto run = active_run_;
  const adapters::SimulationPlan plan = debug_plan_->simulation;
  const std::string compile_output = simulation_output_;
  const std::uint64_t generation = generation_;
  const core::Status launch_status = testbench_execution_.Start(
      application::TestbenchOperation::kDebug,
      application::TestbenchExecutionState::kRunning, generation, plan.execute,
      [channel, generation, run, plan,
       compile_output](application::TestbenchExecutionEvent execution) {
        if (execution.kind ==
            application::TestbenchExecutionEventKind::kOutput) {
          application::RunStore store;
          static_cast<void>(store.AppendLog(*run, execution.output));
          WorkspaceEvent event;
          event.kind = WorkspaceEventKind::kDebugOutput;
          event.generation = generation;
          event.output = std::move(execution.output);
          HWND target = nullptr;
          {
            std::scoped_lock lock(channel->mutex);
            if (!channel->window || channel->generation != generation) return;
            channel->events.push_back(std::move(event));
            target = channel->window;
          }
          PostMessageW(target, kEventMessage, 0, 0);
          return;
        }
        runtime::ProcessResult result = std::move(execution.result);
        application::RunStore store;
        WorkspaceEvent event;
        event.kind = WorkspaceEventKind::kDebugComplete;
        event.generation = generation;
        event.process_result = result;
        adapters::IcarusSimulationAdapter adapter;
        event.diagnostics =
            adapter.ParseDiagnostics(compile_output + result.output);
        std::vector<application::RunArtifact> artifacts;
        std::error_code error;
        for (std::filesystem::directory_iterator
                 iterator(plan.waveform_path.parent_path(), error),
             end;
             !error && iterator != end; iterator.increment(error)) {
          if (!iterator->is_regular_file(error) ||
              iterator->path().extension() != L".vcd" ||
              iterator->file_size(error) == 0) {
            continue;
          }
          const DWORD attributes = GetFileAttributesW(iterator->path().c_str());
          if (attributes == INVALID_FILE_ATTRIBUTES ||
              (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            continue;
          }
          artifacts.push_back({"waveform", "vcd",
                               WideToUtf8(std::filesystem::relative(
                                              iterator->path(), run->directory)
                                              .generic_wstring()),
                               iterator->file_size(error), plan.input_hash});
        }
        for (application::RunArtifact& artifact : artifacts) {
          artifact.partial = result.cancelled;
        }
        const bool waveform_required = !plan.wrapper_path.empty();
        const bool succeeded = result.started && !result.cancelled &&
                               result.exit_code == 0 &&
                               (!waveform_required || !artifacts.empty());
        static_cast<void>(store.Complete(
            run.get(),
            result.cancelled ? application::RunStatus::kCancelled
            : succeeded      ? application::RunStatus::kSucceeded
                             : application::RunStatus::kFailed,
            result.exit_code, event.diagnostics, std::move(artifacts)));
        event.run = run;
        HWND target = nullptr;
        {
          std::scoped_lock lock(channel->mutex);
          if (!channel->window || channel->generation != generation) return;
          channel->events.push_back(std::move(event));
          target = channel->window;
        }
        PostMessageW(target, kEventMessage, 0, 0);
      });
  if (!launch_status.Ok()) {
    std::vector<core::Diagnostic> diagnostics;
    static_cast<void>(run_store_.Complete(
        active_run_.get(), application::RunStatus::kFailed, 0, diagnostics));
    runtime::ProcessResult result;
    FinishDebug(result, active_run_);
    return;
  }
}

void VerilogWindow::HandleDebugOutput(std::string_view output) {
  if (debug_transcript_ != nullptr) {
    const std::wstring text = Utf8ToWide(output);
    const int length = GetWindowTextLengthW(debug_transcript_);
    SendMessageW(debug_transcript_, EM_SETSEL, length, length);
    SendMessageW(debug_transcript_, EM_REPLACESEL, FALSE,
                 reinterpret_cast<LPARAM>(text.c_str()));
  }
  const std::vector<adapters::DebugProtocolEvent> events =
      debug_parser_.Feed(output);
  if (events.empty()) return;
  bool stopped = false;
  bool prompt = false;
  for (const adapters::DebugProtocolEvent& event : events) {
    stopped |= event.kind == adapters::DebugProtocolEventKind::kStopped;
    prompt |= event.kind == adapters::DebugProtocolEventKind::kPromptReady;
    if (event.kind == adapters::DebugProtocolEventKind::kEvaluatedValue &&
        !debug_evaluation_target_.empty()) {
      debug_values_[debug_evaluation_target_] = event.text;
      debug_evaluation_target_.clear();
    }
    if (event.kind == adapters::DebugProtocolEventKind::kSourceStopLocation &&
        event.line > 0) {
      core::Diagnostic location;
      location.severity = core::DiagnosticSeverity::kInfo;
      location.code = "VVP-STOP";
      location.file = event.file;
      const std::string stop_filename = WideToUtf8(
          std::filesystem::path(Utf8ToWide(event.file)).filename().wstring());
      const auto managed_source = std::find_if(
          sources_.begin(), sources_.end(), [&](const auto& source) {
            return source.relative_path == event.file ||
                   WideToUtf8(source.windows_path.filename().wstring()) ==
                       stop_filename;
          });
      if (managed_source != sources_.end()) {
        location.file = managed_source->relative_path;
      }
      location.line = event.line;
      location.column = 1;
      location.message = "VVP stopped here";
      std::erase_if(diagnostics_, [](const core::Diagnostic& diagnostic) {
        return diagnostic.code == "VVP-STOP";
      });
      diagnostics_.push_back(location);
      PopulateProblems();
      editor_host_.SetDiagnostics(diagnostics_, editor_documents_);
      OpenSourceByDiagnostic(location);
    }
  }
  debug_session_.Apply(events);
  if (prompt) debug_command_pending_ = false;
  if (stopped) {
    debug_command_queue_.push_back(
        {application::DebugCommandKind::kConsole, "time"});
    debug_command_queue_.push_back(
        {application::DebugCommandKind::kConsole, "where"});
    debug_command_queue_.push_back(
        {application::DebugCommandKind::kConsole, "list"});
  }
  PopulateDebugView();
  DispatchNextDebugCommand();
}

void VerilogWindow::QueueDebugCommand(application::DebugCommandKind kind,
                                      std::string command) {
  if (active_operation_ != ActiveOperation::kDebug) return;
  debug_command_queue_.push_back({kind, std::move(command)});
  DispatchNextDebugCommand();
}

void VerilogWindow::DispatchNextDebugCommand() {
  if (!testbench_execution_.Snapshot().active || debug_command_pending_ ||
      debug_command_queue_.empty() || !debug_session_.Snapshot().prompt_ready) {
    PopulateDebugView();
    return;
  }
  PendingDebugCommand pending = std::move(debug_command_queue_.front());
  debug_command_queue_.pop_front();
  auto prepared = debug_session_.PrepareCommand(pending.kind, pending.command);
  if (!prepared.Ok()) {
    SetStatus(Utf8ToWide(prepared.GetStatus().message));
    debug_command_queue_.clear();
    PopulateDebugView();
    return;
  }
  std::string command = std::move(prepared).Value();
  adapters::IcarusDebugAdapter adapter;
  const core::Status validation = adapter.ValidateConsoleCommand(command);
  if (!validation.Ok()) {
    SetStatus(Utf8ToWide(validation.message));
    PopulateDebugView();
    return;
  }
  if (command.starts_with("$display ")) {
    debug_evaluation_target_ = command.substr(9);
  }
  debug_parser_.SetPendingCommand(command);
  const runtime::InputWriteResult written =
      testbench_execution_.WriteInput(command + "\n");
  if (written != runtime::InputWriteResult::kAccepted) {
    SetStatus(L"Unable to queue the VVP debug command");
    CancelActiveOperation();
    return;
  }
  debug_command_pending_ = true;
  SetWindowTextW(debug_command_, L"");
  PopulateDebugView();
}

void VerilogWindow::FinishDebug(const runtime::ProcessResult& result,
                                std::shared_ptr<application::RunRecord> run) {
  const std::vector<adapters::DebugProtocolEvent> remaining =
      debug_parser_.Finish();
  debug_session_.Apply(remaining);
  const bool succeeded =
      run && run->status == application::RunStatus::kSucceeded;
  debug_session_.Complete(result.cancelled, succeeded);
  active_operation_ = ActiveOperation::kNone;
  cpu_lease_.reset();
  debug_plan_.reset();
  debug_command_queue_.clear();
  debug_command_pending_ = false;
  if (!run) diagnostics_.clear();
  if (run) {
    active_run_ = std::move(run);
    run_records_.insert(run_records_.begin(), *active_run_);
    PopulateArtifacts(active_run_.get());
  }
  PopulateRuns();
  PopulateProblems();
  editor_host_.SetDiagnostics(diagnostics_, editor_documents_);
  PopulateDebugView();
  UpdateInspector();
  SetStatus(result.cancelled ? L"Debug cancelled"
            : succeeded      ? L"Debug completed"
                             : L"Debug failed");
}

void VerilogWindow::PopulateDebugView() {
  if (debug_state_label_ == nullptr) return;
  if (common_control_notification_depth_ > 0) {
    // TreeView and ListView notifications retain references to their current
    // items until the parent callback returns. Deleting those items here would
    // make comctl32 resume the notification with freed internal nodes.
    if (!debug_view_rebuild_pending_) {
      debug_view_rebuild_pending_ = true;
      if (!PostMessageW(window_, kDebugViewRebuildMessage, 0, 0)) {
        debug_view_rebuild_pending_ = false;
      }
    }
    return;
  }
  const application::DebugSnapshot& snapshot = debug_session_.Snapshot();
  const std::wstring state =
      L"Debugger: " + Utf8ToWide(application::DebugStateName(snapshot.state)) +
      L"   Time: " +
      Utf8ToWide(snapshot.simulation_time.empty() ? "-"
                                                  : snapshot.simulation_time) +
      L"   Scope: " +
      Utf8ToWide(snapshot.scope.empty() ? "$root" : snapshot.scope);
  SetWindowTextW(debug_state_label_, state.c_str());
  TreeView_DeleteAllItems(debug_scope_tree_);
  TVINSERTSTRUCTW root{};
  root.hParent = TVI_ROOT;
  root.hInsertAfter = TVI_LAST;
  root.item.mask = TVIF_TEXT;
  std::wstring root_name =
      Utf8ToWide(snapshot.scope.empty() ? "$root" : snapshot.scope);
  root.item.pszText = root_name.data();
  const HTREEITEM root_item = TreeView_InsertItem(debug_scope_tree_, &root);
  if (!snapshot.scope.empty()) {
    TVINSERTSTRUCTW parent{};
    parent.hParent = root_item;
    parent.hInsertAfter = TVI_LAST;
    parent.item.mask = TVIF_TEXT;
    std::wstring up = L"..";
    parent.item.pszText = up.data();
    TreeView_InsertItem(debug_scope_tree_, &parent);
  }
  for (const adapters::DebugScopeItem& item : snapshot.scope_items) {
    if (item.kind != "module" && item.kind != "package") continue;
    TVINSERTSTRUCTW child{};
    child.hParent = root_item;
    child.hInsertAfter = TVI_LAST;
    child.item.mask = TVIF_TEXT;
    std::wstring name = Utf8ToWide(item.name);
    child.item.pszText = name.data();
    TreeView_InsertItem(debug_scope_tree_, &child);
  }
  TreeView_Expand(debug_scope_tree_, root_item, TVE_EXPAND);

  ListView_DeleteAllItems(debug_variables_);
  debug_variable_names_.clear();
  int row = 0;
  for (const adapters::DebugScopeItem& item : snapshot.scope_items) {
    if (item.kind == "module" || item.kind == "package") continue;
    debug_variable_names_.push_back(item.name);
    std::wstring name = Utf8ToWide(item.name);
    LVITEMW list_item{};
    list_item.mask = LVIF_TEXT;
    list_item.iItem = row;
    list_item.pszText = name.data();
    ListView_InsertItem(debug_variables_, &list_item);
    std::wstring kind = Utf8ToWide(item.kind);
    std::wstring value = Utf8ToWide(debug_values_[item.name]);
    ListView_SetItemText(debug_variables_, row, 1, kind.data());
    ListView_SetItemText(debug_variables_, row, 2, value.data());
    ++row;
  }
  const bool paused = active_operation_ == ActiveOperation::kDebug &&
                      snapshot.state == application::DebugState::kPaused &&
                      snapshot.prompt_ready && !debug_command_pending_ &&
                      debug_command_queue_.empty();
  EnableWindow(debug_continue_button_, paused);
  EnableWindow(debug_step_button_, paused);
  EnableWindow(debug_finish_button_, paused);
  EnableWindow(debug_command_, paused);
  EnableWindow(debug_send_button_, paused);
}

void VerilogWindow::OpenWaveform() {
  if (last_waveform_.empty() || viewer_execution_.IsActive()) return;
  const DWORD attributes = GetFileAttributesW(last_waveform_.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES ||
      (attributes &
       (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
    MessageBoxW(window_, L"Waveform artifact를 찾을 수 없습니다.", L"GTKWave",
                MB_OK | MB_ICONERROR);
    return;
  }
  runtime::PathMapper mapper;
  auto mapped = mapper.WindowsToWsl(last_waveform_);
  if (!mapped.Ok()) {
    MessageBoxW(window_, Utf8ToWide(mapped.GetStatus().message).c_str(),
                L"GTKWave", MB_OK | MB_ICONERROR);
    return;
  }
  auto lease = resource_coordinator_.TryAcquireCpu(1);
  if (!lease.Ok()) {
    MessageBoxW(window_, Utf8ToWide(lease.GetStatus().message).c_str(),
                L"GTKWave", MB_OK | MB_ICONINFORMATION);
    return;
  }
  viewer_cpu_lease_ =
      std::make_unique<runtime::CpuTokenLease>(std::move(lease).Value());
  runtime::WslCommand command;
  command.program = L"gtkwave";
  command.arguments.push_back(mapped.Value());
  const auto channel = event_channel_;
  const std::uint64_t generation = generation_;
  const core::Status started = viewer_execution_.Start(
      command,
      [channel, generation](std::string output) {
        WorkspaceEvent event;
        event.kind = WorkspaceEventKind::kOutput;
        event.generation = generation;
        event.output = std::move(output);
        HWND target = nullptr;
        {
          std::scoped_lock lock(channel->mutex);
          if (!channel->window || channel->generation != generation) return;
          channel->events.push_back(std::move(event));
          target = channel->window;
        }
        PostMessageW(target, kEventMessage, 0, 0);
      },
      [channel, generation](application::WslGuiExecutionResult result) {
        WorkspaceEvent event;
        event.kind = WorkspaceEventKind::kViewerComplete;
        event.generation = generation;
        event.status = std::move(result.status);
        event.process_result = std::move(result.process_result);
        HWND target = nullptr;
        {
          std::scoped_lock lock(channel->mutex);
          if (!channel->window || channel->generation != generation) return;
          channel->events.push_back(std::move(event));
          target = channel->window;
        }
        PostMessageW(target, kEventMessage, 0, 0);
      });
  if (!started.Ok()) {
    viewer_cpu_lease_.reset();
    MessageBoxW(window_, Utf8ToWide(started.message).c_str(), L"GTKWave",
                MB_OK | MB_ICONERROR);
    return;
  }
}

void VerilogWindow::CancelActiveOperation() {
  if (testbench_execution_.Snapshot().active) {
    testbench_execution_.Cancel();
  } else if (process_) {
    process_->Cancel();
  }
  SetStatus(L"Cancelling operation...");
}

void VerilogWindow::AppendOutput(std::wstring_view text) {
  if (!output_ || text.empty()) return;
  std::wstring normalized(text);
  int length = GetWindowTextLengthW(output_);
  if (length + static_cast<int>(normalized.size()) > kMaximumOutputCharacters) {
    const int remove = std::min(length, kMaximumOutputCharacters / 10);
    SendMessageW(output_, EM_SETSEL, 0, remove);
    SendMessageW(output_, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(L""));
    length = GetWindowTextLengthW(output_);
  }
  SendMessageW(output_, EM_SETSEL, length, length);
  SendMessageW(output_, EM_REPLACESEL, FALSE,
               reinterpret_cast<LPARAM>(normalized.c_str()));
  SendMessageW(output_, EM_SCROLLCARET, 0, 0);
  if (central_log_) {
    const core::Cell* cell = FindCell(library_, request_.cell_id);
    const std::wstring prefix =
        cell ? L"[Workspace " + Utf8ToWide(cell->name) + L"] "
             : L"[Workspace] ";
    central_log_(prefix + normalized);
  }
}

void VerilogWindow::SetStatus(std::wstring_view text) const {
  if (!status_) return;
  const std::wstring value(text);
  SendMessageW(status_, SB_SETTEXTW, 0,
               reinterpret_cast<LPARAM>(value.c_str()));
}

void VerilogWindow::UpdateTitle() {
  const core::Cell* cell = FindCell(library_, request_.cell_id);
  std::wstring title = L"Design++ Verilog — ";
  title += cell ? Utf8ToWide(cell->name) : L"Unknown Cell";
  if (document_ && document_->read_only) title += L" [Read-only]";
  if (dirty_ || !dirty_document_ids_.empty()) title += L" *";
  SetWindowTextW(window_, title.c_str());
}

void VerilogWindow::ShutdownChannel() {
  if (!event_channel_) return;
  std::scoped_lock lock(event_channel_->mutex);
  event_channel_->window = nullptr;
  ++event_channel_->generation;
  event_channel_->events.clear();
}

}  // namespace designpp::gui

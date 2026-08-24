// Copyright 2026 The Design++ Authors

#include "designpp/gui/synthesis_window.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace designpp::gui {
namespace {

constexpr wchar_t kSynthesisWindowClassName[] =
    L"DesignPlusPlus.SynthesisWindow";
constexpr int kRunButtonId = 6101;
constexpr int kCancelButtonId = 6102;
constexpr int kReportsButtonId = 6103;
constexpr int kArtifactsButtonId = 6104;
constexpr int kScriptButtonId = 6105;
constexpr int kViewSelectorId = 6106;
constexpr int kMaximumOutputCharacters = 2'000'000;

enum class WindowEventKind {
  kLoaded,
  kSynthesis,
  kScene,
  kModuleLoaded,
};

struct WindowEvent {
  WindowEventKind kind = WindowEventKind::kLoaded;
  std::uint64_t generation = 0;
  core::Status status;
  std::unique_ptr<application::ProjectDocument> document;
  std::vector<application::ResolvedSource> sources;
  std::vector<application::RunRecord> runs;
  std::shared_ptr<application::RunRecord> active_run;
  adapters::SynthesisMetrics metrics;
  core::SchematicModel gate_schematic;
  core::SchematicModel readable_schematic;
  core::Status readable_schematic_status;
  core::SchematicModel module_schematic;
  SchematicViewMode module_mode = SchematicViewMode::kReadable;
  std::string previous_module;
  bool add_module_history = false;
  std::uint64_t module_load_generation = 0;
  std::string script_text;
  application::SynthesisRunEvent synthesis;
  SchematicBuildEvent scene;
};

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

const core::Cell* FindCell(const application::LibraryRecord& library,
                           std::string_view cell_id) {
  const auto cell = std::find_if(
      library.library.cells.begin(), library.library.cells.end(),
      [cell_id](const core::Cell& value) { return value.id == cell_id; });
  return cell == library.library.cells.end() ? nullptr : &*cell;
}

const core::View* FindView(const core::Cell* cell, std::string_view view_id) {
  if (cell == nullptr) return nullptr;
  const auto view = std::find_if(
      cell->views.begin(), cell->views.end(),
      [view_id](const core::View& value) { return value.id == view_id; });
  return view == cell->views.end() ? nullptr : &*view;
}

}  // namespace

struct SynthesisWindow::EventChannel final {
  std::mutex mutex;
  HWND window = nullptr;
  std::uint64_t generation = 0;
  std::deque<WindowEvent> events;
};

SynthesisWindow::~SynthesisWindow() {
  synthesis_service_.Shutdown();
  schematic_build_service_.Shutdown();
  scheduler_.RequestStop();
  ShutdownChannel();
  Close();
}

bool SynthesisWindow::Create(HINSTANCE instance,
                             const application::WorkspaceOpenRequest& request,
                             application::LibraryRecord library,
                             ViewWindowLogCallback central_log,
                             ViewWindowLibraryChangedCallback library_changed) {
  instance_ = instance;
  request_ = request;
  library_ = std::move(library);
  central_log_ = std::move(central_log);
  library_changed_ = std::move(library_changed);
  event_channel_ = std::make_shared<EventChannel>();
  event_channel_->generation = generation_;

  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.hInstance = instance_;
  window_class.lpfnWndProc = WindowProcedure;
  window_class.lpszClassName = kSynthesisWindowClassName;
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
  window_class.hbrBackground =
      reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_WINDOW + 1));
  if (RegisterClassExW(&window_class) == 0 &&
      GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return false;
  }
  dpi_ = GetSystemDpi();
  window_ = CreateWindowExW(
      0, kSynthesisWindowClassName, L"Design++ Synthesis Schematic",
      WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
      ScaleForDpi(1280, dpi_), ScaleForDpi(820, dpi_), nullptr, nullptr,
      instance_, this);
  if (window_ == nullptr) return false;
  ShowWindow(window_, SW_SHOW);
  UpdateWindow(window_);
  return true;
}

ViewWindowKind SynthesisWindow::Kind() const noexcept {
  return ViewWindowKind::kSynthesis;
}

bool SynthesisWindow::CanActivate(
    const application::WorkspaceOpenRequest& request,
    core::ViewKind view_kind) const {
  return view_kind == core::ViewKind::kSynthesis &&
         request_.library_id == request.library_id &&
         request_.cell_id == request.cell_id &&
         request_.view_id == request.view_id;
}

void SynthesisWindow::Activate(
    const application::WorkspaceOpenRequest& request) {
  if (!CanActivate(request, core::ViewKind::kSynthesis) || window_ == nullptr) {
    return;
  }
  ShowWindow(window_, SW_RESTORE);
  SetForegroundWindow(window_);
}

bool SynthesisWindow::BelongsToLibrary(std::string_view library_id) const {
  return request_.library_id == library_id;
}

bool SynthesisWindow::MatchesCell(std::string_view library_id,
                                  std::string_view cell_id) const {
  return request_.library_id == library_id && request_.cell_id == cell_id;
}

bool SynthesisWindow::IsOpen() const noexcept { return window_ != nullptr; }

void SynthesisWindow::RefreshLibrary(application::LibraryRecord library) {
  if (library.library.id != request_.library_id) return;
  library_ = std::move(library);
  PopulateIdentity();
}

bool SynthesisWindow::PrepareClose() {
  if (!synthesis_service_.IsActive()) return true;
  const int choice =
      MessageBoxW(window_, L"실행 중인 합성을 취소하고 창을 닫으시겠습니까?",
                  L"Design++ Synthesis", MB_OKCANCEL | MB_ICONQUESTION);
  if (choice != IDOK) return false;
  synthesis_service_.Cancel();
  return true;
}

void SynthesisWindow::Close() {
  if (window_ != nullptr) DestroyWindow(window_);
}

bool SynthesisWindow::TranslateAccelerator(const MSG& message) {
  return window_ != nullptr &&
         IsDialogMessageW(window_, const_cast<MSG*>(&message));
}

LRESULT CALLBACK SynthesisWindow::WindowProcedure(HWND window, UINT message,
                                                  WPARAM wparam,
                                                  LPARAM lparam) {
  SynthesisWindow* self = reinterpret_cast<SynthesisWindow*>(
      GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<SynthesisWindow*>(create->lpCreateParams);
    self->window_ = window;
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  }
  return self != nullptr ? self->HandleMessage(message, wparam, lparam)
                         : DefWindowProcW(window, message, wparam, lparam);
}

LRESULT SynthesisWindow::HandleMessage(UINT message, WPARAM wparam,
                                       LPARAM lparam) {
  switch (message) {
    case WM_CREATE:
      if (!CreateControls()) return -1;
      {
        std::scoped_lock lock(event_channel_->mutex);
        event_channel_->window = window_;
      }
      PopulateIdentity();
      BeginLoad();
      return 0;
    case WM_SIZE:
      LayoutControls(LOWORD(lparam), HIWORD(lparam));
      return 0;
    case WM_GETMINMAXINFO: {
      auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
      info->ptMinTrackSize = {ScaleForDpi(900, dpi_), ScaleForDpi(600, dpi_)};
      return 0;
    }
    case WM_COMMAND:
      if (LOWORD(wparam) == kRunButtonId) {
        StartSynthesis();
        return 0;
      }
      if (LOWORD(wparam) == kReportsButtonId) {
        ShowReports();
        return 0;
      }
      if (LOWORD(wparam) == kArtifactsButtonId) {
        ShowArtifacts();
        return 0;
      }
      if (LOWORD(wparam) == kScriptButtonId) {
        ShowScript();
        return 0;
      }
      if (LOWORD(wparam) == kCancelButtonId) {
        synthesis_service_.Cancel();
        return 0;
      }
      if (LOWORD(wparam) == kViewSelectorId &&
          HIWORD(wparam) == CBN_SELCHANGE) {
        selected_view_mode_ = ComboBox_GetCurSel(view_selector_) == 1
                                  ? SchematicViewMode::kGate
                                  : SchematicViewMode::kReadable;
        ++module_load_generation_;
        if (schematic_build_active_) schematic_build_service_.Cancel();
        auto& models = selected_view_mode_ == SchematicViewMode::kReadable
                           ? readable_module_cache_
                           : gate_module_cache_;
        const auto selected = models.find(active_module_);
        if (selected == models.end()) {
          if (!active_module_.empty()) BeginModuleLoad(active_module_, false);
        } else {
          if (selected_view_mode_ == SchematicViewMode::kReadable) {
            readable_schematic_ = selected->second;
            const auto scene = readable_scene_cache_.find(active_module_);
            readable_scene_ =
                scene == readable_scene_cache_.end() ? nullptr : scene->second;
          } else {
            gate_schematic_ = selected->second;
            const auto scene = gate_scene_cache_.find(active_module_);
            gate_scene_ =
                scene == gate_scene_cache_.end() ? nullptr : scene->second;
          }
          UpdateCanvas();
        }
        return 0;
      }
      break;
    case WM_NOTIFY:
      if (reinterpret_cast<NMHDR*>(lparam)->hwndFrom == bottom_tabs_ &&
          reinterpret_cast<NMHDR*>(lparam)->code == TCN_SELCHANGE) {
        const int selected = TabCtrl_GetCurSel(bottom_tabs_);
        if (selected == 0) {
          ShowProblems();
        } else if (selected == 1) {
          ShowRuns();
        }
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
      synthesis_service_.Shutdown();
      schematic_build_service_.Shutdown();
      scheduler_.RequestStop();
      ShutdownChannel();
      window_ = nullptr;
      return 0;
    default:
      break;
  }
  return DefWindowProcW(window_, message, wparam, lparam);
}

bool SynthesisWindow::CreateControls() {
  identity_ = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0,
                              0, window_, nullptr, instance_, nullptr);
  run_button_ = CreateWindowExW(
      0, L"BUTTON", L"Run Synthesis",
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0, window_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRunButtonId)), instance_,
      nullptr);
  cancel_button_ = CreateWindowExW(
      0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0,
      0, window_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCancelButtonId)), instance_,
      nullptr);
  reports_button_ = CreateWindowExW(
      0, L"BUTTON", L"Reports", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0,
      0, window_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kReportsButtonId)),
      instance_, nullptr);
  artifacts_button_ = CreateWindowExW(
      0, L"BUTTON", L"Artifacts", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0,
      0, 0, window_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kArtifactsButtonId)),
      instance_, nullptr);
  script_button_ = CreateWindowExW(
      0, L"BUTTON", L"Script", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0,
      0, window_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kScriptButtonId)), instance_,
      nullptr);
  view_selector_ = CreateWindowExW(
      0, WC_COMBOBOXW, L"",
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST, 0, 0, 0, 0,
      window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kViewSelectorId)),
      instance_, nullptr);
  ComboBox_AddString(view_selector_, L"Readable");
  ComboBox_AddString(view_selector_, L"Gate");
  ComboBox_SetCurSel(view_selector_, 0);
  navigator_ = CreateWindowExW(
      WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
      WS_CHILD | WS_VISIBLE | TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT,
      0, 0, 0, 0, window_, nullptr, instance_, nullptr);
  if (!canvas_.Create(instance_, window_)) return false;
  canvas_.SetNavigationCallbacks(
      [this](std::string_view module_name) { NavigateToModule(module_name); },
      [this]() { NavigateBack(); });
  properties_ =
      CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC",
                      L"Synthesis Setup\r\n\r\nTop Module: loading\r\nLiberty: "
                      L"not selected\r\nFlatten: off\r\nYosys: probing pending",
                      WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, 0, 0, 0, 0,
                      window_, nullptr, instance_, nullptr);
  flatten_checkbox_ =
      CreateWindowExW(0, L"BUTTON", L"Flatten hierarchy",
                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0,
                      0, 0, 0, window_, nullptr, instance_, nullptr);
  liberty_label_ = CreateWindowExW(0, L"STATIC", L"Technology Liberty",
                                   WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0,
                                   window_, nullptr, instance_, nullptr);
  liberty_list_ = CreateWindowExW(
      WS_EX_CLIENTEDGE, L"LISTBOX", L"",
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | LBS_EXTENDEDSEL | WS_VSCROLL, 0, 0,
      0, 0, window_, nullptr, instance_, nullptr);
  bottom_tabs_ =
      CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0,
                      window_, nullptr, instance_, nullptr);
  for (const wchar_t* title : {L"Problems", L"Runs", L"Output"}) {
    TCITEMW item{};
    item.mask = TCIF_TEXT;
    item.pszText = const_cast<wchar_t*>(title);
    TabCtrl_InsertItem(bottom_tabs_, TabCtrl_GetItemCount(bottom_tabs_), &item);
  }
  output_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
                            L"Design++ Synthesis Output\r\n",
                            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY |
                                ES_AUTOVSCROLL | WS_VSCROLL,
                            0, 0, 0, 0, window_, nullptr, instance_, nullptr);
  status_ = CreateWindowExW(0, STATUSCLASSNAMEW, L"Synthesis ready",
                            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr,
                            instance_, nullptr);
  if (identity_ == nullptr || run_button_ == nullptr ||
      cancel_button_ == nullptr || reports_button_ == nullptr ||
      artifacts_button_ == nullptr || script_button_ == nullptr ||
      view_selector_ == nullptr || navigator_ == nullptr ||
      properties_ == nullptr || flatten_checkbox_ == nullptr ||
      liberty_label_ == nullptr || liberty_list_ == nullptr ||
      bottom_tabs_ == nullptr || output_ == nullptr || status_ == nullptr) {
    return false;
  }
  SendMessageW(output_, EM_SETLIMITTEXT, kMaximumOutputCharacters, 0);
  EnableWindow(run_button_, FALSE);
  EnableWindow(cancel_button_, FALSE);
  font_ = CreateUiFont(dpi_);
  ApplyFontToWindowTree(window_, font_.Get());
  RECT client{};
  GetClientRect(window_, &client);
  LayoutControls(client.right, client.bottom);
  return true;
}

void SynthesisWindow::LayoutControls(int width, int height) {
  if (status_ == nullptr) return;
  SendMessageW(status_, WM_SIZE, 0, 0);
  RECT status_bounds{};
  GetWindowRect(status_, &status_bounds);
  const int status_height = status_bounds.bottom - status_bounds.top;
  const int gap = ScaleForDpi(6, dpi_);
  const int header_height = ScaleForDpi(34, dpi_);
  const int button_width = ScaleForDpi(105, dpi_);
  const int selector_width = ScaleForDpi(110, dpi_);
  const int navigator_width = ScaleForDpi(240, dpi_);
  const int property_width = ScaleForDpi(280, dpi_);
  const int bottom_height = ScaleForDpi(190, dpi_);
  const int content_bottom = height - status_height - bottom_height - gap;
  MoveWindow(identity_, gap, gap,
             std::max(0, width - button_width * 5 - selector_width - gap * 8),
             header_height, TRUE);
  int button_x =
      std::max(gap, width - button_width * 5 - selector_width - gap * 6);
  MoveWindow(view_selector_, button_x, gap, selector_width,
             ScaleForDpi(220, dpi_), TRUE);
  button_x += selector_width + gap;
  for (HWND button : {run_button_, cancel_button_, reports_button_,
                      artifacts_button_, script_button_}) {
    MoveWindow(button, button_x, gap, button_width, header_height, TRUE);
    button_x += button_width + gap;
  }
  const int body_top = header_height + gap * 2;
  const int body_height = std::max(0, content_bottom - body_top);
  const int property_x = std::max(0, width - property_width);
  MoveWindow(navigator_, 0, body_top, navigator_width, body_height, TRUE);
  MoveWindow(properties_, property_x, body_top, property_width,
             ScaleForDpi(108, dpi_), TRUE);
  MoveWindow(flatten_checkbox_, property_x + gap,
             body_top + ScaleForDpi(114, dpi_), property_width - gap * 2,
             ScaleForDpi(24, dpi_), TRUE);
  MoveWindow(liberty_label_, property_x + gap,
             body_top + ScaleForDpi(144, dpi_), property_width - gap * 2,
             ScaleForDpi(22, dpi_), TRUE);
  MoveWindow(liberty_list_, property_x + gap, body_top + ScaleForDpi(168, dpi_),
             property_width - gap * 2,
             std::max(0, body_height - ScaleForDpi(174, dpi_)), TRUE);
  canvas_.Move(navigator_width + gap, body_top,
               std::max(0, width - navigator_width - property_width - gap * 2),
               std::max(0, content_bottom - body_top));
  const int tabs_top = height - status_height - bottom_height;
  MoveWindow(bottom_tabs_, 0, tabs_top, width, bottom_height, TRUE);
  RECT page{0, 0, width, bottom_height};
  TabCtrl_AdjustRect(bottom_tabs_, FALSE, &page);
  MoveWindow(output_, page.left, tabs_top + page.top,
             std::max(0, static_cast<int>(page.right - page.left)),
             std::max(0, static_cast<int>(page.bottom - page.top)), TRUE);
}

void SynthesisWindow::BeginLoad() {
  const auto channel = event_channel_;
  const application::LibraryRecord library = library_;
  const std::string cell_id = request_.cell_id;
  const std::uint64_t generation = generation_;
  const bool accepted = scheduler_.Submit(
      [channel, library, cell_id, generation](std::stop_token stop_token) {
        if (stop_token.stop_requested()) return;
        WindowEvent event;
        event.kind = WindowEventKind::kLoaded;
        event.generation = generation;
        application::ProjectService service;
        auto opened = service.OpenOrCreate(library, cell_id);
        if (!opened.Ok()) {
          event.status = opened.GetStatus();
        } else {
          event.document = std::make_unique<application::ProjectDocument>(
              std::move(opened).Value());
          auto sources =
              service.ResolveSources(library, cell_id, event.document->project);
          if (!sources.Ok()) {
            event.status = sources.GetStatus();
          } else {
            event.sources = std::move(sources).Value();
            const std::filesystem::path cell_directory =
                library.directory / L"cells" / Utf8ToWide(cell_id);
            application::RunStore run_store;
            static_cast<void>(run_store.RecoverInterrupted(cell_directory));
            auto listed = run_store.List(cell_directory);
            if (listed.Ok()) {
              event.runs = std::move(listed).Value();
              const auto latest = std::find_if(
                  event.runs.begin(), event.runs.end(),
                  [](const auto& run) { return run.stage == "Synthesis"; });
              if (latest != event.runs.end()) {
                event.active_run =
                    std::make_shared<application::RunRecord>(*latest);
                for (const application::RunArtifact& artifact :
                     latest->artifacts) {
                  const std::filesystem::path path =
                      latest->directory / artifact.relative_path;
                  if (artifact.kind == "script" && artifact.format == "yosys") {
                    std::ifstream input(path, std::ios::binary);
                    std::ostringstream contents;
                    contents << input.rdbuf();
                    if (input || input.eof())
                      event.script_text = contents.str();
                  } else if (artifact.kind == "statistics" &&
                             artifact.format == "json") {
                    std::ifstream input(path, std::ios::binary);
                    std::ostringstream contents;
                    contents << input.rdbuf();
                    auto parsed = adapters::YosysAdapter().ParseStatistics(
                        contents.str());
                    if (parsed.Ok()) event.metrics = std::move(parsed).Value();
                  } else if (artifact.kind == "netlist" &&
                             artifact.format == "json") {
                    std::ifstream input(path, std::ios::binary);
                    std::ostringstream contents;
                    contents << input.rdbuf();
                    auto parsed = adapters::YosysAdapter().ParseNetlist(
                        contents.str(), event.document->project.top_module);
                    if (parsed.Ok()) {
                      event.gate_schematic = std::move(parsed).Value();
                    }
                  } else if (artifact.kind == "schematic" &&
                             artifact.format == "yosys-structural-json") {
                    std::ifstream input(path, std::ios::binary);
                    std::ostringstream contents;
                    contents << input.rdbuf();
                    auto parsed = adapters::YosysAdapter().ParseNetlist(
                        contents.str(), event.document->project.top_module);
                    if (parsed.Ok()) {
                      event.readable_schematic = std::move(parsed).Value();
                    } else {
                      event.readable_schematic_status = parsed.GetStatus();
                    }
                  }
                }
                if (event.readable_schematic.top_module.empty() &&
                    event.readable_schematic_status.Ok()) {
                  event.readable_schematic_status = {
                      core::ErrorCode::kNotFound,
                      "Legacy run has no readable schematic artifact", 0};
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
  if (!accepted) SetWindowTextW(status_, L"Project load queue unavailable");
}

void SynthesisWindow::HandleEvents() {
  std::deque<WindowEvent> events;
  {
    std::scoped_lock lock(event_channel_->mutex);
    events.swap(event_channel_->events);
  }
  for (WindowEvent& event : events) {
    if (event.generation != generation_) continue;
    if (event.kind == WindowEventKind::kLoaded) {
      if (!event.status.Ok() || event.document == nullptr) {
        SetWindowTextW(status_, Utf8ToWide(event.status.message).c_str());
        AppendOutput(L"[Project] " + Utf8ToWide(event.status.message) +
                     L"\r\n");
        continue;
      }
      document_ = std::move(event.document);
      sources_ = std::move(event.sources);
      runs_ = std::move(event.runs);
      active_run_ = std::move(event.active_run);
      metrics_ = event.metrics;
      gate_schematic_ = std::move(event.gate_schematic);
      readable_schematic_ = std::move(event.readable_schematic);
      readable_schematic_status_ = std::move(event.readable_schematic_status);
      gate_scene_.reset();
      readable_scene_.reset();
      gate_module_cache_.clear();
      readable_module_cache_.clear();
      gate_scene_cache_.clear();
      readable_scene_cache_.clear();
      module_history_.clear();
      active_module_ = document_->project.top_module;
      if (!gate_schematic_.top_module.empty()) {
        gate_module_cache_.insert_or_assign(gate_schematic_.top_module,
                                            gate_schematic_);
      }
      if (!readable_schematic_.top_module.empty()) {
        readable_module_cache_.insert_or_assign(readable_schematic_.top_module,
                                                readable_schematic_);
      }
      script_text_ = std::move(event.script_text);
      PopulateSynthesisConfiguration();
      const std::size_t enabled_rtl = std::count_if(
          sources_.begin(), sources_.end(), [](const auto& source) {
            return source.enabled && source.exists &&
                   source.view_kind == core::ViewKind::kVerilog;
          });
      std::wstring setup =
          L"Synthesis Setup\r\n\r\nTop Module: " +
          Utf8ToWide(document_->project.top_module) + L"\r\nEnabled RTL: " +
          std::to_wstring(enabled_rtl) + L"\r\nLiberty: " +
          std::to_wstring(document_->project.synthesis.liberty_paths.size()) +
          L" file(s)\r\nFlatten: " +
          std::wstring(document_->project.synthesis.flatten ? L"on" : L"off") +
          L"\r\nYosys: probe on run";
      SetWindowTextW(properties_, setup.c_str());
      EnableWindow(run_button_, !document_->project.top_module.empty());
      UpdateCanvas();
      SetWindowTextW(status_, L"Synthesis ready");
      continue;
    }
    if (event.kind == WindowEventKind::kModuleLoaded) {
      if (event.module_load_generation != module_load_generation_) continue;
      if (!event.status.Ok() || event.module_schematic.top_module.empty()) {
        SetWindowTextW(status_, Utf8ToWide(event.status.message).c_str());
        AppendOutput(L"\r\n[Schematic hierarchy] " +
                     Utf8ToWide(event.status.message) + L"\r\n");
        UpdateCanvas();
        continue;
      }
      auto& models = event.module_mode == SchematicViewMode::kReadable
                         ? readable_module_cache_
                         : gate_module_cache_;
      models.insert_or_assign(event.module_schematic.top_module,
                              event.module_schematic);
      if (event.add_module_history && !event.previous_module.empty()) {
        module_history_.push_back(event.previous_module);
      }
      active_module_ = event.module_schematic.top_module;
      if (event.module_mode == SchematicViewMode::kReadable) {
        readable_schematic_ = std::move(event.module_schematic);
        readable_scene_.reset();
      } else {
        gate_schematic_ = std::move(event.module_schematic);
        gate_scene_.reset();
      }
      UpdateCanvas();
      continue;
    }
    if (event.kind == WindowEventKind::kScene) {
      if (event.scene.generation != schematic_generation_) continue;
      schematic_build_active_ = false;
      if (!event.scene.status.Ok() || event.scene.scene == nullptr) {
        if (event.scene.status.code != core::ErrorCode::kCancelled) {
          canvas_.SetMessage(Utf8ToWide(event.scene.status.message));
          AppendOutput(L"\r\n[Schematic] " +
                       Utf8ToWide(event.scene.status.message) + L"\r\n");
        }
      } else if (event.scene.scene->mode == SchematicViewMode::kReadable) {
        diagnostics_.insert(diagnostics_.end(),
                            event.scene.scene->diagnostics.begin(),
                            event.scene.scene->diagnostics.end());
        readable_scene_ = std::move(event.scene.scene);
        readable_scene_cache_.insert_or_assign(readable_scene_->top_module,
                                               readable_scene_);
      } else {
        diagnostics_.insert(diagnostics_.end(),
                            event.scene.scene->diagnostics.begin(),
                            event.scene.scene->diagnostics.end());
        gate_scene_ = std::move(event.scene.scene);
        gate_scene_cache_.insert_or_assign(gate_scene_->top_module,
                                           gate_scene_);
      }
      UpdateCanvas();
      continue;
    }
    application::SynthesisRunEvent& synthesis = event.synthesis;
    if (synthesis.kind == application::SynthesisRunEventKind::kOutput) {
      AppendOutput(Utf8ToWide(synthesis.output));
    } else if (synthesis.kind ==
               application::SynthesisRunEventKind::kStateChanged) {
      ApplyState(synthesis.state);
    } else {
      diagnostics_ = std::move(synthesis.diagnostics);
      active_run_ = std::move(synthesis.run);
      metrics_ = synthesis.metrics;
      gate_schematic_ = std::move(synthesis.gate_schematic);
      readable_schematic_ = std::move(synthesis.readable_schematic);
      readable_schematic_status_ =
          std::move(synthesis.readable_schematic_status);
      if (!readable_schematic_.top_module.empty()) {
        selected_view_mode_ = SchematicViewMode::kReadable;
        ComboBox_SetCurSel(view_selector_, 0);
      }
      gate_scene_.reset();
      readable_scene_.reset();
      gate_module_cache_.clear();
      readable_module_cache_.clear();
      gate_scene_cache_.clear();
      readable_scene_cache_.clear();
      module_history_.clear();
      active_module_ =
          document_ == nullptr ? std::string() : document_->project.top_module;
      if (!gate_schematic_.top_module.empty()) {
        gate_module_cache_.insert_or_assign(gate_schematic_.top_module,
                                            gate_schematic_);
      }
      if (!readable_schematic_.top_module.empty()) {
        readable_module_cache_.insert_or_assign(readable_schematic_.top_module,
                                                readable_schematic_);
      }
      script_text_ = std::move(synthesis.script_text);
      if (active_run_) runs_.insert(runs_.begin(), *active_run_);
      application::SynthesisRunState display_state = synthesis.state;
      if (active_run_ != nullptr) {
        if (active_run_->status == application::RunStatus::kSucceeded &&
            active_run_->outcome.result_succeeded) {
          display_state = application::SynthesisRunState::kSucceeded;
        } else if (active_run_->status == application::RunStatus::kCancelled) {
          display_state = application::SynthesisRunState::kCancelled;
        } else {
          display_state = application::SynthesisRunState::kFailed;
        }
      }
      ApplyState(display_state);
      UpdateCanvas();
      if (!synthesis.status.Ok()) {
        AppendOutput(L"\r\n[Synthesis] " +
                     Utf8ToWide(synthesis.status.message) + L"\r\n");
        ShowProblems();
      } else {
        ShowReports();
      }
    }
  }
}

void SynthesisWindow::PopulateSynthesisConfiguration() {
  if (document_ == nullptr) return;
  SendMessageW(liberty_list_, LB_RESETCONTENT, 0, 0);
  liberty_candidates_.clear();
  std::unordered_set<std::wstring> library_level_liberty;
  for (const application::ResolvedSource& source : sources_) {
    std::wstring extension = source.windows_path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   ::towlower);
    if (source.view_id.empty() && source.exists &&
        (extension == L".lib" || extension == L".liberty")) {
      std::wstring filename = source.windows_path.filename().wstring();
      std::transform(filename.begin(), filename.end(), filename.begin(),
                     ::towlower);
      library_level_liberty.insert(std::move(filename));
    }
  }
  std::unordered_set<std::wstring> visible_liberty;
  for (const application::ResolvedSource& source : sources_) {
    const bool supported_view =
        source.view_kind == core::ViewKind::kConstraints ||
        source.view_kind == core::ViewKind::kSynthesis ||
        source.view_kind == core::ViewKind::kTiming;
    if (!source.exists || !supported_view) {
      continue;
    }
    std::wstring extension = source.windows_path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   ::towlower);
    if (extension != L".lib" && extension != L".liberty") continue;
    std::wstring filename = source.windows_path.filename().wstring();
    std::transform(filename.begin(), filename.end(), filename.begin(),
                   ::towlower);
    if (!source.view_id.empty() && library_level_liberty.contains(filename)) {
      continue;
    }
    if (!visible_liberty.insert(filename).second) continue;
    liberty_candidates_.push_back(&source);
    std::wstring label = Utf8ToWide(source.relative_path);
    if (source.view_id.empty()) {
      label += L"  [Library]";
    } else if (source.view_kind != core::ViewKind::kConstraints) {
      label += L"  [legacy managed Liberty]";
    }
    const LRESULT index = SendMessageW(liberty_list_, LB_ADDSTRING, 0,
                                       reinterpret_cast<LPARAM>(label.c_str()));
    if (std::find(document_->project.synthesis.liberty_paths.begin(),
                  document_->project.synthesis.liberty_paths.end(),
                  source.relative_path) !=
        document_->project.synthesis.liberty_paths.end()) {
      SendMessageW(liberty_list_, LB_SETSEL, TRUE, index);
    }
  }
  if (liberty_candidates_.empty()) {
    SendMessageW(
        liberty_list_, LB_ADDSTRING, 0,
        reinterpret_cast<LPARAM>(L"No managed Liberty in Constraints View"));
  }
  Button_SetCheck(flatten_checkbox_, document_->project.synthesis.flatten
                                         ? BST_CHECKED
                                         : BST_UNCHECKED);
  const bool editable = !document_->read_only;
  EnableWindow(flatten_checkbox_, editable);
  EnableWindow(liberty_list_, editable && !liberty_candidates_.empty());
}

void SynthesisWindow::StartSynthesis() {
  if (document_ == nullptr || synthesis_service_.IsActive()) return;
  if (!document_->read_only) {
    document_->project.synthesis.flatten =
        Button_GetCheck(flatten_checkbox_) == BST_CHECKED;
    document_->project.synthesis.liberty_paths.clear();
    for (std::size_t index = 0; index < liberty_candidates_.size(); ++index) {
      if (SendMessageW(liberty_list_, LB_GETSEL, index, 0) > 0) {
        document_->project.synthesis.liberty_paths.push_back(
            liberty_candidates_[index]->relative_path);
      }
    }
    const core::Status saved = project_service_.Save(document_.get());
    if (!saved.Ok()) {
      SetWindowTextW(status_, Utf8ToWide(saved.message).c_str());
      return;
    }
  }
  application::SynthesisRunRequest request;
  request.project = document_->project;
  request.sources = sources_;
  request.library_directory = library_.directory;
  request.cell_directory =
      library_.directory / L"cells" / Utf8ToWide(request_.cell_id);
  request.generation = generation_;
  const auto channel = event_channel_;
  const std::uint64_t generation = generation_;
  const core::Status started = synthesis_service_.Start(
      std::move(request),
      [channel, generation](application::SynthesisRunEvent synthesis) {
        WindowEvent event;
        event.kind = WindowEventKind::kSynthesis;
        event.generation = generation;
        event.synthesis = std::move(synthesis);
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
  if (!started.Ok()) {
    SetWindowTextW(status_, Utf8ToWide(started.message).c_str());
  }
}

void SynthesisWindow::ApplyState(application::SynthesisRunState state) {
  const bool active = state == application::SynthesisRunState::kProbing ||
                      state == application::SynthesisRunState::kPreparing ||
                      state == application::SynthesisRunState::kRunning ||
                      state == application::SynthesisRunState::kCancelling;
  EnableWindow(run_button_, !active && document_ != nullptr &&
                                !document_->project.top_module.empty());
  EnableWindow(cancel_button_, active);
  const wchar_t* text =
      state == application::SynthesisRunState::kProbing ? L"Probing Yosys..."
      : state == application::SynthesisRunState::kPreparing
          ? L"Preparing run..."
      : state == application::SynthesisRunState::kRunning
          ? L"Running synthesis..."
      : state == application::SynthesisRunState::kCancelling
          ? L"Cancelling synthesis..."
      : state == application::SynthesisRunState::kSucceeded
          ? L"Synthesis succeeded"
      : state == application::SynthesisRunState::kCancelled
          ? L"Synthesis cancelled"
      : state == application::SynthesisRunState::kFailed ? L"Synthesis failed"
                                                         : L"Synthesis ready";
  SetWindowTextW(status_, text);
}

void SynthesisWindow::AppendOutput(std::wstring_view text) {
  if (text.empty()) return;
  const std::wstring value(text);
  SendMessageW(output_, EM_SETSEL, static_cast<WPARAM>(-1),
               static_cast<LPARAM>(-1));
  SendMessageW(output_, EM_REPLACESEL, FALSE,
               reinterpret_cast<LPARAM>(value.c_str()));
}

void SynthesisWindow::ShowProblems() {
  TabCtrl_SetCurSel(bottom_tabs_, 0);
  std::wstring text = L"Synthesis Problems\r\n\r\n";
  if (diagnostics_.empty()) {
    text += L"No diagnostics.";
  } else {
    for (const core::Diagnostic& diagnostic : diagnostics_) {
      text += Utf8ToWide(diagnostic.code) + L"  " +
              Utf8ToWide(diagnostic.file) + L":" +
              std::to_wstring(diagnostic.line) + L"  " +
              Utf8ToWide(diagnostic.message) + L"\r\n";
    }
  }
  SetWindowTextW(output_, text.c_str());
}

void SynthesisWindow::ShowRuns() {
  TabCtrl_SetCurSel(bottom_tabs_, 1);
  std::wstring text = L"Synthesis Runs\r\n\r\n";
  if (runs_.empty()) text += L"No runs.";
  for (const application::RunRecord& run : runs_) {
    text += Utf8ToWide(run.started_utc) + L"  " + Utf8ToWide(run.tool) + L"  " +
            Utf8ToWide(application::RunStatusName(run.status)) + L"\r\n";
  }
  SetWindowTextW(output_, text.c_str());
}

void SynthesisWindow::ShowReports() {
  if (!active_run_) {
    SetWindowTextW(status_, L"No synthesis run is selected");
    return;
  }
  std::wstring text =
      L"Synthesis Report\r\n\r\nTop Module: " +
      Utf8ToWide(document_ ? document_->project.top_module : std::string{}) +
      L"\r\nTool: " + Utf8ToWide(active_run_->tool_version) +
      L"\r\nCell Count: " + std::to_wstring(metrics_.cell_count) +
      L"\r\nArea: ";
  text += metrics_.has_area ? std::to_wstring(metrics_.area) : L"N/A";
  const std::shared_ptr<const SchematicScene> scene =
      selected_view_mode_ == SchematicViewMode::kReadable ? readable_scene_
                                                          : gate_scene_;
  if (scene != nullptr) {
    text += L"\r\nDisplayed Nodes: " +
            std::to_wstring(scene->summary.displayed_node_count) +
            L"\r\nRegister Banks: " +
            std::to_wstring(scene->summary.register_bank_count) +
            L"\r\nBuses: " + std::to_wstring(scene->summary.bus_count) +
            L"\r\nUnsupported Cells: " +
            std::to_wstring(scene->summary.unsupported_cell_count) +
            L"\r\nCombinational Loops: " +
            std::to_wstring(scene->summary.combinational_loop_count);
  }
  text += L"\r\nRun: " + active_run_->directory.wstring();
  SetWindowTextW(output_, text.c_str());
  SetWindowTextW(status_, L"Showing synthesis report");
}

void SynthesisWindow::ShowArtifacts() {
  if (!active_run_) {
    SetWindowTextW(status_, L"No synthesis run is selected");
    return;
  }
  std::wstring text = L"Synthesis Artifacts\r\n\r\n";
  for (const application::RunArtifact& artifact : active_run_->artifacts) {
    text += Utf8ToWide(artifact.kind) + L" / " + Utf8ToWide(artifact.format) +
            L"\r\n  " + Utf8ToWide(artifact.relative_path) + L"\r\n";
  }
  SetWindowTextW(output_, text.c_str());
  SetWindowTextW(status_, L"Showing synthesis artifacts");
}

void SynthesisWindow::ShowScript() {
  if (script_text_.empty()) {
    SetWindowTextW(status_, L"No synthesis script is available");
    return;
  }
  SetWindowTextW(output_, Utf8ToWide(script_text_).c_str());
  SetWindowTextW(status_, L"Showing Yosys script");
}

void SynthesisWindow::NavigateToModule(std::string_view module_name) {
  if (module_name.empty() || module_name == active_module_) return;
  auto& models = selected_view_mode_ == SchematicViewMode::kReadable
                     ? readable_module_cache_
                     : gate_module_cache_;
  const auto found = models.find(std::string(module_name));
  if (found == models.end()) {
    BeginModuleLoad(std::string(module_name), true);
    return;
  }
  module_history_.push_back(active_module_);
  active_module_ = found->first;
  if (selected_view_mode_ == SchematicViewMode::kReadable) {
    readable_schematic_ = found->second;
    const auto scene = readable_scene_cache_.find(active_module_);
    readable_scene_ =
        scene == readable_scene_cache_.end() ? nullptr : scene->second;
  } else {
    gate_schematic_ = found->second;
    const auto scene = gate_scene_cache_.find(active_module_);
    gate_scene_ = scene == gate_scene_cache_.end() ? nullptr : scene->second;
  }
  UpdateCanvas();
}

void SynthesisWindow::NavigateBack() {
  if (module_history_.empty()) {
    SetWindowTextW(status_, L"Already at the synthesis hierarchy root");
    return;
  }
  const std::string target = module_history_.back();
  module_history_.pop_back();
  auto& models = selected_view_mode_ == SchematicViewMode::kReadable
                     ? readable_module_cache_
                     : gate_module_cache_;
  const auto found = models.find(target);
  if (found == models.end()) {
    BeginModuleLoad(target, false);
    return;
  }
  active_module_ = target;
  if (selected_view_mode_ == SchematicViewMode::kReadable) {
    readable_schematic_ = found->second;
    const auto scene = readable_scene_cache_.find(target);
    readable_scene_ =
        scene == readable_scene_cache_.end() ? nullptr : scene->second;
  } else {
    gate_schematic_ = found->second;
    const auto scene = gate_scene_cache_.find(target);
    gate_scene_ = scene == gate_scene_cache_.end() ? nullptr : scene->second;
  }
  UpdateCanvas();
}

void SynthesisWindow::BeginModuleLoad(std::string module_name,
                                      bool add_history) {
  if (active_run_ == nullptr || module_name.empty()) return;
  const application::RunRecord run = *active_run_;
  const SchematicViewMode mode = selected_view_mode_;
  const std::string previous_module = active_module_;
  const std::uint64_t load_generation = ++module_load_generation_;
  const std::uint64_t window_generation = generation_;
  const auto channel = event_channel_;
  canvas_.SetMessage(L"Loading module schematic...");
  SetWindowTextW(
      status_, (L"Loading module " + Utf8ToWide(module_name) + L"...").c_str());
  const bool accepted = scheduler_.Submit(
      [channel, run, mode, module_name = std::move(module_name),
       previous_module, add_history, load_generation,
       window_generation](std::stop_token stop_token) mutable {
        if (stop_token.stop_requested()) return;
        WindowEvent event;
        event.kind = WindowEventKind::kModuleLoaded;
        event.generation = window_generation;
        event.module_mode = mode;
        event.previous_module = previous_module;
        event.add_module_history = add_history;
        event.module_load_generation = load_generation;
        const application::RunArtifact* selected_artifact = nullptr;
        for (const application::RunArtifact& artifact : run.artifacts) {
          const bool matches =
              mode == SchematicViewMode::kReadable
                  ? artifact.kind == "schematic" &&
                        artifact.format == "yosys-structural-json"
                  : artifact.kind == "netlist" && artifact.format == "json";
          if (matches) {
            selected_artifact = &artifact;
            break;
          }
        }
        if (selected_artifact == nullptr) {
          event.status = {core::ErrorCode::kNotFound,
                          "Selected run has no compatible schematic artifact",
                          0};
        } else {
          const std::filesystem::path path =
              run.directory / selected_artifact->relative_path;
          std::ifstream input(path, std::ios::binary);
          std::ostringstream contents;
          contents << input.rdbuf();
          if (!input && !input.eof()) {
            event.status = {core::ErrorCode::kIoError,
                            "Cannot read the synthesis schematic artifact", 0};
          } else {
            auto parsed = adapters::YosysAdapter().ParseNetlist(contents.str(),
                                                                module_name);
            if (parsed.Ok()) {
              event.module_schematic = std::move(parsed).Value();
              event.status = core::Status::Success();
            } else {
              event.status = parsed.GetStatus();
            }
          }
        }
        HWND target = nullptr;
        {
          std::scoped_lock lock(channel->mutex);
          if (channel->window == nullptr ||
              channel->generation != window_generation) {
            return;
          }
          channel->events.push_back(std::move(event));
          target = channel->window;
        }
        PostMessageW(target, kEventMessage, 0, 0);
      });
  if (!accepted) {
    SetWindowTextW(status_, L"Module load queue unavailable");
  }
}

void SynthesisWindow::UpdateCanvas() {
  if (active_run_ == nullptr) {
    canvas_.SetMessage(
        L"No synthesized design.\r\nRun Yosys to generate a gate-level "
        L"netlist.");
  } else if (active_run_->status == application::RunStatus::kSucceeded &&
             active_run_->outcome.result_succeeded) {
    if (selected_view_mode_ == SchematicViewMode::kReadable &&
        readable_schematic_.top_module.empty()) {
      selected_view_mode_ = SchematicViewMode::kGate;
      ComboBox_SetCurSel(view_selector_, 1);
      if (!readable_schematic_status_.Ok()) {
        AppendOutput(L"\r\n[Schematic] " +
                     Utf8ToWide(readable_schematic_status_.message) +
                     L"; showing Gate view.\r\n");
      }
    }
    const std::shared_ptr<const SchematicScene>& selected_scene =
        selected_view_mode_ == SchematicViewMode::kReadable ? readable_scene_
                                                            : gate_scene_;
    const core::SchematicModel& selected_model =
        selected_view_mode_ == SchematicViewMode::kReadable
            ? readable_schematic_
            : gate_schematic_;
    if (selected_scene != nullptr) {
      canvas_.SetScene(selected_scene);
      SetWindowTextW(status_,
                     (L"Module: " + Utf8ToWide(active_module_) +
                      L" — Double-click submodule; Ctrl+Z / Shift+F: back")
                         .c_str());
    } else if (!selected_model.top_module.empty()) {
      if (!schematic_build_active_) StartSceneBuild();
      canvas_.SetMessage(L"Building schematic layout...");
      SetWindowTextW(status_, L"Building schematic...");
    } else {
      canvas_.SetMessage(
          L"Synthesis succeeded, but the gate schematic could not be "
          L"decoded.\r\nOpen Artifacts to inspect netlist.json.");
    }
  } else if (active_run_->status == application::RunStatus::kCancelled) {
    canvas_.SetMessage(
        L"Synthesis cancelled.\r\nNo completed gate netlist is selected.");
  } else {
    canvas_.SetMessage(
        L"Synthesis failed.\r\nOpen Problems and Output for diagnostics.");
  }
}

void SynthesisWindow::StartSceneBuild() {
  if (schematic_build_active_) return;
  SchematicBuildRequest request;
  request.mode = selected_view_mode_;
  request.model = selected_view_mode_ == SchematicViewMode::kReadable
                      ? readable_schematic_
                      : gate_schematic_;
  if (request.model.top_module.empty()) return;
  schematic_build_active_ = true;
  const std::uint64_t scene_generation = ++schematic_generation_;
  const std::uint64_t window_generation = generation_;
  const auto channel = event_channel_;
  const core::Status started = schematic_build_service_.Start(
      scene_generation, std::move(request),
      [channel, window_generation](SchematicBuildEvent scene) {
        WindowEvent event;
        event.kind = WindowEventKind::kScene;
        event.generation = window_generation;
        event.scene = std::move(scene);
        HWND target = nullptr;
        {
          std::scoped_lock lock(channel->mutex);
          if (channel->window == nullptr ||
              channel->generation != window_generation) {
            return;
          }
          channel->events.push_back(std::move(event));
          target = channel->window;
        }
        PostMessageW(target, kEventMessage, 0, 0);
      });
  if (!started.Ok()) {
    schematic_build_active_ = false;
    canvas_.SetMessage(Utf8ToWide(started.message));
  }
}

void SynthesisWindow::ShutdownChannel() {
  if (!event_channel_) return;
  std::scoped_lock lock(event_channel_->mutex);
  event_channel_->window = nullptr;
  ++event_channel_->generation;
  event_channel_->events.clear();
}

void SynthesisWindow::PopulateIdentity() {
  const core::Cell* cell = FindCell(library_, request_.cell_id);
  const core::View* view = FindView(cell, request_.view_id);
  const std::wstring cell_name =
      cell ? Utf8ToWide(cell->name) : L"Unknown Cell";
  const std::wstring view_name = view ? Utf8ToWide(view->name) : L"Synthesis";
  const std::wstring identity = Utf8ToWide(library_.library.name) + L" / " +
                                cell_name + L" / " + view_name;
  SetWindowTextW(identity_, identity.c_str());
  SetWindowTextW(window_,
                 (L"Design++ Synthesis Schematic — " + cell_name).c_str());

  TreeView_DeleteAllItems(navigator_);
  TVINSERTSTRUCTW insert{};
  insert.hParent = TVI_ROOT;
  insert.hInsertAfter = TVI_LAST;
  insert.item.mask = TVIF_TEXT;
  std::wstring top = L"Design — " + cell_name;
  insert.item.pszText = top.data();
  const HTREEITEM root = TreeView_InsertItem(navigator_, &insert);
  for (const wchar_t* label : {L"Hierarchy", L"Instances", L"Nets", L"Runs"}) {
    insert.hParent = root;
    insert.item.pszText = const_cast<wchar_t*>(label);
    TreeView_InsertItem(navigator_, &insert);
  }
  TreeView_Expand(navigator_, root, TVE_EXPAND);
}

}  // namespace designpp::gui

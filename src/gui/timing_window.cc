// Copyright 2026 The Design++ Authors

#include "designpp/gui/timing_window.h"

#include <commctrl.h>

#include <algorithm>
#include <cwctype>
#include <deque>
#include <iomanip>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace designpp::gui {
namespace {

constexpr wchar_t kWindowClassName[] = L"DesignPlusPlus.TimingWindow";
constexpr int kRunButton = 7101;
constexpr int kCancelButton = 7102;
constexpr int kReportsButton = 7103;
constexpr int kArtifactsButton = 7104;
constexpr int kScriptButton = 7105;
constexpr int kSynthesisButton = 7106;
constexpr int kRunsList = 7110;
constexpr int kInitialWindowWidth = 1280;
constexpr int kInitialWindowHeight = 820;
constexpr int kMinimumWindowWidth = 900;
constexpr int kMinimumWindowHeight = 600;

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

std::wstring ControlText(HWND control) {
  const int length = GetWindowTextLengthW(control);
  std::wstring value(static_cast<std::size_t>(std::max(length, 0)) + 1, L'\0');
  GetWindowTextW(control, value.data(), static_cast<int>(value.size()));
  value.resize(std::wcslen(value.c_str()));
  return value;
}

const core::Cell* FindCell(const application::LibraryRecord& library,
                           std::string_view cell_id) {
  const auto found =
      std::find_if(library.library.cells.begin(), library.library.cells.end(),
                   [cell_id](const auto& cell) { return cell.id == cell_id; });
  return found == library.library.cells.end() ? nullptr : &*found;
}

std::wstring LowercaseExtension(const application::ResolvedSource& source) {
  std::wstring extension = source.windows_path.extension().wstring();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 ::towlower);
  return extension;
}

std::wstring LowercaseFilename(const application::ResolvedSource& source) {
  std::wstring filename = source.windows_path.filename().wstring();
  std::transform(filename.begin(), filename.end(), filename.begin(),
                 ::towlower);
  return filename;
}

bool IsNonEmptyFile(const application::ResolvedSource& source) {
  if (!source.exists) return false;
  std::error_code error;
  return std::filesystem::file_size(source.windows_path, error) > 0 && !error;
}

bool IsLibraryLevel(const application::ResolvedSource& source) {
  return source.view_id.empty();
}

void AddListViewColumn(HWND list, int index, int width,
                       const wchar_t* heading) {
  LVCOLUMNW column{};
  column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
  column.iSubItem = index;
  column.cx = width;
  column.pszText = const_cast<wchar_t*>(heading);
  ListView_InsertColumn(list, index, &column);
}

void AddViolationRow(HWND list, int row,
                     const adapters::TimingViolation& violation) {
  std::wostringstream slack;
  slack << std::fixed << std::setprecision(6) << violation.slack;
  std::wstring values[] = {Utf8ToWide(violation.check_type),
                           Utf8ToWide(violation.corner),
                           Utf8ToWide(violation.startpoint),
                           Utf8ToWide(violation.endpoint), slack.str()};
  LVITEMW item{};
  item.mask = LVIF_TEXT;
  item.iItem = row;
  item.pszText = values[0].data();
  const int inserted = ListView_InsertItem(list, &item);
  if (inserted < 0) return;
  for (int column = 1; column < static_cast<int>(std::size(values)); ++column) {
    ListView_SetItemText(list, inserted, column, values[column].data());
  }
}

}  // namespace

struct TimingWindow::EventChannel final {
  std::mutex mutex;
  HWND window = nullptr;
  std::uint64_t generation = 0;
  struct Event {
    bool loaded = false;
    core::Status status;
    std::unique_ptr<application::ProjectDocument> document;
    std::vector<application::ResolvedSource> sources;
    std::vector<application::TimingRunSnapshot> run_snapshots;
    std::shared_ptr<application::RunRecord> active_run;
    std::shared_ptr<application::RunRecord> compatible_synthesis_run;
    core::Status compatibility_status;
    adapters::TimingMetrics metrics;
    std::vector<core::Diagnostic> diagnostics;
    std::string script_text;
    application::TimingRunEvent timing;
  };
  std::deque<Event> events;
};

TimingWindow::~TimingWindow() { Close(); }

bool TimingWindow::Create(HINSTANCE instance,
                          const application::WorkspaceOpenRequest& request,
                          application::LibraryRecord library,
                          ViewWindowLogCallback central_log,
                          ViewWindowLibraryChangedCallback library_changed,
                          ViewWindowOpenCallback open_view) {
  instance_ = instance;
  request_ = request;
  library_ = std::move(library);
  central_log_ = std::move(central_log);
  library_changed_ = std::move(library_changed);
  open_view_ = std::move(open_view);
  event_channel_ = std::make_shared<EventChannel>();

  WNDCLASSEXW window_class{sizeof(window_class)};
  window_class.lpfnWndProc = WindowProcedure;
  window_class.hInstance = instance_;
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  window_class.lpszClassName = kWindowClassName;
  if (!RegisterClassExW(&window_class) &&
      GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return false;
  }
  dpi_ = GetSystemDpi();
  window_ =
      CreateWindowExW(0, kWindowClassName, L"Design++ Timing",
                      WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                      CW_USEDEFAULT, ScaleForDpi(kInitialWindowWidth, dpi_),
                      ScaleForDpi(kInitialWindowHeight, dpi_), nullptr, nullptr,
                      instance_, this);
  if (!window_) return false;
  {
    std::scoped_lock lock(event_channel_->mutex);
    event_channel_->window = window_;
    event_channel_->generation = generation_;
  }
  ShowWindow(window_, SW_SHOW);
  UpdateWindow(window_);
  BeginLoad();
  return true;
}

ViewWindowKind TimingWindow::Kind() const noexcept {
  return ViewWindowKind::kTiming;
}

bool TimingWindow::CanActivate(const application::WorkspaceOpenRequest& request,
                               core::ViewKind view_kind) const {
  return view_kind == core::ViewKind::kTiming &&
         MatchesCell(request.library_id, request.cell_id);
}

void TimingWindow::Activate(const application::WorkspaceOpenRequest& request) {
  if (!CanActivate(request, core::ViewKind::kTiming) || !window_) return;
  ShowWindow(window_, SW_RESTORE);
  SetForegroundWindow(window_);
}

bool TimingWindow::BelongsToLibrary(std::string_view library_id) const {
  return request_.library_id == library_id;
}

bool TimingWindow::MatchesCell(std::string_view library_id,
                               std::string_view cell_id) const {
  return request_.library_id == library_id && request_.cell_id == cell_id;
}

bool TimingWindow::IsOpen() const noexcept { return window_ != nullptr; }

void TimingWindow::RefreshLibrary(application::LibraryRecord library) {
  if (library.library.id != request_.library_id) return;
  library_ = std::move(library);
  ++generation_;
  {
    std::scoped_lock lock(event_channel_->mutex);
    event_channel_->generation = generation_;
  }
  BeginLoad();
}

bool TimingWindow::PrepareClose() {
  timing_service_.Cancel();
  return true;
}

void TimingWindow::Close() {
  if (!window_) return;
  timing_service_.Shutdown();
  scheduler_.RequestStop();
  ShutdownChannel();
  DestroyWindow(window_);
  window_ = nullptr;
}

bool TimingWindow::TranslateAccelerator(const MSG&) { return false; }

LRESULT CALLBACK TimingWindow::WindowProcedure(HWND window, UINT message,
                                               WPARAM wparam, LPARAM lparam) {
  TimingWindow* self =
      reinterpret_cast<TimingWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<TimingWindow*>(create->lpCreateParams);
    self->window_ = window;
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  }
  return self ? self->HandleMessage(message, wparam, lparam)
              : DefWindowProcW(window, message, wparam, lparam);
}

LRESULT TimingWindow::HandleMessage(UINT message, WPARAM wparam,
                                    LPARAM lparam) {
  switch (message) {
    case WM_CREATE:
      dpi_ = GetDpiForWindow(window_);
      return CreateControls() ? 0 : -1;
    case WM_SIZE:
      LayoutControls(LOWORD(lparam), HIWORD(lparam));
      return 0;
    case WM_GETMINMAXINFO: {
      auto* information = reinterpret_cast<MINMAXINFO*>(lparam);
      information->ptMinTrackSize = {ScaleForDpi(kMinimumWindowWidth, dpi_),
                                     ScaleForDpi(kMinimumWindowHeight, dpi_)};
      return 0;
    }
    case WM_COMMAND:
      switch (LOWORD(wparam)) {
        case kRunButton:
          StartTiming();
          return 0;
        case kCancelButton:
          timing_service_.Cancel();
          return 0;
        case kReportsButton:
          ShowReports();
          return 0;
        case kArtifactsButton:
          ShowArtifacts();
          return 0;
        case kScriptButton:
          ShowScript();
          return 0;
        case kSynthesisButton:
          OpenSynthesis();
          return 0;
      }
      break;
    case WM_NOTIFY:
      if (reinterpret_cast<NMHDR*>(lparam)->hwndFrom == bottom_tabs_ &&
          reinterpret_cast<NMHDR*>(lparam)->code == TCN_SELCHANGE) {
        TabCtrl_GetCurSel(bottom_tabs_) == 0 ? ShowProblems() : ShowRuns();
      } else if (reinterpret_cast<NMHDR*>(lparam)->hwndFrom == runs_list_ &&
                 reinterpret_cast<NMHDR*>(lparam)->code == LVN_ITEMCHANGED) {
        const int selected =
            ListView_GetNextItem(runs_list_, -1, LVNI_SELECTED);
        if (selected >= 0) SelectRun(static_cast<std::size_t>(selected));
      }
      return 0;
    case kEventMessage:
      HandleEvents();
      return 0;
    case WM_CLOSE:
      Close();
      return 0;
    case WM_NCDESTROY: {
      const HWND destroyed_window = window_;
      window_ = nullptr;
      return DefWindowProcW(destroyed_window, message, wparam, lparam);
    }
  }
  return DefWindowProcW(window_, message, wparam, lparam);
}

bool TimingWindow::CreateControls() {
  font_ = CreateUiFont(dpi_);
  const auto create = [this](const wchar_t* cls, const wchar_t* text,
                             DWORD style, int id) {
    HWND control = CreateWindowExW(
        0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, window_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_.Get()),
                 TRUE);
    return control;
  };
  identity_ = create(L"STATIC", L"OpenSTA Timing", SS_LEFT, 0);
  run_button_ = create(L"BUTTON", L"Run Timing", BS_PUSHBUTTON, kRunButton);
  cancel_button_ = create(L"BUTTON", L"Cancel", BS_PUSHBUTTON, kCancelButton);
  reports_button_ =
      create(L"BUTTON", L"Reports", BS_PUSHBUTTON, kReportsButton);
  artifacts_button_ =
      create(L"BUTTON", L"Artifacts", BS_PUSHBUTTON, kArtifactsButton);
  script_button_ = create(L"BUTTON", L"Script", BS_PUSHBUTTON, kScriptButton);
  synthesis_button_ =
      create(L"BUTTON", L"Open Synthesis", BS_PUSHBUTTON, kSynthesisButton);
  corner_label_ = create(L"STATIC", L"Corner", SS_LEFT, 0);
  sdc_label_ = create(L"STATIC", L"SDC", SS_LEFT, 0);
  liberty_label_ = create(L"STATIC", L"Liberty", SS_LEFT, 0);
  corner_edit_ = create(L"EDIT", L"typical", WS_BORDER | ES_AUTOHSCROLL, 0);
  sdc_combo_ = create(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL, 0);
  liberty_list_ =
      create(L"LISTBOX", L"", WS_BORDER | LBS_EXTENDEDSEL | WS_VSCROLL, 0);
  summary_ = create(L"EDIT", L"No timing result",
                    WS_BORDER | ES_MULTILINE | ES_READONLY, 0);
  violations_ =
      create(WC_LISTVIEWW, L"",
             WS_BORDER | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, 0);
  ListView_SetExtendedListViewStyle(violations_, LVS_EX_FULLROWSELECT |
                                                     LVS_EX_GRIDLINES |
                                                     LVS_EX_DOUBLEBUFFER);
  AddListViewColumn(violations_, 0, 90, L"Check");
  AddListViewColumn(violations_, 1, 90, L"Corner");
  AddListViewColumn(violations_, 2, 330, L"Startpoint");
  AddListViewColumn(violations_, 3, 430, L"Endpoint");
  AddListViewColumn(violations_, 4, 100, L"Slack (ns)");
  bottom_tabs_ = create(WC_TABCONTROLW, L"", 0, 0);
  TCITEMW item{TCIF_TEXT};
  item.pszText = const_cast<wchar_t*>(L"Problems");
  TabCtrl_InsertItem(bottom_tabs_, 0, &item);
  item.pszText = const_cast<wchar_t*>(L"Runs");
  TabCtrl_InsertItem(bottom_tabs_, 1, &item);
  output_ = create(L"EDIT", L"", ES_MULTILINE | ES_READONLY | WS_VSCROLL, 0);
  runs_list_ =
      create(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
             kRunsList);
  ListView_SetExtendedListViewStyle(runs_list_, LVS_EX_FULLROWSELECT |
                                                    LVS_EX_DOUBLEBUFFER |
                                                    LVS_EX_GRIDLINES);
  AddListViewColumn(runs_list_, 0, 145, L"Started");
  AddListViewColumn(runs_list_, 1, 90, L"Corner");
  AddListViewColumn(runs_list_, 2, 100, L"Setup WNS");
  AddListViewColumn(runs_list_, 3, 100, L"Setup TNS");
  AddListViewColumn(runs_list_, 4, 100, L"Hold WNS");
  AddListViewColumn(runs_list_, 5, 100, L"Hold TNS");
  AddListViewColumn(runs_list_, 6, 90, L"Status");
  ShowWindow(runs_list_, SW_HIDE);
  status_ = create(L"STATIC", L"Loading timing project...", SS_LEFT, 0);
  EnableWindow(cancel_button_, FALSE);
  return identity_ && run_button_ && cancel_button_ && reports_button_ &&
         artifacts_button_ && script_button_ && synthesis_button_ &&
         corner_label_ && sdc_label_ && liberty_label_ && corner_edit_ &&
         sdc_combo_ && liberty_list_ && summary_ && violations_ &&
         bottom_tabs_ && output_ && runs_list_ && status_;
}

void TimingWindow::LayoutControls(int width, int height) {
  const int scale = static_cast<int>(dpi_);
  const auto px = [scale](int value) { return MulDiv(value, scale, 96); };
  const int header = px(42);
  const int gap = px(4);
  const int identity_width = std::min(px(230), width / 3);
  MoveWindow(identity_, px(8), px(8), identity_width - px(12), px(25), TRUE);
  HWND buttons[] = {run_button_,       cancel_button_, reports_button_,
                    artifacts_button_, script_button_, synthesis_button_};
  int x = identity_width;
  const int button_width = std::max(px(64), (width - x - px(8) - gap * 5) / 6);
  for (HWND button : buttons) {
    MoveWindow(button, x, px(6), button_width, px(29), TRUE);
    x += button_width + gap;
  }
  const int setup_width = std::min(px(330), std::max(px(250), width / 2));
  const int label_width = px(62);
  const int field_x = px(10) + label_width;
  const int field_width = setup_width - field_x - px(10);
  MoveWindow(corner_label_, px(10), header + px(14), label_width, px(22), TRUE);
  MoveWindow(corner_edit_, field_x, header + px(10), field_width, px(25), TRUE);
  MoveWindow(sdc_label_, px(10), header + px(49), label_width, px(22), TRUE);
  MoveWindow(sdc_combo_, field_x, header + px(45), field_width, px(180), TRUE);
  MoveWindow(liberty_label_, px(10), header + px(84), label_width, px(22),
             TRUE);
  MoveWindow(liberty_list_, field_x, header + px(80), field_width, px(190),
             TRUE);
  const int bottom = px(210);
  const int result_height = std::max(0, height - header - bottom);
  const int summary_height = std::min(px(178), result_height);
  MoveWindow(summary_, setup_width, header, width - setup_width, summary_height,
             TRUE);
  MoveWindow(violations_, setup_width, header + summary_height,
             width - setup_width, std::max(0, result_height - summary_height),
             TRUE);
  MoveWindow(bottom_tabs_, 0, height - bottom, width, bottom - px(22), TRUE);
  RECT page{0, 0, width, bottom - px(22)};
  TabCtrl_AdjustRect(bottom_tabs_, FALSE, &page);
  MoveWindow(output_, page.left, height - bottom + page.top,
             page.right - page.left, page.bottom - page.top, TRUE);
  MoveWindow(runs_list_, page.left, height - bottom + page.top,
             page.right - page.left, page.bottom - page.top, TRUE);
  MoveWindow(status_, 0, height - px(22), width, px(22), TRUE);
}

void TimingWindow::BeginLoad() {
  const auto channel = event_channel_;
  const auto library = library_;
  const auto cell_id = request_.cell_id;
  const std::uint64_t generation = generation_;
  static_cast<void>(scheduler_.Submit([channel, library, cell_id,
                                       generation](std::stop_token token) {
    if (token.stop_requested()) return;
    EventChannel::Event event;
    event.loaded = true;
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
        core::Project& project = event.document->project;
        if (project.timing.liberty_paths.empty() &&
            !project.synthesis.liberty_paths.empty()) {
          const bool all_managed = std::all_of(
              project.synthesis.liberty_paths.begin(),
              project.synthesis.liberty_paths.end(), [&](const auto& path) {
                return std::any_of(
                    event.sources.begin(), event.sources.end(),
                    [&](const auto& source) {
                      const std::wstring extension = LowercaseExtension(source);
                      return source.relative_path == path &&
                             IsNonEmptyFile(source) &&
                             (extension == L".lib" || extension == L".liberty");
                    });
              });
          if (all_managed) {
            project.timing.liberty_paths = project.synthesis.liberty_paths;
          }
        }
        if (project.timing.sdc_path.empty()) {
          const auto sdc = std::find_if(
              event.sources.begin(), event.sources.end(),
              [](const auto& source) {
                return source.view_kind == core::ViewKind::kConstraints &&
                       LowercaseExtension(source) == L".sdc" &&
                       IsNonEmptyFile(source);
              });
          if (sdc != event.sources.end()) {
            project.timing.sdc_path = sdc->relative_path;
          } else {
            const auto empty_sdc = std::find_if(
                event.sources.begin(), event.sources.end(),
                [](const auto& source) {
                  return source.exists &&
                         source.view_kind == core::ViewKind::kConstraints &&
                         LowercaseExtension(source) == L".sdc";
                });
            if (empty_sdc != event.sources.end()) {
              project.timing.sdc_path = empty_sdc->relative_path;
            }
          }
        }
        application::TimingRunRequest timing_request;
        timing_request.project = project;
        timing_request.sources = event.sources;
        timing_request.library_directory = library.directory;
        timing_request.cell_directory =
            library.directory / L"cells" / Utf8ToWide(cell_id);
        auto compatible =
            application::ResolveCompatibleSynthesisRun(timing_request);
        if (compatible.Ok()) {
          event.compatible_synthesis_run = std::move(compatible).Value();
          event.compatibility_status = core::Status::Success();
        } else {
          event.compatibility_status = compatible.GetStatus();
        }
        const std::filesystem::path cell_directory =
            library.directory / L"cells" / Utf8ToWide(cell_id);
        auto history = application::LoadTimingRunHistory(
            cell_directory, event.document->project.timing.corner_name);
        if (history.Ok()) {
          event.run_snapshots = std::move(history).Value();
        }
        if (!event.run_snapshots.empty()) {
          const application::TimingRunSnapshot& latest =
              event.run_snapshots.front();
          event.active_run =
              std::make_shared<application::RunRecord>(latest.run);
          event.metrics = latest.metrics;
          event.diagnostics = latest.diagnostics;
          event.script_text = latest.script_text;
        }
        event.status = core::Status::Success();
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
  }));
}

void TimingWindow::HandleEvents() {
  std::deque<EventChannel::Event> events;
  {
    std::scoped_lock lock(event_channel_->mutex);
    events.swap(event_channel_->events);
  }
  for (auto& event : events) {
    if (event.loaded) {
      if (!event.status.Ok()) {
        SetWindowTextW(status_, Utf8ToWide(event.status.message).c_str());
        continue;
      }
      document_ = std::move(event.document);
      sources_ = std::move(event.sources);
      run_snapshots_ = std::move(event.run_snapshots);
      active_run_ = std::move(event.active_run);
      compatible_synthesis_run_ = std::move(event.compatible_synthesis_run);
      compatibility_status_ = std::move(event.compatibility_status);
      metrics_ = std::move(event.metrics);
      diagnostics_ = std::move(event.diagnostics);
      script_text_ = std::move(event.script_text);
      PopulateConfiguration();
      PopulateRunHistory();
      if (active_run_) ShowReports();
      continue;
    }
    const application::TimingRunEvent& timing = event.timing;
    if (timing.kind == application::TimingRunEventKind::kOutput) {
      AppendOutput(Utf8ToWide(timing.output));
    } else if (timing.kind == application::TimingRunEventKind::kStateChanged) {
      ApplyState(timing.state);
    } else {
      ApplyState(timing.state);
      active_run_ = timing.run;
      diagnostics_ = timing.diagnostics;
      if (!timing.status.Ok() && diagnostics_.empty()) {
        core::Diagnostic diagnostic;
        diagnostic.severity = core::DiagnosticSeverity::kError;
        diagnostic.code = "TIMING";
        diagnostic.message = timing.status.message;
        diagnostics_.push_back(std::move(diagnostic));
      }
      metrics_ = timing.metrics;
      script_text_ = timing.script_text;
      if (timing.run) {
        application::TimingRunSnapshot snapshot;
        snapshot.run = *timing.run;
        snapshot.corner =
            document_ ? document_->project.timing.corner_name : std::string{};
        snapshot.metrics = timing.metrics;
        snapshot.diagnostics = diagnostics_;
        snapshot.script_text = timing.script_text;
        run_snapshots_.insert(run_snapshots_.begin(), std::move(snapshot));
        PopulateRunHistory();
      }
      ShowReports();
      if (!timing.status.Ok()) ShowProblems();
    }
  }
}

void TimingWindow::PopulateConfiguration() {
  if (!document_) return;
  SetWindowTextW(corner_edit_,
                 Utf8ToWide(document_->project.timing.corner_name).c_str());
  SendMessageW(sdc_combo_, CB_RESETCONTENT, 0, 0);
  SendMessageW(liberty_list_, LB_RESETCONTENT, 0, 0);
  sdc_candidates_.clear();
  liberty_candidates_.clear();
  bool has_empty_sdc = false;
  std::unordered_set<std::wstring> library_level_liberty;
  for (const auto& source : sources_) {
    const std::wstring extension = LowercaseExtension(source);
    if (IsLibraryLevel(source) && IsNonEmptyFile(source) &&
        (extension == L".lib" || extension == L".liberty")) {
      library_level_liberty.insert(LowercaseFilename(source));
    }
  }
  std::unordered_set<std::wstring> visible_liberty;
  for (const auto& source : sources_) {
    const bool constraints = source.view_kind == core::ViewKind::kConstraints;
    const bool legacy_tool_input =
        source.view_kind == core::ViewKind::kSynthesis ||
        source.view_kind == core::ViewKind::kTiming;
    if (!source.exists || (!constraints && !legacy_tool_input)) continue;
    const std::wstring extension = LowercaseExtension(source);
    if (extension == L".sdc") {
      if (!constraints) continue;
      if (!IsNonEmptyFile(source)) {
        has_empty_sdc = true;
        continue;
      }
      sdc_candidates_.push_back(&source);
      const LRESULT index = SendMessageW(
          sdc_combo_, CB_ADDSTRING, 0,
          reinterpret_cast<LPARAM>(Utf8ToWide(source.relative_path).c_str()));
      if (source.relative_path == document_->project.timing.sdc_path)
        SendMessageW(sdc_combo_, CB_SETCURSEL, index, 0);
    } else if (extension == L".lib" || extension == L".liberty") {
      if (!IsNonEmptyFile(source)) continue;
      const std::wstring filename = LowercaseFilename(source);
      if (!IsLibraryLevel(source) && library_level_liberty.contains(filename)) {
        continue;
      }
      if (!visible_liberty.insert(filename).second) continue;
      liberty_candidates_.push_back(&source);
      std::wstring label = Utf8ToWide(source.relative_path);
      if (IsLibraryLevel(source)) {
        label += L"  [Library]";
      } else if (legacy_tool_input) {
        label += L"  [legacy managed Liberty]";
      }
      const LRESULT index =
          SendMessageW(liberty_list_, LB_ADDSTRING, 0,
                       reinterpret_cast<LPARAM>(label.c_str()));
      if (std::find(document_->project.timing.liberty_paths.begin(),
                    document_->project.timing.liberty_paths.end(),
                    source.relative_path) !=
          document_->project.timing.liberty_paths.end()) {
        SendMessageW(liberty_list_, LB_SETSEL, TRUE, index);
      }
    }
  }
  if (sdc_candidates_.empty()) {
    SendMessageW(
        sdc_combo_, CB_ADDSTRING, 0,
        reinterpret_cast<LPARAM>(
            has_empty_sdc
                ? L"Managed .sdc is empty — define clocks and constraints"
                : L"No managed .sdc file in Constraints View"));
    SendMessageW(sdc_combo_, CB_SETCURSEL, 0, 0);
  }
  if (liberty_candidates_.empty()) {
    SendMessageW(liberty_list_, LB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(
                     L"No shared .lib/.liberty file in the Library"));
  }
  const bool editable = !document_->read_only;
  EnableWindow(corner_edit_, editable);
  EnableWindow(sdc_combo_, editable);
  EnableWindow(liberty_list_, editable);
  EnableWindow(run_button_, compatibility_status_.Ok() &&
                                !sdc_candidates_.empty() &&
                                !liberty_candidates_.empty());
  SetWindowTextW(identity_, (L"OpenSTA Timing — " +
                             Utf8ToWide(document_->project.top_module))
                                .c_str());
  if (compatibility_status_.Ok()) {
    SetWindowTextW(status_, (L"Compatible synthesis: " +
                             Utf8ToWide(compatible_synthesis_run_->id))
                                .c_str());
  } else {
    SetWindowTextW(status_, Utf8ToWide(compatibility_status_.message).c_str());
  }
}

void TimingWindow::StartTiming() {
  if (!document_ || timing_service_.IsActive()) return;
  if (!document_->read_only) {
    document_->project.timing.corner_name =
        WideToUtf8(ControlText(corner_edit_));
    const LRESULT sdc_index = SendMessageW(sdc_combo_, CB_GETCURSEL, 0, 0);
    document_->project.timing.sdc_path =
        sdc_index >= 0 &&
                static_cast<std::size_t>(sdc_index) < sdc_candidates_.size()
            ? sdc_candidates_[static_cast<std::size_t>(sdc_index)]
                  ->relative_path
            : std::string{};
    document_->project.timing.liberty_paths.clear();
    for (std::size_t index = 0; index < liberty_candidates_.size(); ++index) {
      if (SendMessageW(liberty_list_, LB_GETSEL, index, 0) > 0) {
        document_->project.timing.liberty_paths.push_back(
            liberty_candidates_[index]->relative_path);
      }
    }
    const core::Status saved = project_service_.Save(document_.get());
    if (!saved.Ok()) {
      SetWindowTextW(status_, Utf8ToWide(saved.message).c_str());
      return;
    }
  }
  application::TimingRunRequest request;
  request.project = document_->project;
  request.sources = sources_;
  request.library_directory = library_.directory;
  request.cell_directory =
      library_.directory / L"cells" / Utf8ToWide(request_.cell_id);
  request.generation = ++generation_;
  {
    std::scoped_lock lock(event_channel_->mutex);
    event_channel_->generation = request.generation;
  }
  const auto channel = event_channel_;
  const core::Status started = timing_service_.Start(
      std::move(request), [channel](application::TimingRunEvent timing) {
        HWND target = nullptr;
        {
          std::scoped_lock lock(channel->mutex);
          if (!channel->window || timing.generation != channel->generation)
            return;
          EventChannel::Event event;
          event.timing = std::move(timing);
          channel->events.push_back(std::move(event));
          target = channel->window;
        }
        PostMessageW(target, kEventMessage, 0, 0);
      });
  if (!started.Ok())
    SetWindowTextW(status_, Utf8ToWide(started.message).c_str());
}

void TimingWindow::ApplyState(application::TimingRunState state) {
  const bool active = state == application::TimingRunState::kProbing ||
                      state == application::TimingRunState::kPreparing ||
                      state == application::TimingRunState::kRunning ||
                      state == application::TimingRunState::kCancelling;
  EnableWindow(run_button_, !active && compatibility_status_.Ok() &&
                                !sdc_candidates_.empty() &&
                                !liberty_candidates_.empty());
  EnableWindow(cancel_button_, active);
  std::wstring status_text;
  if (active) {
    status_text = L"OpenSTA timing analysis running...";
  } else if (state == application::TimingRunState::kFailed) {
    status_text = L"Timing analysis failed — see Problems";
  } else if (state == application::TimingRunState::kCancelled) {
    status_text = L"Timing analysis cancelled";
  } else {
    status_text = L"Timing analysis complete";
  }
  SetWindowTextW(status_, status_text.c_str());
}

void TimingWindow::AppendCentralLog(std::wstring_view text) const {
  if (!central_log_ || text.empty()) return;
  const core::Cell* cell = FindCell(library_, request_.cell_id);
  const std::wstring prefix =
      cell ? L"[Timing " + Utf8ToWide(cell->name) + L"] " : L"[Timing] ";
  central_log_(prefix + std::wstring(text));
}

void TimingWindow::AppendOutput(std::wstring_view text) {
  AppendCentralLog(text);
  const int length = GetWindowTextLengthW(output_);
  SendMessageW(output_, EM_SETSEL, length, length);
  SendMessageW(output_, EM_REPLACESEL, FALSE,
               reinterpret_cast<LPARAM>(std::wstring(text).c_str()));
}

void TimingWindow::ShowProblems() {
  TabCtrl_SetCurSel(bottom_tabs_, 0);
  ShowWindow(runs_list_, SW_HIDE);
  ShowWindow(output_, SW_SHOW);
  std::wstring text = L"Timing Problems\r\n\r\n";
  if (!compatibility_status_.Ok()) {
    text += L"TIMING-COMPATIBILITY  " +
            Utf8ToWide(compatibility_status_.message) + L"\r\n";
  }
  if (diagnostics_.empty() && compatibility_status_.Ok())
    text += L"No diagnostics.";
  for (const auto& diagnostic : diagnostics_) {
    text += Utf8ToWide(diagnostic.code) + L"  " +
            Utf8ToWide(diagnostic.message) + L"\r\n";
  }
  SetWindowTextW(output_, text.c_str());
}

void TimingWindow::ShowRuns() {
  TabCtrl_SetCurSel(bottom_tabs_, 1);
  ShowWindow(output_, SW_HIDE);
  ShowWindow(runs_list_, SW_SHOW);
}

void TimingWindow::PopulateRunHistory() {
  ListView_DeleteAllItems(runs_list_);
  const auto metric_text = [](bool has_paths, double value) {
    if (!has_paths) return std::wstring(L"N/A");
    std::wostringstream text;
    text << std::fixed << std::setprecision(6) << value << L" ns";
    return text.str();
  };
  int row = 0;
  for (const application::TimingRunSnapshot& snapshot : run_snapshots_) {
    const application::RunRecord& run = snapshot.run;
    std::wstring values[] = {
        Utf8ToWide(run.started_utc),
        Utf8ToWide(snapshot.corner),
        metric_text(snapshot.metrics.setup.has_paths,
                    snapshot.metrics.setup.wns),
        metric_text(snapshot.metrics.setup.has_paths,
                    snapshot.metrics.setup.tns),
        metric_text(snapshot.metrics.hold.has_paths, snapshot.metrics.hold.wns),
        metric_text(snapshot.metrics.hold.has_paths, snapshot.metrics.hold.tns),
        Utf8ToWide(application::RunStatusName(run.status))};
    LVITEMW item{};
    item.mask = LVIF_TEXT | LVIF_PARAM;
    item.iItem = row;
    item.lParam = static_cast<LPARAM>(row);
    item.pszText = values[0].data();
    const int inserted = ListView_InsertItem(runs_list_, &item);
    for (int column = 1; inserted >= 0 && column < 7; ++column) {
      ListView_SetItemText(runs_list_, inserted, column, values[column].data());
    }
    ++row;
  }
  if (row > 0) {
    ListView_SetItemState(runs_list_, 0, LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);
  }
}

void TimingWindow::SelectRun(std::size_t index) {
  if (index >= run_snapshots_.size()) return;
  const application::TimingRunSnapshot& snapshot = run_snapshots_[index];
  active_run_ = std::make_shared<application::RunRecord>(snapshot.run);
  metrics_ = snapshot.metrics;
  diagnostics_ = snapshot.diagnostics;
  script_text_ = snapshot.script_text;
  ShowReports();
  SetWindowTextW(
      status_,
      (L"Selected timing run " + Utf8ToWide(snapshot.run.started_utc)).c_str());
}

void TimingWindow::ShowReports() {
  adapters::OpenStaAdapter adapter;
  metrics_ = adapter.NormalizeMetrics(std::move(metrics_));
  std::wostringstream text;
  text << (compatibility_status_.Ok() ? L"OpenSTA Timing Summary"
                                      : L"Previous OpenSTA Timing Summary")
       << L"\r\n\r\nSetup (max) WNS: ";
  text << std::fixed << std::setprecision(6);
  if (metrics_.setup.has_paths) {
    text << metrics_.setup.wns << L" ns\r\nSetup (max) TNS: "
         << metrics_.setup.tns << L" ns";
  } else {
    text << L"N/A\r\nSetup (max) TNS: N/A";
  }
  text << L"\r\nMinimum (hold/removal) WNS: ";
  if (metrics_.hold.has_paths) {
    text << metrics_.hold.wns << L" ns\r\nMinimum (hold/removal) TNS: "
         << metrics_.hold.tns << L" ns";
  } else {
    text << L"N/A\r\nMinimum (hold/removal) TNS: N/A";
  }
  const bool analysis_complete =
      metrics_.setup.has_paths && metrics_.hold.has_paths;
  text << L"\r\nViolations: setup " << metrics_.violation_counts.setup
       << L" | hold " << metrics_.violation_counts.hold << L" | recovery "
       << metrics_.violation_counts.recovery << L" | removal "
       << metrics_.violation_counts.removal;
  if (metrics_.violation_counts.unknown != 0) {
    text << L" | unknown " << metrics_.violation_counts.unknown;
  }
  text << L"\r\nResult: "
       << (!analysis_complete  ? L"ANALYSIS FAILED"
           : metrics_.Passed() ? L"PASS"
                               : L"FAIL");
  if (!analysis_complete && !diagnostics_.empty()) {
    text << L"\r\nFailure: " << Utf8ToWide(diagnostics_.front().message);
  }
  if (metrics_.violations_truncated) {
    text << L" (violation table truncated at 1,000 entries)";
  }
  SetWindowTextW(summary_, text.str().c_str());
  ListView_DeleteAllItems(violations_);
  int row = 0;
  for (const auto& violation : metrics_.violations) {
    AddViolationRow(violations_, row++, violation);
  }
}

void TimingWindow::ShowArtifacts() {
  if (!active_run_) return;
  ShowWindow(runs_list_, SW_HIDE);
  ShowWindow(output_, SW_SHOW);
  std::wstring text = L"Timing Artifacts\r\n\r\n";
  for (const auto& artifact : active_run_->artifacts) {
    text += Utf8ToWide(artifact.kind) + L" / " + Utf8ToWide(artifact.format) +
            L"\r\n  " + Utf8ToWide(artifact.relative_path) + L"\r\n";
  }
  SetWindowTextW(output_, text.c_str());
}

void TimingWindow::ShowScript() {
  ShowWindow(runs_list_, SW_HIDE);
  ShowWindow(output_, SW_SHOW);
  SetWindowTextW(output_, Utf8ToWide(script_text_).c_str());
}

void TimingWindow::OpenSynthesis() {
  const core::Cell* cell = FindCell(library_, request_.cell_id);
  if (!cell || !open_view_) return;
  const auto view = std::find_if(
      cell->views.begin(), cell->views.end(), [](const auto& candidate) {
        return candidate.kind == core::ViewKind::kSynthesis;
      });
  if (view == cell->views.end()) {
    SetWindowTextW(status_, L"Create a Synthesis View first");
    return;
  }
  application::WorkspaceOpenRequest request{request_.library_id,
                                            request_.cell_id, view->id};
  if (!open_view_(request, library_))
    SetWindowTextW(status_, L"Cannot open Synthesis View");
}

void TimingWindow::ShutdownChannel() {
  if (!event_channel_) return;
  std::scoped_lock lock(event_channel_->mutex);
  event_channel_->window = nullptr;
  event_channel_->events.clear();
}

}  // namespace designpp::gui

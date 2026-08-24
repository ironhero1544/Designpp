// Copyright 2026 The Design++ Authors

#include "designpp/gui/layout_window_impl.h"

#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <utility>

#include "designpp/application/synthesis_fingerprint.h"
#include "designpp/application/toolchain_profile_store.h"

namespace designpp::gui {
namespace {

constexpr wchar_t kWindowClassName[] = L"DesignPlusPlus.LayoutWindow";
constexpr int kRunButton = 7601;
constexpr int kCancelButton = 7602;
constexpr int kResumeButton = 7603;
constexpr int kConfigButton = 7604;
constexpr int kReportsButton = 7605;
constexpr int kArtifactsButton = 7606;
constexpr int kPdkCombo = 7607;
constexpr int kSclCombo = 7608;
constexpr int kRunsList = 7610;
constexpr int kInitialWindowWidth = 1420;
constexpr int kInitialWindowHeight = 900;
constexpr int kMinimumWindowWidth = 1040;
constexpr int kMinimumWindowHeight = 680;
constexpr int kSetupOk = 7701;
constexpr int kSetupCancel = 7702;
constexpr int kSetupPdk = 7703;
constexpr int kSetupScl = 7704;
constexpr int kSetupClock = 7705;
constexpr int kSetupPeriod = 7706;
constexpr int kSetupUtilization = 7707;
constexpr int kSetupDensity = 7708;
constexpr int kSetupDieArea = 7709;
constexpr int kSetupPnrSdc = 7710;
constexpr int kSetupSignoffSdc = 7711;

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

void SetControlText(HWND control, std::string_view text) {
  const std::wstring wide = Utf8ToWide(text);
  SetWindowTextW(control, wide.c_str());
}

std::vector<std::string> Split(std::string_view value, char delimiter) {
  std::vector<std::string> result;
  std::size_t begin = 0;
  while (begin <= value.size()) {
    const std::size_t end = value.find(delimiter, begin);
    std::string item(value.substr(begin, end - begin));
    const std::size_t first = item.find_first_not_of(" \t\r\n");
    const std::size_t last = item.find_last_not_of(" \t\r\n");
    if (first != std::string::npos) {
      result.push_back(item.substr(first, last - first + 1));
    }
    if (end == std::string_view::npos) break;
    begin = end + 1;
  }
  return result;
}

std::string Join(const std::vector<std::string>& values,
                 std::string_view delimiter) {
  std::ostringstream output;
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (index != 0) output << delimiter;
    output << values[index];
  }
  return output.str();
}

std::optional<std::string> JsonString(std::string_view json,
                                      std::string_view key) {
  const std::string token = "\"" + std::string(key) + "\"";
  const std::size_t key_position = json.find(token);
  if (key_position == std::string_view::npos) return std::nullopt;
  const std::size_t colon = json.find(':', key_position + token.size());
  const std::size_t quote = json.find('"', colon + 1);
  if (colon == std::string_view::npos || quote == std::string_view::npos) {
    return std::nullopt;
  }
  std::string result;
  bool escaped = false;
  for (std::size_t index = quote + 1; index < json.size(); ++index) {
    const char character = json[index];
    if (escaped) {
      result.push_back(character);
      escaped = false;
    } else if (character == '\\') {
      escaped = true;
    } else if (character == '"') {
      return result;
    } else {
      result.push_back(character);
    }
  }
  return std::nullopt;
}

std::optional<double> JsonNumber(std::string_view json, std::string_view key) {
  const std::string token = "\"" + std::string(key) + "\"";
  const std::size_t key_position = json.find(token);
  const std::size_t colon = key_position == std::string_view::npos
                                ? std::string_view::npos
                                : json.find(':', key_position + token.size());
  if (colon == std::string_view::npos) return std::nullopt;
  const char* begin = json.data() + colon + 1;
  char* end = nullptr;
  const double value = std::strtod(begin, &end);
  return end != begin ? std::optional<double>{value} : std::nullopt;
}

adapters::ManagedFlowMetrics LoadRunMetrics(const application::RunRecord& run) {
  adapters::ManagedFlowMetrics metrics;
  if (run.outcome.summary_relative_path.empty()) return metrics;
  std::ifstream input(run.directory / run.outcome.summary_relative_path,
                      std::ios::binary);
  if (!input) return metrics;
  std::ostringstream output;
  output << input.rdbuf();
  const std::string json = output.str();
  metrics.core_area = JsonNumber(json, "core_area");
  metrics.die_area = JsonNumber(json, "die_area");
  metrics.utilization = JsonNumber(json, "utilization");
  metrics.instance_count = JsonNumber(json, "instance_count");
  metrics.setup_wns = JsonNumber(json, "setup_wns");
  metrics.setup_tns = JsonNumber(json, "setup_tns");
  metrics.hold_wns = JsonNumber(json, "hold_wns");
  metrics.hold_tns = JsonNumber(json, "hold_tns");
  metrics.drc_violations = JsonNumber(json, "drc_violations");
  metrics.lvs_errors = JsonNumber(json, "lvs_errors");
  return metrics;
}

std::optional<application::ManagedFlowResumeRequest> LoadResume(
    const application::RunRecord& run) {
  if ((run.stage != "physical_implementation" &&
       run.stage != "physical_design") ||
      (run.status != application::RunStatus::kFailed &&
       run.status != application::RunStatus::kInterrupted) ||
      run.outcome.summary_relative_path.empty()) {
    return std::nullopt;
  }
  std::ifstream input(run.directory / run.outcome.summary_relative_path,
                      std::ios::binary);
  if (!input) return std::nullopt;
  std::ostringstream output;
  output << input.rdbuf();
  const std::string json = output.str();
  auto lineage = JsonString(json, "lineage_id");
  auto step = JsonString(json, "last_step");
  auto fingerprint = JsonString(json, "configuration_fingerprint");
  if (!lineage || lineage->empty() || !step || step->empty() || !fingerprint ||
      fingerprint->empty()) {
    return std::nullopt;
  }
  application::ManagedFlowResumeRequest resume;
  resume.parent_run_id = run.id;
  resume.lineage_id = std::move(*lineage);
  resume.resume_step = std::move(*step);
  resume.configuration_fingerprint = std::move(*fingerprint);
  const std::filesystem::path checkpoint =
      run.directory / "artifacts" / "checkpoint-state.json";
  auto hash = application::CalculateFileSha256(checkpoint);
  if (!hash.Ok()) return std::nullopt;
  resume.checkpoint_hash = std::move(hash).Value();
  return resume;
}

void AddColumn(HWND list, int index, int width, const wchar_t* heading) {
  LVCOLUMNW column{};
  column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
  column.iSubItem = index;
  column.cx = width;
  column.pszText = const_cast<wchar_t*>(heading);
  ListView_InsertColumn(list, index, &column);
}

std::wstring Metric(const std::optional<double>& value) {
  if (!value) return L"N/A";
  std::wostringstream output;
  output << std::fixed << std::setprecision(4) << *value;
  return output.str();
}

std::filesystem::path FindGds(const application::RunRecord& run) {
  for (const application::RunArtifact& artifact : run.artifacts) {
    if (artifact.partial || artifact.kind != "final.gds") continue;
    const std::filesystem::path path = run.directory / artifact.relative_path;
    std::error_code error;
    if (std::filesystem::is_regular_file(path, error) &&
        std::filesystem::file_size(path, error) > 0) {
      return path;
    }
  }
  return {};
}

struct LayoutSetupDialogState {
  core::PhysicalImplementationConfiguration configuration;
  const std::vector<const application::ResolvedSource*>* sdc_candidates =
      nullptr;
  bool accepted = false;
};

LRESULT CALLBACK LayoutSetupProcedure(HWND window, UINT message, WPARAM wparam,
                                      LPARAM lparam) {
  auto* state = reinterpret_cast<LayoutSetupDialogState*>(
      GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    state = static_cast<LayoutSetupDialogState*>(create->lpCreateParams);
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
  }
  if (state == nullptr) return DefWindowProcW(window, message, wparam, lparam);
  if (message == WM_CREATE) {
    const HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    const auto control = [window, font](const wchar_t* cls, const wchar_t* text,
                                        DWORD style, int id, int x, int y,
                                        int width, int height) {
      HWND result = CreateWindowExW(
          0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, width, height,
          window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
          GetModuleHandleW(nullptr), nullptr);
      SendMessageW(result, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
      return result;
    };
    const wchar_t* labels[] = {L"PDK",
                               L"Standard cell library",
                               L"Clock ports (;)",
                               L"Period (ns)",
                               L"Core utilization (%)",
                               L"Placement density (%)",
                               L"Die area",
                               L"PNR SDC",
                               L"Signoff SDC"};
    for (int index = 0; index < 9; ++index) {
      control(L"STATIC", labels[index], SS_LEFT, 0, 14, 18 + index * 38, 165,
              24);
    }
    control(L"EDIT", Utf8ToWide(state->configuration.pdk).c_str(),
            WS_BORDER | ES_AUTOHSCROLL, kSetupPdk, 182, 14, 330, 27);
    control(L"EDIT",
            Utf8ToWide(state->configuration.standard_cell_library).c_str(),
            WS_BORDER | ES_AUTOHSCROLL, kSetupScl, 182, 52, 330, 27);
    control(L"EDIT",
            Utf8ToWide(Join(state->configuration.clock_ports, ";")).c_str(),
            WS_BORDER | ES_AUTOHSCROLL, kSetupClock, 182, 90, 330, 27);
    control(L"EDIT", Utf8ToWide(state->configuration.clock_period_ns).c_str(),
            WS_BORDER | ES_AUTOHSCROLL, kSetupPeriod, 182, 128, 330, 27);
    control(
        L"EDIT",
        std::to_wstring(state->configuration.core_utilization_percent).c_str(),
        WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL, kSetupUtilization, 182, 166,
        330, 27);
    control(
        L"EDIT",
        Utf8ToWide(state->configuration.placement_density_percent.value_or(""))
            .c_str(),
        WS_BORDER | ES_AUTOHSCROLL, kSetupDensity, 182, 204, 330, 27);
    control(L"EDIT",
            Utf8ToWide(Join(state->configuration.die_area, ",")).c_str(),
            WS_BORDER | ES_AUTOHSCROLL, kSetupDieArea, 182, 242, 330, 27);
    HWND pnr = control(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL,
                       kSetupPnrSdc, 182, 280, 330, 180);
    HWND signoff = control(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL,
                           kSetupSignoffSdc, 182, 318, 330, 180);
    for (HWND combo : {pnr, signoff}) {
      SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"(None)"));
    }
    for (std::size_t index = 0; index < state->sdc_candidates->size();
         ++index) {
      const std::wstring label =
          Utf8ToWide((*state->sdc_candidates)[index]->relative_path);
      for (HWND combo : {pnr, signoff}) {
        SendMessageW(combo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(label.c_str()));
      }
      if ((*state->sdc_candidates)[index]->relative_path ==
          state->configuration.pnr_sdc_path) {
        SendMessageW(pnr, CB_SETCURSEL, index + 1, 0);
      }
      if ((*state->sdc_candidates)[index]->relative_path ==
          state->configuration.signoff_sdc_path) {
        SendMessageW(signoff, CB_SETCURSEL, index + 1, 0);
      }
    }
    if (SendMessageW(pnr, CB_GETCURSEL, 0, 0) == CB_ERR) {
      SendMessageW(pnr, CB_SETCURSEL, 0, 0);
    }
    if (SendMessageW(signoff, CB_GETCURSEL, 0, 0) == CB_ERR) {
      SendMessageW(signoff, CB_SETCURSEL, 0, 0);
    }
    control(L"BUTTON", L"Save", BS_DEFPUSHBUTTON, kSetupOk, 316, 365, 94, 31);
    control(L"BUTTON", L"Cancel", BS_PUSHBUTTON, kSetupCancel, 418, 365, 94,
            31);
    return 0;
  }
  if (message == WM_COMMAND && LOWORD(wparam) == kSetupOk) {
    state->configuration.pdk =
        WideToUtf8(ControlText(GetDlgItem(window, kSetupPdk)));
    state->configuration.standard_cell_library =
        WideToUtf8(ControlText(GetDlgItem(window, kSetupScl)));
    state->configuration.clock_ports =
        Split(WideToUtf8(ControlText(GetDlgItem(window, kSetupClock))), ';');
    state->configuration.clock_period_ns =
        WideToUtf8(ControlText(GetDlgItem(window, kSetupPeriod)));
    const std::string utilization =
        WideToUtf8(ControlText(GetDlgItem(window, kSetupUtilization)));
    state->configuration.core_utilization_percent = static_cast<std::uint32_t>(
        std::strtoul(utilization.c_str(), nullptr, 10));
    const std::string density =
        WideToUtf8(ControlText(GetDlgItem(window, kSetupDensity)));
    state->configuration.placement_density_percent =
        density.empty() ? std::optional<std::string>{}
                        : std::optional<std::string>{density};
    state->configuration.die_area =
        Split(WideToUtf8(ControlText(GetDlgItem(window, kSetupDieArea))), ',');
    const auto selected_path = [state, window](int id) {
      const LRESULT selected =
          SendDlgItemMessageW(window, id, CB_GETCURSEL, 0, 0);
      return selected > 0 && static_cast<std::size_t>(selected) <=
                                 state->sdc_candidates->size()
                 ? (*state->sdc_candidates)[static_cast<std::size_t>(selected -
                                                                     1)]
                       ->relative_path
                 : std::string{};
    };
    state->configuration.pnr_sdc_path = selected_path(kSetupPnrSdc);
    state->configuration.signoff_sdc_path = selected_path(kSetupSignoffSdc);
    core::Project validation_project;
    validation_project.id = "setup";
    validation_project.library_id = "setup";
    validation_project.cell_id = "setup";
    validation_project.name = "setup";
    validation_project.top_module = "setup";
    validation_project.created_utc = "setup";
    validation_project.modified_utc = "setup";
    validation_project.physical_implementation = state->configuration;
    if (!core::ValidateProject(validation_project).Ok()) {
      MessageBoxW(window, L"The physical implementation setup is invalid.",
                  L"Layout Setup", MB_OK | MB_ICONERROR);
      return 0;
    }
    state->accepted = true;
    DestroyWindow(window);
    return 0;
  }
  if ((message == WM_COMMAND && LOWORD(wparam) == kSetupCancel) ||
      message == WM_CLOSE) {
    DestroyWindow(window);
    return 0;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

bool ShowLayoutSetupDialog(
    HWND owner, HINSTANCE instance,
    core::PhysicalImplementationConfiguration* configuration,
    const std::vector<const application::ResolvedSource*>& sdc_candidates) {
  constexpr wchar_t kSetupClass[] = L"DesignPlusPlus.LayoutSetupDialog";
  WNDCLASSEXW window_class{sizeof(window_class)};
  window_class.lpfnWndProc = LayoutSetupProcedure;
  window_class.hInstance = instance;
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  window_class.lpszClassName = kSetupClass;
  if (!RegisterClassExW(&window_class) &&
      GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return false;
  }
  LayoutSetupDialogState state{*configuration, &sdc_candidates, false};
  RECT owner_rect{};
  GetWindowRect(owner, &owner_rect);
  HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME, kSetupClass,
                                L"Layout Setup", WS_CAPTION | WS_SYSMENU,
                                owner_rect.left + 60, owner_rect.top + 60, 545,
                                445, owner, nullptr, instance, &state);
  if (!dialog) return false;
  EnableWindow(owner, FALSE);
  ShowWindow(dialog, SW_SHOW);
  MSG message{};
  while (IsWindow(dialog) && GetMessageW(&message, nullptr, 0, 0) > 0) {
    if (!IsDialogMessageW(dialog, &message)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }
  EnableWindow(owner, TRUE);
  SetForegroundWindow(owner);
  if (state.accepted) *configuration = std::move(state.configuration);
  return state.accepted;
}

}  // namespace

struct LayoutWindowImplementation::EventChannel final {
  std::mutex mutex;
  HWND window = nullptr;
  std::uint64_t generation = 0;
  struct Event {
    bool loaded = false;
    bool saved = false;
    bool generate_after_save = false;
    bool discovered = false;
    bool resume = false;
    core::Status status;
    std::shared_ptr<application::ProjectDocument> document;
    std::vector<application::ResolvedSource> sources;
    core::ToolchainProfile profile;
    std::vector<application::RunRecord> runs;
    std::vector<adapters::ManagedFlowMetrics> run_metrics;
    std::vector<std::optional<application::ManagedFlowResumeRequest>> resumes;
    application::PhysicalImplementationEvent flow;
    application::LayoutViewerEvent viewer;
    application::OpenLaneDiscoveryResult discovery;
  };
  std::deque<Event> events;
  bool message_posted = false;
};

LayoutWindowImplementation::~LayoutWindowImplementation() { Close(); }

bool LayoutWindowImplementation::Create(
    HINSTANCE instance, const application::WorkspaceOpenRequest& request,
    application::LibraryRecord library, ViewWindowLogCallback central_log,
    ViewWindowLibraryChangedCallback library_changed) {
  const auto cell =
      std::find_if(library.library.cells.begin(), library.library.cells.end(),
                   [&request](const core::Cell& candidate) {
                     return candidate.id == request.cell_id;
                   });
  if (cell == library.library.cells.end()) return false;
  const auto view = std::find_if(cell->views.begin(), cell->views.end(),
                                 [&request](const core::View& candidate) {
                                   return candidate.id == request.view_id;
                                 });
  if (view == cell->views.end() || view->kind != core::ViewKind::kLayout) {
    return false;
  }
  instance_ = instance;
  request_ = request;
  view_kind_ = view->kind;
  library_ = std::move(library);
  central_log_ = std::move(central_log);
  library_changed_ = std::move(library_changed);
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
      CreateWindowExW(0, kWindowClassName, L"Design++ Layout",
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

ViewWindowKind LayoutWindowImplementation::Kind() const noexcept {
  return ViewWindowKind::kLayout;
}

bool LayoutWindowImplementation::CanActivate(
    const application::WorkspaceOpenRequest& request,
    core::ViewKind view_kind) const {
  return view_kind == view_kind_ &&
         MatchesCell(request.library_id, request.cell_id);
}

void LayoutWindowImplementation::Activate(
    const application::WorkspaceOpenRequest& request) {
  if (!CanActivate(request, view_kind_) || !window_) return;
  ShowWindow(window_, SW_RESTORE);
  SetForegroundWindow(window_);
}

bool LayoutWindowImplementation::BelongsToLibrary(
    std::string_view library_id) const {
  return request_.library_id == library_id;
}

bool LayoutWindowImplementation::MatchesCell(std::string_view library_id,
                                             std::string_view cell_id) const {
  return request_.library_id == library_id && request_.cell_id == cell_id;
}

bool LayoutWindowImplementation::IsOpen() const noexcept {
  return window_ != nullptr;
}

void LayoutWindowImplementation::RefreshLibrary(
    application::LibraryRecord library) {
  if (library.library.id != request_.library_id) return;
  library_ = std::move(library);
  ++generation_;
  {
    std::scoped_lock lock(event_channel_->mutex);
    event_channel_->generation = generation_;
  }
  BeginLoad();
}

bool LayoutWindowImplementation::PrepareClose() {
  flow_service_.Cancel();
  viewer_service_.Cancel();
  return true;
}

void LayoutWindowImplementation::Close() {
  if (!window_) return;
  flow_service_.Shutdown();
  viewer_service_.Shutdown();
  discovery_service_.Cancel();
  scheduler_.RequestStop();
  ShutdownChannel();
  DestroyWindow(window_);
  window_ = nullptr;
}

bool LayoutWindowImplementation::TranslateAccelerator(const MSG&) {
  return false;
}

LRESULT CALLBACK LayoutWindowImplementation::WindowProcedure(HWND window,
                                                             UINT message,
                                                             WPARAM wparam,
                                                             LPARAM lparam) {
  LayoutWindowImplementation* self =
      reinterpret_cast<LayoutWindowImplementation*>(
          GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<LayoutWindowImplementation*>(create->lpCreateParams);
    self->window_ = window;
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  }
  return self ? self->HandleMessage(message, wparam, lparam)
              : DefWindowProcW(window, message, wparam, lparam);
}

LRESULT LayoutWindowImplementation::HandleMessage(UINT message, WPARAM wparam,
                                                  LPARAM lparam) {
  switch (message) {
    case WM_CREATE:
      dpi_ = GetDpiForWindow(window_);
      return CreateControls() ? 0 : -1;
    case WM_SIZE:
      LayoutControls(LOWORD(lparam), HIWORD(lparam));
      return 0;
    case WM_PAINT: {
      PAINTSTRUCT paint{};
      BeginPaint(window_, &paint);
      EndPaint(window_, &paint);
      return 0;
    }
    case WM_GETMINMAXINFO: {
      auto* information = reinterpret_cast<MINMAXINFO*>(lparam);
      information->ptMinTrackSize = {ScaleForDpi(kMinimumWindowWidth, dpi_),
                                     ScaleForDpi(kMinimumWindowHeight, dpi_)};
      return 0;
    }
    case WM_COMMAND:
      if (LOWORD(wparam) == kPdkCombo && HIWORD(wparam) == CBN_SELCHANGE) {
        PopulateStandardCellLibraries();
        return 0;
      }
      switch (LOWORD(wparam)) {
        case kRunButton:
          SaveAndStart(false);
          return 0;
        case kCancelButton:
          flow_service_.Cancel();
          return 0;
        case kResumeButton:
          OpenLayout();
          return 0;
        case kConfigButton:
          EditSetup();
          return 0;
        case kReportsButton:
          ShowReports();
          return 0;
        case kArtifactsButton:
          ShowArtifacts();
          return 0;
      }
      break;
    case WM_NOTIFY:
      if (reinterpret_cast<NMHDR*>(lparam)->hwndFrom == runs_list_ &&
          reinterpret_cast<NMHDR*>(lparam)->code == LVN_ITEMCHANGED) {
        SelectRun(ListView_GetNextItem(runs_list_, -1, LVNI_SELECTED));
      }
      return 0;
    case kEventMessage:
      HandleEvents();
      return 0;
    case WM_CLOSE:
      Close();
      return 0;
    case WM_NCDESTROY: {
      const HWND destroyed = window_;
      window_ = nullptr;
      return DefWindowProcW(destroyed, message, wparam, lparam);
    }
  }
  return DefWindowProcW(window_, message, wparam, lparam);
}

bool LayoutWindowImplementation::CreateControls() {
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
  identity_ = create(L"STATIC", L"Layout", SS_LEFT, 0);
  run_button_ =
      create(L"BUTTON", L"Generate / Update Layout", BS_PUSHBUTTON, kRunButton);
  cancel_button_ = create(L"BUTTON", L"Cancel", BS_PUSHBUTTON, kCancelButton);
  resume_button_ =
      create(L"BUTTON", L"Open Layout", BS_PUSHBUTTON, kResumeButton);
  config_button_ = create(L"BUTTON", L"Setup", BS_PUSHBUTTON, kConfigButton);
  reports_button_ =
      create(L"BUTTON", L"Reports", BS_PUSHBUTTON, kReportsButton);
  artifacts_button_ =
      create(L"BUTTON", L"Artifacts", BS_PUSHBUTTON, kArtifactsButton);
  pdk_edit_ = create(WC_COMBOBOXW, L"sky130A",
                     WS_BORDER | CBS_DROPDOWN | WS_VSCROLL, kPdkCombo);
  scl_edit_ = create(WC_COMBOBOXW, L"sky130_fd_sc_hd",
                     WS_BORDER | CBS_DROPDOWN | WS_VSCROLL, kSclCombo);
  clocks_edit_ = create(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, 0);
  period_edit_ = create(L"EDIT", L"10.0", WS_BORDER | ES_AUTOHSCROLL, 0);
  utilization_edit_ =
      create(L"EDIT", L"40", WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL, 0);
  density_edit_ = create(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, 0);
  die_area_edit_ = create(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, 0);
  pnr_sdc_combo_ = create(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL, 0);
  signoff_sdc_combo_ =
      create(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL, 0);
  advanced_edit_ =
      create(L"EDIT", L"{}",
             WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL, 0);
  stages_list_ = create(L"LISTBOX", L"", WS_BORDER | WS_VSCROLL, 0);
  summary_ = create(L"EDIT", L"No compatible layout result",
                    WS_BORDER | ES_MULTILINE | ES_READONLY | WS_VSCROLL, 0);
  runs_list_ = create(
      WC_LISTVIEWW, L"",
      WS_BORDER | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, kRunsList);
  ListView_SetExtendedListViewStyle(runs_list_, LVS_EX_FULLROWSELECT |
                                                    LVS_EX_GRIDLINES |
                                                    LVS_EX_DOUBLEBUFFER);
  AddColumn(runs_list_, 0, 150, L"Started");
  AddColumn(runs_list_, 1, 95, L"Status");
  AddColumn(runs_list_, 2, 190, L"Run ID");
  output_ = create(
      L"EDIT", L"",
      WS_BORDER | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, 0);
  status_ = create(L"STATIC", L"Loading layout state...", SS_LEFT, 0);
  EnableWindow(cancel_button_, FALSE);
  EnableWindow(resume_button_, FALSE);
  for (HWND hidden :
       {pdk_edit_, scl_edit_, clocks_edit_, period_edit_, utilization_edit_,
        density_edit_, die_area_edit_, pnr_sdc_combo_, signoff_sdc_combo_,
        advanced_edit_, stages_list_, output_}) {
    ShowWindow(hidden, SW_HIDE);
  }
  return identity_ && run_button_ && cancel_button_ && resume_button_ &&
         config_button_ && reports_button_ && artifacts_button_ && pdk_edit_ &&
         scl_edit_ && clocks_edit_ && period_edit_ && utilization_edit_ &&
         density_edit_ && die_area_edit_ && pnr_sdc_combo_ &&
         signoff_sdc_combo_ && advanced_edit_ && stages_list_ && summary_ &&
         runs_list_ && output_ && status_;
}

void LayoutWindowImplementation::LayoutControls(int width, int height) {
  const auto px = [this](int value) {
    return MulDiv(value, static_cast<int>(dpi_), 96);
  };
  const int gap = px(4);
  const int header = px(44);
  MoveWindow(identity_, px(8), px(9), px(210), px(26), TRUE);
  HWND buttons[] = {run_button_,    cancel_button_,  resume_button_,
                    config_button_, reports_button_, artifacts_button_};
  int x = px(220);
  const int button_width = std::max(px(100), (width - x - px(8) - gap * 5) / 6);
  for (HWND button : buttons) {
    MoveWindow(button, x, px(6), button_width, px(31), TRUE);
    x += button_width + gap;
  }
  const int summary_height = std::min(px(250), std::max(px(150), height / 3));
  MoveWindow(summary_, px(8), header, width - px(16), summary_height, TRUE);
  MoveWindow(runs_list_, px(8), header + summary_height + gap, width - px(16),
             std::max(px(100), height - header - summary_height - gap - px(30)),
             TRUE);
  MoveWindow(status_, 0, height - px(22), width, px(22), TRUE);
}

void LayoutWindowImplementation::BeginLoad() {
  const auto channel = event_channel_;
  const auto library = library_;
  const std::string cell_id = request_.cell_id;
  const std::uint64_t generation = generation_;
  static_cast<void>(scheduler_.Submit([channel, library, cell_id,
                                       generation](std::stop_token token) {
    if (token.stop_requested()) return;
    EventChannel::Event event;
    event.loaded = true;
    application::ProjectService projects;
    auto opened = projects.OpenOrCreate(library, cell_id);
    if (!opened.Ok()) {
      event.status = opened.GetStatus();
    } else {
      event.document = std::make_shared<application::ProjectDocument>(
          std::move(opened).Value());
      auto sources =
          projects.ResolveSources(library, cell_id, event.document->project);
      if (!sources.Ok()) {
        event.status = sources.GetStatus();
      } else {
        event.sources = std::move(sources).Value();
        application::ToolchainProfileStore profiles;
        auto settings = profiles.LoadOrCreateDefaults();
        if (!settings.Ok()) {
          event.status = settings.GetStatus();
        } else {
          const core::ToolchainSettings& values = settings.Value();
          const auto selected =
              std::find_if(values.profiles.begin(), values.profiles.end(),
                           [&values](const core::ToolchainProfile& profile) {
                             return profile.id == values.selected_profile_id;
                           });
          if (selected == values.profiles.end()) {
            event.status = {core::ErrorCode::kNotFound,
                            "Selected Toolchain Profile is unavailable", 0};
          } else {
            event.profile = *selected;
            application::RunStore store;
            const std::filesystem::path cell_directory =
                library.directory / L"cells" / Utf8ToWide(cell_id);
            const core::Status recovery =
                store.RecoverInterrupted(cell_directory);
            if (!recovery.Ok()) {
              event.status = recovery;
            } else {
              auto runs = store.List(cell_directory);
              if (runs.Ok()) {
                std::vector<application::RunRecord> loaded_runs =
                    std::move(runs).Value();
                for (application::RunRecord& run : loaded_runs) {
                  if (run.stage != "physical_implementation" &&
                      run.stage != "physical_design") {
                    continue;
                  }
                  event.resumes.push_back(LoadResume(run));
                  event.run_metrics.push_back(LoadRunMetrics(run));
                  event.runs.push_back(std::move(run));
                }
              } else {
                event.status = runs.GetStatus();
              }
            }
          }
        }
      }
    }
    HWND target = nullptr;
    bool post = false;
    {
      std::scoped_lock lock(channel->mutex);
      if (channel->generation != generation || !channel->window) return;
      channel->events.push_back(std::move(event));
      target = channel->window;
      post = !channel->message_posted;
      channel->message_posted = true;
    }
    if (post) PostMessageW(target, kEventMessage, 0, 0);
  }));
}

void LayoutWindowImplementation::HandleEvents() {
  std::deque<EventChannel::Event> events;
  {
    std::scoped_lock lock(event_channel_->mutex);
    events.swap(event_channel_->events);
    event_channel_->message_posted = false;
  }
  for (EventChannel::Event& event : events) {
    if (event.loaded) {
      if (!event.status.Ok()) {
        SetControlText(status_, event.status.message);
        EnableWindow(run_button_, FALSE);
        continue;
      }
      document_ = std::move(event.document);
      sources_ = std::move(event.sources);
      profile_ = std::move(event.profile);
      runs_ = std::move(event.runs);
      run_metrics_ = std::move(event.run_metrics);
      resume_candidates_ = std::move(event.resumes);
      PopulateConfiguration();
      PopulateRuns();
      if (!runs_.empty()) SelectRun(0);
      SetWindowTextW(status_, L"Ready to generate or update the layout");
      EnableWindow(run_button_, document_ && !document_->read_only);
      const auto channel = event_channel_;
      const std::uint64_t generation = generation_;
      core::Status discovery_started = discovery_service_.Start(
          profile_, generation,
          [channel, generation](application::OpenLaneDiscoveryResult result) {
            HWND target = nullptr;
            bool post = false;
            {
              std::scoped_lock lock(channel->mutex);
              if (channel->generation != generation || !channel->window) return;
              EventChannel::Event event;
              event.discovered = true;
              event.discovery = std::move(result);
              channel->events.push_back(std::move(event));
              target = channel->window;
              post = !channel->message_posted;
              channel->message_posted = true;
            }
            if (post) PostMessageW(target, kEventMessage, 0, 0);
          });
      if (!discovery_started.Ok()) {
        SetControlText(status_, discovery_started.message);
      }
      continue;
    }
    if (event.discovered) {
      if (event.discovery.status.Ok()) {
        pdk_candidates_ = std::move(event.discovery.candidates);
        std::vector<std::string> pdks;
        for (const auto& candidate : pdk_candidates_) {
          if (std::find(pdks.begin(), pdks.end(), candidate.pdk) ==
              pdks.end()) {
            pdks.push_back(candidate.pdk);
          }
        }
        const std::wstring selected_pdk = ControlText(pdk_edit_);
        SendMessageW(pdk_edit_, CB_RESETCONTENT, 0, 0);
        for (const std::string& pdk : pdks) {
          const std::wstring value = Utf8ToWide(pdk);
          SendMessageW(pdk_edit_, CB_ADDSTRING, 0,
                       reinterpret_cast<LPARAM>(value.c_str()));
        }
        SetWindowTextW(pdk_edit_, selected_pdk.c_str());
        PopulateStandardCellLibraries();
      } else {
        SetControlText(status_, event.discovery.status.message);
      }
      continue;
    }
    if (event.saved) {
      document_ = std::move(event.document);
      if (!event.status.Ok()) {
        SetControlText(status_, event.status.message);
        ApplyState(application::ManagedFlowRunState::kFailed);
      } else if (event.generate_after_save) {
        StartPreparedRun(event.resume);
      } else {
        PopulateConfiguration();
        ApplyState(application::ManagedFlowRunState::kIdle);
        SetWindowTextW(status_, L"Layout setup saved");
      }
      continue;
    }
    if (event.viewer.completed || !event.viewer.output.empty()) {
      if (!event.viewer.output.empty() && central_log_) {
        central_log_(Utf8ToWide(event.viewer.output));
      }
      if (event.viewer.completed)
        SetControlText(status_, event.viewer.status.message);
      continue;
    }
    const application::PhysicalImplementationEvent& flow = event.flow;
    if (flow.kind ==
        application::PhysicalImplementationEventKind::kStateChanged) {
      ApplyState(flow.state);
    } else if (flow.kind ==
               application::PhysicalImplementationEventKind::kOutput) {
      AppendOutput(Utf8ToWide(flow.output));
      if (central_log_) central_log_(Utf8ToWide(flow.output));
    } else if (flow.kind ==
               application::PhysicalImplementationEventKind::kProgress) {
      const std::wstring step = Utf8ToWide(flow.progress.step_id);
      SendMessageW(stages_list_, LB_ADDSTRING, 0,
                   reinterpret_cast<LPARAM>(step.c_str()));
      SetControlText(status_, flow.progress.message);
    } else if (flow.kind ==
               application::PhysicalImplementationEventKind::kCompleted) {
      active_run_ = flow.run;
      metrics_ = flow.metrics;
      ApplyState(flow.state);
      std::wostringstream summary;
      summary << L"Layout Summary\r\n\r\n"
              << L"Generated: "
              << (flow.run ? Utf8ToWide(flow.run->finished_utc) : L"N/A")
              << L"\r\nBackend result: "
              << (flow.status.Ok() ? L"PASS" : L"FAILED") << L"\r\n"
              << L"Die area: " << Metric(metrics_.die_area) << L"\r\n"
              << L"Core area: " << Metric(metrics_.core_area) << L"\r\n"
              << L"Utilization: " << Metric(metrics_.utilization) << L"\r\n"
              << L"Instances: " << Metric(metrics_.instance_count) << L"\r\n"
              << L"Setup WNS/TNS: " << Metric(metrics_.setup_wns) << L" / "
              << Metric(metrics_.setup_tns) << L"\r\n"
              << L"Hold WNS/TNS: " << Metric(metrics_.hold_wns) << L" / "
              << Metric(metrics_.hold_tns) << L"\r\n"
              << L"Wire length: " << Metric(metrics_.wire_length) << L"\r\n"
              << L"DRC/LVS: " << Metric(metrics_.drc_violations) << L" / "
              << Metric(metrics_.lvs_errors) << L"\r\n"
              << L"Timing setup/hold WNS: " << Metric(metrics_.setup_wns)
              << L" / " << Metric(metrics_.hold_wns);
      SetWindowTextW(summary_, summary.str().c_str());
      SetControlText(status_, flow.reused
                                  ? "Compatible GDS is already up to date"
                              : flow.status.Ok() ? "Layout generation completed"
                                                 : flow.status.message);
      MergeCompletedRun(flow.run);
    }
  }
}

void LayoutWindowImplementation::PopulateStandardCellLibraries() {
  const std::string selected_pdk = WideToUtf8(ControlText(pdk_edit_));
  const std::wstring selected_library = ControlText(scl_edit_);
  SendMessageW(scl_edit_, CB_RESETCONTENT, 0, 0);
  for (const auto& candidate : pdk_candidates_) {
    if (candidate.pdk != selected_pdk) continue;
    const std::wstring value = Utf8ToWide(candidate.standard_cell_library);
    SendMessageW(scl_edit_, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(value.c_str()));
  }
  SetWindowTextW(scl_edit_, selected_library.c_str());
}

void LayoutWindowImplementation::PopulateConfiguration() {
  if (!document_) return;
  const core::PhysicalImplementationConfiguration& config =
      document_->project.physical_implementation;
  SetControlText(pdk_edit_, config.pdk);
  SetControlText(scl_edit_, config.standard_cell_library);
  SetControlText(clocks_edit_, Join(config.clock_ports, ";"));
  SetControlText(period_edit_, config.clock_period_ns);
  SetControlText(utilization_edit_,
                 std::to_string(config.core_utilization_percent));
  SetControlText(density_edit_, config.placement_density_percent.value_or(""));
  SetControlText(die_area_edit_, Join(config.die_area, ","));
  SetControlText(advanced_edit_, config.advanced_overrides_json);
  SendMessageW(pnr_sdc_combo_, CB_RESETCONTENT, 0, 0);
  SendMessageW(signoff_sdc_combo_, CB_RESETCONTENT, 0, 0);
  SendMessageW(pnr_sdc_combo_, CB_ADDSTRING, 0,
               reinterpret_cast<LPARAM>(L"(None)"));
  SendMessageW(signoff_sdc_combo_, CB_ADDSTRING, 0,
               reinterpret_cast<LPARAM>(L"(None)"));
  sdc_candidates_.clear();
  for (const application::ResolvedSource& source : sources_) {
    std::wstring extension = source.windows_path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   ::towlower);
    if (!source.exists || source.view_kind != core::ViewKind::kConstraints ||
        extension != L".sdc") {
      continue;
    }
    sdc_candidates_.push_back(&source);
    const std::wstring label = Utf8ToWide(source.relative_path);
    SendMessageW(pnr_sdc_combo_, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(label.c_str()));
    SendMessageW(signoff_sdc_combo_, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(label.c_str()));
  }
  auto select = [this](HWND combo, const std::string& relative) {
    int index = 0;
    for (std::size_t candidate = 0; candidate < sdc_candidates_.size();
         ++candidate) {
      if (sdc_candidates_[candidate]->relative_path == relative) {
        index = static_cast<int>(candidate + 1);
        break;
      }
    }
    SendMessageW(combo, CB_SETCURSEL, index, 0);
  };
  select(pnr_sdc_combo_, config.pnr_sdc_path);
  select(signoff_sdc_combo_, config.signoff_sdc_path);
}

void LayoutWindowImplementation::EditSetup() {
  if (!document_ || document_->read_only || flow_service_.IsActive()) return;
  core::PhysicalImplementationConfiguration configuration =
      document_->project.physical_implementation;
  if (!ShowLayoutSetupDialog(window_, instance_, &configuration,
                             sdc_candidates_)) {
    return;
  }
  document_->project.physical_implementation = std::move(configuration);
  SaveSetup();
}

void LayoutWindowImplementation::SaveSetup() {
  ApplyState(application::ManagedFlowRunState::kPreparing);
  const auto channel = event_channel_;
  const auto document = document_;
  const std::uint64_t generation = generation_;
  static_cast<void>(
      scheduler_.Submit([channel, document, generation](std::stop_token token) {
        EventChannel::Event event;
        event.saved = true;
        event.generate_after_save = false;
        event.document = document;
        if (token.stop_requested()) {
          event.status = {core::ErrorCode::kCancelled,
                          "Layout setup save cancelled", 0};
        } else {
          application::ProjectService service;
          event.status = service.Save(event.document.get());
        }
        HWND target = nullptr;
        bool post = false;
        {
          std::scoped_lock lock(channel->mutex);
          if (channel->generation != generation || !channel->window) return;
          channel->events.push_back(std::move(event));
          target = channel->window;
          post = !channel->message_posted;
          channel->message_posted = true;
        }
        if (post) PostMessageW(target, kEventMessage, 0, 0);
      }));
}

void LayoutWindowImplementation::SaveAndStart(bool resume) {
  if (!document_ || document_->read_only || flow_service_.IsActive()) return;
  discovery_service_.Cancel();
  if (resume && !selected_resume_) {
    SetWindowTextW(status_, L"Select a compatible failed Run to resume");
    return;
  }
  core::Project project = document_->project;
  project.physical_implementation.pdk = WideToUtf8(ControlText(pdk_edit_));
  project.physical_implementation.standard_cell_library =
      WideToUtf8(ControlText(scl_edit_));
  project.physical_implementation.clock_ports =
      Split(WideToUtf8(ControlText(clocks_edit_)), ';');
  project.physical_implementation.clock_period_ns =
      WideToUtf8(ControlText(period_edit_));
  const std::string utilization = WideToUtf8(ControlText(utilization_edit_));
  const unsigned long parsed = std::strtoul(utilization.c_str(), nullptr, 10);
  project.physical_implementation.core_utilization_percent =
      static_cast<std::uint32_t>(parsed);
  const std::string density = WideToUtf8(ControlText(density_edit_));
  project.physical_implementation.placement_density_percent =
      density.empty() ? std::optional<std::string>{}
                      : std::optional<std::string>{density};
  project.physical_implementation.die_area =
      Split(WideToUtf8(ControlText(die_area_edit_)), ',');
  project.physical_implementation.advanced_overrides_json =
      WideToUtf8(ControlText(advanced_edit_));
  const auto selected_path = [this](HWND combo) {
    const LRESULT selected = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (selected <= 0 ||
        static_cast<std::size_t>(selected) > sdc_candidates_.size()) {
      return std::string{};
    }
    return sdc_candidates_[static_cast<std::size_t>(selected - 1)]
        ->relative_path;
  };
  project.physical_implementation.pnr_sdc_path = selected_path(pnr_sdc_combo_);
  project.physical_implementation.signoff_sdc_path =
      selected_path(signoff_sdc_combo_);
  core::Status validation = core::ValidateProject(project);
  adapters::OpenLane2Adapter adapter;
  if (validation.Ok()) {
    validation = adapter.ValidateAdvancedOverrides(
        project.physical_implementation.advanced_overrides_json);
  }
  if (!validation.Ok()) {
    SetControlText(status_, validation.message);
    return;
  }
  ApplyState(application::ManagedFlowRunState::kPreparing);
  const auto channel = event_channel_;
  const auto document = document_;
  const std::uint64_t generation = generation_;
  static_cast<void>(
      scheduler_.Submit([channel, document, project = std::move(project),
                         generation, resume](std::stop_token token) mutable {
        EventChannel::Event event;
        event.saved = true;
        event.generate_after_save = true;
        event.resume = resume;
        event.document = document;
        if (token.stop_requested()) {
          event.status = {core::ErrorCode::kCancelled,
                          "OpenLane configuration save cancelled", 0};
        } else {
          event.document->project = std::move(project);
          application::ProjectService service;
          event.status = service.Save(event.document.get());
        }
        HWND target = nullptr;
        bool post = false;
        {
          std::scoped_lock lock(channel->mutex);
          if (channel->generation != generation || !channel->window) return;
          channel->events.push_back(std::move(event));
          target = channel->window;
          post = !channel->message_posted;
          channel->message_posted = true;
        }
        if (post) PostMessageW(target, kEventMessage, 0, 0);
      }));
}

void LayoutWindowImplementation::StartPreparedRun(bool resume) {
  application::ManagedFlowRunRequest run_request;
  run_request.project = document_->project;
  run_request.profile = profile_;
  run_request.sources = sources_;
  run_request.library_directory = library_.directory;
  run_request.cell_directory =
      library_.directory / L"cells" / Utf8ToWide(request_.cell_id);
  run_request.generation = generation_;
  if (resume) run_request.resume = selected_resume_;
  const auto channel = event_channel_;
  const std::uint64_t generation = generation_;
  core::Status started = flow_service_.EnsureLayout(
      std::move(run_request),
      [channel, generation](application::PhysicalImplementationEvent flow) {
        HWND target = nullptr;
        bool post = false;
        {
          std::scoped_lock lock(channel->mutex);
          if (channel->generation != generation || !channel->window) return;
          if (flow.kind ==
                  application::PhysicalImplementationEventKind::kOutput &&
              !channel->events.empty() &&
              channel->events.back().flow.kind ==
                  application::PhysicalImplementationEventKind::kOutput) {
            std::string& pending = channel->events.back().flow.output;
            constexpr std::size_t kMaximumPendingOutput = 1024 * 1024;
            if (pending.size() < kMaximumPendingOutput) {
              pending.append(flow.output, 0,
                             std::min(flow.output.size(),
                                      kMaximumPendingOutput - pending.size()));
            }
          } else {
            EventChannel::Event event;
            event.flow = std::move(flow);
            channel->events.push_back(std::move(event));
          }
          target = channel->window;
          post = !channel->message_posted;
          channel->message_posted = true;
        }
        if (post) PostMessageW(target, kEventMessage, 0, 0);
      });
  if (!started.Ok()) {
    SetControlText(status_, started.message);
    ApplyState(application::ManagedFlowRunState::kFailed);
  }
}

void LayoutWindowImplementation::ApplyState(
    application::ManagedFlowRunState state) {
  const bool active = state == application::ManagedFlowRunState::kProbing ||
                      state == application::ManagedFlowRunState::kPreparing ||
                      state == application::ManagedFlowRunState::kValidating ||
                      state == application::ManagedFlowRunState::kRunning ||
                      state == application::ManagedFlowRunState::kCollecting ||
                      state == application::ManagedFlowRunState::kCancelling;
  EnableWindow(run_button_, !active && document_ && !document_->read_only);
  EnableWindow(cancel_button_, active);
  EnableWindow(resume_button_, !active && active_run_ != nullptr &&
                                   !FindGds(*active_run_).empty());
}

void LayoutWindowImplementation::AppendOutput(std::wstring_view text) {
  constexpr int kMaximumVisibleLogCharacters = 2 * 1024 * 1024;
  int length = GetWindowTextLengthW(output_);
  if (length > kMaximumVisibleLogCharacters) {
    SetWindowTextW(output_,
                   L"[Earlier output remains available in raw.log]\r\n");
    length = GetWindowTextLengthW(output_);
  }
  SendMessageW(output_, EM_SETSEL, length, length);
  SendMessageW(output_, EM_REPLACESEL, FALSE,
               reinterpret_cast<LPARAM>(std::wstring(text).c_str()));
}

void LayoutWindowImplementation::MergeCompletedRun(
    const std::shared_ptr<application::RunRecord>& completed_run) {
  if (!completed_run) return;

  const auto existing = std::find_if(runs_.begin(), runs_.end(),
                                     [&completed_run](const auto& run) {
                                       return run.id == completed_run->id;
                                     });
  const std::optional<application::ManagedFlowResumeRequest> resume =
      LoadResume(*completed_run);
  std::size_t selected_index = 0;
  if (existing == runs_.end()) {
    runs_.insert(runs_.begin(), *completed_run);
    run_metrics_.insert(run_metrics_.begin(), metrics_);
    resume_candidates_.insert(resume_candidates_.begin(), resume);
  } else {
    selected_index =
        static_cast<std::size_t>(std::distance(runs_.begin(), existing));
    *existing = *completed_run;
    if (selected_index < run_metrics_.size()) {
      run_metrics_[selected_index] = metrics_;
    }
    if (selected_index < resume_candidates_.size()) {
      resume_candidates_[selected_index] = resume;
    }
  }
  active_run_ = completed_run;
  selected_resume_ = resume;
  PopulateRuns();
  if (selected_index < runs_.size()) {
    const int row = static_cast<int>(selected_index);
    ListView_SetItemState(runs_list_, row, LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(runs_list_, row, FALSE);
  }
  ApplyState(flow_service_.IsActive()
                 ? application::ManagedFlowRunState::kRunning
                 : application::ManagedFlowRunState::kIdle);
}

void LayoutWindowImplementation::PopulateRuns() {
  ListView_DeleteAllItems(runs_list_);
  for (std::size_t index = 0; index < runs_.size(); ++index) {
    std::wstring values[] = {
        Utf8ToWide(runs_[index].started_utc),
        Utf8ToWide(application::RunStatusName(runs_[index].status)),
        Utf8ToWide(runs_[index].id)};
    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = static_cast<int>(index);
    item.pszText = values[0].data();
    const int row = ListView_InsertItem(runs_list_, &item);
    for (int column = 1; column < 3 && row >= 0; ++column) {
      ListView_SetItemText(runs_list_, row, column, values[column].data());
    }
  }
}

void LayoutWindowImplementation::SelectRun(int index) {
  selected_resume_.reset();
  active_run_.reset();
  if (index >= 0 && static_cast<std::size_t>(index) < runs_.size()) {
    active_run_ = std::make_shared<application::RunRecord>(runs_[index]);
    if (static_cast<std::size_t>(index) < run_metrics_.size()) {
      metrics_ = run_metrics_[static_cast<std::size_t>(index)];
      std::wostringstream summary;
      summary << L"Layout Summary\r\n\r\nGenerated: "
              << Utf8ToWide(active_run_->finished_utc)
              << L"\r\nBackend result: "
              << (active_run_->outcome.result_succeeded ? L"PASS" : L"FAILED")
              << L"\r\nDie area: " << Metric(metrics_.die_area)
              << L"\r\nCore area: " << Metric(metrics_.core_area)
              << L"\r\nUtilization: " << Metric(metrics_.utilization)
              << L"\r\nDRC/LVS: " << Metric(metrics_.drc_violations) << L" / "
              << Metric(metrics_.lvs_errors) << L"\r\nTiming setup/hold WNS: "
              << Metric(metrics_.setup_wns) << L" / "
              << Metric(metrics_.hold_wns);
      SetWindowTextW(summary_, summary.str().c_str());
    }
    if (static_cast<std::size_t>(index) < resume_candidates_.size()) {
      selected_resume_ = resume_candidates_[index];
    }
  }
  ApplyState(flow_service_.IsActive()
                 ? application::ManagedFlowRunState::kRunning
                 : application::ManagedFlowRunState::kIdle);
}

void LayoutWindowImplementation::ShowReports() {
  if (!active_run_) return;
  const std::filesystem::path path = active_run_->directory / "reports";
  ShellExecuteW(window_, L"open", path.c_str(), nullptr, nullptr,
                SW_SHOWNORMAL);
}

void LayoutWindowImplementation::ShowArtifacts() {
  if (!active_run_) return;
  const std::filesystem::path path = active_run_->directory / "artifacts";
  ShellExecuteW(window_, L"open", path.c_str(), nullptr, nullptr,
                SW_SHOWNORMAL);
}

void LayoutWindowImplementation::OpenLayout() {
  if (!active_run_ || viewer_service_.IsActive()) return;
  std::filesystem::path gds = FindGds(*active_run_);
  if (gds.empty()) {
    SetWindowTextW(status_, L"Generate or update the layout before opening it");
    return;
  }
  const auto channel = event_channel_;
  const std::uint64_t generation = generation_;
  application::LayoutViewerRequest request{profile_, std::move(gds),
                                           generation};
  const core::Status started = viewer_service_.Open(
      std::move(request),
      [channel, generation](application::LayoutViewerEvent viewer) {
        HWND target = nullptr;
        bool post = false;
        {
          std::scoped_lock lock(channel->mutex);
          if (channel->generation != generation || !channel->window) return;
          EventChannel::Event event;
          event.viewer = std::move(viewer);
          channel->events.push_back(std::move(event));
          target = channel->window;
          post = !channel->message_posted;
          channel->message_posted = true;
        }
        if (post) PostMessageW(target, kEventMessage, 0, 0);
      });
  if (!started.Ok()) SetControlText(status_, started.message);
}

void LayoutWindowImplementation::ShutdownChannel() {
  if (!event_channel_) return;
  std::scoped_lock lock(event_channel_->mutex);
  event_channel_->window = nullptr;
  event_channel_->events.clear();
}

}  // namespace designpp::gui

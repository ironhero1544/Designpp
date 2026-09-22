// Copyright 2026 The Design++ Authors

#include "designpp/gui/pdk_manager_window.h"

#include <algorithm>
#include <deque>
#include <mutex>
#include <ranges>
#include <utility>
#include <vector>

#include "designpp/application/openlane_discovery_service.h"
#include "designpp/application/orfs_discovery_service.h"
#include "designpp/application/pdk_selection_service.h"
#include "designpp/application/physical_verification_service.h"
#include "designpp/gui/dpi.h"
#include "designpp/runtime/task_scheduler.h"

namespace designpp::gui {
namespace {
std::wstring Wide(std::string_view text) {
  if (text.empty()) return {};
  const int length =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), nullptr, 0);
  if (length <= 0) return L"Invalid UTF-8";
  std::wstring result(length, L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                      static_cast<int>(text.size()), result.data(), length);
  return result;
}
struct Row {
  application::PdkSelection selection;
  bool ready = false;
  bool drc_ready = false;
  bool lvs_ready = false;
  std::string drc_status;
  std::string lvs_status;
  std::string reason;
};
struct Event {
  enum class Kind { kLoaded, kDiscovered, kSaved };
  Kind kind = Kind::kLoaded;
  std::uint64_t generation = 0;
  core::Status status;
  application::PdkCellContext context;
  std::vector<Row> rows;
};
// Workers never own HWNDs. A bounded UI timer drains per-session events;
// late results retain only this channel and are discarded after close.
struct Channel {
  std::mutex mutex;
  bool live = true;
  std::deque<Event> events;
  void Push(Event event) {
    std::scoped_lock lock(mutex);
    if (live) events.push_back(std::move(event));
  }
};
}  // namespace

struct PdkManagerWindow::Implementation {
  explicit Implementation(runtime::ExecutionProvider* injected_provider)
      : provider(injected_provider != nullptr ? injected_provider
                                              : &owned_provider),
        openlane(provider),
        orfs(provider) {}

  HWND window = nullptr, heading = nullptr, list = nullptr, status = nullptr;
  HWND refresh = nullptr, apply = nullptr, paths = nullptr;
  HINSTANCE instance = nullptr;
  UINT dpi = 96;
  UniqueFont font;
  application::LibraryRecord library;
  std::string cell_id;
  std::function<void()> manage_paths;
  std::function<void()> saved;
  application::PdkCellContext context;
  std::vector<Row> rows;
  std::uint64_t generation = 0;
  int pending = 0;
  bool saving = false;
  std::shared_ptr<Channel> channel;
  runtime::TaskScheduler scheduler{1};
  runtime::WslExecutionProvider owned_provider;
  runtime::ExecutionProvider* provider = nullptr;
  application::OpenLaneDiscoveryService openlane;
  application::OrfsDiscoveryService orfs;

  ~Implementation() {
    if (window) DestroyWindow(window);
    scheduler.RequestStop();
  }
  void SetStatus(const std::string& text) {
    SetWindowTextW(status, Wide(text).c_str());
  }
  void Layout() {
    RECT r{};
    GetClientRect(window, &r);
    const int m = ScaleForDpi(12, dpi), h = ScaleForDpi(32, dpi);
    const int width = std::max(1L, r.right - 2 * m);
    MoveWindow(heading, m, m, width, h * 2, TRUE);
    MoveWindow(list, m, m * 2 + h * 2, width,
               std::max(1L, r.bottom - m * 5 - h * 5), TRUE);
    MoveWindow(status, m, r.bottom - m * 2 - h * 3, width, h * 2, TRUE);
    const int button_width = (width - m * 2) / 3;
    MoveWindow(refresh, m, r.bottom - m - h, button_width, h, TRUE);
    MoveWindow(paths, m * 2 + button_width, r.bottom - m - h, button_width, h,
               TRUE);
    MoveWindow(apply, m * 3 + button_width * 2, r.bottom - m - h, button_width,
               h, TRUE);
  }
  void UpdateButtons() {
    const auto index = SendMessageW(list, LB_GETCURSEL, 0, 0);
    EnableWindow(apply, !saving && pending == 0 && index >= 0 &&
                            static_cast<std::size_t>(index) < rows.size() &&
                            rows[index].ready);
    EnableWindow(refresh, !saving && pending == 0);
    EnableWindow(paths, !saving);
    EnableWindow(list, !saving);
  }
  void Load() {
    ++generation;
    pending = 1;
    rows.clear();
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    SetStatus("Loading selected Cell and its toolchain profile...");
    UpdateButtons();
    const auto target = channel;
    const auto current = generation;
    if (!scheduler.Submit([target, current, record = library,
                           id = cell_id](std::stop_token token) {
          if (token.stop_requested()) return;
          Event event;
          event.generation = current;
          auto loaded = application::PdkSelectionService{}.Load(record, id);
          if (loaded.Ok())
            event.context = std::move(loaded).Value();
          else
            event.status = loaded.GetStatus();
          target->Push(std::move(event));
        })) {
      pending = 0;
      SetStatus("PDK task queue unavailable");
      UpdateButtons();
    }
  }
  void Discover() {
    pending = 2;
    const auto target = channel;
    const auto current = generation;
    const auto profile = context.profile;
    const bool queued = scheduler.Submit(
        [this, target, current, profile](std::stop_token token) {
          if (token.stop_requested()) return;
          const auto ol = openlane.Start(
              profile, current,
              [target](application::OpenLaneDiscoveryResult result) {
                Event event;
                event.kind = Event::Kind::kDiscovered;
                event.generation = result.generation;
                event.status = result.status;
                for (const auto& item : result.candidates) {
                  event.rows.push_back(
                      {{"openlane2", item.pdk, item.standard_cell_library},
                       true,
                       false,
                       false,
                       "Flow results only",
                       "Flow results only",
                       {}});
                }
                target->Push(std::move(event));
              });
          if (!ol.Ok()) {
            Event event;
            event.kind = Event::Kind::kDiscovered;
            event.generation = current;
            event.status = ol;
            target->Push(std::move(event));
          }
          if (token.stop_requested()) return;
          const auto of = orfs.Start(
              profile, current,
              [target, profile](application::OrfsDiscoveryResult result) {
                Event event;
                event.kind = Event::Kind::kDiscovered;
                event.generation = result.generation;
                event.status = result.status;
                for (const auto& item : result.candidates) {
                  const auto capability =
                      application::ResolveVerificationCapability(
                          profile, "orfs", item.name, item.runnable,
                          item.drc_ready, item.lvs_ready);
                  event.rows.push_back({{"orfs", item.name, {}},
                                        item.runnable,
                                        capability.drc_ready,
                                        capability.lvs_ready,
                                        capability.drc_status,
                                        capability.lvs_status,
                                        item.reason});
                }
                target->Push(std::move(event));
              });
          if (!of.Ok()) {
            Event event;
            event.kind = Event::Kind::kDiscovered;
            event.generation = current;
            event.status = of;
            target->Push(std::move(event));
          }
        });
    if (!queued) {
      pending = 0;
      SetStatus("PDK discovery queue unavailable");
    }
    UpdateButtons();
  }
  void Drain() {
    std::deque<Event> events;
    {
      std::scoped_lock lock(channel->mutex);
      events.swap(channel->events);
    }
    for (auto& event : events) {
      if (event.generation != generation) continue;
      if (event.kind == Event::Kind::kSaved) {
        saving = false;
        if (event.status.Ok()) {
          context.revision = event.context.revision;
          context.selection = std::move(event.context.selection);
          SetStatus(
              "PDK saved to this Cell only. Existing Runs are unchanged.");
          if (saved) saved();
        } else {
          std::string message = event.status.message;
          if (event.status.code == core::ErrorCode::kPermissionDenied) {
            message += " This Cell is read-only.";
          } else if (event.status.code ==
                     core::ErrorCode::kExternalModification) {
            message += " Refresh before applying again.";
          }
          SetStatus(message);
        }
      } else if (event.kind == Event::Kind::kLoaded) {
        pending = 0;
        if (!event.status.Ok())
          SetStatus(event.status.message);
        else {
          context = std::move(event.context);
          SetStatus("Current: " + context.selection.backend + " / " +
                    context.selection.name +
                    ". Discovering installed PDKs/platforms...");
          Discover();
        }
      } else {
        --pending;
        if (!event.status.Ok()) SetStatus(event.status.message);
        for (auto& row : event.rows) {
          std::string label =
              row.selection.backend + " | " + row.selection.name;
          if (!row.selection.standard_cell_library.empty()) {
            label += " | " + row.selection.standard_cell_library;
          }
          label += row.ready ? " | Installed" : " | Unavailable";
          if (row.selection.backend == "orfs") {
            label += " | DRC: " + row.drc_status;
            label += " | LVS: " + row.lvs_status;
          } else {
            label += " | DRC/LVS: flow results only";
          }
          if (!row.reason.empty()) label += " | " + row.reason;
          const auto wide_label = Wide(label);
          SendMessageW(list, LB_ADDSTRING, 0,
                       reinterpret_cast<LPARAM>(wide_label.c_str()));
          rows.push_back(std::move(row));
        }
      }
    }
    UpdateButtons();
  }
  void Apply() {
    const auto index = SendMessageW(list, LB_GETCURSEL, 0, 0);
    if (saving || pending || index < 0 ||
        static_cast<std::size_t>(index) >= rows.size() || !rows[index].ready)
      return;
    saving = true;
    UpdateButtons();
    SetStatus("Saving PDK to the captured Cell...");
    const auto target = channel;
    const auto current = generation;
    if (!scheduler.Submit([target, current, record = library, id = cell_id,
                           revision = context.revision,
                           choice =
                               rows[index].selection](std::stop_token token) {
          Event event;
          event.kind = Event::Kind::kSaved;
          event.generation = current;
          if (token.stop_requested())
            event.status = {core::ErrorCode::kCancelled, "Save cancelled", 0};
          else {
            auto saved = application::PdkSelectionService{}.Apply(
                record, id, revision, choice);
            if (saved.Ok())
              event.context = {core::ToolchainProfile{}, saved.Value(), choice};
            else
              event.status = saved.GetStatus();
          }
          target->Push(std::move(event));
        })) {
      saving = false;
      SetStatus("PDK save queue unavailable");
      UpdateButtons();
    }
  }
  static LRESULT CALLBACK Procedure(HWND hwnd, UINT message, WPARAM wparam,
                                    LPARAM lparam) {
    auto* self = reinterpret_cast<Implementation*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
      self = static_cast<Implementation*>(
          reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
      self->window = hwnd;
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(hwnd, message, wparam, lparam);
    switch (message) {
      case WM_SIZE:
        self->Layout();
        return 0;
      case WM_DPICHANGED: {
        self->dpi = HIWORD(wparam);
        self->font = CreateUiFont(self->dpi);
        ApplyFontToWindowTree(hwnd, self->font.Get());
        const auto* rect = reinterpret_cast<RECT*>(lparam);
        SetWindowPos(hwnd, nullptr, rect->left, rect->top,
                     rect->right - rect->left, rect->bottom - rect->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        SendMessageW(self->list, LB_SETHORIZONTALEXTENT,
                     ScaleForDpi(1200, self->dpi), 0);
        return 0;
      }
      case WM_TIMER:
        self->Drain();
        return 0;
      case WM_COMMAND:
        if (LOWORD(wparam) == 1) self->Load();
        if (LOWORD(wparam) == 2) self->Apply();
        if (LOWORD(wparam) == 3 && self->manage_paths) self->manage_paths();
        self->UpdateButtons();
        return 0;
      case WM_CLOSE:
        if (self->saving) {
          self->SetStatus("Save in progress. Close after completion.");
          return 0;
        }
        DestroyWindow(hwnd);
        return 0;
      case WM_DESTROY:
        KillTimer(hwnd, 1);
        if (self->channel) {
          std::scoped_lock lock(self->channel->mutex);
          self->channel->live = false;
          self->channel->events.clear();
        }
        static_cast<void>(self->scheduler.Submit([self](std::stop_token) {
          self->openlane.Cancel();
          self->orfs.Cancel();
        }));
        self->window = nullptr;
        return 0;
      case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
        info->ptMinTrackSize = {ScaleForDpi(500, self->dpi),
                                ScaleForDpi(340, self->dpi)};
        return 0;
      }
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
  }
};

PdkManagerWindow::PdkManagerWindow(
    runtime::ExecutionProvider* execution_provider)
    : implementation_(std::make_unique<Implementation>(execution_provider)) {}
PdkManagerWindow::~PdkManagerWindow() = default;

bool PdkManagerWindow::CreateOrShow(HINSTANCE instance, HWND owner,
                                    application::LibraryRecord library,
                                    std::string cell_id, std::string cell_name,
                                    std::function<void()> manage_paths,
                                    std::function<void()> saved) {
  auto& state = *implementation_;
  if (state.window) {
    ShowWindow(state.window, SW_RESTORE);
    SetForegroundWindow(state.window);
    return true;
  }
  state.library = std::move(library);
  state.cell_id = std::move(cell_id);
  state.manage_paths = std::move(manage_paths);
  state.saved = std::move(saved);
  state.instance = instance;
  state.dpi = GetWindowDpi(owner);
  state.channel = std::make_shared<Channel>();
  WNDCLASSW wc{};
  wc.lpfnWndProc = Implementation::Procedure;
  wc.hInstance = instance;
  wc.lpszClassName = L"DesignPdkManager";
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    return false;
  if (!CreateWindowExW(0, wc.lpszClassName, L"Design++ PDK 관리",
                       WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                       CW_USEDEFAULT, ScaleForDpi(800, state.dpi),
                       ScaleForDpi(540, state.dpi), nullptr, nullptr, instance,
                       &state))
    return false;
  auto control = [&](const wchar_t* kind, const wchar_t* title, DWORD style,
                     int id) {
    auto handle = CreateWindowExW(
        0, kind, title, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, state.window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    SendMessageW(handle, WM_SETFONT,
                 reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),
                 TRUE);
    return handle;
  };
  state.heading = control(
      L"STATIC",
      Wide("Target Cell: " + cell_name + " (" + state.cell_id + ")").c_str(), 0,
      0);
  state.list = control(L"LISTBOX", L"",
                       WS_TABSTOP | WS_BORDER | WS_VSCROLL | WS_HSCROLL |
                           LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                       4);
  SendMessageW(state.list, LB_SETHORIZONTALEXTENT, ScaleForDpi(1200, state.dpi),
               0);
  state.status = control(L"STATIC", L"", 0, 0);
  state.refresh = control(L"BUTTON", L"새로 고침", WS_TABSTOP, 1);
  state.apply = control(L"BUTTON", L"선택한 Cell에 적용", WS_TABSTOP, 2);
  state.paths = control(L"BUTTON", L"PDK 경로 관리...", WS_TABSTOP, 3);
  state.font = CreateUiFont(state.dpi);
  ApplyFontToWindowTree(state.window, state.font.Get());
  MONITORINFO monitor{sizeof(MONITORINFO)};
  if (GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST),
                      &monitor)) {
    const RECT work = monitor.rcWork;
    SetWindowPos(state.window, nullptr, work.left, work.top,
                 std::min(ScaleForDpi(800, state.dpi),
                          static_cast<int>(work.right - work.left)),
                 std::min(ScaleForDpi(540, state.dpi),
                          static_cast<int>(work.bottom - work.top)),
                 SWP_NOZORDER | SWP_NOACTIVATE);
  }
  state.Layout();
  SetTimer(state.window, 1, 100, nullptr);
  state.Load();
  ShowWindow(state.window, SW_SHOW);
  return true;
}
}  // namespace designpp::gui

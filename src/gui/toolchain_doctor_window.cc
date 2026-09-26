// Copyright 2026 The Design++ Authors

#include "designpp/gui/toolchain_doctor_window.h"

#include <commctrl.h>
#include <objbase.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <deque>
#include <mutex>
#include <string>
#include <utility>

#include "Resource.h"

namespace designpp::gui {
namespace {

constexpr wchar_t kWindowClassName[] = L"DesignPlusPlus.ToolchainDoctorWindow";
constexpr int kProfileSelectorId = 4101;
constexpr int kSaveButtonId = 4102;
constexpr int kDiagnoseButtonId = 4103;
constexpr int kCancelButtonId = 4104;
constexpr int kAddButtonId = 4105;
constexpr int kDuplicateButtonId = 4106;
constexpr int kDeleteButtonId = 4107;
constexpr int kRepairWslButtonId = 4108;

std::string WideToUtf8(std::wstring_view text);

std::string NewUuid() {
  GUID guid{};
  if (CoCreateGuid(&guid) != S_OK) return {};
  wchar_t buffer[40]{};
  const int length =
      StringFromGUID2(guid, buffer, static_cast<int>(std::size(buffer)));
  if (length <= 3) return {};
  std::wstring value(buffer + 1, buffer + length - 2);
  return WideToUtf8(value);
}

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
  const int size = WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
      nullptr, 0, nullptr, nullptr);
  if (size <= 0) return {};
  std::string result(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                      static_cast<int>(text.size()), result.data(), size,
                      nullptr, nullptr);
  return result;
}

std::wstring WindowText(HWND window) {
  const int length = GetWindowTextLengthW(window);
  if (length <= 0) return {};
  std::wstring result(static_cast<std::size_t>(length + 1), L'\0');
  GetWindowTextW(window, result.data(), length + 1);
  result.resize(static_cast<std::size_t>(length));
  return result;
}

void SetCheckRow(HWND list, application::DoctorCheckId id,
                 std::wstring_view state, std::wstring_view detail) {
  const int row = static_cast<int>(id);
  std::wstring state_text(state);
  std::wstring detail_text(detail);
  ListView_SetItemText(list, row, 1, state_text.data());
  ListView_SetItemText(list, row, 2, detail_text.data());
}

}  // namespace

struct ToolchainDoctorWindow::UiEvent {
  enum class Kind { kLoaded, kSaved, kDistributions, kDoctor };

  Kind kind = Kind::kLoaded;
  std::uint64_t generation = 0;
  core::Status status;
  core::ToolchainSettings settings;
  std::vector<application::WslDistribution> distributions;
  application::DoctorEvent doctor;
};

struct ToolchainDoctorWindow::EventChannel {
  std::mutex mutex;
  HWND window = nullptr;
  bool accepting = true;
  std::deque<UiEvent> events;
};

ToolchainDoctorWindow::ToolchainDoctorWindow()
    : doctor_(&execution_provider_) {}

ToolchainDoctorWindow::~ToolchainDoctorWindow() {
  doctor_.Shutdown();
  distribution_service_.Shutdown();
  scheduler_.RequestStop();
  ShutdownChannel();
  if (window_ != nullptr) DestroyWindow(window_);
}

bool ToolchainDoctorWindow::CreateOrShow(HINSTANCE instance, HWND owner) {
  if (window_ != nullptr) {
    ShowWindow(window_, SW_RESTORE);
    SetForegroundWindow(window_);
    return true;
  }
  instance_ = instance;
  owner_ = owner;
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
      CreateWindowExW(0, kWindowClassName, L"Design++ Toolchain Doctor",
                      WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                      CW_USEDEFAULT, ScaleForDpi(980, dpi_),
                      ScaleForDpi(680, dpi_), owner, nullptr, instance, this);
  if (window_ == nullptr) return false;
  ShowWindow(window_, SW_SHOW);
  UpdateWindow(window_);
  return true;
}

LRESULT CALLBACK ToolchainDoctorWindow::WindowProcedure(HWND window,
                                                        UINT message,
                                                        WPARAM wparam,
                                                        LPARAM lparam) {
  auto* self = reinterpret_cast<ToolchainDoctorWindow*>(
      GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<ToolchainDoctorWindow*>(create->lpCreateParams);
    self->window_ = window;
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  }
  return self != nullptr ? self->HandleMessage(message, wparam, lparam)
                         : DefWindowProcW(window, message, wparam, lparam);
}

LRESULT ToolchainDoctorWindow::HandleMessage(UINT message, WPARAM wparam,
                                             LPARAM lparam) {
  switch (message) {
    case WM_CREATE:
      event_channel_ = std::make_shared<EventChannel>();
      event_channel_->window = window_;
      dpi_ = GetWindowDpi(window_);
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
      information->ptMinTrackSize = {ScaleForDpi(760, dpi_),
                                     ScaleForDpi(520, dpi_)};
      return 0;
    }
    case WM_COMMAND:
      if (LOWORD(wparam) == kProfileSelectorId &&
          HIWORD(wparam) == CBN_SELCHANGE) {
        PopulateSelectedProfile();
        return 0;
      }
      if (LOWORD(wparam) == kSaveButtonId) {
        BeginSave();
        return 0;
      }
      if (LOWORD(wparam) == kAddButtonId) {
        AddProfile();
        return 0;
      }
      if (LOWORD(wparam) == kDuplicateButtonId) {
        DuplicateProfile();
        return 0;
      }
      if (LOWORD(wparam) == kDeleteButtonId) {
        DeleteProfile();
        return 0;
      }
      if (LOWORD(wparam) == kDiagnoseButtonId) {
        BeginDiagnosis();
        return 0;
      }
      if (LOWORD(wparam) == kRepairWslButtonId) {
        if (owner_ != nullptr && IsWindow(owner_)) {
          PostMessageW(owner_, WM_COMMAND, MAKEWPARAM(IDM_WSL_SETUP, 0), 0);
        }
        return 0;
      }
      if (LOWORD(wparam) == kCancelButtonId) {
        CancelDiagnosis();
        return 0;
      }
      break;
    case kEventMessage:
      HandleEvents();
      return 0;
    case WM_CLOSE:
      CancelDiagnosis();
      distribution_service_.Cancel();
      DestroyWindow(window_);
      if (owner_ != nullptr && IsWindow(owner_) && IsWindowVisible(owner_) &&
          !IsIconic(owner_)) {
        SetForegroundWindow(owner_);
      }
      return 0;
    case WM_DESTROY:
      ShutdownChannel();
      window_ = nullptr;
      profile_selector_ = nullptr;
      distribution_edit_ = nullptr;
      checks_ = nullptr;
      status_ = nullptr;
      return 0;
    default:
      break;
  }
  return DefWindowProcW(window_, message, wparam, lparam);
}

bool ToolchainDoctorWindow::CreateControls() {
  const auto create_label = [this](const wchar_t* text) {
    return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE, 0, 0, 0,
                           0, window_, nullptr, instance_, nullptr);
  };
  const auto create_edit = [this]() {
    return CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                           WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0,
                           window_, nullptr, instance_, nullptr);
  };
  create_label(L"Profile");
  profile_selector_ = CreateWindowExW(
      0, WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 0, 0, 0,
      0, window_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kProfileSelectorId)),
      instance_, nullptr);
  create_label(L"Name");
  name_edit_ = create_edit();
  create_label(L"WSL distribution");
  distribution_edit_ = CreateWindowExW(
      0, WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 0, 0, 0,
      0, window_, nullptr, instance_, nullptr);
  create_label(L"OpenLane 2 root");
  openlane_edit_ = create_edit();
  create_label(L"ORFS root");
  orfs_edit_ = create_edit();
  create_label(L"OpenLane PDK root");
  pdk_edit_ = create_edit();
  create_label(L"CPU budget");
  cpu_edit_ = create_edit();
  add_button_ = CreateWindowExW(
      0, L"BUTTON", L"New", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0,
      window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddButtonId)),
      instance_, nullptr);
  duplicate_button_ = CreateWindowExW(
      0, L"BUTTON", L"Duplicate", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0,
      0, 0, window_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDuplicateButtonId)),
      instance_, nullptr);
  delete_button_ = CreateWindowExW(
      0, L"BUTTON", L"Delete", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0,
      0, window_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDeleteButtonId)), instance_,
      nullptr);
  save_button_ = CreateWindowExW(
      0, L"BUTTON", L"Save Profile", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0,
      0, 0, 0, window_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSaveButtonId)), instance_,
      nullptr);
  diagnose_button_ = CreateWindowExW(
      0, L"BUTTON", L"Run Doctor", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0,
      0, 0, window_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDiagnoseButtonId)),
      instance_, nullptr);
  repair_wsl_button_ = CreateWindowExW(
      0, L"BUTTON", L"Repair WSL...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0,
      0, 0, 0, window_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRepairWslButtonId)),
      instance_, nullptr);
  cancel_button_ = CreateWindowExW(
      0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0,
      0, window_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCancelButtonId)), instance_,
      nullptr);
  checks_ = CreateWindowExW(
      WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
      WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, 0,
      0, 0, 0, window_, nullptr, instance_, nullptr);
  status_ = CreateWindowExW(0, STATUSCLASSNAMEW, L"Loading profiles...",
                            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window_, nullptr,
                            instance_, nullptr);
  if (profile_selector_ == nullptr || name_edit_ == nullptr ||
      distribution_edit_ == nullptr || openlane_edit_ == nullptr ||
      orfs_edit_ == nullptr || pdk_edit_ == nullptr || cpu_edit_ == nullptr ||
      add_button_ == nullptr || duplicate_button_ == nullptr ||
      delete_button_ == nullptr || save_button_ == nullptr ||
      diagnose_button_ == nullptr || repair_wsl_button_ == nullptr ||
      cancel_button_ == nullptr || checks_ == nullptr || status_ == nullptr) {
    return false;
  }
  ListView_SetExtendedListViewStyle(
      checks_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);
  PopulateChecks();
  ApplyDpi(dpi_);
  SetBusy(true);
  return true;
}

void ToolchainDoctorWindow::LayoutControls(int width, int height) const {
  if (profile_selector_ == nullptr) return;
  const int margin = ScaleForDpi(12, dpi_);
  const int gap = ScaleForDpi(8, dpi_);
  const int label_width = ScaleForDpi(130, dpi_);
  const int edit_height = ScaleForDpi(28, dpi_);
  const int left_width = std::max(ScaleForDpi(360, dpi_), width * 42 / 100);
  const int editor_width = left_width - margin * 2 - label_width - gap;
  const std::array<HWND, 7> fields = {
      profile_selector_, name_edit_, distribution_edit_, openlane_edit_,
      orfs_edit_,        pdk_edit_,  cpu_edit_};
  HWND child = GetWindow(window_, GW_CHILD);
  int y = margin;
  for (std::size_t index = 0; index < fields.size(); ++index) {
    if (child != nullptr) {
      MoveWindow(child, margin, y, label_width, edit_height, TRUE);
      child = GetWindow(child, GW_HWNDNEXT);
    }
    MoveWindow(fields[index], margin + label_width + gap, y, editor_width,
               index == 0 ? ScaleForDpi(240, dpi_) : edit_height, TRUE);
    child = GetWindow(fields[index], GW_HWNDNEXT);
    y += edit_height + gap;
  }
  const int profile_button_width = (left_width - margin * 2 - gap * 3) / 4;
  MoveWindow(add_button_, margin, y, profile_button_width, edit_height, TRUE);
  MoveWindow(duplicate_button_, margin + profile_button_width + gap, y,
             profile_button_width, edit_height, TRUE);
  MoveWindow(delete_button_, margin + (profile_button_width + gap) * 2, y,
             profile_button_width, edit_height, TRUE);
  MoveWindow(save_button_, margin + (profile_button_width + gap) * 3, y,
             profile_button_width, edit_height, TRUE);
  y += edit_height + gap;
  const int run_button_width = (left_width - margin * 2 - gap * 2) / 3;
  MoveWindow(diagnose_button_, margin, y, run_button_width, edit_height, TRUE);
  MoveWindow(repair_wsl_button_, margin + run_button_width + gap, y,
             run_button_width, edit_height, TRUE);
  MoveWindow(cancel_button_, margin + (run_button_width + gap) * 2, y,
             run_button_width, edit_height, TRUE);
  RECT status_rectangle{};
  SendMessageW(status_, SB_GETRECT, 0,
               reinterpret_cast<LPARAM>(&status_rectangle));
  const int status_height = std::max(ScaleForDpi(24, dpi_),
                                     static_cast<int>(status_rectangle.bottom));
  MoveWindow(checks_, left_width + gap, margin,
             std::max(0, width - left_width - gap - margin),
             std::max(0, height - margin * 2 - status_height), TRUE);
  SendMessageW(status_, WM_SIZE, 0, 0);
}

void ToolchainDoctorWindow::ApplyDpi(UINT dpi) {
  dpi_ = dpi == 0 ? kDefaultDpi : dpi;
  font_ = CreateUiFont(dpi_);
  ApplyFontToWindowTree(window_, font_.Get());
  ListView_SetColumnWidth(checks_, 0, ScaleForDpi(150, dpi_));
  ListView_SetColumnWidth(checks_, 1, ScaleForDpi(90, dpi_));
  ListView_SetColumnWidth(checks_, 2, ScaleForDpi(500, dpi_));
  RECT client{};
  GetClientRect(window_, &client);
  LayoutControls(client.right, client.bottom);
}

void ToolchainDoctorWindow::BeginLoad() {
  const std::uint64_t generation = ++generation_;
  const auto channel = event_channel_;
  SetBusy(true);
  if (!scheduler_.Submit([this, channel, generation](std::stop_token stop) {
        if (stop.stop_requested()) return;
        auto loaded = store_.LoadOrCreateDefaults();
        UiEvent event;
        event.kind = UiEvent::Kind::kLoaded;
        event.generation = generation;
        event.status =
            loaded.Ok() ? core::Status::Success() : loaded.GetStatus();
        if (loaded.Ok()) event.settings = std::move(loaded).Value();
        HWND target = nullptr;
        {
          std::scoped_lock lock(channel->mutex);
          if (!channel->accepting) return;
          channel->events.push_back(std::move(event));
          target = channel->window;
        }
        if (target != nullptr) PostMessageW(target, kEventMessage, 0, 0);
      })) {
    SetBusy(false);
    SetStatus(L"Profile loader is busy");
  }
  BeginDistributionDiscovery(generation);
}

void ToolchainDoctorWindow::BeginDistributionDiscovery(
    std::uint64_t generation) {
  const auto channel = event_channel_;
  const core::Status started = distribution_service_.Start(
      generation, [channel](application::WslDistributionEvent result) {
        UiEvent event;
        event.kind = UiEvent::Kind::kDistributions;
        event.generation = result.generation;
        event.status = std::move(result.status);
        event.distributions = std::move(result.distributions);
        HWND target = nullptr;
        {
          std::scoped_lock lock(channel->mutex);
          if (!channel->accepting) return;
          channel->events.push_back(std::move(event));
          target = channel->window;
        }
        if (target != nullptr) PostMessageW(target, kEventMessage, 0, 0);
      });
  if (!started.Ok()) SetStatus(Utf8ToWide(started.message));
}

void ToolchainDoctorWindow::BeginSave() {
  core::ToolchainProfile profile;
  if (!ReadSelectedProfile(&profile)) return;
  core::ToolchainSettings candidate = settings_;
  const auto found = std::find_if(
      candidate.profiles.begin(), candidate.profiles.end(),
      [&profile](const auto& item) { return item.id == profile.id; });
  if (found == candidate.profiles.end()) return;
  *found = std::move(profile);
  candidate.selected_profile_id = found->id;
  const core::Status validation = core::ValidateToolchainSettings(candidate);
  if (!validation.Ok()) {
    SetStatus(Utf8ToWide(validation.message));
    return;
  }
  const std::uint64_t generation = ++generation_;
  const auto channel = event_channel_;
  SetBusy(true);
  if (!scheduler_.Submit(
          [this, channel, generation, candidate](std::stop_token stop) {
            if (stop.stop_requested()) return;
            UiEvent event;
            event.kind = UiEvent::Kind::kSaved;
            event.generation = generation;
            event.status = store_.Save(candidate);
            if (event.status.Ok()) event.settings = candidate;
            HWND target = nullptr;
            {
              std::scoped_lock lock(channel->mutex);
              if (!channel->accepting) return;
              channel->events.push_back(std::move(event));
              target = channel->window;
            }
            if (target != nullptr) PostMessageW(target, kEventMessage, 0, 0);
          })) {
    SetBusy(false);
    SetStatus(L"Profile saver is busy");
  }
}

void ToolchainDoctorWindow::AddProfile() {
  core::ToolchainProfile profile =
      application::ToolchainProfileStore::CreateDefaults().profiles.front();
  profile.id = NewUuid();
  if (profile.id.empty()) {
    SetStatus(L"Cannot create a profile identity");
    return;
  }
  std::size_t suffix = settings_.profiles.size() + 1;
  for (;;) {
    profile.name = "Toolchain " + std::to_string(suffix);
    const bool exists =
        std::any_of(settings_.profiles.begin(), settings_.profiles.end(),
                    [&profile](const core::ToolchainProfile& item) {
                      return item.name == profile.name;
                    });
    if (!exists) break;
    ++suffix;
  }
  settings_.profiles.push_back(std::move(profile));
  settings_.selected_profile_id = settings_.profiles.back().id;
  PopulateProfiles();
  SetStatus(L"New profile added; save to persist it");
}

void ToolchainDoctorWindow::DuplicateProfile() {
  core::ToolchainProfile profile;
  if (!ReadSelectedProfile(&profile)) return;
  profile.id = NewUuid();
  if (profile.id.empty()) {
    SetStatus(L"Cannot create a profile identity");
    return;
  }
  profile.name.append(" Copy");
  std::string base_name = profile.name;
  std::size_t suffix = 2;
  while (std::any_of(settings_.profiles.begin(), settings_.profiles.end(),
                     [&profile](const core::ToolchainProfile& item) {
                       return item.name == profile.name;
                     })) {
    profile.name = base_name + " " + std::to_string(suffix++);
  }
  settings_.profiles.push_back(std::move(profile));
  settings_.selected_profile_id = settings_.profiles.back().id;
  PopulateProfiles();
  SetStatus(L"Profile duplicated; save to persist it");
}

void ToolchainDoctorWindow::DeleteProfile() {
  if (settings_.profiles.size() <= 1) {
    SetStatus(L"At least one profile must remain");
    return;
  }
  const int selected =
      static_cast<int>(SendMessageW(profile_selector_, CB_GETCURSEL, 0, 0));
  if (selected < 0 ||
      static_cast<std::size_t>(selected) >= settings_.profiles.size()) {
    return;
  }
  if (MessageBoxW(window_, L"Delete the selected toolchain profile?",
                  L"Delete Toolchain Profile",
                  MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
    return;
  }
  settings_.profiles.erase(settings_.profiles.begin() + selected);
  const std::size_t next = std::min<std::size_t>(
      static_cast<std::size_t>(selected), settings_.profiles.size() - 1);
  settings_.selected_profile_id = settings_.profiles[next].id;
  PopulateProfiles();
  SetStatus(L"Profile deleted; save to persist the change");
}

void ToolchainDoctorWindow::BeginDiagnosis() {
  core::ToolchainProfile profile;
  if (!ReadSelectedProfile(&profile)) return;
  const std::uint64_t generation = ++generation_;
  const auto channel = event_channel_;
  SetBusy(true);
  PopulateChecks();
  const core::Status started =
      doctor_.Start(std::move(profile), generation,
                    [channel](application::DoctorEvent doctor_event) {
                      UiEvent event;
                      event.kind = UiEvent::Kind::kDoctor;
                      event.generation = doctor_event.generation;
                      event.doctor = std::move(doctor_event);
                      HWND target = nullptr;
                      {
                        std::scoped_lock lock(channel->mutex);
                        if (!channel->accepting) return;
                        channel->events.push_back(std::move(event));
                        target = channel->window;
                      }
                      if (target != nullptr)
                        PostMessageW(target, kEventMessage, 0, 0);
                    });
  if (!started.Ok()) {
    SetBusy(false);
    SetStatus(Utf8ToWide(started.message));
  }
}

void ToolchainDoctorWindow::CancelDiagnosis() {
  doctor_.Cancel();
  SetStatus(L"Cancelling diagnosis...");
}

void ToolchainDoctorWindow::HandleEvents() {
  std::deque<UiEvent> events;
  {
    std::scoped_lock lock(event_channel_->mutex);
    events.swap(event_channel_->events);
  }
  for (UiEvent& event : events) {
    if (event.generation != generation_) continue;
    if (event.kind == UiEvent::Kind::kLoaded ||
        event.kind == UiEvent::Kind::kSaved) {
      SetBusy(false);
      if (!event.status.Ok()) {
        SetStatus(Utf8ToWide(event.status.message));
        continue;
      }
      settings_ = std::move(event.settings);
      PopulateProfiles();
      SetStatus(event.kind == UiEvent::Kind::kLoaded ? L"Profile loaded"
                                                     : L"Profile saved");
    } else if (event.kind == UiEvent::Kind::kDistributions) {
      if (!event.status.Ok()) {
        SetStatus(Utf8ToWide(event.status.message));
        continue;
      }
      distributions_ = std::move(event.distributions);
      PopulateDistributions();
      SetStatus(L"WSL distributions discovered");
    } else {
      ApplyDoctorEvent(std::move(event.doctor));
    }
  }
}

void ToolchainDoctorWindow::PopulateProfiles() {
  SendMessageW(profile_selector_, CB_RESETCONTENT, 0, 0);
  int selected = 0;
  for (std::size_t index = 0; index < settings_.profiles.size(); ++index) {
    const std::wstring name = Utf8ToWide(settings_.profiles[index].name);
    SendMessageW(profile_selector_, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(name.c_str()));
    if (settings_.profiles[index].id == settings_.selected_profile_id) {
      selected = static_cast<int>(index);
    }
  }
  SendMessageW(profile_selector_, CB_SETCURSEL, selected, 0);
  EnableWindow(delete_button_, settings_.profiles.size() > 1);
  PopulateSelectedProfile();
}

void ToolchainDoctorWindow::PopulateSelectedProfile() {
  const int selected =
      static_cast<int>(SendMessageW(profile_selector_, CB_GETCURSEL, 0, 0));
  if (selected < 0 ||
      static_cast<std::size_t>(selected) >= settings_.profiles.size()) {
    return;
  }
  const core::ToolchainProfile& profile = settings_.profiles[selected];
  SetWindowTextW(name_edit_, Utf8ToWide(profile.name).c_str());
  PopulateDistributions();
  SetWindowTextW(openlane_edit_, Utf8ToWide(profile.openlane_root).c_str());
  SetWindowTextW(orfs_edit_, Utf8ToWide(profile.orfs_root).c_str());
  SetWindowTextW(pdk_edit_, Utf8ToWide(profile.pdk_root).c_str());
  SetWindowTextW(cpu_edit_, std::to_wstring(profile.cpu_budget).c_str());
}

bool ToolchainDoctorWindow::ReadSelectedProfile(
    core::ToolchainProfile* profile) {
  const int selected =
      static_cast<int>(SendMessageW(profile_selector_, CB_GETCURSEL, 0, 0));
  if (profile == nullptr || selected < 0 ||
      static_cast<std::size_t>(selected) >= settings_.profiles.size()) {
    SetStatus(L"Select a toolchain profile");
    return false;
  }
  *profile = settings_.profiles[selected];
  profile->name = WideToUtf8(WindowText(name_edit_));
  const int distribution_index =
      static_cast<int>(SendMessageW(distribution_edit_, CB_GETCURSEL, 0, 0));
  if (distribution_index < 0 || static_cast<std::size_t>(distribution_index) >=
                                    distribution_values_.size()) {
    SetStatus(L"Select an installed WSL distribution");
    return false;
  }
  profile->wsl_distribution = distribution_values_[distribution_index];
  if (!distributions_.empty()) {
    const auto installed = std::find_if(
        distributions_.begin(), distributions_.end(),
        [profile](const application::WslDistribution& distribution) {
          return distribution.name == profile->wsl_distribution;
        });
    if (installed == distributions_.end()) {
      SetStatus(L"Selected WSL distribution is not installed");
      return false;
    }
    if (installed->version != 2) {
      SetStatus(L"Selected distribution uses WSL1; WSL2 is required");
      return false;
    }
  }
  profile->openlane_root = WideToUtf8(WindowText(openlane_edit_));
  profile->orfs_root = WideToUtf8(WindowText(orfs_edit_));
  profile->pdk_root = WideToUtf8(WindowText(pdk_edit_));
  const std::string cpu_text = WideToUtf8(WindowText(cpu_edit_));
  unsigned int cpu_budget = 0;
  const auto converted = std::from_chars(
      cpu_text.data(), cpu_text.data() + cpu_text.size(), cpu_budget);
  if (converted.ec != std::errc{} ||
      converted.ptr != cpu_text.data() + cpu_text.size()) {
    SetStatus(L"CPU budget must be a positive integer");
    return false;
  }
  profile->cpu_budget = cpu_budget;
  core::ToolchainSettings validation_settings;
  validation_settings.selected_profile_id = profile->id;
  validation_settings.profiles.push_back(*profile);
  const core::Status validation =
      core::ValidateToolchainSettings(validation_settings);
  if (!validation.Ok()) {
    SetStatus(Utf8ToWide(validation.message));
    return false;
  }
  return true;
}

void ToolchainDoctorWindow::PopulateDistributions() {
  if (distribution_edit_ == nullptr) return;
  std::string configured;
  const int profile_index =
      static_cast<int>(SendMessageW(profile_selector_, CB_GETCURSEL, 0, 0));
  if (profile_index >= 0 &&
      static_cast<std::size_t>(profile_index) < settings_.profiles.size()) {
    configured = settings_.profiles[profile_index].wsl_distribution;
  }

  SendMessageW(distribution_edit_, CB_RESETCONTENT, 0, 0);
  distribution_values_.clear();
  int selected = -1;
  int default_index = -1;
  for (const application::WslDistribution& distribution : distributions_) {
    std::wstring label = Utf8ToWide(distribution.name);
    label.append(distribution.is_default ? L" [default, WSL" : L" [WSL");
    label.append(std::to_wstring(distribution.version));
    label.push_back(L']');
    const int index =
        static_cast<int>(SendMessageW(distribution_edit_, CB_ADDSTRING, 0,
                                      reinterpret_cast<LPARAM>(label.c_str())));
    if (index < 0) continue;
    distribution_values_.push_back(distribution.name);
    if (distribution.name == configured) selected = index;
    if (distribution.is_default) default_index = index;
  }
  if (!configured.empty() && selected < 0) {
    std::wstring label = Utf8ToWide(configured);
    label.append(L" [not installed]");
    selected =
        static_cast<int>(SendMessageW(distribution_edit_, CB_ADDSTRING, 0,
                                      reinterpret_cast<LPARAM>(label.c_str())));
    if (selected >= 0) distribution_values_.push_back(configured);
  }
  if (selected < 0) selected = default_index >= 0 ? default_index : 0;
  if (!distribution_values_.empty()) {
    SendMessageW(distribution_edit_, CB_SETCURSEL, selected, 0);
  }
}

void ToolchainDoctorWindow::PopulateChecks() {
  ListView_DeleteAllItems(checks_);
  if (Header_GetItemCount(ListView_GetHeader(checks_)) == 0) {
    const wchar_t* headings[] = {L"Check", L"State", L"Detail"};
    for (int column_index = 0; column_index < 3; ++column_index) {
      LVCOLUMNW column{};
      column.mask = LVCF_TEXT | LVCF_WIDTH;
      column.cx = 100;
      column.pszText = const_cast<wchar_t*>(headings[column_index]);
      ListView_InsertColumn(checks_, column_index, &column);
    }
  }
  for (int index = 0; index < 5; ++index) {
    const auto id = static_cast<application::DoctorCheckId>(index);
    std::wstring name = Utf8ToWide(application::DoctorCheckName(id));
    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = index;
    item.pszText = name.data();
    ListView_InsertItem(checks_, &item);
    SetCheckRow(checks_, id, L"Pending", L"");
  }
}

void ToolchainDoctorWindow::ApplyDoctorEvent(application::DoctorEvent event) {
  if (event.kind == application::DoctorEventKind::kStarted) {
    SetStatus(L"Diagnosing selected profile...");
    return;
  }
  if (event.kind == application::DoctorEventKind::kCheckCompleted) {
    SetCheckRow(checks_, event.check.id, event.check.passed ? L"Pass" : L"Fail",
                Utf8ToWide(event.check.message));
    return;
  }
  SetBusy(false);
  SetStatus(event.status.Ok() ? L"Toolchain profile is ready"
                              : Utf8ToWide(event.status.message));
}

void ToolchainDoctorWindow::SetBusy(bool busy) const {
  EnableWindow(profile_selector_, !busy);
  EnableWindow(distribution_edit_, !busy);
  EnableWindow(add_button_, !busy);
  EnableWindow(duplicate_button_, !busy);
  EnableWindow(delete_button_, !busy && settings_.profiles.size() > 1);
  EnableWindow(save_button_, !busy);
  EnableWindow(diagnose_button_, !busy);
  EnableWindow(repair_wsl_button_, !busy);
  EnableWindow(cancel_button_, busy);
}

void ToolchainDoctorWindow::SetStatus(std::wstring_view text) const {
  if (status_ != nullptr) SetWindowTextW(status_, std::wstring(text).c_str());
}

void ToolchainDoctorWindow::ShutdownChannel() {
  if (event_channel_ == nullptr) return;
  std::scoped_lock lock(event_channel_->mutex);
  event_channel_->accepting = false;
  event_channel_->window = nullptr;
  event_channel_->events.clear();
}

}  // namespace designpp::gui

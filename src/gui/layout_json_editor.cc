// Copyright 2026 The Design++ Authors

#include "designpp/gui/layout_json_editor.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "designpp/adapters/openlane2_adapter.h"
#include "designpp/adapters/orfs_adapter.h"
#include "designpp/application/managed_source_service.h"
#include "designpp/gui/monaco_editor_host.h"

namespace designpp::gui {
namespace {

constexpr wchar_t kWindowClassName[] = L"DesignPlusPlus.LayoutMonacoJsonEditor";
constexpr int kValidateButton = 7781;
constexpr int kFormatButton = 7782;
constexpr int kApplyButton = 7783;
constexpr int kCancelButton = 7784;
constexpr int kStatusLabel = 7785;
constexpr UINT kShutdownMessage = WM_APP + 89;
constexpr UINT kDestroyMessage = WM_APP + 90;

enum class PendingAction { kNone, kValidate, kFormat, kApply, kSave };

struct DialogState {
  core::PhysicalImplementationConfiguration configuration;
  std::shared_ptr<MonacoEditorEnvironment> environment;
  MonacoEditorHost editor;
  std::string session_id;
  std::string document_id;
  LayoutJsonApplyCallback apply_configuration;
  HWND window = nullptr;
  bool saving = false;
  bool close_after_save = false;
  PendingAction pending_action = PendingAction::kNone;
  bool accepted = false;
  bool close_posted = false;
  bool editor_shutdown = false;
  UINT dpi = 96;
  HFONT font = nullptr;
  HWND status = nullptr;
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

std::string NewEditorId(std::string_view prefix) {
  static std::atomic_uint64_t sequence = 0;
  return std::string(prefix) + "-" + std::to_string(GetCurrentProcessId()) +
         "-" + std::to_string(++sequence);
}

HWND CreateControl(HWND parent, const wchar_t* class_name, const wchar_t* text,
                   DWORD style, int id) {
  return CreateWindowExW(0, class_name, text, WS_CHILD | WS_VISIBLE | style, 0,
                         0, 0, 0, parent,
                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                         GetModuleHandleW(nullptr), nullptr);
}

BOOL CALLBACK ApplyFont(HWND window, LPARAM font_value) {
  SendMessageW(window, WM_SETFONT, static_cast<WPARAM>(font_value), TRUE);
  return TRUE;
}

void UpdateFont(HWND window, DialogState* state) {
  NONCLIENTMETRICSW metrics{sizeof(metrics)};
  LOGFONTW logical_font{};
  if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics),
                                 &metrics, 0, state->dpi)) {
    logical_font = metrics.lfMessageFont;
  } else {
    logical_font.lfHeight = -MulDiv(9, static_cast<int>(state->dpi), 72);
    wcscpy_s(logical_font.lfFaceName, L"Segoe UI");
  }
  HFONT replacement = CreateFontIndirectW(&logical_font);
  if (!replacement) return;
  EnumChildWindows(window, ApplyFont, reinterpret_cast<LPARAM>(replacement));
  if (state->font) DeleteObject(state->font);
  state->font = replacement;
}

void SetStatus(DialogState* state, std::string_view text) {
  const std::wstring wide = Utf8ToWide(text);
  SetWindowTextW(state->status, wide.c_str());
}

void SetActionButtonsEnabled(HWND window, bool enabled) {
  for (int id : {kValidateButton, kFormatButton, kApplyButton}) {
    EnableWindow(GetDlgItem(window, id), enabled ? TRUE : FALSE);
  }
}

void ApplyEditorText(HWND window, DialogState* state, std::string text,
                     PendingAction action) {
  if (state->close_posted) return;
  const bool orfs = state->configuration.backend_id == "orfs";
  adapters::OpenLane2Adapter openlane_adapter;
  adapters::OrfsAdapter orfs_adapter;
  core::Result<core::PhysicalImplementationConfiguration> applied =
      orfs ? core::Result<core::PhysicalImplementationConfiguration>(
                 state->configuration)
           : openlane_adapter.ApplyEditableConfiguration(text,
                                                         state->configuration);
  if (orfs) {
    const core::Status status = orfs_adapter.ValidateAdvancedVariables(text);
    if (!status.Ok()) {
      SetStatus(state, status.message);
      state->pending_action = PendingAction::kNone;
      SetActionButtonsEnabled(window, true);
      return;
    }
  }
  if (!applied.Ok()) {
    SetStatus(state, applied.GetStatus().message);
    state->pending_action = PendingAction::kNone;
    SetActionButtonsEnabled(window, true);
    return;
  }
  if (action == PendingAction::kFormat) {
    if (orfs) {
      auto canonical = orfs_adapter.CanonicalizeAdvancedVariables(text);
      if (!canonical.Ok()) {
        SetStatus(state, canonical.GetStatus().message);
      } else {
        state->editor.ReplaceDocumentText(state->document_id,
                                          canonical.Value());
        SetStatus(state, "ORFS variables JSON formatted");
      }
    } else {
      auto formatted =
          openlane_adapter.EncodeEditableConfiguration(applied.Value());
      if (!formatted.Ok()) {
        SetStatus(state, formatted.GetStatus().message);
      } else {
        state->editor.ReplaceDocumentText(state->document_id,
                                          formatted.Value());
        SetStatus(state, "OpenLane JSON formatted");
      }
    }
  } else if (action == PendingAction::kApply ||
             action == PendingAction::kSave) {
    if (orfs) {
      auto canonical = orfs_adapter.CanonicalizeAdvancedVariables(text);
      if (!canonical.Ok()) {
        SetStatus(state, canonical.GetStatus().message);
        state->pending_action = PendingAction::kNone;
        SetActionButtonsEnabled(window, true);
        return;
      }
      state->configuration.orfs.advanced_variables_json =
          std::move(canonical).Value();
      state->editor.ReplaceDocumentText(
          state->document_id,
          state->configuration.orfs.advanced_variables_json);
    } else {
      state->configuration = std::move(applied).Value();
    }
    state->accepted = true;
    if (state->apply_configuration) {
      state->apply_configuration(state->configuration,
                                 action == PendingAction::kSave);
    }
    SetStatus(state, action == PendingAction::kSave
                         ? "Configuration accepted for the selected Cell"
                         : (orfs ? "ORFS variables applied to the setup draft"
                                 : "OpenLane JSON applied to the setup draft"));
  } else {
    SetStatus(state, "OpenLane JSON is valid");
  }
  state->pending_action = PendingAction::kNone;
  SetActionButtonsEnabled(window, !state->saving);
}

void HandleEditorMessage(HWND window, DialogState* state,
                         application::EditorWebMessage message) {
  if (message.type == "document_text") {
    const PendingAction action = state->pending_action;
    if (action != PendingAction::kNone) {
      ApplyEditorText(window, state, std::move(message.text), action);
    }
  } else if (message.type == "save_document") {
    ApplyEditorText(window, state, std::move(message.text),
                    PendingAction::kSave);
  } else if (message.type == "close_document_requested") {
    if (!state->close_posted) {
      // Preserve a configuration that was already accepted with Apply.
      state->close_posted = true;
      PostMessageW(window, kShutdownMessage, 0, 0);
    }
  } else if (message.type == "fatal_error") {
    SetStatus(state, message.text);
    state->pending_action = PendingAction::kNone;
    SetActionButtonsEnabled(window, true);
  }
}

void RequestAction(HWND window, DialogState* state, PendingAction action) {
  if (state->close_posted || state->pending_action != PendingAction::kNone) {
    return;
  }
  state->pending_action = action;
  SetActionButtonsEnabled(window, false);
  SetStatus(state, "Reading editor contents...");
  state->editor.RequestDocumentText(state->document_id);
}

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wparam,
                                 LPARAM lparam) {
  auto* state =
      reinterpret_cast<DialogState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    state = static_cast<DialogState*>(create->lpCreateParams);
    state->window = window;
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
  }
  if (!state) return DefWindowProcW(window, message, wparam, lparam);
  switch (message) {
    case WM_CREATE: {
      state->dpi = GetDpiForWindow(window);
      state->status =
          CreateControl(window, L"STATIC", L"Loading Monaco editor...",
                        SS_LEFT | SS_ENDELLIPSIS, kStatusLabel);
      CreateControl(window, L"BUTTON", L"Validate", BS_PUSHBUTTON,
                    kValidateButton);
      CreateControl(window, L"BUTTON", L"Format", BS_PUSHBUTTON, kFormatButton);
      CreateControl(window, L"BUTTON", L"Apply", BS_DEFPUSHBUTTON,
                    kApplyButton);
      CreateControl(window, L"BUTTON", L"Cancel", BS_PUSHBUTTON, kCancelButton);
      UpdateFont(window, state);
      if (!state->environment ||
          !state->editor.Create(
              window, state->environment, state->session_id,
              [window, state](application::EditorWebMessage editor_message) {
                HandleEditorMessage(window, state, std::move(editor_message));
              },
              [state](core::Status status) {
                SetStatus(state, status.message);
              })) {
        SetStatus(state, "Cannot create the Monaco JSON editor");
        return 0;
      }
      std::string initial_text;
      if (state->configuration.backend_id == "orfs") {
        initial_text = state->configuration.orfs.advanced_variables_json;
      } else {
        adapters::OpenLane2Adapter adapter;
        auto encoded =
            adapter.EncodeEditableConfiguration(state->configuration);
        if (!encoded.Ok()) {
          SetStatus(state, encoded.GetStatus().message);
          return 0;
        }
        initial_text = std::move(encoded).Value();
      }
      application::EditorDocumentSnapshot document;
      document.id = state->document_id;
      document.model_uri = "inmemory://designpp/layout-config.json";
      document.relative_path = "layout-config.json";
      document.text = std::move(initial_text);
      state->editor.OpenDocument(document, "layout-config.json", "json");
      SetStatus(state,
                "Apply updates the draft; Ctrl+S saves the selected Cell");
      return 0;
    }
    case WM_SIZE: {
      const auto px = [state](int value) {
        return MulDiv(value, static_cast<int>(state->dpi), 96);
      };
      const int width = LOWORD(lparam);
      const int height = HIWORD(lparam);
      RECT editor_bounds{px(12), px(12), width - px(12),
                         std::max(px(112), height - px(92))};
      state->editor.Resize(editor_bounds);
      MoveWindow(state->status, px(12), height - px(72),
                 std::max(1, width - px(528)), px(31), TRUE);
      int x = width - px(404);
      for (int id :
           {kValidateButton, kFormatButton, kApplyButton, kCancelButton}) {
        MoveWindow(GetDlgItem(window, id), x, height - px(72), px(92), px(31),
                   TRUE);
        x += px(98);
      }
      return 0;
    }
    case WM_GETMINMAXINFO: {
      auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
      limits->ptMinTrackSize = {MulDiv(620, static_cast<int>(state->dpi), 96),
                                MulDiv(420, static_cast<int>(state->dpi), 96)};
      return 0;
    }
    case WM_DPICHANGED: {
      state->dpi = HIWORD(wparam);
      const auto* suggested = reinterpret_cast<const RECT*>(lparam);
      SetWindowPos(window, nullptr, suggested->left, suggested->top,
                   suggested->right - suggested->left,
                   suggested->bottom - suggested->top,
                   SWP_NOACTIVATE | SWP_NOZORDER);
      UpdateFont(window, state);
      return 0;
    }
    case WM_COMMAND:
      switch (LOWORD(wparam)) {
        case kValidateButton:
          RequestAction(window, state, PendingAction::kValidate);
          return 0;
        case kFormatButton:
          RequestAction(window, state, PendingAction::kFormat);
          return 0;
        case kApplyButton:
          RequestAction(window, state, PendingAction::kApply);
          return 0;
        case kCancelButton:
          if (state->saving) {
            state->close_after_save = true;
            return 0;
          }
          if (!state->close_posted) {
            state->accepted = false;
            state->close_posted = true;
            PostMessageW(window, kShutdownMessage, 0, 0);
          }
          return 0;
      }
      break;
    case WM_CLOSE:
      if (state->saving) {
        state->close_after_save = true;
        return 0;
      }
      if (!state->close_posted) {
        state->accepted = false;
        state->close_posted = true;
        PostMessageW(window, kShutdownMessage, 0, 0);
      }
      return 0;
    case kShutdownMessage:
      if (!state->editor_shutdown) {
        state->editor.Shutdown();
        state->editor_shutdown = true;
      }
      PostMessageW(window, kDestroyMessage, 0, 0);
      return 0;
    case kDestroyMessage:
      DestroyWindow(window);
      return 0;
    case WM_DESTROY:
      EnableWindow(GetWindow(window, GW_OWNER), TRUE);
      state->window = nullptr;
      if (!state->editor_shutdown) {
        state->editor.Shutdown();
        state->editor_shutdown = true;
      }
      if (state->font) {
        DeleteObject(state->font);
        state->font = nullptr;
      }
      return 0;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

class JsonSession final : public LayoutJsonSession {
 public:
  DialogState state;
  ~JsonSession() override { Close(); }
  bool IsOpen() const override { return state.window != nullptr; }
  void Close() override {
    if (!state.editor_shutdown) {
      state.editor.Shutdown();
      state.editor_shutdown = true;
    }
    if (state.window) DestroyWindow(state.window);
  }
  void Saving() override {
    state.saving = true;
    SetActionButtonsEnabled(state.window, false);
    SetStatus(&state, "Saving selected Cell...");
  }
  void Saved(const core::Status& status) override {
    state.saving = false;
    if (!state.window) return;
    SetActionButtonsEnabled(state.window, true);
    SetStatus(&state,
              status.Ok() ? "Saved to the selected Cell" : status.message);
    if (status.Ok() && state.close_after_save)
      PostMessageW(state.window, WM_CLOSE, 0, 0);
    state.close_after_save = false;
  }
};

}  // namespace

std::unique_ptr<LayoutJsonSession> ShowLayoutJsonEditor(
    HWND owner, std::shared_ptr<MonacoEditorEnvironment> editor_environment,
    const core::PhysicalImplementationConfiguration& configuration,
    LayoutJsonApplyCallback apply_configuration) {
  if (!owner || !editor_environment) return {};
  WNDCLASSEXW window_class{sizeof(window_class)};
  window_class.lpfnWndProc = WindowProcedure;
  window_class.hInstance = GetModuleHandleW(nullptr);
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  window_class.lpszClassName = kWindowClassName;
  if (!RegisterClassExW(&window_class) &&
      GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return {};
  }
  auto session = std::make_unique<JsonSession>();
  auto& state = session->state;
  state.configuration = configuration;
  state.apply_configuration = std::move(apply_configuration);
  state.environment = std::move(editor_environment);
  state.session_id = NewEditorId("layout-json-session");
  state.document_id = NewEditorId("layout-json-document");
  RECT owner_rect{};
  GetWindowRect(owner, &owner_rect);
  const UINT dpi = GetDpiForWindow(owner);
  HMONITOR monitor = MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST);
  MONITORINFO monitor_info{sizeof(monitor_info)};
  GetMonitorInfoW(monitor, &monitor_info);
  const int available_width =
      monitor_info.rcWork.right - monitor_info.rcWork.left;
  const int available_height =
      monitor_info.rcWork.bottom - monitor_info.rcWork.top;
  const int width =
      std::min(MulDiv(920, dpi, 96), std::max(400, available_width - 24));
  const int height =
      std::min(MulDiv(700, dpi, 96), std::max(300, available_height - 24));
  const int x =
      std::clamp(owner_rect.left + MulDiv(40, dpi, 96),
                 monitor_info.rcWork.left, monitor_info.rcWork.right - width);
  const int y =
      std::clamp(owner_rect.top + MulDiv(40, dpi, 96), monitor_info.rcWork.top,
                 monitor_info.rcWork.bottom - height);
  HWND dialog = CreateWindowExW(
      WS_EX_DLGMODALFRAME, kWindowClassName,
      state.configuration.backend_id == "orfs" ? L"ORFS Variables JSON"
                                               : L"OpenLane JSON Configuration",
      WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MAXIMIZEBOX |
          WS_CLIPCHILDREN,
      x, y, width, height, owner, nullptr, GetModuleHandleW(nullptr), &state);
  if (!dialog) return {};
  // Only one editing surface can mutate the shared draft at a time. The
  // application message loop remains in control; no nested modal loop is used.
  EnableWindow(owner, FALSE);
  ShowWindow(dialog, SW_SHOW);
  return session;
}

}  // namespace designpp::gui

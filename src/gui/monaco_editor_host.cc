// Copyright 2026 The Design++ Authors

#include "designpp/gui/monaco_editor_host.h"

#include <combaseapi.h>
#include <unknwn.h>
#include <wrl.h>

// WebView2.h requires the COM interface declarations above.
#include <WebView2.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace designpp::gui {
namespace {

using core::ErrorCode;
using core::Status;
using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

// `.example` is a reserved special-use domain. WebView2 explicitly recommends
// avoiding `.local` because it can invoke local-network name handling.
constexpr wchar_t kEditorHostName[] = L"designpp-editor.example";
constexpr wchar_t kEditorOrigin[] = L"https://designpp-editor.example/";
constexpr wchar_t kEditorDocumentUrl[] =
    L"https://designpp-editor.example/index.html";

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

std::filesystem::path ExecutableDirectory() {
  std::wstring path(32768, L'\0');
  const DWORD size =
      GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  path.resize(size);
  return std::filesystem::path(path).parent_path();
}

std::filesystem::path UserDataDirectory() {
  std::vector<wchar_t> local_app_data(32768);
  const DWORD size =
      GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data.data(),
                              static_cast<DWORD>(local_app_data.size()));
  const std::filesystem::path root =
      size > 0 && size < local_app_data.size()
          ? std::filesystem::path(local_app_data.data())
          : std::filesystem::temp_directory_path();
  return root / L"Design++" / L"WebView2" / Utf8ToWide(NewUuid());
}

Status HresultStatus(std::string message, HRESULT result) {
  char detail[32]{};
  std::snprintf(detail, sizeof(detail), " (HRESULT 0x%08lX)",
                static_cast<unsigned long>(result));
  message += detail;
  return {ErrorCode::kIoError, std::move(message),
          static_cast<unsigned long>(result)};
}

Status WebErrorStatus(std::string message,
                      COREWEBVIEW2_WEB_ERROR_STATUS web_error) {
  char detail[48]{};
  std::snprintf(detail, sizeof(detail), " (WebView2 status %d)",
                static_cast<int>(web_error));
  message += detail;
  return {ErrorCode::kIoError, std::move(message),
          static_cast<unsigned long>(web_error)};
}

std::string Envelope(std::string_view type, std::string_view session_id,
                     std::string_view fields = {}) {
  std::string json =
      "{\"protocol\":1,\"type\":" + application::EscapeEditorJson(type) +
      ",\"session_id\":" + application::EscapeEditorJson(session_id);
  if (!fields.empty()) json += "," + std::string(fields);
  json += "}";
  return json;
}

std::string SeverityName(core::DiagnosticSeverity severity) {
  return severity == core::DiagnosticSeverity::kError ? "error" : "warning";
}

}  // namespace

struct MonacoEditorEnvironment::Implementation final {
  using ControllerCallback =
      std::function<void(HRESULT, ICoreWebView2Controller*)>;
  struct PendingController {
    HWND parent = nullptr;
    ControllerCallback callback;
  };

  void Initialize(std::shared_ptr<MonacoEditorEnvironment> owner) {
    const std::filesystem::path user_data = UserDataDirectory();
    std::error_code error;
    std::filesystem::create_directories(user_data, error);
    const HRESULT result = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, user_data.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [owner = std::move(owner)](HRESULT result,
                                       ICoreWebView2Environment* environment) {
              Implementation* implementation = owner->implementation_.get();
              implementation->initializing = false;
              implementation->initialization_result = result;
              if (SUCCEEDED(result) && environment != nullptr) {
                implementation->environment = environment;
              }
              std::vector<PendingController> pending;
              pending.swap(implementation->pending);
              for (PendingController& request : pending) {
                implementation->CreateController(request.parent,
                                                 std::move(request.callback));
              }
              return S_OK;
            })
            .Get());
    if (FAILED(result)) {
      initializing = false;
      initialization_result = result;
    }
  }

  void CreateController(HWND parent, ControllerCallback callback) {
    if (initializing) {
      pending.push_back({parent, std::move(callback)});
      return;
    }
    if (!environment || FAILED(initialization_result)) {
      callback(initialization_result, nullptr);
      return;
    }
    environment->CreateCoreWebView2Controller(
        parent,
        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [callback = std::move(callback)](
                HRESULT result, ICoreWebView2Controller* controller) {
              callback(result, controller);
              return S_OK;
            })
            .Get());
  }

  ComPtr<ICoreWebView2Environment> environment;
  std::vector<PendingController> pending;
  HRESULT initialization_result = E_PENDING;
  bool initializing = true;
};

MonacoEditorEnvironment::MonacoEditorEnvironment(
    std::unique_ptr<Implementation> implementation)
    : implementation_(std::move(implementation)) {}

MonacoEditorEnvironment::~MonacoEditorEnvironment() = default;

std::shared_ptr<MonacoEditorEnvironment> MonacoEditorEnvironment::Create() {
  auto environment = std::shared_ptr<MonacoEditorEnvironment>(
      new MonacoEditorEnvironment(std::make_unique<Implementation>()));
  environment->implementation_->Initialize(environment);
  return environment;
}

struct MonacoEditorHost::Implementation final {
  struct Lifetime final {
    Implementation* implementation = nullptr;
  };

  void PostNow(std::string_view json) const {
    if (!webview) return;
    const std::wstring wide = Utf8ToWide(json);
    webview->PostWebMessageAsJson(wide.c_str());
  }

  void Post(std::string json) {
    if (!ready && json.find("\"type\":\"initialize\"") == std::string::npos &&
        json.find("\"type\":\"shutdown\"") == std::string::npos) {
      pending_messages.push_back(std::move(json));
      return;
    }
    PostNow(json);
  }

  void InitializeWebView(ICoreWebView2Controller* created_controller) {
    controller = created_controller;
    if (!controller || FAILED(controller->get_CoreWebView2(&webview))) {
      status_callback(
          HresultStatus("Cannot acquire the WebView2 controller", E_FAIL));
      return;
    }
    controller->put_Bounds(bounds);
    ComPtr<ICoreWebView2Settings> settings;
    if (SUCCEEDED(webview->get_Settings(&settings))) {
      settings->put_IsScriptEnabled(TRUE);
      settings->put_IsWebMessageEnabled(TRUE);
      settings->put_AreDefaultScriptDialogsEnabled(FALSE);
#ifdef NDEBUG
      settings->put_AreDevToolsEnabled(FALSE);
      ComPtr<ICoreWebView2Settings3> settings3;
      if (SUCCEEDED(settings.As(&settings3))) {
        settings3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
      }
#endif
    }
    ComPtr<ICoreWebView2_3> webview3;
    if (FAILED(webview.As(&webview3))) {
      status_callback(HresultStatus(
          "The installed WebView2 Runtime lacks virtual host mapping",
          E_NOINTERFACE));
      return;
    }
    const std::filesystem::path assets =
        ExecutableDirectory() / L"assets" / L"editor";
    if (!std::filesystem::exists(assets / L"index.html")) {
      status_callback(
          Status{ErrorCode::kNotFound, "Monaco editor assets are missing", 0});
      return;
    }
    const HRESULT mapping_result = webview3->SetVirtualHostNameToFolderMapping(
        kEditorHostName, assets.c_str(),
        COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
    if (FAILED(mapping_result)) {
      status_callback(
          HresultStatus("Cannot map Monaco editor assets", mapping_result));
      return;
    }
    webview->add_NavigationStarting(
        Callback<ICoreWebView2NavigationStartingEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) {
              LPWSTR uri = nullptr;
              if (SUCCEEDED(args->get_Uri(&uri))) {
                const bool allowed =
                    std::wstring_view(uri).starts_with(kEditorOrigin);
                CoTaskMemFree(uri);
                if (!allowed) args->put_Cancel(TRUE);
              }
              return S_OK;
            })
            .Get(),
        &navigation_token);
    webview->add_NavigationCompleted(
        Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [this](ICoreWebView2*,
                   ICoreWebView2NavigationCompletedEventArgs* args) {
              BOOL succeeded = FALSE;
              if (FAILED(args->get_IsSuccess(&succeeded)) || succeeded) {
                return S_OK;
              }
              COREWEBVIEW2_WEB_ERROR_STATUS web_error =
                  COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
              args->get_WebErrorStatus(&web_error);
              status_callback(WebErrorStatus(
                  "Cannot load the local Monaco editor assets", web_error));
              return S_OK;
            })
            .Get(),
        &navigation_completed_token);
    webview->add_NewWindowRequested(
        Callback<ICoreWebView2NewWindowRequestedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) {
              args->put_Handled(TRUE);
              return S_OK;
            })
            .Get(),
        &new_window_token);
    webview->add_PermissionRequested(
        Callback<ICoreWebView2PermissionRequestedEventHandler>(
            [](ICoreWebView2*,
               ICoreWebView2PermissionRequestedEventArgs* args) {
              args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY);
              return S_OK;
            })
            .Get(),
        &permission_token);
    ComPtr<ICoreWebView2_4> webview4;
    if (SUCCEEDED(webview.As(&webview4))) {
      webview4->add_DownloadStarting(
          Callback<ICoreWebView2DownloadStartingEventHandler>(
              [](ICoreWebView2*, ICoreWebView2DownloadStartingEventArgs* args) {
                args->put_Cancel(TRUE);
                return S_OK;
              })
              .Get(),
          &download_token);
    }
    webview->add_WebMessageReceived(
        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [this](ICoreWebView2*,
                   ICoreWebView2WebMessageReceivedEventArgs* args) {
              LPWSTR source = nullptr;
              LPWSTR json = nullptr;
              if (FAILED(args->get_Source(&source)) || source == nullptr ||
                  !std::wstring_view(source).starts_with(kEditorOrigin) ||
                  FAILED(args->get_WebMessageAsJson(&json)) ||
                  json == nullptr) {
                CoTaskMemFree(source);
                CoTaskMemFree(json);
                return S_OK;
              }
              const std::string utf8 = WideToUtf8(json);
              CoTaskMemFree(source);
              CoTaskMemFree(json);
              auto decoded =
                  application::DecodeEditorWebMessage(utf8, session_id);
              if (!decoded.Ok()) return S_OK;
              if (decoded.Value().type == "ready_for_initialize") {
                const UINT dpi = GetDpiForWindow(parent);
                Post(Envelope(
                    "initialize", session_id,
                    "\"theme\":\"dark\",\"read_only\":false," +
                        std::string("\"font_family\":") +
                        application::EscapeEditorJson(
                            "Cascadia Mono, Consolas, monospace") +
                        ",\"font_size\":14,\"dpi\":" + std::to_string(dpi)));
              } else {
                if (decoded.Value().type == "ready") {
                  ready = true;
                  std::vector<std::string> pending;
                  pending.swap(pending_messages);
                  for (const std::string& message : pending) PostNow(message);
                }
                message_callback(std::move(decoded).Value());
              }
              return S_OK;
            })
            .Get(),
        &message_token);
    const HRESULT navigation_result = webview->Navigate(kEditorDocumentUrl);
    if (FAILED(navigation_result)) {
      status_callback(HresultStatus("Cannot navigate to the Monaco editor",
                                    navigation_result));
    }
  }

  HWND parent = nullptr;
  std::string session_id;
  MessageCallback message_callback;
  StatusCallback status_callback;
  std::shared_ptr<MonacoEditorEnvironment> environment;
  ComPtr<ICoreWebView2Controller> controller;
  ComPtr<ICoreWebView2> webview;
  RECT bounds{};
  EventRegistrationToken navigation_token{};
  EventRegistrationToken navigation_completed_token{};
  EventRegistrationToken new_window_token{};
  EventRegistrationToken permission_token{};
  EventRegistrationToken download_token{};
  EventRegistrationToken message_token{};
  bool ready = false;
  bool shutting_down = false;
  std::vector<std::string> pending_messages;
  std::shared_ptr<Lifetime> lifetime = std::make_shared<Lifetime>();
};

MonacoEditorHost::MonacoEditorHost()
    : implementation_(std::make_unique<Implementation>()) {}

MonacoEditorHost::~MonacoEditorHost() { Shutdown(); }

bool MonacoEditorHost::Create(
    HWND parent, std::shared_ptr<MonacoEditorEnvironment> environment,
    std::string session_id, MessageCallback message_callback,
    StatusCallback status_callback) {
  if (parent == nullptr || !environment || session_id.empty()) return false;
  implementation_->parent = parent;
  implementation_->environment = environment;
  implementation_->session_id = std::move(session_id);
  implementation_->message_callback = std::move(message_callback);
  implementation_->status_callback = std::move(status_callback);
  implementation_->lifetime->implementation = implementation_.get();
  const std::weak_ptr<Implementation::Lifetime> weak_lifetime =
      implementation_->lifetime;
  environment->implementation_->CreateController(
      parent,
      [weak_lifetime](HRESULT result, ICoreWebView2Controller* controller) {
        const auto lifetime = weak_lifetime.lock();
        if (!lifetime || lifetime->implementation == nullptr) return;
        Implementation* implementation = lifetime->implementation;
        if (implementation->shutting_down) return;
        if (FAILED(result) || controller == nullptr) {
          implementation->status_callback(
              HresultStatus("Cannot initialize the WebView2 editor", result));
          return;
        }
        implementation->InitializeWebView(controller);
      });
  return true;
}

void MonacoEditorHost::Resize(const RECT& bounds) const {
  implementation_->bounds = bounds;
  if (implementation_->controller) {
    implementation_->controller->put_Bounds(bounds);
  }
}

bool MonacoEditorHost::Ready() const noexcept { return implementation_->ready; }

bool MonacoEditorHost::ContainsFocus() const noexcept {
  return implementation_->controller &&
         IsChild(implementation_->parent, GetFocus());
}

void MonacoEditorHost::OpenDocument(
    const application::EditorDocumentSnapshot& document,
    std::string_view display_name, std::string_view language) {
  std::string fields =
      "\"document_id\":" + application::EscapeEditorJson(document.id) +
      ",\"model_uri\":" + application::EscapeEditorJson(document.model_uri) +
      ",\"display_name\":" + application::EscapeEditorJson(display_name) +
      ",\"language\":" + application::EscapeEditorJson(language) +
      ",\"text\":" + application::EscapeEditorJson(document.text) +
      ",\"version\":1,\"read_only\":" +
      std::string(document.read_only ? "true" : "false");
  implementation_->Post(
      Envelope("open_document", implementation_->session_id, fields));
}

void MonacoEditorHost::CloseDocument(std::string_view document_id) {
  implementation_->Post(Envelope(
      "close_document", implementation_->session_id,
      "\"document_id\":" + application::EscapeEditorJson(document_id)));
}

void MonacoEditorHost::SaveResult(std::string_view document_id, bool succeeded,
                                  std::string_view message) {
  implementation_->Post(
      Envelope("save_result", implementation_->session_id,
               "\"document_id\":" + application::EscapeEditorJson(document_id) +
                   ",\"succeeded\":" + (succeeded ? "true" : "false") +
                   ",\"message\":" + application::EscapeEditorJson(message)));
}

void MonacoEditorHost::SetDiagnostics(
    const std::vector<core::Diagnostic>& diagnostics,
    const std::vector<application::EditorDocumentSnapshot>& documents) {
  std::string fields = "\"diagnostics\":[";
  bool first = true;
  for (const core::Diagnostic& diagnostic : diagnostics) {
    auto document = std::find_if(
        documents.begin(), documents.end(), [&](const auto& candidate) {
          return std::filesystem::path(Utf8ToWide(candidate.relative_path))
                         .filename()
                         .wstring() == Utf8ToWide(diagnostic.file) ||
                 candidate.relative_path == diagnostic.file;
        });
    if (document == documents.end()) continue;
    if (!first) fields += ',';
    first = false;
    fields +=
        "{\"document_id\":" + application::EscapeEditorJson(document->id) +
        ",\"severity\":" +
        application::EscapeEditorJson(SeverityName(diagnostic.severity)) +
        ",\"code\":" + application::EscapeEditorJson(diagnostic.code) +
        ",\"message\":" + application::EscapeEditorJson(diagnostic.message) +
        ",\"line\":" + std::to_string(diagnostic.line) +
        ",\"column\":" + std::to_string(diagnostic.column) + "}";
  }
  fields += "]";
  implementation_->Post(
      Envelope("set_diagnostics", implementation_->session_id, fields));
}

void MonacoEditorHost::RevealLocation(std::string_view document_id,
                                      std::uint32_t line,
                                      std::uint32_t column) {
  implementation_->Post(
      Envelope("reveal_location", implementation_->session_id,
               "\"document_id\":" + application::EscapeEditorJson(document_id) +
                   ",\"line\":" + std::to_string(std::max(1u, line)) +
                   ",\"column\":" + std::to_string(std::max(1u, column))));
}

void MonacoEditorHost::SetReadOnly(std::string_view document_id,
                                   bool read_only) {
  implementation_->Post(
      Envelope("set_read_only", implementation_->session_id,
               "\"document_id\":" + application::EscapeEditorJson(document_id) +
                   ",\"read_only\":" + (read_only ? "true" : "false")));
}

void MonacoEditorHost::RequestSaveAll() {
  implementation_->Post(
      Envelope("request_save_all", implementation_->session_id));
}

void MonacoEditorHost::Shutdown() {
  if (!implementation_ || implementation_->shutting_down) return;
  implementation_->shutting_down = true;
  implementation_->lifetime->implementation = nullptr;
  implementation_->Post(Envelope("shutdown", implementation_->session_id));
  if (implementation_->webview) {
    ComPtr<ICoreWebView2_4> webview4;
    if (SUCCEEDED(implementation_->webview.As(&webview4))) {
      webview4->remove_DownloadStarting(implementation_->download_token);
    }
    implementation_->webview->remove_WebMessageReceived(
        implementation_->message_token);
    implementation_->webview->remove_PermissionRequested(
        implementation_->permission_token);
    implementation_->webview->remove_NewWindowRequested(
        implementation_->new_window_token);
    implementation_->webview->remove_NavigationStarting(
        implementation_->navigation_token);
    implementation_->webview->remove_NavigationCompleted(
        implementation_->navigation_completed_token);
  }
  if (implementation_->controller) implementation_->controller->Close();
  implementation_->webview.Reset();
  implementation_->controller.Reset();
  implementation_->environment.reset();
}

}  // namespace designpp::gui

// Copyright 2026 The Design++ Authors

#include "designpp/application/recent_workspace_store.h"

#include <algorithm>
#include <utility>

namespace designpp::application {
namespace {

constexpr wchar_t kDefaultSubkey[] = L"Software\\DesignPlusPlus";
constexpr wchar_t kValueName[] = L"RecentWorkspacesV1";
constexpr wchar_t kWriterMutexName[] =
    L"Local\\DesignPlusPlus.RecentWorkspaceStore.v1";

class MutexLease final {
 public:
  MutexLease() = default;
  MutexLease(const MutexLease&) = delete;
  MutexLease& operator=(const MutexLease&) = delete;
  ~MutexLease() {
    if (mutex_ == nullptr) return;
    if (acquired_) ReleaseMutex(mutex_);
    CloseHandle(mutex_);
  }

  [[nodiscard]] core::Status Acquire() {
    mutex_ = CreateMutexW(nullptr, FALSE, kWriterMutexName);
    if (mutex_ == nullptr) {
      return {core::ErrorCode::kIoError,
              "Cannot open recent workspace writer lock", GetLastError()};
    }
    const DWORD result = WaitForSingleObject(mutex_, 5000);
    if (result != WAIT_OBJECT_0 && result != WAIT_ABANDONED) {
      return {core::ErrorCode::kConflict,
              "Recent workspaces are being updated by another process", 0};
    }
    acquired_ = true;
    return core::Status::Success();
  }

 private:
  HANDLE mutex_ = nullptr;
  bool acquired_ = false;
};

std::wstring Utf8ToWide(std::string_view text) {
  if (text.empty()) return {};
  const int size =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), nullptr, 0);
  if (size <= 0) return {};
  std::wstring result(static_cast<std::size_t>(size), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), result.data(),
                          size) <= 0) {
    return {};
  }
  return result;
}

std::string WideToUtf8(std::wstring_view text) {
  if (text.empty()) return {};
  const int size = WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
      nullptr, 0, nullptr, nullptr);
  if (size <= 0) return {};
  std::string result(static_cast<std::size_t>(size), '\0');
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), result.data(), size,
                          nullptr, nullptr) <= 0) {
    return {};
  }
  return result;
}

bool Valid(const RecentWorkspace& workspace) {
  return !workspace.request.library_id.empty() &&
         !workspace.request.cell_id.empty() &&
         !workspace.request.view_id.empty() && !workspace.display_name.empty();
}

bool SameWorkspace(const RecentWorkspace& left, const RecentWorkspace& right) {
  return left.request.library_id == right.request.library_id &&
         left.request.cell_id == right.request.cell_id &&
         left.request.view_id == right.request.view_id;
}

}  // namespace

RecentWorkspaceStore::RecentWorkspaceStore()
    : registry_subkey_(kDefaultSubkey) {}

RecentWorkspaceStore::RecentWorkspaceStore(std::wstring registry_subkey)
    : registry_subkey_(std::move(registry_subkey)) {}

core::Result<std::vector<RecentWorkspace>> RecentWorkspaceStore::Load() const {
  HKEY key = nullptr;
  const LONG opened = RegOpenKeyExW(HKEY_CURRENT_USER, registry_subkey_.c_str(),
                                    0, KEY_QUERY_VALUE, &key);
  if (opened == ERROR_FILE_NOT_FOUND) return std::vector<RecentWorkspace>{};
  if (opened != ERROR_SUCCESS) {
    return core::Status{core::ErrorCode::kIoError,
                        "Cannot open recent workspace settings",
                        static_cast<unsigned long>(opened)};
  }
  DWORD type = 0;
  DWORD bytes = 0;
  LONG queried =
      RegQueryValueExW(key, kValueName, nullptr, &type, nullptr, &bytes);
  if (queried == ERROR_FILE_NOT_FOUND) {
    RegCloseKey(key);
    return std::vector<RecentWorkspace>{};
  }
  if (queried != ERROR_SUCCESS || type != REG_MULTI_SZ || bytes > 256 * 1024 ||
      bytes % sizeof(wchar_t) != 0) {
    RegCloseKey(key);
    return core::Status{core::ErrorCode::kCorruptData,
                        "Recent workspace settings are invalid",
                        static_cast<unsigned long>(queried)};
  }
  std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
  queried = RegQueryValueExW(key, kValueName, nullptr, &type,
                             reinterpret_cast<BYTE*>(buffer.data()), &bytes);
  RegCloseKey(key);
  if (queried != ERROR_SUCCESS) {
    return core::Status{core::ErrorCode::kIoError,
                        "Cannot read recent workspace settings",
                        static_cast<unsigned long>(queried)};
  }

  std::vector<std::wstring> fields;
  const wchar_t* cursor = buffer.data();
  const wchar_t* end = buffer.data() + bytes / sizeof(wchar_t);
  while (cursor < end && *cursor != L'\0') {
    const std::size_t length = std::wcslen(cursor);
    if (cursor + length >= end) {
      return core::Status{core::ErrorCode::kCorruptData,
                          "Recent workspace entry is unterminated", 0};
    }
    fields.emplace_back(cursor, length);
    cursor += length + 1;
  }
  if (fields.size() % 4 != 0) {
    return core::Status{core::ErrorCode::kCorruptData,
                        "Recent workspace entry is incomplete", 0};
  }
  std::vector<RecentWorkspace> result;
  for (std::size_t index = 0;
       index < fields.size() && result.size() < kMaximumEntries; index += 4) {
    RecentWorkspace workspace{
        {WideToUtf8(fields[index]), WideToUtf8(fields[index + 1]),
         WideToUtf8(fields[index + 2])},
        WideToUtf8(fields[index + 3])};
    if (Valid(workspace)) result.push_back(std::move(workspace));
  }
  return result;
}

core::Status RecentWorkspaceStore::Touch(
    const RecentWorkspace& workspace) const {
  if (!Valid(workspace)) {
    return {core::ErrorCode::kInvalidArgument,
            "Recent workspace entry is incomplete", 0};
  }
  MutexLease lease;
  core::Status locked = lease.Acquire();
  if (!locked.Ok()) return locked;
  auto loaded = Load();
  if (!loaded.Ok()) return loaded.GetStatus();
  std::vector<RecentWorkspace> workspaces = std::move(loaded).Value();
  std::erase_if(workspaces, [&workspace](const RecentWorkspace& candidate) {
    return SameWorkspace(candidate, workspace);
  });
  workspaces.insert(workspaces.begin(), workspace);
  if (workspaces.size() > kMaximumEntries) {
    workspaces.resize(kMaximumEntries);
  }
  return Save(workspaces);
}

core::Status RecentWorkspaceStore::Clear() const { return Replace({}); }

core::Status RecentWorkspaceStore::Replace(
    const std::vector<RecentWorkspace>& workspaces) const {
  MutexLease lease;
  core::Status locked = lease.Acquire();
  if (!locked.Ok()) return locked;
  std::vector<RecentWorkspace> bounded = workspaces;
  if (bounded.size() > kMaximumEntries) bounded.resize(kMaximumEntries);
  return Save(bounded);
}

core::Status RecentWorkspaceStore::Save(
    const std::vector<RecentWorkspace>& workspaces) const {
  std::vector<wchar_t> data;
  for (const RecentWorkspace& workspace : workspaces) {
    if (!Valid(workspace)) continue;
    for (const std::string* field :
         {&workspace.request.library_id, &workspace.request.cell_id,
          &workspace.request.view_id, &workspace.display_name}) {
      const std::wstring value = Utf8ToWide(*field);
      if (value.empty()) {
        return {core::ErrorCode::kInvalidEncoding,
                "Recent workspace contains invalid UTF-8", 0};
      }
      data.insert(data.end(), value.begin(), value.end());
      data.push_back(L'\0');
    }
  }
  data.push_back(L'\0');
  if (data.size() == 1) data.push_back(L'\0');
  HKEY key = nullptr;
  DWORD disposition = 0;
  const LONG created = RegCreateKeyExW(
      HKEY_CURRENT_USER, registry_subkey_.c_str(), 0, nullptr,
      REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &key, &disposition);
  if (created != ERROR_SUCCESS) {
    return {core::ErrorCode::kIoError,
            "Cannot create recent workspace settings",
            static_cast<unsigned long>(created)};
  }
  const LONG written =
      RegSetValueExW(key, kValueName, 0, REG_MULTI_SZ,
                     reinterpret_cast<const BYTE*>(data.data()),
                     static_cast<DWORD>(data.size() * sizeof(wchar_t)));
  RegCloseKey(key);
  if (written != ERROR_SUCCESS) {
    return {core::ErrorCode::kIoError, "Cannot save recent workspace settings",
            static_cast<unsigned long>(written)};
  }
  return core::Status::Success();
}

}  // namespace designpp::application

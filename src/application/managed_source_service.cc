// Copyright 2026 The Design++ Authors

#include "designpp/application/managed_source_service.h"

#include <combaseapi.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "designpp/application/module_scanner.h"
#include "designpp/core/project.h"

namespace designpp::application {
namespace {

using core::ErrorCode;
using core::Status;

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
  std::transform(id.begin(), id.end(), id.begin(), [](char character) {
    return static_cast<char>(
        std::tolower(static_cast<unsigned char>(character)));
  });
  return id;
}

std::string UtcNow() {
  SYSTEMTIME time{};
  GetSystemTime(&time);
  char buffer[32]{};
  std::snprintf(buffer, sizeof(buffer), "%04u-%02u-%02uT%02u:%02u:%02uZ",
                time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
                time.wSecond);
  return buffer;
}

class WriterLease final {
 public:
  explicit WriterLease(const std::filesystem::path& path) {
    handle_ =
        CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                    nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) return;
    OVERLAPPED overlapped{};
    if (!LockFileEx(handle_,
                    LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1,
                    0, &overlapped)) {
      CloseHandle(handle_);
      handle_ = INVALID_HANDLE_VALUE;
    }
  }
  WriterLease(const WriterLease&) = delete;
  WriterLease& operator=(const WriterLease&) = delete;
  ~WriterLease() {
    if (handle_ == INVALID_HANDLE_VALUE) return;
    OVERLAPPED overlapped{};
    UnlockFileEx(handle_, 0, 1, 0, &overlapped);
    CloseHandle(handle_);
  }
  [[nodiscard]] bool Acquired() const noexcept {
    return handle_ != INVALID_HANDLE_VALUE;
  }

 private:
  HANDLE handle_ = INVALID_HANDLE_VALUE;
};

Status Win32Status(ErrorCode code, std::string message,
                   unsigned long error = GetLastError()) {
  return {code, std::move(message), error};
}

bool IsValidUtf8(std::string_view text) {
  if (text.empty()) return true;
  return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                             static_cast<int>(text.size()), nullptr, 0) > 0;
}

std::string HashBytes(std::string_view bytes) {
  std::uint64_t value = 14695981039346656037ull;
  for (const unsigned char character : bytes) {
    value ^= character;
    value *= 1099511628211ull;
  }
  std::array<char, 17> buffer{};
  std::to_chars(buffer.data(), buffer.data() + 16, value, 16);
  return std::string(buffer.data(), 16);
}

core::Result<std::string> ReadBytes(const std::filesystem::path& path) {
  HANDLE file =
      CreateFileW(path.c_str(), GENERIC_READ,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    const unsigned long error = GetLastError();
    return Win32Status(error == ERROR_FILE_NOT_FOUND ? ErrorCode::kNotFound
                                                     : ErrorCode::kIoError,
                       "Cannot open managed source", error);
  }
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
      size.QuadPart >
          static_cast<LONGLONG>(ManagedSourceService::kMaximumEditableBytes)) {
    const unsigned long error = GetLastError();
    CloseHandle(file);
    return Status{ErrorCode::kFileTooLarge,
                  "Managed source exceeds the 16 MiB editor limit", error};
  }
  std::string bytes(static_cast<std::size_t>(size.QuadPart), '\0');
  DWORD read = 0;
  const bool succeeded =
      bytes.empty() ||
      (ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read,
                nullptr) &&
       read == bytes.size());
  const unsigned long error = succeeded ? ERROR_SUCCESS : GetLastError();
  CloseHandle(file);
  return succeeded
             ? core::Result<std::string>(std::move(bytes))
             : core::Result<std::string>(Win32Status(
                   ErrorCode::kIoError, "Cannot read managed source", error));
}

struct PreviewBytes {
  std::string bytes;
  std::uint64_t full_size = 0;
};

core::Result<PreviewBytes> ReadPreviewBytes(const std::filesystem::path& path,
                                            std::size_t maximum_bytes) {
  HANDLE file =
      CreateFileW(path.c_str(), GENERIC_READ,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return Win32Status(ErrorCode::kIoError,
                       "Cannot open managed source preview");
  }
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file, &size) || size.QuadPart < 0) {
    const unsigned long error = GetLastError();
    CloseHandle(file);
    return Win32Status(ErrorCode::kIoError,
                       "Cannot inspect managed source preview", error);
  }
  const std::size_t preview_size = static_cast<std::size_t>(
      std::min<LONGLONG>(size.QuadPart, maximum_bytes));
  std::string bytes(preview_size, '\0');
  DWORD read = 0;
  const bool succeeded =
      bytes.empty() ||
      (ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read,
                nullptr) &&
       read == bytes.size());
  const unsigned long error = succeeded ? ERROR_SUCCESS : GetLastError();
  CloseHandle(file);
  return succeeded
             ? core::Result<PreviewBytes>(PreviewBytes{
                   std::move(bytes), static_cast<std::uint64_t>(size.QuadPart)})
             : core::Result<PreviewBytes>(
                   Win32Status(ErrorCode::kIoError,
                               "Cannot read managed source preview", error));
}

std::string HexPreview(std::string_view bytes) {
  constexpr char kHex[] = "0123456789abcdef";
  std::string result = "Design++ read-only binary/invalid UTF-8 preview\n\n";
  const std::size_t count = std::min<std::size_t>(bytes.size(), 4096);
  for (std::size_t index = 0; index < count; ++index) {
    if (index != 0 && index % 16 == 0) result.push_back('\n');
    const unsigned char value = static_cast<unsigned char>(bytes[index]);
    result.push_back(kHex[value >> 4]);
    result.push_back(kHex[value & 0x0f]);
    result.push_back(' ');
  }
  if (bytes.size() > count) result += "\n... preview truncated ...";
  return result;
}

bool IsWithin(const std::filesystem::path& root,
              const std::filesystem::path& candidate) {
  std::error_code error;
  const auto canonical_root = std::filesystem::weakly_canonical(root, error);
  if (error) return false;
  const auto canonical_candidate =
      std::filesystem::weakly_canonical(candidate, error);
  if (error) return false;
  auto root_part = canonical_root.begin();
  auto candidate_part = canonical_candidate.begin();
  for (; root_part != canonical_root.end(); ++root_part, ++candidate_part) {
    if (candidate_part == canonical_candidate.end() ||
        _wcsicmp(root_part->c_str(), candidate_part->c_str()) != 0) {
      return false;
    }
  }
  return true;
}

const core::ManagedFile* FindManagedFile(const LibraryRecord& record,
                                         std::string_view cell_id,
                                         std::string_view view_id,
                                         std::string_view relative_path) {
  for (const core::Cell& cell : record.library.cells) {
    if (cell.id != cell_id) continue;
    for (const core::View& view : cell.views) {
      if (view.id != view_id) continue;
      for (const core::ManagedFile& file : view.files) {
        if (file.relative_path == relative_path) return &file;
      }
    }
  }
  return nullptr;
}

core::ManagedFile* FindManagedFile(LibraryRecord* record,
                                   std::string_view cell_id,
                                   std::string_view view_id,
                                   std::string_view relative_path) {
  for (core::Cell& cell : record->library.cells) {
    if (cell.id != cell_id) continue;
    for (core::View& view : cell.views) {
      if (view.id != view_id) continue;
      for (core::ManagedFile& file : view.files) {
        if (file.relative_path == relative_path) return &file;
      }
    }
  }
  return nullptr;
}

std::string NormalizeLineEndings(std::string_view text,
                                 TextLineEnding line_ending) {
  std::string lf;
  lf.reserve(text.size());
  for (std::size_t index = 0; index < text.size(); ++index) {
    if (text[index] == '\r' && index + 1 < text.size() &&
        text[index + 1] == '\n') {
      lf.push_back('\n');
      ++index;
    } else {
      lf.push_back(text[index]);
    }
  }
  if (line_ending == TextLineEnding::kLf) return lf;
  std::string crlf;
  crlf.reserve(lf.size() + lf.size() / 16);
  for (const char character : lf) {
    if (character == '\n') crlf.push_back('\r');
    crlf.push_back(character);
  }
  return crlf;
}

Status WriteTemporary(const std::filesystem::path& path,
                      std::string_view bytes) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                            FILE_ATTRIBUTE_TEMPORARY, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return Win32Status(ErrorCode::kIoError,
                       "Cannot create staged managed source");
  }
  DWORD written = 0;
  const bool succeeded =
      WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written,
                nullptr) &&
      written == bytes.size() && FlushFileBuffers(file);
  const unsigned long error = succeeded ? ERROR_SUCCESS : GetLastError();
  CloseHandle(file);
  return succeeded ? Status::Success()
                   : Win32Status(ErrorCode::kIoError,
                                 "Cannot write staged managed source", error);
}

std::string ModelUri(const LibraryRecord& library, std::string_view cell_id,
                     std::string_view view_id, std::string_view relative_path) {
  return "designpp://library/" + library.library.id + "/cell/" +
         std::string(cell_id) + "/view/" + std::string(view_id) + "/" +
         std::string(relative_path);
}

bool IsWindowsReservedStem(std::string_view stem) {
  std::string upper(stem);
  std::transform(upper.begin(), upper.end(), upper.begin(), [](char character) {
    return static_cast<char>(
        std::toupper(static_cast<unsigned char>(character)));
  });
  if (upper == "CON" || upper == "PRN" || upper == "AUX" || upper == "NUL") {
    return true;
  }
  if (upper.size() == 4 && upper[3] >= '1' && upper[3] <= '9') {
    return upper.starts_with("COM") || upper.starts_with("LPT");
  }
  return false;
}

std::optional<std::filesystem::path> AutomaticModulePath(
    std::string_view text, const std::filesystem::path& current_path) {
  std::wstring extension = current_path.extension().wstring();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](wchar_t character) { return std::towlower(character); });
  if (extension != L".v" && extension != L".sv") return std::nullopt;
  SystemVerilogModuleScanner scanner;
  const std::vector<ModuleDeclaration> modules =
      scanner.FindModulesInText(text);
  if (modules.size() != 1 || IsWindowsReservedStem(modules.front().name)) {
    return std::nullopt;
  }
  const std::filesystem::path desired =
      current_path.parent_path() /
      (Utf8ToWide(modules.front().name) + extension);
  if (desired.filename() == current_path.filename()) return std::nullopt;
  return desired;
}

}  // namespace

core::Result<EditorDocumentSnapshot> ManagedSourceService::LoadDocument(
    const LibraryRecord& library, std::string_view cell_id,
    std::string_view view_id, std::string_view relative_path,
    bool workspace_read_only) const {
  if (library.library.id.empty() ||
      FindManagedFile(library, cell_id, view_id, relative_path) == nullptr ||
      !core::IsSafeRelativePath(relative_path)) {
    return Status{ErrorCode::kInvalidArgument,
                  "Managed source identity is invalid", 0};
  }
  const std::filesystem::path path =
      library.directory / Utf8ToWide(relative_path);
  if (!IsWithin(library.directory, path)) {
    return Status{ErrorCode::kPermissionDenied,
                  "Managed source escapes the Library boundary", 0};
  }
  auto loaded = ReadBytes(path);
  bool size_limited = false;
  std::uint64_t full_size = 0;
  std::string bytes;
  if (loaded.Ok()) {
    bytes = std::move(loaded).Value();
    full_size = bytes.size();
  } else if (loaded.GetStatus().code == ErrorCode::kFileTooLarge) {
    auto preview = ReadPreviewBytes(path, 1024 * 1024);
    if (!preview.Ok()) return preview.GetStatus();
    full_size = preview.Value().full_size;
    bytes = std::move(preview).Value().bytes;
    size_limited = true;
  } else {
    return loaded.GetStatus();
  }
  const bool has_bom = bytes.size() >= 3 &&
                       static_cast<unsigned char>(bytes[0]) == 0xef &&
                       static_cast<unsigned char>(bytes[1]) == 0xbb &&
                       static_cast<unsigned char>(bytes[2]) == 0xbf;
  std::string_view text(bytes);
  if (has_bom) text.remove_prefix(3);
  const bool invalid_text =
      text.find('\0') != std::string_view::npos || !IsValidUtf8(text);
  const core::ManagedFile* file =
      FindManagedFile(library, cell_id, view_id, relative_path);
  EditorDocumentSnapshot snapshot;
  snapshot.id = NewUuid();
  snapshot.library_id = library.library.id;
  snapshot.cell_id = std::string(cell_id);
  snapshot.view_id = std::string(view_id);
  snapshot.relative_path = std::string(relative_path);
  snapshot.model_uri = ModelUri(library, cell_id, view_id, relative_path);
  snapshot.text = invalid_text ? HexPreview(text) : std::string(text);
  if (invalid_text) {
    snapshot.diagnostic =
        "Managed source is invalid UTF-8 or contains NUL; showing a read-only "
        "hex preview";
  } else if (size_limited) {
    snapshot.text += "\n\n// Design++ read-only preview truncated at 1 MiB\n";
    snapshot.diagnostic =
        "Managed source exceeds 16 MiB; showing a read-only preview";
  }
  snapshot.content_hash = HashBytes(bytes);
  snapshot.size = full_size;
  snapshot.modified_utc = file->modified_utc;
  snapshot.line_ending = text.find("\r\n") != std::string_view::npos
                             ? TextLineEnding::kCrLf
                             : TextLineEnding::kLf;
  snapshot.has_utf8_bom = has_bom;
  snapshot.read_only = workspace_read_only || invalid_text || size_limited;
  return snapshot;
}

core::Result<SaveDocumentResult> ManagedSourceService::SaveDocument(
    const LibraryRecord& library, const EditorDocumentSnapshot& snapshot,
    std::string_view utf8_text, bool overwrite_external) const {
  if (snapshot.read_only) {
    return Status{ErrorCode::kPermissionDenied,
                  "The managed source is read-only", 0};
  }
  if (snapshot.library_id != library.library.id || !IsValidUtf8(utf8_text) ||
      utf8_text.find('\0') != std::string_view::npos) {
    return Status{ErrorCode::kInvalidEncoding,
                  "Editor content is not valid UTF-8 text", 0};
  }
  std::string encoded = NormalizeLineEndings(utf8_text, snapshot.line_ending);
  if (snapshot.has_utf8_bom) encoded.insert(0, "\xef\xbb\xbf");
  if (encoded.size() > kMaximumEditableBytes) {
    return Status{ErrorCode::kFileTooLarge,
                  "Editor content exceeds the 16 MiB limit", 0};
  }
  WriterLease lease(library.directory / L".designpp" / L"writer.lease");
  if (!lease.Acquired()) {
    return Status{ErrorCode::kConflict, "Library is busy", 0};
  }
  LibraryStore store;
  auto current_result = store.Load(library.directory);
  if (!current_result.Ok()) return current_result.GetStatus();
  LibraryRecord current = std::move(current_result).Value();
  core::ManagedFile* managed = FindManagedFile(
      &current, snapshot.cell_id, snapshot.view_id, snapshot.relative_path);
  if (managed == nullptr) {
    return Status{ErrorCode::kNotFound,
                  "Managed source was removed from the Library", 0};
  }
  const std::filesystem::path target =
      current.directory / Utf8ToWide(snapshot.relative_path);
  if (!IsWithin(current.directory, target)) {
    return Status{ErrorCode::kPermissionDenied,
                  "Managed source escapes the Library boundary", 0};
  }
  auto existing_result = ReadBytes(target);
  if (!existing_result.Ok()) return existing_result.GetStatus();
  const std::string existing = std::move(existing_result).Value();
  if (!overwrite_external && HashBytes(existing) != snapshot.content_hash) {
    return Status{ErrorCode::kExternalModification,
                  "Managed source changed outside this editor", 0};
  }
  std::error_code error;
  const std::optional<std::filesystem::path> automatic_path =
      AutomaticModulePath(utf8_text, target);
  if (automatic_path) {
    const bool same_file_name = _wcsicmp(automatic_path->filename().c_str(),
                                         target.filename().c_str()) == 0;
    if (!same_file_name && std::filesystem::exists(*automatic_path, error)) {
      return Status{ErrorCode::kAlreadyExists,
                    "A managed source already uses the top module file name",
                    0};
    }
    if (error) {
      return Status{ErrorCode::kIoError,
                    "Cannot check the automatic module file name",
                    static_cast<unsigned long>(error.value())};
    }
  }

  const std::string operation_id = NewUuid();
  const std::filesystem::path staging_directory =
      current.directory / L".designpp" / L"staging" / Utf8ToWide(operation_id);
  const std::filesystem::path recovery_directory =
      current.directory / L".designpp" / L"recovery";
  std::filesystem::create_directories(staging_directory, error);
  std::filesystem::create_directories(recovery_directory, error);
  if (error) {
    return Status{ErrorCode::kIoError, "Cannot create save staging",
                  static_cast<unsigned long>(error.value())};
  }
  const std::filesystem::path staged = staging_directory / L"content.tmp";
  const std::filesystem::path backup =
      recovery_directory / (L"source-" + Utf8ToWide(operation_id) + L".bak");
  Status status = WriteTemporary(staged, encoded);
  if (!status.Ok()) return status;
  if (!ReplaceFileW(target.c_str(), staged.c_str(), backup.c_str(),
                    REPLACEFILE_WRITE_THROUGH, nullptr, nullptr)) {
    const unsigned long native_error = GetLastError();
    std::filesystem::remove_all(staging_directory, error);
    return Win32Status(ErrorCode::kIoError,
                       "Cannot atomically replace managed source",
                       native_error);
  }

  std::filesystem::path committed_target = target;
  if (automatic_path) {
    if (!MoveFileExW(target.c_str(), automatic_path->c_str(),
                     MOVEFILE_WRITE_THROUGH)) {
      const unsigned long rename_error = GetLastError();
      if (!ReplaceFileW(target.c_str(), backup.c_str(), nullptr,
                        REPLACEFILE_WRITE_THROUGH, nullptr, nullptr)) {
        return Status{ErrorCode::kIoError,
                      "Automatic rename failed and source recovery is required",
                      GetLastError()};
      }
      return Win32Status(ErrorCode::kIoError,
                         "Cannot rename source to the top module name",
                         rename_error);
    }
    committed_target = *automatic_path;
    managed->relative_path = WideToUtf8(
        std::filesystem::relative(committed_target, current.directory)
            .generic_wstring());
  }

  const std::uint64_t expected_revision = current.library.revision;
  managed->size = encoded.size();
  managed->modified_utc = UtcNow();
  current.library.revision = expected_revision + 1;
  current.library.modified_utc = managed->modified_utc;
  status = store.Save(current, expected_revision);
  if (!status.Ok()) {
    if (automatic_path && !MoveFileExW(committed_target.c_str(), target.c_str(),
                                       MOVEFILE_WRITE_THROUGH)) {
      return Status{ErrorCode::kIoError,
                    "Manifest save failed and renamed source recovery is "
                    "required",
                    GetLastError()};
    }
    if (!ReplaceFileW(target.c_str(), backup.c_str(), nullptr,
                      REPLACEFILE_WRITE_THROUGH, nullptr, nullptr)) {
      return Status{ErrorCode::kIoError,
                    "Manifest save failed and source recovery is required",
                    GetLastError()};
    }
    return status;
  }
  std::filesystem::remove_all(staging_directory, error);

  EditorDocumentSnapshot updated = snapshot;
  updated.text = std::string(utf8_text);
  updated.relative_path = managed->relative_path;
  updated.model_uri = ModelUri(current, updated.cell_id, updated.view_id,
                               updated.relative_path);
  updated.content_hash = HashBytes(encoded);
  updated.size = encoded.size();
  updated.modified_utc = managed->modified_utc;
  updated.externally_modified = false;
  updated.missing = false;
  return SaveDocumentResult{std::move(current), std::move(updated)};
}

core::Result<EditorDocumentSnapshot> ManagedSourceService::RefreshDocument(
    const LibraryRecord& library,
    const EditorDocumentSnapshot& snapshot) const {
  return LoadDocument(library, snapshot.cell_id, snapshot.view_id,
                      snapshot.relative_path, snapshot.read_only);
}

Status ManagedSourceService::CloseDocument(std::string_view document_id) const {
  return document_id.empty() ? Status{ErrorCode::kInvalidArgument,
                                      "Editor document ID is empty", 0}
                             : Status::Success();
}

}  // namespace designpp::application

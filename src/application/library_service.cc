// Copyright 2026 The Design++ Authors

#include "designpp/application/library_service.h"

// Windows SDK shell headers require the base declarations first.
// clang-format off
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
// clang-format on

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace designpp::application {
namespace {

using core::ErrorCode;
using core::Status;

Status Win32Status(ErrorCode code, std::string message,
                   unsigned long error = GetLastError()) {
  return {code, std::move(message), error};
}

std::string WideToUtf8(std::wstring_view text) {
  if (text.empty()) return {};
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                       static_cast<int>(text.size()), nullptr,
                                       0, nullptr, nullptr);
  std::string result(static_cast<std::size_t>(std::max(size, 0)), '\0');
  if (size > 0) {
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        result.data(), size, nullptr, nullptr);
  }
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

std::wstring SafeFolderName(std::string_view name) {
  std::wstring result = Utf8ToWide(name);
  for (wchar_t& character : result) {
    if (std::wstring_view(L"<>:\"/\\|?*").find(character) !=
        std::wstring_view::npos) {
      character = L'_';
    }
  }
  while (!result.empty() && (result.back() == L'.' || result.back() == L' ')) {
    result.pop_back();
  }
  return result.empty() ? L"Library" : result;
}

class JsonValue final {
 public:
  using Object = std::map<std::string, JsonValue>;
  using Array = std::vector<JsonValue>;
  using Value = std::variant<std::nullptr_t, bool, std::uint64_t, std::string,
                             Object, Array>;
  explicit JsonValue(Value value) : value_(std::move(value)) {}
  template <typename T>
  const T* Get() const {
    return std::get_if<T>(&value_);
  }

 private:
  Value value_;
};

class JsonParser final {
 public:
  explicit JsonParser(std::string_view input) : input_(input) {}
  std::optional<JsonValue> Parse() {
    std::optional<JsonValue> value = ParseValue();
    SkipSpace();
    if (!value || position_ != input_.size()) return std::nullopt;
    return value;
  }

 private:
  void SkipSpace() {
    while (position_ < input_.size() &&
           std::isspace(static_cast<unsigned char>(input_[position_])))
      ++position_;
  }
  bool Take(char expected) {
    SkipSpace();
    if (position_ >= input_.size() || input_[position_] != expected)
      return false;
    ++position_;
    return true;
  }
  std::optional<std::string> ParseString() {
    if (!Take('"')) return std::nullopt;
    std::string value;
    while (position_ < input_.size()) {
      const char character = input_[position_++];
      if (character == '"') return value;
      if (character == '\\') {
        if (position_ >= input_.size()) return std::nullopt;
        const char escaped = input_[position_++];
        switch (escaped) {
          case '"':
            value.push_back('"');
            break;
          case '\\':
            value.push_back('\\');
            break;
          case '/':
            value.push_back('/');
            break;
          case 'b':
            value.push_back('\b');
            break;
          case 'f':
            value.push_back('\f');
            break;
          case 'n':
            value.push_back('\n');
            break;
          case 'r':
            value.push_back('\r');
            break;
          case 't':
            value.push_back('\t');
            break;
          default:
            return std::nullopt;
        }
      } else {
        if (static_cast<unsigned char>(character) < 0x20) return std::nullopt;
        value.push_back(character);
      }
    }
    return std::nullopt;
  }
  std::optional<JsonValue> ParseValue() {
    SkipSpace();
    if (position_ >= input_.size()) return std::nullopt;
    if (input_[position_] == '{') return ParseObject();
    if (input_[position_] == '[') return ParseArray();
    if (input_[position_] == '"') {
      auto value = ParseString();
      if (value) return JsonValue(std::move(*value));
      return std::nullopt;
    }
    const std::size_t start = position_;
    while (position_ < input_.size() &&
           std::isdigit(static_cast<unsigned char>(input_[position_])))
      ++position_;
    if (start == position_) return std::nullopt;
    std::uint64_t number = 0;
    if (std::from_chars(input_.data() + start, input_.data() + position_,
                        number)
            .ec != std::errc())
      return std::nullopt;
    return JsonValue(number);
  }
  std::optional<JsonValue> ParseObject() {
    if (!Take('{')) return std::nullopt;
    JsonValue::Object object;
    SkipSpace();
    if (Take('}')) return JsonValue(std::move(object));
    for (;;) {
      auto key = ParseString();
      if (!key || !Take(':')) return std::nullopt;
      auto value = ParseValue();
      if (!value || !object.emplace(std::move(*key), std::move(*value)).second)
        return std::nullopt;
      if (Take('}')) return JsonValue(std::move(object));
      if (!Take(',')) return std::nullopt;
    }
  }
  std::optional<JsonValue> ParseArray() {
    if (!Take('[')) return std::nullopt;
    JsonValue::Array array;
    SkipSpace();
    if (Take(']')) return JsonValue(std::move(array));
    for (;;) {
      auto value = ParseValue();
      if (!value) return std::nullopt;
      array.push_back(std::move(*value));
      if (Take(']')) return JsonValue(std::move(array));
      if (!Take(',')) return std::nullopt;
    }
  }
  std::string_view input_;
  std::size_t position_ = 0;
};

std::string EscapeJson(std::string_view input) {
  std::string output;
  for (char character : input) {
    switch (character) {
      case '"':
        output += "\\\"";
        break;
      case '\\':
        output += "\\\\";
        break;
      case '\n':
        output += "\\n";
        break;
      case '\r':
        output += "\\r";
        break;
      case '\t':
        output += "\\t";
        break;
      default:
        output.push_back(character);
        break;
    }
  }
  return output;
}

const JsonValue* Member(const JsonValue::Object& object, std::string_view key) {
  const auto iterator = object.find(std::string(key));
  return iterator == object.end() ? nullptr : &iterator->second;
}

bool ReadString(const JsonValue::Object& object, std::string_view key,
                std::string* output) {
  const JsonValue* value = Member(object, key);
  const std::string* text =
      value == nullptr ? nullptr : value->Get<std::string>();
  if (text == nullptr) return false;
  *output = *text;
  return true;
}

bool ReadNumber(const JsonValue::Object& object, std::string_view key,
                std::uint64_t* output) {
  const JsonValue* value = Member(object, key);
  const std::uint64_t* number =
      value == nullptr ? nullptr : value->Get<std::uint64_t>();
  if (number == nullptr) return false;
  *output = *number;
  return true;
}

std::optional<core::ViewKind> ParseViewKind(std::string_view kind) {
  for (core::ViewKind candidate :
       {core::ViewKind::kVerilog, core::ViewKind::kTestbench,
        core::ViewKind::kConstraints, core::ViewKind::kSynthesis,
        core::ViewKind::kTiming, core::ViewKind::kPhysicalDesign,
        core::ViewKind::kLayout, core::ViewKind::kReport}) {
    if (core::ViewKindName(candidate) == kind) return candidate;
  }
  return std::nullopt;
}

core::Result<core::Library> DecodeLibrary(std::string_view json) {
  auto root_value = JsonParser(json).Parse();
  const auto* root =
      root_value ? root_value->Get<JsonValue::Object>() : nullptr;
  if (root == nullptr)
    return Status{ErrorCode::kCorruptData, "Invalid JSON", 0};
  core::Library library;
  std::uint64_t schema = 0;
  if (!ReadNumber(*root, "schema_version", &schema) ||
      !ReadString(*root, "library_id", &library.id) ||
      !ReadNumber(*root, "revision", &library.revision) ||
      !ReadString(*root, "name", &library.name) ||
      !ReadString(*root, "description", &library.description) ||
      !ReadString(*root, "created_utc", &library.created_utc) ||
      !ReadString(*root, "modified_utc", &library.modified_utc)) {
    return Status{ErrorCode::kCorruptData, "Required manifest field is missing",
                  0};
  }
  if (schema < 1 || schema > core::Library::kSchemaVersion) {
    return Status{ErrorCode::kUnsupportedSchema, "Unsupported library schema",
                  0};
  }
  library.schema_version = core::Library::kSchemaVersion;
  if (schema >= 3) {
    const JsonValue* files_value = Member(*root, "files");
    const auto* files =
        files_value ? files_value->Get<JsonValue::Array>() : nullptr;
    if (files == nullptr) {
      return Status{ErrorCode::kCorruptData, "Library files is invalid", 0};
    }
    for (const JsonValue& file_value : *files) {
      const auto* file_object = file_value.Get<JsonValue::Object>();
      core::ManagedFile file;
      if (file_object == nullptr ||
          !ReadString(*file_object, "relative_path", &file.relative_path) ||
          !ReadString(*file_object, "role", &file.role) ||
          !ReadNumber(*file_object, "size", &file.size) ||
          !ReadString(*file_object, "modified_utc", &file.modified_utc)) {
        return Status{ErrorCode::kCorruptData,
                      "Managed library file is invalid", 0};
      }
      library.files.push_back(std::move(file));
    }
  }
  const JsonValue* cells_value = Member(*root, "cells");
  const auto* cells =
      cells_value ? cells_value->Get<JsonValue::Array>() : nullptr;
  if (cells == nullptr)
    return Status{ErrorCode::kCorruptData, "cells is invalid", 0};
  for (const JsonValue& cell_value : *cells) {
    const auto* object = cell_value.Get<JsonValue::Object>();
    core::Cell cell;
    if (object == nullptr || !ReadString(*object, "cell_id", &cell.id) ||
        !ReadString(*object, "name", &cell.name) ||
        !ReadString(*object, "description", &cell.description))
      return Status{ErrorCode::kCorruptData, "Cell is invalid", 0};
    const JsonValue* views_value = Member(*object, "views");
    const auto* views =
        views_value ? views_value->Get<JsonValue::Array>() : nullptr;
    if (views == nullptr)
      return Status{ErrorCode::kCorruptData, "views is invalid", 0};
    for (const JsonValue& view_value : *views) {
      const auto* view_object = view_value.Get<JsonValue::Object>();
      core::View view;
      std::string kind;
      if (view_object == nullptr ||
          !ReadString(*view_object, "view_id", &view.id) ||
          !ReadString(*view_object, "name", &view.name) ||
          !ReadString(*view_object, "description", &view.description) ||
          !ReadString(*view_object, "kind", &kind))
        return Status{ErrorCode::kCorruptData, "View is invalid", 0};
      auto parsed_kind = ParseViewKind(kind);
      if (!parsed_kind)
        return Status{ErrorCode::kCorruptData, "Unknown view kind", 0};
      view.kind = *parsed_kind;
      const JsonValue* files_value = Member(*view_object, "files");
      const auto* files =
          files_value ? files_value->Get<JsonValue::Array>() : nullptr;
      if (files == nullptr)
        return Status{ErrorCode::kCorruptData, "files is invalid", 0};
      for (const JsonValue& file_value : *files) {
        const auto* file_object = file_value.Get<JsonValue::Object>();
        core::ManagedFile file;
        if (file_object == nullptr ||
            !ReadString(*file_object, "relative_path", &file.relative_path) ||
            !ReadString(*file_object, "role", &file.role) ||
            !ReadNumber(*file_object, "size", &file.size) ||
            !ReadString(*file_object, "modified_utc", &file.modified_utc))
          return Status{ErrorCode::kCorruptData, "Managed file is invalid", 0};
        view.files.push_back(std::move(file));
      }
      cell.views.push_back(std::move(view));
    }
    if (schema < 5) {
      const bool has_layout = std::any_of(
          cell.views.begin(), cell.views.end(), [](const core::View& view) {
            return view.kind == core::ViewKind::kLayout;
          });
      if (has_layout) {
        std::erase_if(cell.views, [](const core::View& view) {
          return view.kind == core::ViewKind::kPhysicalDesign;
        });
      } else {
        for (core::View& view : cell.views) {
          if (view.kind != core::ViewKind::kPhysicalDesign) continue;
          view.kind = core::ViewKind::kLayout;
          if (view.name == "physical" || view.name == "physical_design" ||
              view.name == "Physical Design") {
            view.name = "layout";
          }
        }
      }
    }
    library.cells.push_back(std::move(cell));
  }
  Status validation = core::ValidateLibrary(library);
  return validation.Ok() ? core::Result<core::Library>(std::move(library))
                         : core::Result<core::Library>(std::move(validation));
}

std::string EncodeLibrary(const core::Library& library) {
  std::ostringstream output;
  auto quote = [&output](std::string_view value) {
    output << '"' << EscapeJson(value) << '"';
  };
  output << "{\n  \"schema_version\": " << library.schema_version
         << ",\n  \"library_id\": ";
  quote(library.id);
  output << ",\n  \"revision\": " << library.revision << ",\n  \"name\": ";
  quote(library.name);
  output << ",\n  \"description\": ";
  quote(library.description);
  output << ",\n  \"created_utc\": ";
  quote(library.created_utc);
  output << ",\n  \"modified_utc\": ";
  quote(library.modified_utc);
  output << ",\n  \"files\": [";
  for (std::size_t file_index = 0; file_index < library.files.size();
       ++file_index) {
    const core::ManagedFile& file = library.files[file_index];
    output << (file_index ? "," : "") << "{\"relative_path\": ";
    quote(file.relative_path);
    output << ", \"role\": ";
    quote(file.role);
    output << ", \"size\": " << file.size << ", \"modified_utc\": ";
    quote(file.modified_utc);
    output << '}';
  }
  output << "],\n  \"cells\": [";
  for (std::size_t cell_index = 0; cell_index < library.cells.size();
       ++cell_index) {
    const core::Cell& cell = library.cells[cell_index];
    output << (cell_index ? "," : "") << "\n    {\"cell_id\": ";
    quote(cell.id);
    output << ", \"name\": ";
    quote(cell.name);
    output << ", \"description\": ";
    quote(cell.description);
    output << ", \"views\": [";
    for (std::size_t view_index = 0; view_index < cell.views.size();
         ++view_index) {
      const core::View& view = cell.views[view_index];
      output << (view_index ? "," : "") << "\n      {\"view_id\": ";
      quote(view.id);
      output << ", \"name\": ";
      quote(view.name);
      output << ", \"description\": ";
      quote(view.description);
      output << ", \"kind\": ";
      quote(core::ViewKindName(view.kind));
      output << ", \"files\": [";
      for (std::size_t file_index = 0; file_index < view.files.size();
           ++file_index) {
        const core::ManagedFile& file = view.files[file_index];
        output << (file_index ? "," : "") << "{\"relative_path\": ";
        quote(file.relative_path);
        output << ", \"role\": ";
        quote(file.role);
        output << ", \"size\": " << file.size << ", \"modified_utc\": ";
        quote(file.modified_utc);
        output << '}';
      }
      output << "]}";
    }
    output << "]}";
  }
  output << "\n  ]\n}\n";
  return output.str();
}

Status WriteAtomic(const std::filesystem::path& path, std::string_view bytes) {
  const std::filesystem::path temporary =
      path.parent_path() / (L".manifest-" + Utf8ToWide(NewUuid()) + L".tmp");
  HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return Win32Status(ErrorCode::kIoError, "Cannot create temporary manifest");
  DWORD written = 0;
  const bool succeeded =
      WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written,
                nullptr) &&
      written == bytes.size() && FlushFileBuffers(file);
  const unsigned long write_error = succeeded ? ERROR_SUCCESS : GetLastError();
  CloseHandle(file);
  if (!succeeded) {
    DeleteFileW(temporary.c_str());
    return Win32Status(ErrorCode::kIoError, "Cannot write manifest",
                       write_error);
  }
  const std::filesystem::path backup = path.wstring() + L".bak";
  bool replaced = false;
  if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
    replaced =
        ReplaceFileW(path.c_str(), temporary.c_str(), backup.c_str(),
                     REPLACEFILE_WRITE_THROUGH, nullptr, nullptr) != FALSE;
  } else {
    replaced = MoveFileExW(temporary.c_str(), path.c_str(),
                           MOVEFILE_WRITE_THROUGH) != FALSE;
  }
  if (!replaced) {
    const unsigned long error = GetLastError();
    DeleteFileW(temporary.c_str());
    return Win32Status(ErrorCode::kIoError,
                       "Cannot atomically replace manifest", error);
  }
  return Status::Success();
}

class Lease final {
 public:
  explicit Lease(const std::filesystem::path& path) {
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
  ~Lease() {
    if (handle_ != INVALID_HANDLE_VALUE) {
      OVERLAPPED overlapped{};
      UnlockFileEx(handle_, 0, 1, 0, &overlapped);
      CloseHandle(handle_);
    }
  }
  [[nodiscard]] bool Acquired() const {
    return handle_ != INVALID_HANDLE_VALUE;
  }

 private:
  HANDLE handle_ = INVALID_HANDLE_VALUE;
};

std::filesystem::path ViewDirectory(const LibraryRecord& record,
                                    std::string_view cell_id,
                                    std::string_view view_id) {
  return record.directory / L"cells" / Utf8ToWide(cell_id) / L"views" /
         Utf8ToWide(view_id);
}

core::Cell* FindCell(core::Library* library, std::string_view id) {
  auto iterator =
      std::find_if(library->cells.begin(), library->cells.end(),
                   [id](const core::Cell& cell) { return cell.id == id; });
  return iterator == library->cells.end() ? nullptr : &*iterator;
}

core::View* FindView(core::Cell* cell, std::string_view id) {
  if (cell == nullptr) return nullptr;
  auto iterator =
      std::find_if(cell->views.begin(), cell->views.end(),
                   [id](const core::View& view) { return view.id == id; });
  return iterator == cell->views.end() ? nullptr : &*iterator;
}

Status CommitMutation(const LibraryStore& store, LibraryRecord* record,
                      std::uint64_t expected_revision) {
  record->library.revision = expected_revision + 1;
  record->library.modified_utc = UtcNow();
  Status validation = core::ValidateLibrary(record->library);
  return validation.Ok() ? store.Save(*record, expected_revision) : validation;
}

bool IsUncPath(const std::filesystem::path& path) {
  return path.native().starts_with(L"\\\\");
}

Status RecyclePath(const std::filesystem::path& path) {
  std::wstring from = path.wstring();
  from.push_back(L'\0');
  SHFILEOPSTRUCTW operation{};
  operation.wFunc = FO_DELETE;
  operation.pFrom = from.c_str();
  operation.fFlags =
      FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
  const int result = SHFileOperationW(&operation);
  return result == 0 && !operation.fAnyOperationsAborted
             ? Status::Success()
             : Status{ErrorCode::kIoError, "Recycle Bin operation failed",
                      static_cast<unsigned long>(result)};
}

}  // namespace

core::Result<std::vector<LibraryRecord>> LibraryStore::Scan(
    const std::filesystem::path& root) const {
  std::error_code error;
  if (!std::filesystem::exists(root, error)) {
    return Status{ErrorCode::kNotFound, "Library Root does not exist",
                  static_cast<unsigned long>(error.value())};
  }
  std::vector<LibraryRecord> records;
  std::filesystem::directory_iterator iterator(root, error);
  const std::filesystem::directory_iterator end;
  while (!error && iterator != end) {
    if (iterator->is_directory(error) &&
        std::filesystem::exists(iterator->path() / L"library.dplib", error)) {
      auto record = Load(iterator->path());
      if (record.Ok())
        records.push_back(std::move(record).Value());
      else
        records.push_back({{},
                           iterator->path(),
                           core::LibraryStatus::kInvalid,
                           record.GetStatus().message});
    }
    iterator.increment(error);
  }
  if (error)
    return Status{ErrorCode::kIoError, "Cannot scan Library Root",
                  static_cast<unsigned long>(error.value())};
  return records;
}

core::Result<LibraryRecord> LibraryStore::Load(
    const std::filesystem::path& directory) const {
  std::ifstream input(directory / L"library.dplib", std::ios::binary);
  if (!input)
    return Status{ErrorCode::kNotFound, "library.dplib is missing", 0};
  std::string bytes((std::istreambuf_iterator<char>(input)),
                    std::istreambuf_iterator<char>());
  auto library = DecodeLibrary(bytes);
  if (!library.Ok()) return library.GetStatus();
  LibraryRecord record{
      std::move(library).Value(), directory, core::LibraryStatus::kReady, {}};
  if (record.library.cells.empty() && record.library.files.empty()) {
    record.status = core::LibraryStatus::kEmpty;
  }
  return record;
}

Status LibraryStore::Save(const LibraryRecord& record,
                          std::uint64_t expected_revision) const {
  if (expected_revision > 0) {
    auto current = Load(record.directory);
    if (!current.Ok()) return current.GetStatus();
    if (current.Value().library.revision != expected_revision)
      return {ErrorCode::kConflict, "Library revision changed", 0};
  }
  return WriteAtomic(record.directory / L"library.dplib",
                     EncodeLibrary(record.library));
}

core::Result<std::filesystem::path> LibraryService::GetLibraryRoot() const {
  std::vector<wchar_t> buffer(32768);
  DWORD size = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
  const LSTATUS result =
      RegGetValueW(HKEY_CURRENT_USER, L"Software\\Design++", L"LibraryRoot",
                   RRF_RT_REG_SZ, nullptr, buffer.data(), &size);
  if (result == ERROR_SUCCESS) return std::filesystem::path(buffer.data());
  PWSTR documents = nullptr;
  if (SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr,
                           &documents) != S_OK)
    return Status{ErrorCode::kNotFound, "Library Root is not configured", 0};
  std::filesystem::path root =
      std::filesystem::path(documents) / L"Design++ Libraries";
  CoTaskMemFree(documents);
  return root;
}

Status LibraryService::SetLibraryRoot(const std::filesystem::path& root) const {
  std::error_code error;
  std::filesystem::create_directories(root, error);
  if (error)
    return {ErrorCode::kIoError, "Cannot create Library Root",
            static_cast<unsigned long>(error.value())};
  const std::wstring value = root.wstring();
  const LSTATUS result = RegSetKeyValueW(
      HKEY_CURRENT_USER, L"Software\\Design++", L"LibraryRoot", REG_SZ,
      value.c_str(), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
  return result == ERROR_SUCCESS
             ? Status::Success()
             : Status{ErrorCode::kIoError, "Cannot save Library Root",
                      static_cast<unsigned long>(result)};
}

core::Result<std::vector<LibraryRecord>> LibraryService::Refresh() const {
  auto root = GetLibraryRoot();
  if (!root.Ok()) return root.GetStatus();
  Status status = SetLibraryRoot(root.Value());
  if (!status.Ok()) return status;
  return store_.Scan(root.Value());
}

core::Result<LibraryRecord> LibraryService::CreateLibrary(
    std::string name, std::string description) const {
  Status validation = core::ValidateDisplayName(name);
  if (!validation.Ok()) return validation;
  auto root = GetLibraryRoot();
  if (!root.Ok()) return root.GetStatus();
  validation = SetLibraryRoot(root.Value());
  if (!validation.Ok()) return validation;
  std::filesystem::create_directories(root.Value() / L".designpp");
  Lease lease(root.Value() / L".designpp" / L"root.lease");
  if (!lease.Acquired())
    return Status{ErrorCode::kConflict, "Library Root is busy", 0};
  const std::string id = NewUuid();
  const std::filesystem::path directory =
      root.Value() /
      (SafeFolderName(name) + L"--" + Utf8ToWide(id.substr(0, 8)));
  std::error_code error;
  if (!std::filesystem::create_directories(directory / L".designpp", error) ||
      error)
    return Status{ErrorCode::kIoError, "Cannot create library directory",
                  static_cast<unsigned long>(error.value())};
  std::filesystem::create_directories(directory / L"cells", error);
  std::filesystem::create_directories(directory / L".designpp" / L"staging",
                                      error);
  std::filesystem::create_directories(directory / L".designpp" / L"recovery",
                                      error);
  const std::string now = UtcNow();
  LibraryRecord record{{core::Library::kSchemaVersion,
                        id,
                        1,
                        std::move(name),
                        std::move(description),
                        now,
                        now,
                        {}},
                       directory,
                       core::LibraryStatus::kEmpty,
                       {}};
  validation = store_.Save(record, 0);
  return validation.Ok() ? core::Result<LibraryRecord>(std::move(record))
                         : core::Result<LibraryRecord>(std::move(validation));
}

core::Result<LibraryRecord> LibraryService::CreateCell(
    const LibraryRecord& input, std::string name,
    std::string description) const {
  LibraryRecord record = input;
  Lease lease(record.directory / L".designpp" / L"writer.lease");
  if (!lease.Acquired())
    return Status{ErrorCode::kConflict, "Library is read-only or busy", 0};
  record.library.cells.push_back(
      {NewUuid(), std::move(name), std::move(description), {}});
  Status status = CommitMutation(store_, &record, input.library.revision);
  if (!status.Ok()) return status;
  std::error_code error;
  std::filesystem::create_directories(
      record.directory / L"cells" / Utf8ToWide(record.library.cells.back().id) /
          L"views",
      error);
  record.status = core::LibraryStatus::kReady;
  return record;
}

core::Result<LibraryRecord> LibraryService::CreateView(
    const LibraryRecord& input, std::string_view cell_id,
    const CreateViewRequest& request) const {
  if (request.kind == core::ViewKind::kPhysicalDesign) {
    return Status{ErrorCode::kInvalidArgument,
                  "Physical Design views were replaced by Layout views", 0};
  }
  LibraryRecord record = input;
  Lease lease(record.directory / L".designpp" / L"writer.lease");
  if (!lease.Acquired())
    return Status{ErrorCode::kConflict, "Library is read-only or busy", 0};
  core::Cell* cell = FindCell(&record.library, cell_id);
  if (cell == nullptr) return Status{ErrorCode::kNotFound, "Cell not found", 0};
  Status validation = core::ValidateDisplayName(request.name);
  if (!validation.Ok()) return validation;
  core::View view{
      NewUuid(), request.name, request.description, request.kind, {}};
  const std::filesystem::path files =
      ViewDirectory(record, cell_id, view.id) / L"files";
  std::error_code error;
  std::filesystem::create_directories(files, error);
  if (error)
    return Status{ErrorCode::kIoError, "Cannot create view directory",
                  static_cast<unsigned long>(error.value())};
  if (request.kind == core::ViewKind::kVerilog ||
      request.kind == core::ViewKind::kTestbench ||
      request.kind == core::ViewKind::kConstraints) {
    std::string extension = request.kind == core::ViewKind::kConstraints
                                ? ".sdc"
                                : request.source_extension;
    if (extension != ".v" && extension != ".sv" && extension != ".sdc")
      extension = ".sv";
    const std::filesystem::path file =
        files / (SafeFolderName(request.name) + Utf8ToWide(extension));
    std::ofstream empty(file, std::ios::binary);
    if (!empty)
      return Status{ErrorCode::kIoError, "Cannot create view file", 0};
    view.files.push_back(
        {WideToUtf8(std::filesystem::relative(file, record.directory)
                        .generic_wstring()),
         "primary", 0, UtcNow()});
  }
  cell->views.push_back(std::move(view));
  Status status = CommitMutation(store_, &record, input.library.revision);
  if (!status.Ok()) {
    std::filesystem::remove_all(
        ViewDirectory(record, cell_id, cell->views.back().id), error);
  }
  return status.Ok() ? core::Result<LibraryRecord>(std::move(record))
                     : core::Result<LibraryRecord>(std::move(status));
}

core::Result<LibraryRecord> LibraryService::RenameItem(
    const LibraryRecord& input, std::string_view cell_id,
    std::string_view view_id, std::string name, std::string description) const {
  LibraryRecord record = input;
  Lease lease(record.directory / L".designpp" / L"writer.lease");
  if (!lease.Acquired())
    return Status{ErrorCode::kConflict, "Library is read-only or busy", 0};
  if (cell_id.empty()) {
    record.library.name = std::move(name);
    record.library.description = std::move(description);
  } else {
    core::Cell* cell = FindCell(&record.library, cell_id);
    if (cell == nullptr)
      return Status{ErrorCode::kNotFound, "Cell not found", 0};
    if (view_id.empty()) {
      cell->name = std::move(name);
      cell->description = std::move(description);
    } else {
      core::View* view = FindView(cell, view_id);
      if (view == nullptr)
        return Status{ErrorCode::kNotFound, "View not found", 0};
      view->name = std::move(name);
      view->description = std::move(description);
    }
  }
  Status status = CommitMutation(store_, &record, input.library.revision);
  return status.Ok() ? core::Result<LibraryRecord>(std::move(record))
                     : core::Result<LibraryRecord>(std::move(status));
}

core::Result<LibraryRecord> LibraryService::ImportFiles(
    const LibraryRecord& input, std::string_view cell_id,
    std::string_view view_id,
    const std::vector<std::filesystem::path>& sources) const {
  LibraryRecord record = input;
  Lease lease(record.directory / L".designpp" / L"writer.lease");
  if (!lease.Acquired())
    return Status{ErrorCode::kConflict, "Library is read-only or busy", 0};
  const bool library_level = cell_id.empty() && view_id.empty();
  if (cell_id.empty() != view_id.empty()) {
    return Status{ErrorCode::kInvalidArgument,
                  "Cell and View must both be selected", 0};
  }
  core::View* view = nullptr;
  std::vector<core::ManagedFile>* managed_files = &record.library.files;
  std::filesystem::path destination = record.directory / L"files";
  if (!library_level) {
    view = FindView(FindCell(&record.library, cell_id), view_id);
    if (view == nullptr) {
      return Status{ErrorCode::kNotFound, "View not found", 0};
    }
    managed_files = &view->files;
    destination = ViewDirectory(record, cell_id, view_id) / L"files";
  }
  for (const auto& source : sources) {
    std::wstring extension = source.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](wchar_t character) { return std::towlower(character); });
    if (!library_level && (extension == L".lib" || extension == L".liberty")) {
      return Status{ErrorCode::kInvalidArgument,
                    "Liberty files must be imported at Library scope", 0};
    }
    if (!library_level && view->kind == core::ViewKind::kConstraints &&
        extension != L".sdc") {
      return Status{ErrorCode::kInvalidArgument,
                    "Constraints Views accept only SDC files", 0};
    }
  }
  std::error_code error;
  std::filesystem::create_directories(destination, error);
  if (error) {
    return Status{ErrorCode::kIoError, "Cannot create import directory",
                  static_cast<unsigned long>(error.value())};
  }
  std::vector<std::filesystem::path> targets;
  std::set<std::wstring> target_names;
  for (const auto& source : sources) {
    error.clear();
    if (!std::filesystem::is_regular_file(source, error)) {
      return Status{ErrorCode::kInvalidArgument, "Import source is not a file",
                    0};
    }
    const std::filesystem::path target = destination / source.filename();
    std::wstring normalized_name = source.filename().wstring();
    std::transform(normalized_name.begin(), normalized_name.end(),
                   normalized_name.begin(),
                   [](wchar_t character) { return std::towlower(character); });
    error.clear();
    if (!target_names.insert(normalized_name).second ||
        std::filesystem::exists(target, error)) {
      return Status{ErrorCode::kAlreadyExists,
                    "A managed file has the same name", 0};
    }
    targets.push_back(target);
  }
  std::vector<std::filesystem::path> copied;
  for (std::size_t index = 0; index < sources.size(); ++index) {
    error.clear();
    const std::filesystem::path& source = sources[index];
    const std::filesystem::path& target = targets[index];
    std::filesystem::copy_file(source, target,
                               std::filesystem::copy_options::none, error);
    if (error) {
      for (const auto& path : copied) {
        std::error_code cleanup_error;
        std::filesystem::remove(path, cleanup_error);
      }
      return Status{ErrorCode::kIoError, "Cannot import file",
                    static_cast<unsigned long>(error.value())};
    }
    copied.push_back(target);
    error.clear();
    const std::uintmax_t size = std::filesystem::file_size(target, error);
    if (error) {
      for (const auto& path : copied) {
        std::error_code cleanup_error;
        std::filesystem::remove(path, cleanup_error);
      }
      return Status{ErrorCode::kIoError, "Cannot inspect imported file",
                    static_cast<unsigned long>(error.value())};
    }
    managed_files->push_back(
        {WideToUtf8(std::filesystem::relative(target, record.directory)
                        .generic_wstring()),
         "support", size, UtcNow()});
  }
  Status status = CommitMutation(store_, &record, input.library.revision);
  if (!status.Ok()) {
    for (const auto& path : copied) {
      std::error_code cleanup_error;
      std::filesystem::remove(path, cleanup_error);
    }
  }
  return status.Ok() ? core::Result<LibraryRecord>(std::move(record))
                     : core::Result<LibraryRecord>(std::move(status));
}

core::Result<LibraryRecord> LibraryService::ReplaceLibraryFile(
    const LibraryRecord& input, std::string_view relative_path,
    const std::filesystem::path& source) const {
  LibraryRecord record = input;
  Lease lease(record.directory / L".designpp" / L"writer.lease");
  if (!lease.Acquired()) {
    return Status{ErrorCode::kConflict, "Library is read-only or busy", 0};
  }
  auto managed =
      std::find_if(record.library.files.begin(), record.library.files.end(),
                   [relative_path](const core::ManagedFile& file) {
                     return file.relative_path == relative_path;
                   });
  if (managed == record.library.files.end()) {
    return Status{ErrorCode::kNotFound, "Managed Library file was not found",
                  0};
  }
  std::wstring extension = Utf8ToWide(managed->relative_path);
  extension = std::filesystem::path(extension).extension().wstring();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](wchar_t character) { return std::towlower(character); });
  if (extension != L".lib" && extension != L".liberty") {
    return Status{ErrorCode::kInvalidArgument,
                  "Only managed Liberty files can be replaced here", 0};
  }
  std::error_code error;
  if (!std::filesystem::is_regular_file(source, error)) {
    return Status{ErrorCode::kInvalidArgument,
                  "Replacement source is not a file", 0};
  }
  std::wstring source_extension = source.extension().wstring();
  std::transform(source_extension.begin(), source_extension.end(),
                 source_extension.begin(),
                 [](wchar_t character) { return std::towlower(character); });
  if (source_extension != L".lib" && source_extension != L".liberty") {
    return Status{ErrorCode::kInvalidArgument,
                  "Replacement must be a Liberty file", 0};
  }
  const std::filesystem::path target =
      record.directory / Utf8ToWide(managed->relative_path);
  error.clear();
  if (!std::filesystem::is_regular_file(target, error)) {
    return Status{ErrorCode::kNotFound,
                  "Managed Library file is missing on disk", 0};
  }
  error.clear();
  if (std::filesystem::equivalent(source, target, error) && !error) {
    return Status{ErrorCode::kInvalidArgument,
                  "Replacement source is already the managed file", 0};
  }
  const std::filesystem::path replacement_target =
      target.parent_path() / source.filename();
  std::wstring current_name = target.filename().wstring();
  std::wstring replacement_name = replacement_target.filename().wstring();
  std::transform(current_name.begin(), current_name.end(), current_name.begin(),
                 [](wchar_t character) { return std::towlower(character); });
  std::transform(replacement_name.begin(), replacement_name.end(),
                 replacement_name.begin(),
                 [](wchar_t character) { return std::towlower(character); });
  const bool name_changed = current_name != replacement_name;
  error.clear();
  if (name_changed && std::filesystem::exists(replacement_target, error)) {
    return Status{ErrorCode::kAlreadyExists,
                  "A managed file has the replacement file name", 0};
  }
  if (error) {
    return Status{ErrorCode::kIoError,
                  "Cannot inspect the replacement destination",
                  static_cast<unsigned long>(error.value())};
  }

  const std::wstring transaction_id = Utf8ToWide(NewUuid());
  const std::filesystem::path temporary =
      target.parent_path() / (L".managed-replace-" + transaction_id + L".tmp");
  const std::filesystem::path backup =
      target.parent_path() / (L".managed-replace-" + transaction_id + L".bak");
  error.clear();
  std::filesystem::copy_file(source, temporary,
                             std::filesystem::copy_options::none, error);
  if (error) {
    return Status{ErrorCode::kIoError,
                  "Cannot stage the replacement Library file",
                  static_cast<unsigned long>(error.value())};
  }
  if (!MoveFileExW(target.c_str(), backup.c_str(), MOVEFILE_WRITE_THROUGH)) {
    const unsigned long backup_error = GetLastError();
    DeleteFileW(temporary.c_str());
    return Win32Status(ErrorCode::kIoError,
                       "Cannot stage the existing Library file for replacement",
                       backup_error);
  }
  if (!MoveFileExW(temporary.c_str(), replacement_target.c_str(),
                   MOVEFILE_WRITE_THROUGH)) {
    const unsigned long replace_error = GetLastError();
    DeleteFileW(temporary.c_str());
    MoveFileExW(backup.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH);
    return Win32Status(ErrorCode::kIoError,
                       "Cannot replace the managed Library file",
                       replace_error);
  }

  error.clear();
  const std::uintmax_t size =
      std::filesystem::file_size(replacement_target, error);
  if (!error) {
    managed->relative_path = WideToUtf8(
        std::filesystem::relative(replacement_target, record.directory)
            .generic_wstring());
    managed->size = size;
    managed->modified_utc = UtcNow();
  }
  Status status = error
                      ? Status{ErrorCode::kIoError,
                               "Cannot inspect the replaced Library file",
                               static_cast<unsigned long>(error.value())}
                      : CommitMutation(store_, &record, input.library.revision);
  if (!status.Ok()) {
    DeleteFileW(replacement_target.c_str());
    if (!MoveFileExW(backup.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) {
      const unsigned long rollback_error = GetLastError();
      return Status{ErrorCode::kIoError,
                    status.message +
                        "; replacement rollback failed and the backup remains "
                        "next to the managed file",
                    rollback_error};
    }
    return status;
  }
  DeleteFileW(backup.c_str());
  return record;
}

core::Result<LibraryRecord> LibraryService::RemoveLibraryFile(
    const LibraryRecord& input, std::string_view relative_path) const {
  LibraryRecord record = input;
  Lease lease(record.directory / L".designpp" / L"writer.lease");
  if (!lease.Acquired()) {
    return Status{ErrorCode::kConflict, "Library is read-only or busy", 0};
  }
  const auto managed =
      std::find_if(record.library.files.begin(), record.library.files.end(),
                   [relative_path](const core::ManagedFile& file) {
                     return file.relative_path == relative_path;
                   });
  if (managed == record.library.files.end()) {
    return Status{ErrorCode::kNotFound, "Managed Library file was not found",
                  0};
  }
  std::wstring extension =
      std::filesystem::path(Utf8ToWide(managed->relative_path))
          .extension()
          .wstring();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](wchar_t character) { return std::towlower(character); });
  if (extension != L".lib" && extension != L".liberty") {
    return Status{ErrorCode::kInvalidArgument,
                  "Only managed Liberty files can be removed here", 0};
  }
  const std::filesystem::path target =
      record.directory / Utf8ToWide(managed->relative_path);
  std::error_code error;
  if (!std::filesystem::is_regular_file(target, error)) {
    return Status{ErrorCode::kNotFound,
                  "Managed Library file is missing on disk", 0};
  }
  const std::filesystem::path backup =
      target.parent_path() /
      (L".managed-remove-" + Utf8ToWide(NewUuid()) + L".bak");
  if (!MoveFileExW(target.c_str(), backup.c_str(), MOVEFILE_WRITE_THROUGH)) {
    return Win32Status(ErrorCode::kIoError,
                       "Cannot stage the managed file for removal");
  }
  record.library.files.erase(managed);
  Status status = CommitMutation(store_, &record, input.library.revision);
  if (!status.Ok()) {
    MoveFileExW(backup.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH);
    return status;
  }
  DeleteFileW(backup.c_str());
  return record;
}

Status LibraryService::DeleteItem(const LibraryRecord& input,
                                  std::string_view cell_id,
                                  std::string_view view_id,
                                  bool allow_unc_permanent) const {
  const bool deleting_library = cell_id.empty();
  const std::filesystem::path target =
      deleting_library  ? input.directory
      : view_id.empty() ? input.directory / L"cells" / Utf8ToWide(cell_id)
                        : ViewDirectory(input, cell_id, view_id);
  if (IsUncPath(target)) {
    if (!allow_unc_permanent)
      return {ErrorCode::kPermissionDenied,
              "UNC deletion requires explicit permanent-delete approval", 0};
    std::error_code error;
    std::filesystem::remove_all(target, error);
    return error ? Status{ErrorCode::kIoError, "Permanent deletion failed",
                          static_cast<unsigned long>(error.value())}
                 : Status::Success();
  }
  if (deleting_library) return RecyclePath(target);

  LibraryRecord record = input;
  Lease lease(record.directory / L".designpp" / L"writer.lease");
  if (!lease.Acquired())
    return {ErrorCode::kConflict, "Library is read-only or busy", 0};
  core::Cell* cell = FindCell(&record.library, cell_id);
  if (cell == nullptr) return {ErrorCode::kNotFound, "Cell not found", 0};
  if (view_id.empty()) {
    std::erase_if(record.library.cells, [cell_id](const core::Cell& item) {
      return item.id == cell_id;
    });
  } else {
    std::erase_if(cell->views, [view_id](const core::View& item) {
      return item.id == view_id;
    });
  }
  const std::filesystem::path staging =
      record.directory / L".designpp" / L"staging" / Utf8ToWide(NewUuid());
  std::error_code error;
  std::filesystem::rename(target, staging, error);
  if (error)
    return {ErrorCode::kIoError, "Cannot stage deletion",
            static_cast<unsigned long>(error.value())};
  Status status = CommitMutation(store_, &record, input.library.revision);
  if (!status.Ok()) {
    std::filesystem::rename(staging, target, error);
    return status;
  }
  status = RecyclePath(staging);
  return status.Ok()
             ? status
             : Status{ErrorCode::kIoError,
                      "Item is retained in .designpp/staging for recovery",
                      status.native_error};
}

}  // namespace designpp::application

// Copyright 2026 The Design++ Authors

#include "designpp/application/run_store.h"

#include <combaseapi.h>
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace designpp::application {
namespace {

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

std::string NewUuid() {
  GUID guid{};
  if (CoCreateGuid(&guid) != S_OK) return {};
  wchar_t buffer[40]{};
  const int uuid_length =
      StringFromGUID2(guid, buffer, static_cast<int>(std::size(buffer)));
  if (uuid_length == 0) {
    return {};
  }
  std::string value = WideToUtf8(buffer);
  value.erase(std::remove(value.begin(), value.end(), '{'), value.end());
  value.erase(std::remove(value.begin(), value.end(), '}'), value.end());
  std::transform(value.begin(), value.end(), value.begin(), [](char character) {
    return static_cast<char>(
        std::tolower(static_cast<unsigned char>(character)));
  });
  return value;
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

std::string Escape(std::string_view value) {
  std::string result;
  for (char character : value) {
    if (character == '"' || character == '\\') result.push_back('\\');
    if (character == '\n') {
      result += "\\n";
    } else if (character != '\r') {
      result.push_back(character);
    }
  }
  return result;
}

core::Status ValidateOutcome(const RunRecord& run) {
  if (!run.outcome.summary_relative_path.empty() &&
      !core::IsSafeRelativePath(run.outcome.summary_relative_path)) {
    return {core::ErrorCode::kCorruptData, "Run summary path is invalid", 0};
  }
  if (!run.outcome.summary_relative_path.empty()) {
    const std::filesystem::path summary =
        run.directory /
        std::filesystem::path(run.outcome.summary_relative_path);
    const DWORD attributes = GetFileAttributesW(summary.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
      return {core::ErrorCode::kNotFound, "Run summary is missing or unsafe",
              GetLastError()};
    }
  }
  return core::Status::Success();
}

core::Status WriteManifest(const RunRecord& run) {
  const core::Status outcome_status = ValidateOutcome(run);
  if (!outcome_status.Ok()) return outcome_status;
  std::ostringstream output;
  output << "{\n  \"schema_version\": 3,\n  \"run_id\": \"" << Escape(run.id)
         << "\",\n  \"project_id\": \"" << Escape(run.project_id)
         << "\",\n  \"stage\": \"" << Escape(run.stage) << "\",\n  \"tool\": \""
         << Escape(run.tool) << "\",\n  \"tool_version\": \""
         << Escape(run.tool_version) << "\",\n  \"status\": \""
         << RunStatusName(run.status) << "\",\n  \"started_utc\": \""
         << run.started_utc << "\",\n  \"finished_utc\": \"" << run.finished_utc
         << "\",\n  \"exit_code\": " << run.exit_code
         << ",\n  \"process_succeeded\": "
         << (run.outcome.process_succeeded ? "true" : "false")
         << ",\n  \"result_succeeded\": "
         << (run.outcome.result_succeeded ? "true" : "false")
         << ",\n  \"summary_relative_path\": \""
         << Escape(run.outcome.summary_relative_path) << "\""
         << ",\n  \"artifacts\": [";
  for (std::size_t index = 0; index < run.artifacts.size(); ++index) {
    const RunArtifact& artifact = run.artifacts[index];
    output << (index == 0 ? "" : ",") << "\n    {\"kind\": \""
           << Escape(artifact.kind) << "\", \"format\": \""
           << Escape(artifact.format) << "\", \"relative_path\": \""
           << Escape(artifact.relative_path)
           << "\", \"size\": " << artifact.size << ", \"input_hash\": \""
           << Escape(artifact.input_hash)
           << "\", \"partial\": " << (artifact.partial ? "true" : "false")
           << "}";
  }
  output << "\n  ]\n}\n";
  const std::string bytes = output.str();
  const std::filesystem::path path = run.directory / L"manifest.json";
  const std::filesystem::path temporary =
      run.directory /
      std::filesystem::path(L".run-manifest-" +
                            std::to_wstring(GetCurrentProcessId()) + L"-" +
                            std::to_wstring(GetTickCount64()) + L".tmp");
  HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return {core::ErrorCode::kIoError, "Cannot create run manifest",
            GetLastError()};
  }
  DWORD written = 0;
  const bool succeeded =
      bytes.size() <= MAXDWORD &&
      WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written,
                nullptr) &&
      written == bytes.size() && FlushFileBuffers(file);
  const DWORD write_error = succeeded ? ERROR_SUCCESS : GetLastError();
  CloseHandle(file);
  if (!succeeded) {
    DeleteFileW(temporary.c_str());
    return {core::ErrorCode::kIoError, "Cannot write run manifest",
            write_error};
  }
  const bool exists =
      GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
  const bool replaced =
      exists
          ? ReplaceFileW(path.c_str(), temporary.c_str(), nullptr,
                         REPLACEFILE_WRITE_THROUGH, nullptr, nullptr) != FALSE
          : MoveFileExW(temporary.c_str(), path.c_str(),
                        MOVEFILE_WRITE_THROUGH) != FALSE;
  if (!replaced) {
    const DWORD replace_error = GetLastError();
    DeleteFileW(temporary.c_str());
    return {core::ErrorCode::kIoError, "Cannot replace run manifest",
            replace_error};
  }
  return core::Status::Success();
}

std::string ExtractString(std::string_view json, std::string_view key) {
  const std::string marker = "\"" + std::string(key) + "\": \"";
  const std::size_t start = json.find(marker);
  if (start == std::string_view::npos) return {};
  const std::size_t value_start = start + marker.size();
  const std::size_t end = json.find('"', value_start);
  return end == std::string_view::npos
             ? std::string()
             : std::string(json.substr(value_start, end - value_start));
}

bool ExtractBoolean(std::string_view json, std::string_view key) {
  const std::string marker = "\"" + std::string(key) + "\": ";
  const std::size_t start = json.find(marker);
  if (start == std::string_view::npos) return false;
  return json.substr(start + marker.size(), 4) == "true";
}

std::uint32_t ExtractUint32(std::string_view json, std::string_view key) {
  const std::string marker = "\"" + std::string(key) + "\": ";
  const std::size_t start = json.find(marker);
  if (start == std::string_view::npos) return 0;
  std::uint32_t value = 0;
  const char* first = json.data() + start + marker.size();
  const char* last = json.data() + json.size();
  const auto [position, error] = std::from_chars(first, last, value);
  return error == std::errc() && position != first ? value : 0;
}

std::vector<RunArtifact> ExtractArtifacts(std::string_view json) {
  std::vector<RunArtifact> artifacts;
  std::size_t position = json.find("\"artifacts\": [");
  while (position != std::string_view::npos) {
    const std::size_t object = json.find("{\"kind\": \"", position);
    if (object == std::string_view::npos) break;
    const std::size_t end = json.find('}', object);
    if (end == std::string_view::npos) break;
    const std::string_view item = json.substr(object, end - object + 1);
    RunArtifact artifact;
    artifact.kind = ExtractString(item, "kind");
    artifact.format = ExtractString(item, "format");
    artifact.relative_path = ExtractString(item, "relative_path");
    artifact.input_hash = ExtractString(item, "input_hash");
    artifact.partial = ExtractBoolean(item, "partial");
    const std::string marker = "\"size\": ";
    const std::size_t size_position = item.find(marker);
    if (size_position != std::string_view::npos) {
      std::from_chars(item.data() + size_position + marker.size(),
                      item.data() + item.size(), artifact.size);
    }
    if (!artifact.relative_path.empty())
      artifacts.push_back(std::move(artifact));
    position = end + 1;
  }
  return artifacts;
}

}  // namespace

std::string_view RunStatusName(RunStatus status) noexcept {
  switch (status) {
    case RunStatus::kPending:
      return "Pending";
    case RunStatus::kQueued:
      return "Queued";
    case RunStatus::kRunning:
      return "Running";
    case RunStatus::kSucceeded:
      return "Succeeded";
    case RunStatus::kFailed:
      return "Failed";
    case RunStatus::kCancelled:
      return "Cancelled";
    case RunStatus::kInterrupted:
      return "Interrupted";
  }
  return "Interrupted";
}

core::Result<RunRecord> RunStore::Begin(
    const std::filesystem::path& cell_directory, const core::Project& project,
    std::string stage, std::string tool, std::string tool_version) const {
  RunRecord run;
  run.id = NewUuid();
  run.project_id = project.id;
  run.stage = std::move(stage);
  run.tool = std::move(tool);
  run.tool_version = std::move(tool_version);
  run.status = RunStatus::kRunning;
  run.started_utc = UtcNow();
  run.directory =
      cell_directory / L".designpp" / L"runs" / std::filesystem::path(run.id);
  std::error_code error;
  if (!std::filesystem::create_directories(run.directory / L"logs", error) ||
      error) {
    return core::Status{core::ErrorCode::kIoError,
                        "Cannot create unique run directory",
                        static_cast<unsigned long>(error.value())};
  }
  std::filesystem::create_directories(run.directory / L"reports", error);
  std::filesystem::create_directories(run.directory / L"artifacts", error);
  core::Status status = WriteManifest(run);
  return status.Ok() ? core::Result<RunRecord>(std::move(run))
                     : core::Result<RunRecord>(std::move(status));
}

core::Status RunStore::AppendLog(const RunRecord& run,
                                 std::string_view bytes) const {
  std::ofstream output(run.directory / L"logs" / L"raw.log",
                       std::ios::binary | std::ios::app);
  if (!output) return {core::ErrorCode::kIoError, "Cannot append run log", 0};
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  return output ? core::Status::Success()
                : core::Status{core::ErrorCode::kIoError,
                               "Cannot append run log", 0};
}

core::Status RunStore::Complete(
    RunRecord* run, RunStatus status, std::uint32_t exit_code,
    const std::vector<core::Diagnostic>& diagnostics,
    std::vector<RunArtifact> artifacts, RunOutcome outcome) const {
  if (run == nullptr) {
    return {core::ErrorCode::kInvalidArgument, "Run record is missing", 0};
  }
  std::ofstream output(run->directory / L"diagnostics.jsonl",
                       std::ios::binary | std::ios::trunc);
  if (!output) {
    return {core::ErrorCode::kIoError, "Cannot write diagnostics", 0};
  }
  for (const core::Diagnostic& diagnostic : diagnostics) {
    output << "{\"severity\":" << static_cast<int>(diagnostic.severity)
           << ",\"code\":\"" << Escape(diagnostic.code) << "\",\"file\":\""
           << Escape(diagnostic.file) << "\",\"line\":" << diagnostic.line
           << ",\"column\":" << diagnostic.column << ",\"message\":\""
           << Escape(diagnostic.message) << "\"}\n";
  }
  output.flush();
  if (!output) {
    return {core::ErrorCode::kIoError, "Cannot flush diagnostics", 0};
  }
  for (const RunArtifact& artifact : artifacts) {
    if (!core::IsSafeRelativePath(artifact.relative_path)) {
      return {core::ErrorCode::kCorruptData, "Run artifact path is invalid", 0};
    }
    const std::filesystem::path path =
        run->directory / std::filesystem::path(artifact.relative_path);
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
      return {core::ErrorCode::kNotFound, "Run artifact is missing or unsafe",
              GetLastError()};
    }
    LARGE_INTEGER actual_size{};
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE || !GetFileSizeEx(file, &actual_size) ||
        actual_size.QuadPart <= 0) {
      const DWORD error = file == INVALID_HANDLE_VALUE ? GetLastError() : 0;
      if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
      return {core::ErrorCode::kNotFound,
              "Run artifact is empty or cannot be read", error};
    }
    CloseHandle(file);
  }
  run->artifacts = std::move(artifacts);
  run->status = status;
  run->exit_code = exit_code;
  run->outcome = std::move(outcome);
  if (run->outcome.summary_relative_path.empty()) {
    run->outcome.process_succeeded =
        status == RunStatus::kSucceeded && exit_code == 0;
    run->outcome.result_succeeded = status == RunStatus::kSucceeded;
  }
  run->finished_utc = UtcNow();
  return WriteManifest(*run);
}

core::Result<std::vector<RunRecord>> RunStore::List(
    const std::filesystem::path& cell_directory) const {
  std::vector<RunRecord> records;
  const std::filesystem::path root = cell_directory / L".designpp" / L"runs";
  std::error_code error;
  if (!std::filesystem::exists(root, error)) return records;
  for (std::filesystem::directory_iterator iterator(root, error), end;
       !error && iterator != end; iterator.increment(error)) {
    if (!iterator->is_directory(error)) continue;
    std::ifstream input(iterator->path() / L"manifest.json", std::ios::binary);
    if (!input) continue;
    const std::string json((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
    const std::uint32_t schema_version = ExtractUint32(json, "schema_version");
    RunRecord run;
    run.id = ExtractString(json, "run_id");
    run.project_id = ExtractString(json, "project_id");
    run.stage = ExtractString(json, "stage");
    run.tool = ExtractString(json, "tool");
    run.tool_version = ExtractString(json, "tool_version");
    run.started_utc = ExtractString(json, "started_utc");
    run.finished_utc = ExtractString(json, "finished_utc");
    run.exit_code = ExtractUint32(json, "exit_code");
    run.outcome.process_succeeded = ExtractBoolean(json, "process_succeeded");
    run.outcome.result_succeeded = ExtractBoolean(json, "result_succeeded");
    run.outcome.summary_relative_path =
        ExtractString(json, "summary_relative_path");
    run.artifacts = ExtractArtifacts(json);
    const std::string status = ExtractString(json, "status");
    if (status == "Running")
      run.status = RunStatus::kRunning;
    else if (status == "Succeeded")
      run.status = RunStatus::kSucceeded;
    else if (status == "Failed")
      run.status = RunStatus::kFailed;
    else if (status == "Cancelled")
      run.status = RunStatus::kCancelled;
    else if (status == "Interrupted")
      run.status = RunStatus::kInterrupted;
    if (schema_version < 3) {
      // Schema v1/v2 manifests did not distinguish process and domain success.
      run.outcome.process_succeeded =
          run.status == RunStatus::kSucceeded && run.exit_code == 0;
      run.outcome.result_succeeded = run.status == RunStatus::kSucceeded;
    }
    run.directory = iterator->path();
    if (!run.id.empty()) records.push_back(std::move(run));
  }
  if (error) {
    return core::Status{core::ErrorCode::kIoError, "Cannot enumerate runs",
                        static_cast<unsigned long>(error.value())};
  }
  std::sort(records.begin(), records.end(),
            [](const RunRecord& left, const RunRecord& right) {
              return left.started_utc > right.started_utc;
            });
  return records;
}

core::Status RunStore::RecoverInterrupted(
    const std::filesystem::path& cell_directory) const {
  auto records = List(cell_directory);
  if (!records.Ok()) return records.GetStatus();
  std::vector<RunRecord> loaded = std::move(records).Value();
  for (RunRecord& run : loaded) {
    if (run.status == RunStatus::kRunning || run.status == RunStatus::kQueued) {
      run.status = RunStatus::kInterrupted;
      run.finished_utc = UtcNow();
      core::Status status = WriteManifest(run);
      if (!status.Ok()) return status;
    }
  }
  return core::Status::Success();
}

}  // namespace designpp::application

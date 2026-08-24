// Copyright 2026 The Design++ Authors

#include "designpp/runtime/file_watch_service.h"

#include <windows.h>

#include <cstddef>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

namespace designpp::runtime {
namespace {

using core::ErrorCode;
using core::Status;

}  // namespace

struct FileWatchService::Implementation final {
  void Run(std::stop_token stop_token) {
    std::stop_callback stop_callback(stop_token, [this] {
      if (stop_event != nullptr) SetEvent(stop_event);
      if (directory != INVALID_HANDLE_VALUE) CancelIoEx(directory, nullptr);
    });
    std::vector<std::byte> buffer(64 * 1024);
    while (!stop_token.stop_requested()) {
      OVERLAPPED overlapped{};
      overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      if (overlapped.hEvent == nullptr) break;
      const BOOL started = ReadDirectoryChangesW(
          directory, buffer.data(), static_cast<DWORD>(buffer.size()), TRUE,
          FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE |
              FILE_NOTIFY_CHANGE_SIZE,
          nullptr, &overlapped, nullptr);
      if (!started) {
        CloseHandle(overlapped.hEvent);
        break;
      }
      SetEvent(ready_event);
      const HANDLE waits[] = {overlapped.hEvent, stop_event};
      const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
      if (wait == WAIT_OBJECT_0 && !stop_token.stop_requested()) {
        DWORD transferred = 0;
        if (GetOverlappedResult(directory, &overlapped, &transferred, FALSE) &&
            transferred > 0 && callback) {
          std::vector<std::filesystem::path> changes;
          DWORD offset = 0;
          do {
            const auto* information =
                reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buffer.data() +
                                                                 offset);
            changes.push_back(
                watched_directory /
                std::wstring(information->FileName,
                             information->FileNameLength / sizeof(wchar_t)));
            offset = information->NextEntryOffset == 0
                         ? transferred
                         : offset + information->NextEntryOffset;
          } while (offset < transferred);
          callback(std::move(changes));
        }
      } else {
        CancelIoEx(directory, &overlapped);
      }
      CloseHandle(overlapped.hEvent);
      if (wait != WAIT_OBJECT_0) break;
    }
  }

  HANDLE directory = INVALID_HANDLE_VALUE;
  HANDLE ready_event = nullptr;
  HANDLE stop_event = nullptr;
  ChangeCallback callback;
  std::filesystem::path watched_directory;
  std::jthread worker;
};

FileWatchService::FileWatchService()
    : implementation_(std::make_unique<Implementation>()) {}

FileWatchService::~FileWatchService() { Stop(); }

Status FileWatchService::Start(const std::filesystem::path& directory,
                               ChangeCallback callback) {
  Stop();
  implementation_->directory =
      CreateFileW(directory.c_str(), FILE_LIST_DIRECTORY,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_EXISTING,
                  FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
  if (implementation_->directory == INVALID_HANDLE_VALUE) {
    return {ErrorCode::kIoError, "Cannot watch the Cell directory",
            GetLastError()};
  }
  implementation_->stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  implementation_->ready_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (implementation_->stop_event == nullptr ||
      implementation_->ready_event == nullptr) {
    const unsigned long error = GetLastError();
    if (implementation_->stop_event != nullptr) {
      CloseHandle(implementation_->stop_event);
      implementation_->stop_event = nullptr;
    }
    if (implementation_->ready_event != nullptr) {
      CloseHandle(implementation_->ready_event);
      implementation_->ready_event = nullptr;
    }
    CloseHandle(implementation_->directory);
    implementation_->directory = INVALID_HANDLE_VALUE;
    return {ErrorCode::kIoError, "Cannot create the file watcher event", error};
  }
  implementation_->callback = std::move(callback);
  implementation_->watched_directory = directory;
  implementation_->worker = std::jthread(
      [implementation = implementation_.get()](std::stop_token stop_token) {
        implementation->Run(stop_token);
      });
  if (WaitForSingleObject(implementation_->ready_event, 5000) !=
      WAIT_OBJECT_0) {
    Stop();
    return {ErrorCode::kIoError, "File watcher did not become ready", 0};
  }
  return Status::Success();
}

void FileWatchService::Stop() {
  if (!implementation_) return;
  if (implementation_->worker.joinable()) {
    implementation_->worker.request_stop();
    implementation_->worker.join();
  }
  if (implementation_->directory != INVALID_HANDLE_VALUE) {
    CloseHandle(implementation_->directory);
    implementation_->directory = INVALID_HANDLE_VALUE;
  }
  if (implementation_->stop_event != nullptr) {
    CloseHandle(implementation_->stop_event);
    implementation_->stop_event = nullptr;
  }
  if (implementation_->ready_event != nullptr) {
    CloseHandle(implementation_->ready_event);
    implementation_->ready_event = nullptr;
  }
  implementation_->callback = {};
}

}  // namespace designpp::runtime

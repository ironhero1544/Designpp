// Copyright 2026 The Design++ Authors

#include <CppUnitTest.h>
#include <combaseapi.h>
#include <windows.h>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "designpp/runtime/file_watch_service.h"

using Microsoft::VisualStudio::CppUnitTestFramework::Assert;

namespace designpp::tests {
namespace {

std::filesystem::path NewTestDirectory() {
  wchar_t temporary[MAX_PATH]{};
  GetTempPathW(MAX_PATH, temporary);
  GUID guid{};
  CoCreateGuid(&guid);
  wchar_t id[40]{};
  const int uuid_length =
      StringFromGUID2(guid, id, static_cast<int>(std::size(id)));
  if (uuid_length == 0) {
    return {};
  }
  const std::filesystem::path directory =
      std::filesystem::path(temporary) / L"Design++Tests" / id;
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  return directory;
}

bool WriteTestFile(const std::filesystem::path& path) {
  HANDLE file =
      CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  constexpr char kContent[] = "module watched; endmodule\n";
  DWORD written = 0;
  const bool succeeded =
      WriteFile(file, kContent, sizeof(kContent) - 1, &written, nullptr) &&
      written == sizeof(kContent) - 1;
  CloseHandle(file);
  return succeeded;
}

}  // namespace

// clang-format cannot parse the CppUnitTest class and method declaration
// macros.
// clang-format off
TEST_CLASS(FileWatchServiceTests) {
 public:
  TEST_METHOD(ReportsRecursiveFileChangeAndStops) {
    const std::filesystem::path directory = NewTestDirectory();
    const std::filesystem::path nested = directory / L"nested";
    std::error_code error;
    std::filesystem::create_directories(nested, error);
    Assert::IsFalse(static_cast<bool>(error));

    std::mutex mutex;
    std::condition_variable changed;
    bool observed = false;
    runtime::FileWatchService watcher;
    const core::Status status = watcher.Start(
        directory, [&](std::vector<std::filesystem::path> paths) {
          std::scoped_lock lock(mutex);
          for (const std::filesystem::path& path : paths) {
            if (path.filename() == L"watched.sv") observed = true;
          }
          changed.notify_all();
        });
    Assert::IsTrue(status.Ok());
    Assert::IsTrue(WriteTestFile(nested / L"watched.sv"));

    {
      std::unique_lock lock(mutex);
      Assert::IsTrue(changed.wait_for(lock, std::chrono::seconds(5),
                                      [&] { return observed; }));
    }
    watcher.Stop();
    std::filesystem::remove_all(directory, error);
  }
};
// clang-format on

}  // namespace designpp::tests

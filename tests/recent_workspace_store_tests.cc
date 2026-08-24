// Copyright 2026 The Design++ Authors

#include <CppUnitTest.h>
#include <windows.h>

#include <atomic>
#include <latch>
#include <string>
#include <thread>
#include <vector>

#include "designpp/application/recent_workspace_store.h"

using Microsoft::VisualStudio::CppUnitTestFramework::Assert;

namespace designpp::tests {

TEST_CLASS(RecentWorkspaceStoreTests){
  public : TEST_METHOD(TouchDeduplicatesMovesToFrontAndPersistsUtf8Names){
      static std::atomic_uint64_t sequence = 0;
const std::wstring subkey = L"Software\\DesignPlusPlusTests\\Recent-" +
                            std::to_wstring(GetCurrentProcessId()) + L"-" +
                            std::to_wstring(++sequence);
application::RecentWorkspaceStore store(subkey);
application::RecentWorkspace first{{"library", "cell-a", "view-a"},
                                   "라이브러리 / 셀 A / RTL"};
application::RecentWorkspace second{{"library", "cell-b", "view-b"},
                                    "Library / Cell B / Timing"};
Assert::IsTrue(store.Touch(first).Ok());
Assert::IsTrue(store.Touch(second).Ok());
Assert::IsTrue(store.Touch(first).Ok());

auto loaded = store.Load();

Assert::IsTrue(loaded.Ok());
Assert::AreEqual<std::size_t>(2, loaded.Value().size());
Assert::AreEqual("cell-a", loaded.Value()[0].request.cell_id.c_str());
Assert::AreEqual("라이브러리 / 셀 A / RTL",
                 loaded.Value()[0].display_name.c_str());
Assert::IsTrue(store.Clear().Ok());
Assert::AreEqual<std::size_t>(0, store.Load().Value().size());
RegDeleteTreeW(HKEY_CURRENT_USER, subkey.c_str());
}  // namespace designpp::tests

TEST_METHOD(RejectsIncompleteEntry) {
  application::RecentWorkspaceStore store(
      L"Software\\DesignPlusPlusTests\\RecentInvalid");
  Assert::IsFalse(store.Touch({{"", "cell", "view"}, "invalid"}).Ok());
}

TEST_METHOD(ConcurrentIndependentStoresDoNotLoseEntries) {
  static std::atomic_uint64_t sequence = 0;
  const std::wstring subkey = L"Software\\DesignPlusPlusTests\\RecentRace-" +
                              std::to_wstring(GetCurrentProcessId()) + L"-" +
                              std::to_wstring(++sequence);
  constexpr int kWriterCount = 8;
  std::latch start(1);
  std::vector<std::jthread> writers;
  std::atomic_int failures = 0;
  for (int index = 0; index < kWriterCount; ++index) {
    writers.emplace_back([&, index] {
      start.wait();
      application::RecentWorkspaceStore store(subkey);
      application::RecentWorkspace workspace{
          {"library", "cell-" + std::to_string(index),
           "view-" + std::to_string(index)},
          "Workspace " + std::to_string(index)};
      if (!store.Touch(workspace).Ok()) ++failures;
    });
  }
  start.count_down();
  writers.clear();

  application::RecentWorkspaceStore store(subkey);
  auto loaded = store.Load();
  Assert::AreEqual(0, failures.load());
  Assert::IsTrue(loaded.Ok());
  Assert::AreEqual<std::size_t>(kWriterCount, loaded.Value().size());
  Assert::IsTrue(store.Clear().Ok());
  RegDeleteTreeW(HKEY_CURRENT_USER, subkey.c_str());
}
}
;

}  // namespace designpp::tests

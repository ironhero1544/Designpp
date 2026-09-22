// Copyright 2026 The Design++ Authors

#include <CppUnitTest.h>
#include <combaseapi.h>
#include <windows.h>

#include <filesystem>

#include "designpp/application/pdk_selection_service.h"

using Microsoft::VisualStudio::CppUnitTestFramework::Assert;

namespace designpp::tests {
// clang-format off
TEST_CLASS(PdkSelectionTests) {
 public:
  TEST_METHOD(ApplyIsCellScopedAndCoordinatesWithOpenWriter) {
    wchar_t temporary[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temporary);
    GUID guid{};
    Assert::IsTrue(SUCCEEDED(CoCreateGuid(&guid)));
    wchar_t name[40]{};
    StringFromGUID2(guid, name, 40);
    const auto directory = std::filesystem::path(temporary) / (std::wstring(L"pdk-test-") + name);
    std::filesystem::create_directories(directory);
    application::LibraryRecord record;
    record.directory = directory;
    record.library.id = "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb";
    record.library.name = "test";
    record.library.created_utc = record.library.modified_utc = "2026-09-14T00:00:00Z";
    const std::string first = "cccccccc-cccc-cccc-cccc-cccccccccccc";
    const std::string second = "dddddddd-dddd-dddd-dddd-dddddddddddd";
    record.library.cells.push_back({first, "one", "", {}});
    record.library.cells.push_back({second, "two", "", {}});
    application::ProjectService projects;
    std::uint64_t revision = 0;
    {
      auto one = projects.OpenOrCreate(record, first);
      auto two = projects.OpenOrCreate(record, second);
      Assert::IsTrue(one.Ok()); Assert::IsTrue(two.Ok());
      auto one_document = std::move(one).Value();
      revision = one_document.project.revision;
      auto saved = application::PdkSelectionService{}.Apply(
          record, first, revision, {"orfs", "nangate45", {}});
      Assert::IsTrue(saved.Ok());
      Assert::AreEqual(revision + 1, saved.Value());
      one_document.project.physical_implementation.orfs.platform = "asap7";
      const auto stale_save = projects.Save(&one_document);
      Assert::IsFalse(stale_save.Ok());
      Assert::IsTrue(stale_save.code == core::ErrorCode::kConflict);
      revision = saved.Value();
    }
    application::PdkSelectionService service;
    Assert::IsFalse(
        service.Apply(record, first, revision - 1, {"orfs", "asap7", {}})
            .Ok());
    {
      auto one = projects.OpenOrCreate(record, first);
      auto two = projects.OpenOrCreate(record, second);
      Assert::IsTrue(one.Ok()); Assert::IsTrue(two.Ok());
      Assert::AreEqual(std::string("nangate45"), one.Value().project.physical_implementation.orfs.platform);
      Assert::AreEqual(std::string("sky130A"), one.Value().project.physical_implementation.pdk);
      Assert::AreEqual(std::string("sky130_fd_sc_hd"), one.Value().project.physical_implementation.standard_cell_library);
      Assert::AreEqual(std::string("openlane2"), two.Value().project.physical_implementation.backend_id);
    }
    auto switched = service.Apply(
        record, first, revision,
        {"openlane2", "gf180mcuD", "gf180mcu_fd_sc_mcu7t5v0"});
    Assert::IsTrue(switched.Ok());
    {
      auto reopened = projects.OpenOrCreate(record, first);
      Assert::IsTrue(reopened.Ok());
      Assert::AreEqual(
          std::string("openlane2"),
          reopened.Value().project.physical_implementation.backend_id);
      Assert::AreEqual(
          std::string("gf180mcuD"),
          reopened.Value().project.physical_implementation.pdk);
      Assert::AreEqual(
          std::string("gf180mcu_fd_sc_mcu7t5v0"),
          reopened.Value()
              .project.physical_implementation.standard_cell_library);
      Assert::AreEqual(
          std::string("nangate45"),
          reopened.Value().project.physical_implementation.orfs.platform);
    }
    Assert::IsFalse(service.Apply(record, first, switched.Value(),
                                  {"other", "x", {}})
                        .Ok());
    std::filesystem::remove_all(directory);
  }
};
// clang-format on
}  // namespace designpp::tests

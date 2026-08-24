// Copyright 2026 The Design++ Authors

#include <CppUnitTest.h>
#include <windows.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>

#include "designpp/application/library_service.h"
#include "designpp/application/managed_source_service.h"

using Microsoft::VisualStudio::CppUnitTestFramework::Assert;

namespace designpp::tests {
namespace {

class TemporaryLibrary final {
 public:
  TemporaryLibrary() {
    static std::atomic_uint64_t sequence = 0;
    directory_ = std::filesystem::temp_directory_path() /
                 (L"designpp-library-scope-test-" +
                  std::to_wstring(GetCurrentProcessId()) + L"-" +
                  std::to_wstring(++sequence));
    std::filesystem::create_directories(directory_ / L"input");
    std::filesystem::create_directories(directory_ / L".designpp");

    record_.directory = directory_;
    record_.status = core::LibraryStatus::kReady;
    record_.library.id = kLibraryId;
    record_.library.name = "Scope test";
    record_.library.created_utc = "2026-08-24T00:00:00Z";
    record_.library.modified_utc = record_.library.created_utc;
    core::Cell cell;
    cell.id = kCellId;
    cell.name = "top";
    core::View view;
    view.id = kViewId;
    view.name = "constraints";
    view.kind = core::ViewKind::kConstraints;
    cell.views.push_back(std::move(view));
    record_.library.cells.push_back(std::move(cell));

    application::LibraryStore store;
    Assert::IsTrue(store.Save(record_, 0).Ok());
  }

  ~TemporaryLibrary() {
    std::error_code error;
    std::filesystem::remove_all(directory_, error);
  }

  [[nodiscard]] const application::LibraryRecord& record() const {
    return record_;
  }

  [[nodiscard]] const std::filesystem::path& directory() const {
    return directory_;
  }

  static constexpr char kLibraryId[] = "11111111-1111-4111-8111-111111111111";
  static constexpr char kCellId[] = "22222222-2222-4222-8222-222222222222";
  static constexpr char kViewId[] = "33333333-3333-4333-8333-333333333333";

 private:
  std::filesystem::path directory_;
  application::LibraryRecord record_;
};

void WriteText(const std::filesystem::path& path, std::string_view text) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream << text;
}

std::string ReadText(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(stream),
          std::istreambuf_iterator<char>()};
}

}  // namespace

// clang-format off
TEST_CLASS(LibraryFileScopeTests) {
 public:
  TEST_METHOD(ConstraintsViewRejectsLibertyAndAcceptsSdc) {
    TemporaryLibrary workspace;
    const auto liberty = workspace.directory() / L"input" / L"standard.lib";
    const auto sdc = workspace.directory() / L"input" / L"constraints.sdc";
    WriteText(liberty, "library(test) {}\n");
    WriteText(sdc, "create_clock -period 10 [get_ports clk]\n");

    application::LibraryService service;
    const auto rejected = service.ImportFiles(
        workspace.record(), TemporaryLibrary::kCellId,
        TemporaryLibrary::kViewId, {liberty});
    Assert::IsFalse(rejected.Ok());
    Assert::IsTrue(rejected.GetStatus().code ==
                   core::ErrorCode::kInvalidArgument);

    const auto imported = service.ImportFiles(
        workspace.record(), TemporaryLibrary::kCellId,
        TemporaryLibrary::kViewId, {sdc});
    Assert::IsTrue(imported.Ok());
    Assert::AreEqual(static_cast<std::size_t>(1),
                     imported.Value().library.cells[0].views[0].files.size());
  }

  TEST_METHOD(LibraryLevelImportPersistsSharedLiberty) {
    TemporaryLibrary workspace;
    const auto liberty = workspace.directory() / L"input" / L"standard.lib";
    WriteText(liberty, "library(original) {}\n");

    application::LibraryService service;
    const auto imported =
        service.ImportFiles(workspace.record(), {}, {}, {liberty});
    Assert::IsTrue(imported.Ok());
    Assert::AreEqual(static_cast<std::size_t>(1),
                     imported.Value().library.files.size());
    Assert::AreEqual(std::string("files/standard.lib"),
                     imported.Value().library.files[0].relative_path);
    Assert::IsTrue(std::filesystem::exists(
        workspace.directory() / L"files" / L"standard.lib"));
  }

  TEST_METHOD(ReplaceAndRemoveLibraryLibertyAreManifestSafe) {
    TemporaryLibrary workspace;
    const auto original = workspace.directory() / L"input" / L"standard.lib";
    const auto replacement =
        workspace.directory() / L"input" / L"replacement.lib";
    WriteText(original, "library(original) {}\n");
    WriteText(replacement, "library(replacement) { cell(X) {} }\n");

    application::LibraryService service;
    auto imported = service.ImportFiles(workspace.record(), {}, {}, {original});
    Assert::IsTrue(imported.Ok());
    const std::string relative_path =
        imported.Value().library.files[0].relative_path;

    auto replaced = service.ReplaceLibraryFile(
        imported.Value(), relative_path, replacement);
    Assert::IsTrue(replaced.Ok());
    Assert::AreEqual(std::string("files/replacement.lib"),
                     replaced.Value().library.files[0].relative_path);
    Assert::IsFalse(std::filesystem::exists(
        workspace.directory() / L"files" / L"standard.lib"));
    Assert::AreEqual(
        std::string("library(replacement) { cell(X) {} }\n"),
        ReadText(workspace.directory() / L"files" / L"replacement.lib"));
    Assert::IsTrue(replaced.Value().library.revision >
                   imported.Value().library.revision);

    auto removed =
        service.RemoveLibraryFile(replaced.Value(),
                                  "files/replacement.lib");
    Assert::IsTrue(removed.Ok());
    Assert::IsTrue(removed.Value().library.files.empty());
    Assert::IsFalse(std::filesystem::exists(
        workspace.directory() / L"files" / L"replacement.lib"));

    application::LibraryStore store;
    auto reloaded = store.Load(workspace.directory());
    Assert::IsTrue(reloaded.Ok());
    Assert::IsTrue(reloaded.Value().library.files.empty());
  }

  TEST_METHOD(ReplaceLibraryLibertyRollsBackOnStaleManifest) {
    TemporaryLibrary workspace;
    const auto original = workspace.directory() / L"input" / L"standard.lib";
    const auto replacement =
        workspace.directory() / L"input" / L"replacement.lib";
    WriteText(original, "library(original) {}\n");
    WriteText(replacement, "library(replacement) {}\n");

    application::LibraryService service;
    auto imported = service.ImportFiles(workspace.record(), {}, {}, {original});
    Assert::IsTrue(imported.Ok());
    const std::string relative_path =
        imported.Value().library.files[0].relative_path;

    auto updated = service.RenameItem(imported.Value(), {}, {}, "renamed",
                                      "new revision");
    Assert::IsTrue(updated.Ok());
    auto replaced = service.ReplaceLibraryFile(
        imported.Value(), relative_path, replacement);
    Assert::IsFalse(replaced.Ok());
    Assert::AreEqual(
        std::string("library(original) {}\n"),
        ReadText(workspace.directory() / L"files" / L"standard.lib"));

    application::LibraryStore store;
    auto reloaded = store.Load(workspace.directory());
    Assert::IsTrue(reloaded.Ok());
    Assert::AreEqual(updated.Value().library.revision,
                     reloaded.Value().library.revision);
  }

  TEST_METHOD(LibraryLibertyCanBeEditedAndSaved) {
    TemporaryLibrary workspace;
    const auto liberty = workspace.directory() / L"input" / L"standard.lib";
    WriteText(liberty, "library(original) {}\n");

    application::LibraryService library_service;
    auto imported =
        library_service.ImportFiles(workspace.record(), {}, {}, {liberty});
    Assert::IsTrue(imported.Ok());
    const std::string relative_path =
        imported.Value().library.files[0].relative_path;

    application::ManagedSourceService source_service;
    auto loaded = source_service.LoadDocument(
        imported.Value(), {}, {}, relative_path, false);
    Assert::IsTrue(loaded.Ok());
    Assert::IsFalse(loaded.Value().read_only);

    auto saved = source_service.SaveDocument(
        imported.Value(), loaded.Value(), "library(edited) {}\n", false);
    Assert::IsTrue(saved.Ok());
    Assert::AreEqual(
        std::string("library(edited) {}\n"),
        ReadText(workspace.directory() / L"files" / L"standard.lib"));
    Assert::IsTrue(saved.Value().library.library.revision >
                   imported.Value().library.revision);
  }
};
// clang-format on

}  // namespace designpp::tests

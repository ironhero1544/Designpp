// Copyright 2026 The Design++ Authors

#include <CppUnitTest.h>
#include <combaseapi.h>
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "designpp/application/library_service.h"
#include "designpp/application/managed_source_service.h"
#include "designpp/application/project_service.h"

using Microsoft::VisualStudio::CppUnitTestFramework::Assert;

namespace designpp::tests {
namespace {

constexpr char kLibraryId[] = "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa";
constexpr char kCellId[] = "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb";
constexpr char kViewId[] = "cccccccc-cccc-cccc-cccc-cccccccccccc";
constexpr char kRelativePath[] =
    "cells/bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb/views/"
    "cccccccc-cccc-cccc-cccc-cccccccccccc/files/top.sv";

std::filesystem::path NewSourceTestDirectory() {
  wchar_t temporary[MAX_PATH]{};
  GetTempPathW(static_cast<DWORD>(std::size(temporary)), temporary);
  GUID guid{};
  CoCreateGuid(&guid);
  wchar_t id[40]{};
  const int uuid_length =
      StringFromGUID2(guid, id, static_cast<int>(std::size(id)));
  if (uuid_length == 0) {
    return {};
  }
  const std::filesystem::path directory =
      std::filesystem::path(temporary) /
      (L"designpp-source-test-" + std::wstring(id));
  std::filesystem::create_directories(directory / L".designpp" / L"staging");
  std::filesystem::create_directories(directory / L".designpp" / L"recovery");
  return directory;
}

application::LibraryRecord CreateLibrary(const std::filesystem::path& directory,
                                         std::string_view bytes) {
  const std::filesystem::path source =
      directory / L"cells" / L"bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb" /
      L"views" / L"cccccccc-cccc-cccc-cccc-cccccccccccc" / L"files" / L"top.sv";
  std::filesystem::create_directories(source.parent_path());
  std::ofstream output(source, std::ios::binary | std::ios::trunc);
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  output.close();

  core::Library library;
  library.id = kLibraryId;
  library.name = "test";
  library.created_utc = "2026-08-10T00:00:00Z";
  library.modified_utc = library.created_utc;
  core::View view{
      kViewId,
      "rtl",
      "",
      core::ViewKind::kVerilog,
      {{kRelativePath, "primary", bytes.size(), library.created_utc}}};
  core::Cell cell{kCellId, "top", "", {std::move(view)}};
  library.cells.push_back(std::move(cell));
  application::LibraryRecord record{
      std::move(library), directory, core::LibraryStatus::kReady, {}};
  application::LibraryStore store;
  Assert::IsTrue(store.Save(record, 0).Ok());
  return record;
}

std::filesystem::path SourcePath(const std::filesystem::path& directory) {
  return directory / L"cells" / L"bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb" /
         L"views" / L"cccccccc-cccc-cccc-cccc-cccccccccccc" / L"files" /
         L"top.sv";
}

}  // namespace

// clang-format cannot parse the CppUnitTest class and method declaration
// macros.
// clang-format off
TEST_CLASS(ManagedSourceServiceTests) {
 public:
  TEST_METHOD(LoadAndAtomicSavePreserveBomAndCrLf) {
    const std::filesystem::path directory = NewSourceTestDirectory();
    auto library =
        CreateLibrary(directory, "\xef\xbb\xbfmodule top;\r\nendmodule\r\n");
    application::ManagedSourceService service;
    auto loaded = service.LoadDocument(library, kCellId, kViewId, kRelativePath,
                                       false);
    Assert::IsTrue(loaded.Ok());
    Assert::IsTrue(loaded.Value().has_utf8_bom);
    Assert::IsTrue(loaded.Value().line_ending ==
                   application::TextLineEnding::kCrLf);
    auto saved = service.SaveDocument(
        library, loaded.Value(), "module top;\nlogic a;\nendmodule\n", false);
    Assert::IsTrue(saved.Ok());
    auto refreshed =
        service.RefreshDocument(saved.Value().library, saved.Value().document);
    Assert::IsTrue(refreshed.Ok());
    Assert::IsTrue(refreshed.Value().text.find("logic a") != std::string::npos);
    Assert::IsTrue(refreshed.Value().has_utf8_bom);
    Assert::IsTrue(saved.Value().library.library.revision == 2);
    std::error_code error;
    std::filesystem::remove_all(directory, error);
  }

  TEST_METHOD(ExternalModificationIsNotSilentlyOverwritten) {
    const std::filesystem::path directory = NewSourceTestDirectory();
    auto library = CreateLibrary(directory, "module top; endmodule\n");
    application::ManagedSourceService service;
    auto loaded = service.LoadDocument(library, kCellId, kViewId, kRelativePath,
                                       false);
    Assert::IsTrue(loaded.Ok());
    std::ofstream output(SourcePath(directory),
                         std::ios::binary | std::ios::trunc);
    output << "module changed; endmodule\n";
    output.close();
    auto saved = service.SaveDocument(library, loaded.Value(),
                                      "module editor; endmodule\n", false);
    Assert::IsFalse(saved.Ok());
    Assert::IsTrue(saved.GetStatus().code ==
                   core::ErrorCode::kExternalModification);
    std::error_code error;
    std::filesystem::remove_all(directory, error);
  }

  TEST_METHOD(SaveRenamesSingleModuleSourceAndManifest) {
    const std::filesystem::path directory = NewSourceTestDirectory();
    auto library = CreateLibrary(directory, "module top; endmodule\n");
    application::ManagedSourceService service;
    auto loaded = service.LoadDocument(library, kCellId, kViewId, kRelativePath,
                                       false);
    Assert::IsTrue(loaded.Ok());
    auto saved = service.SaveDocument(
        library, loaded.Value(),
        "module half_adder(input logic a, b, output logic y);\n"
        "  assign y = a ^ b;\n"
        "endmodule\n",
        false);
    Assert::IsTrue(saved.Ok());
    Assert::IsTrue(saved.Value().document.relative_path.ends_with(
        "/files/half_adder.sv"));
    Assert::IsFalse(std::filesystem::exists(SourcePath(directory)));
    Assert::IsTrue(std::filesystem::exists(
        SourcePath(directory).parent_path() / L"half_adder.sv"));
    application::LibraryStore store;
    auto reloaded = store.Load(directory);
    Assert::IsTrue(reloaded.Ok());
    Assert::IsTrue(reloaded.Value()
                       .library.cells[0]
                       .views[0]
                       .files[0]
                       .relative_path.ends_with("/files/half_adder.sv"));
    core::Project project;
    project.id = "dddddddd-dddd-dddd-dddd-dddddddddddd";
    project.library_id = kLibraryId;
    project.cell_id = kCellId;
    project.name = "half_adder";
    project.top_module = "half_adder";
    project.created_utc = "2026-08-10T00:00:00Z";
    project.modified_utc = project.created_utc;
    application::ProjectService project_service;
    auto sources = project_service.ResolveSources(saved.Value().library,
                                                   kCellId, project);
    Assert::IsTrue(sources.Ok());
    Assert::AreEqual(static_cast<std::size_t>(1), sources.Value().size());
    Assert::IsTrue(sources.Value()[0].exists);
    Assert::IsTrue(
        sources.Value()[0].relative_path.ends_with("/files/half_adder.sv"));
    std::error_code error;
    std::filesystem::remove_all(directory, error);
  }

  TEST_METHOD(InvalidUtf8OpensAsReadOnlyHexPreview) {
    const std::filesystem::path directory = NewSourceTestDirectory();
    const std::string invalid("\xc3\x28", 2);
    auto library = CreateLibrary(directory, invalid);
    application::ManagedSourceService service;
    auto loaded = service.LoadDocument(library, kCellId, kViewId, kRelativePath,
                                       false);
    Assert::IsTrue(loaded.Ok());
    Assert::IsTrue(loaded.Value().read_only);
    Assert::IsTrue(loaded.Value().text.find("read-only") != std::string::npos);
    std::error_code error;
    std::filesystem::remove_all(directory, error);
  }

  TEST_METHOD(OversizedSourceOpensAsReadOnlyPreview) {
    const std::filesystem::path directory = NewSourceTestDirectory();
    auto library = CreateLibrary(directory, "module top; endmodule\n");
    HANDLE file = CreateFileW(SourcePath(directory).c_str(), GENERIC_WRITE,
                              FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    Assert::IsTrue(file != INVALID_HANDLE_VALUE);
    LARGE_INTEGER size{};
    size.QuadPart =
        static_cast<LONGLONG>(
            application::ManagedSourceService::kMaximumEditableBytes) +
        1;
    Assert::IsTrue(SetFilePointerEx(file, size, nullptr, FILE_BEGIN) != FALSE);
    Assert::IsTrue(SetEndOfFile(file) != FALSE);
    CloseHandle(file);

    application::ManagedSourceService service;
    auto loaded = service.LoadDocument(library, kCellId, kViewId, kRelativePath,
                                       false);
    Assert::IsTrue(loaded.Ok());
    Assert::IsTrue(loaded.Value().read_only);
    Assert::IsTrue(loaded.Value().size ==
                   application::ManagedSourceService::kMaximumEditableBytes +
                       1);
    std::error_code error;
    std::filesystem::remove_all(directory, error);
  }
};
// clang-format on

}  // namespace designpp::tests

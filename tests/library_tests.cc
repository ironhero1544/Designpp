// Copyright 2026 The Design++ Authors

#include <CppUnitTest.h>
#include <combaseapi.h>
#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

#include "designpp/application/library_browser_model.h"
#include "designpp/application/library_service.h"
#include "designpp/core/library.h"

using Microsoft::VisualStudio::CppUnitTestFramework::Assert;

namespace designpp::tests {
namespace {

std::filesystem::path NewTestDirectory() {
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
      std::filesystem::path(temporary) / (L"designpp-test-" + std::wstring(id));
  std::filesystem::create_directories(directory);
  return directory;
}

core::Library SampleLibrary() {
  core::Library library;
  library.id = "11111111-1111-1111-1111-111111111111";
  library.name = "Digital Library";
  library.created_utc = "2026-01-01T00:00:00Z";
  library.modified_utc = library.created_utc;
  core::Cell cell{"22222222-2222-2222-2222-222222222222", "counter", "", {}};
  core::View view{"33333333-3333-3333-3333-333333333333",
                  "rtl",
                  "",
                  core::ViewKind::kVerilog,
                  {}};
  view.files.push_back({"cells/222/views/333/files/counter.sv", "primary", 0,
                        library.created_utc});
  cell.views.push_back(std::move(view));
  library.cells.push_back(std::move(cell));
  return library;
}

application::LibraryRecord SampleRecord() {
  return {SampleLibrary(), {}, core::LibraryStatus::kReady, {}};
}

}  // namespace

TEST_CLASS(LibraryModelTests){
  public : TEST_METHOD(ValidLibraryPassesValidation){
      Assert::IsTrue(core::ValidateLibrary(SampleLibrary()).Ok());
}  // namespace designpp::tests

TEST_METHOD(DuplicateCellNamesAreRejectedCaseInsensitively) {
  core::Library library = SampleLibrary();
  library.cells.push_back(
      {"44444444-4444-4444-4444-444444444444", "COUNTER", "", {}});
  Assert::IsTrue(core::ValidateLibrary(library).code ==
                 core::ErrorCode::kAlreadyExists);
}

TEST_METHOD(ManagedFileCannotEscapeLibrary) {
  core::Library library = SampleLibrary();
  library.cells[0].views[0].files[0].relative_path = "../outside.sv";
  Assert::IsTrue(core::ValidateLibrary(library).code ==
                 core::ErrorCode::kCorruptData);
}
}
;

TEST_CLASS(LibraryBrowserModelTests){
  public : TEST_METHOD(SearchIsCaseInsensitiveAndPreservesStableIdentity){
      const std::vector<application::LibraryRecord> libraries{SampleRecord()};
const application::LibraryBrowserModel model(libraries);
const application::BrowserResult result =
    model.FilterLibraries({"digital", {}, {}});

Assert::AreEqual(static_cast<size_t>(2), result.items.size());
Assert::AreEqual(std::string("11111111-1111-1111-1111-111111111111"),
                 result.items[0].library_id);
Assert::IsTrue(result.items[1].create_action);
Assert::IsTrue(result.exact_match == std::nullopt);
}

TEST_METHOD(ViewResultsExposeManagedFileNames) {
  const std::vector<application::LibraryRecord> libraries{SampleRecord()};
  const application::LibraryBrowserModel model(libraries);
  const application::BrowserResult result =
      model.FilterViews("11111111-1111-1111-1111-111111111111",
                        "22222222-2222-2222-2222-222222222222", {});

  Assert::AreEqual(static_cast<size_t>(1), result.items.size());
  Assert::AreEqual(std::string("counter.sv"), result.items[0].details);
}

TEST_METHOD(LibraryResultsExposeSharedManagedFileNames) {
  application::LibraryRecord record = SampleRecord();
  record.library.files.push_back(
      {"files/standard.lib", "support", 12, "2026-01-01T00:00:00Z"});
  const std::vector<application::LibraryRecord> libraries{std::move(record)};
  const application::LibraryBrowserModel model(libraries);
  const application::BrowserResult result = model.FilterLibraries({});

  Assert::AreEqual(std::string("standard.lib"), result.items[0].details);
}

TEST_METHOD(ExactMatchHiddenByFilterDoesNotOfferDuplicateCreation) {
  const std::vector<application::LibraryRecord> libraries{SampleRecord()};
  const application::LibraryBrowserModel model(libraries);
  application::BrowserQuery query{
      "Digital Library", core::LibraryStatus::kMissing, {}};
  const application::BrowserResult result = model.FilterLibraries(query);

  Assert::IsTrue(result.exact_match.has_value());
  Assert::IsFalse(result.exact_match_visible);
  Assert::IsFalse(result.can_create);
  Assert::AreEqual(static_cast<size_t>(0), result.items.size());
}

TEST_METHOD(MissingExactNameOffersInlineCreation) {
  const std::vector<application::LibraryRecord> libraries{SampleRecord()};
  const application::LibraryBrowserModel model(libraries);
  const application::BrowserResult result =
      model.FilterLibraries({"New Library", {}, {}});

  Assert::IsTrue(result.can_create);
  Assert::AreEqual(static_cast<size_t>(1), result.items.size());
  Assert::IsTrue(result.items[0].create_action);
}

TEST_METHOD(KoreanSubstringSearchIsSupported) {
  application::LibraryRecord record = SampleRecord();
  record.library.name = "디지털 라이브러리";
  const std::vector<application::LibraryRecord> libraries{std::move(record)};
  const application::LibraryBrowserModel model(libraries);
  const application::BrowserResult result =
      model.FilterLibraries({"라이브", {}, {}});

  Assert::AreEqual(static_cast<size_t>(2), result.items.size());
  Assert::AreEqual(std::string("디지털 라이브러리"), result.items[0].name);
}
}
;

TEST_CLASS(LibraryStoreTests){
  public : TEST_METHOD(ManifestRoundTripPreservesHierarchy){
      const std::filesystem::path directory = NewTestDirectory();
application::LibraryRecord record{
    SampleLibrary(), directory, core::LibraryStatus::kReady, {}};
application::LibraryStore store;
Assert::IsTrue(store.Save(record, 0).Ok());
auto loaded = store.Load(directory);
Assert::IsTrue(loaded.Ok());
Assert::AreEqual(std::string("Digital Library"), loaded.Value().library.name);
Assert::AreEqual(std::string("counter"), loaded.Value().library.cells[0].name);
Assert::AreEqual(std::string("rtl"),
                 loaded.Value().library.cells[0].views[0].name);
std::error_code error;
std::filesystem::remove_all(directory, error);
}

TEST_METHOD(RevisionConflictDoesNotOverwriteManifest) {
  const std::filesystem::path directory = NewTestDirectory();
  application::LibraryRecord record{
      SampleLibrary(), directory, core::LibraryStatus::kReady, {}};
  application::LibraryStore store;
  Assert::IsTrue(store.Save(record, 0).Ok());
  record.library.revision = 2;
  Assert::IsTrue(store.Save(record, 99).code == core::ErrorCode::kConflict);
  auto loaded = store.Load(directory);
  Assert::IsTrue(loaded.Ok());
  Assert::AreEqual(static_cast<std::uint64_t>(1),
                   loaded.Value().library.revision);
  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

TEST_METHOD(SynthesisViewCreatesAnEmptyCellViewContainer) {
  const std::filesystem::path directory = NewTestDirectory();
  std::filesystem::create_directories(directory / L".designpp");
  application::LibraryRecord record{
      SampleLibrary(), directory, core::LibraryStatus::kReady, {}};
  application::LibraryStore store;
  Assert::IsTrue(store.Save(record, 0).Ok());

  application::CreateViewRequest request;
  request.name = "synthesis";
  request.kind = core::ViewKind::kSynthesis;
  auto created = application::LibraryService().CreateView(
      record, record.library.cells[0].id, request);

  Assert::IsTrue(created.Ok());
  Assert::AreEqual(static_cast<std::size_t>(2),
                   created.Value().library.cells[0].views.size());
  const core::View& view = created.Value().library.cells[0].views.back();
  Assert::IsTrue(view.kind == core::ViewKind::kSynthesis);
  Assert::IsTrue(view.files.empty());
  Assert::IsTrue(std::filesystem::is_directory(
      directory / L"cells" / L"22222222-2222-2222-2222-222222222222" /
      L"views" / std::filesystem::path(view.id) / L"files"));
  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

TEST_METHOD(TimingViewRoundTripsAndSchemaOneMigratesInMemory) {
  const std::filesystem::path directory = NewTestDirectory();
  std::filesystem::create_directories(directory / L".designpp");
  application::LibraryRecord record{
      SampleLibrary(), directory, core::LibraryStatus::kReady, {}};
  application::LibraryStore store;
  Assert::IsTrue(store.Save(record, 0).Ok());
  application::CreateViewRequest request;
  request.name = "timing";
  request.kind = core::ViewKind::kTiming;
  auto created = application::LibraryService().CreateView(
      record, record.library.cells[0].id, request);
  Assert::IsTrue(created.Ok());
  Assert::IsTrue(created.Value().library.cells[0].views.back().kind ==
                 core::ViewKind::kTiming);

  const std::filesystem::path manifest = directory / L"library.dplib";
  std::string json;
  {
    std::ifstream input(manifest, std::ios::binary);
    json.assign(std::istreambuf_iterator<char>(input),
                std::istreambuf_iterator<char>());
  }
  const std::size_t schema = json.find("\"schema_version\": 5");
  Assert::IsTrue(schema != std::string::npos);
  json.replace(schema, std::string("\"schema_version\": 5").size(),
               "\"schema_version\": 1");
  std::ofstream(manifest, std::ios::binary | std::ios::trunc) << json;
  auto migrated = store.Load(directory);
  Assert::IsTrue(migrated.Ok());
  Assert::AreEqual(core::Library::kSchemaVersion,
                   migrated.Value().library.schema_version);
  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

TEST_METHOD(PhysicalDesignViewMigratesToLayoutFromSchemaFour) {
  const std::filesystem::path directory = NewTestDirectory();
  std::filesystem::create_directories(directory / L".designpp");
  application::LibraryRecord record{
      SampleLibrary(), directory, core::LibraryStatus::kReady, {}};
  application::LibraryStore store;
  Assert::IsTrue(store.Save(record, 0).Ok());
  application::CreateViewRequest request;
  request.name = "layout";
  request.kind = core::ViewKind::kLayout;
  auto created = application::LibraryService().CreateView(
      record, record.library.cells[0].id, request);
  Assert::IsTrue(created.Ok());
  const std::filesystem::path manifest = directory / L"library.dplib";
  std::ifstream input(manifest, std::ios::binary);
  std::string json((std::istreambuf_iterator<char>(input)),
                   std::istreambuf_iterator<char>());
  const std::size_t schema = json.find("\"schema_version\": 5");
  const std::size_t kind = json.find("\"kind\": \"Layout\"");
  Assert::IsTrue(schema != std::string::npos);
  Assert::IsTrue(kind != std::string::npos);
  json.replace(schema, std::string("\"schema_version\": 5").size(),
               "\"schema_version\": 4");
  json.replace(kind, std::string("\"kind\": \"Layout\"").size(),
               "\"kind\": \"Physical Design\"");
  std::ofstream(manifest, std::ios::binary | std::ios::trunc) << json;
  auto loaded = store.Load(directory);
  Assert::IsTrue(loaded.Ok());
  Assert::IsTrue(loaded.Value().library.cells[0].views.back().kind ==
                 core::ViewKind::kLayout);
  Assert::AreEqual(core::Library::kSchemaVersion,
                   loaded.Value().library.schema_version);
  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

TEST_METHOD(SchemaFourPhysicalDuplicateKeepsExistingLayout) {
  const std::filesystem::path directory = NewTestDirectory();
  std::filesystem::create_directories(directory / L".designpp");
  application::LibraryRecord record{
      SampleLibrary(), directory, core::LibraryStatus::kReady, {}};
  application::LibraryStore initial_store;
  Assert::IsTrue(initial_store.Save(record, 0).Ok());
  application::CreateViewRequest request;
  request.name = "layout";
  request.kind = core::ViewKind::kLayout;
  auto created = application::LibraryService().CreateView(
      record, record.library.cells[0].id, request);
  Assert::IsTrue(created.Ok());
  const std::filesystem::path manifest = directory / L"library.dplib";
  std::ifstream input(manifest, std::ios::binary);
  std::string json((std::istreambuf_iterator<char>(input)),
                   std::istreambuf_iterator<char>());
  const std::size_t schema = json.find("\"schema_version\": 5");
  const std::size_t layout_kind = json.find("\"kind\": \"Layout\"");
  const std::size_t object_begin = json.rfind('{', layout_kind);
  const std::size_t object_end = json.find('}', layout_kind);
  Assert::IsTrue(schema != std::string::npos);
  Assert::IsTrue(object_begin != std::string::npos);
  Assert::IsTrue(object_end != std::string::npos);
  std::string physical =
      json.substr(object_begin, object_end - object_begin + 1);
  const std::size_t physical_kind = physical.find("\"kind\": \"Layout\"");
  physical.replace(physical_kind, std::string("\"kind\": \"Layout\"").size(),
                   "\"kind\": \"Physical Design\"");
  const std::size_t view_id = physical.find("\"view_id\": \"");
  const std::size_t id_begin = view_id + std::string("\"view_id\": \"").size();
  const std::size_t id_end = physical.find('"', id_begin);
  physical.replace(id_begin, id_end - id_begin,
                   "99999999-9999-4999-8999-999999999999");
  json.insert(object_end + 1, ",\n      " + physical);
  json.replace(schema, std::string("\"schema_version\": 5").size(),
               "\"schema_version\": 4");
  std::ofstream(manifest, std::ios::binary | std::ios::trunc) << json;

  application::LibraryStore store;
  auto loaded = store.Load(directory);
  Assert::IsTrue(loaded.Ok());
  const auto& views = loaded.Value().library.cells[0].views;
  Assert::AreEqual(static_cast<std::size_t>(2), views.size());
  Assert::AreEqual(static_cast<std::size_t>(1),
                   static_cast<std::size_t>(std::count_if(
                       views.begin(), views.end(), [](const core::View& view) {
                         return view.kind == core::ViewKind::kLayout;
                       })));
  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

TEST_METHOD(SchemaTwoWithoutSharedFilesMigratesInMemory) {
  const std::filesystem::path directory = NewTestDirectory();
  application::LibraryRecord record{
      SampleLibrary(), directory, core::LibraryStatus::kReady, {}};
  application::LibraryStore store;
  Assert::IsTrue(store.Save(record, 0).Ok());
  const std::filesystem::path manifest = directory / L"library.dplib";
  std::string json;
  {
    std::ifstream input(manifest, std::ios::binary);
    json.assign(std::istreambuf_iterator<char>(input),
                std::istreambuf_iterator<char>());
  }
  const std::size_t schema = json.find("\"schema_version\": 5");
  const std::size_t files = json.find("  \"files\": [],\n");
  Assert::IsTrue(schema != std::string::npos);
  Assert::IsTrue(files != std::string::npos);
  json.replace(schema, std::string("\"schema_version\": 5").size(),
               "\"schema_version\": 2");
  json.erase(files, std::string("  \"files\": [],\n").size());
  std::ofstream(manifest, std::ios::binary | std::ios::trunc) << json;

  auto migrated = store.Load(directory);
  Assert::IsTrue(migrated.Ok());
  Assert::AreEqual(core::Library::kSchemaVersion,
                   migrated.Value().library.schema_version);
  Assert::IsTrue(migrated.Value().library.files.empty());

  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

TEST_METHOD(ImportRejectsDuplicateNamesWithoutLeavingCopiedFiles) {
  const std::filesystem::path directory = NewTestDirectory();
  application::LibraryRecord record{
      SampleLibrary(), directory, core::LibraryStatus::kReady, {}};
  record.library.cells[0].views[0].files.clear();
  const std::filesystem::path files =
      directory / L"cells" / L"22222222-2222-2222-2222-222222222222" /
      L"views" / L"33333333-3333-3333-3333-333333333333" / L"files";
  std::filesystem::create_directories(files);
  std::filesystem::create_directories(directory / L".designpp");
  application::LibraryStore store;
  Assert::IsTrue(store.Save(record, 0).Ok());
  auto loaded = store.Load(directory);
  Assert::IsTrue(loaded.Ok());

  const std::filesystem::path first_directory = directory / L"input-a";
  const std::filesystem::path second_directory = directory / L"input-b";
  std::filesystem::create_directories(first_directory);
  std::filesystem::create_directories(second_directory);
  const std::filesystem::path first = first_directory / L"duplicate.sv";
  const std::filesystem::path second = second_directory / L"duplicate.sv";
  std::ofstream(first) << "module first; endmodule\n";
  std::ofstream(second) << "module second; endmodule\n";

  application::LibraryService service;
  const auto imported = service.ImportFiles(
      loaded.Value(), "22222222-2222-2222-2222-222222222222",
      "33333333-3333-3333-3333-333333333333", {first, second});
  Assert::IsFalse(imported.Ok());
  Assert::IsTrue(imported.GetStatus().code == core::ErrorCode::kAlreadyExists);
  Assert::IsFalse(std::filesystem::exists(files / L"duplicate.sv"));

  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

TEST_METHOD(LibraryLevelImportPersistsSharedLiberty) {
  const std::filesystem::path directory = NewTestDirectory();
  std::filesystem::create_directories(directory / L".designpp");
  application::LibraryRecord record{
      SampleLibrary(), directory, core::LibraryStatus::kReady, {}};
  application::LibraryStore store;
  Assert::IsTrue(store.Save(record, 0).Ok());
  auto loaded = store.Load(directory);
  Assert::IsTrue(loaded.Ok());

  const std::filesystem::path source = directory / L"standard.lib.input";
  std::ofstream(source) << "library(test) {}\n";
  const std::filesystem::path liberty = directory / L"standard.lib";
  std::filesystem::rename(source, liberty);
  application::LibraryService service;
  const auto imported = service.ImportFiles(loaded.Value(), {}, {}, {liberty});

  Assert::IsTrue(imported.Ok());
  Assert::AreEqual(static_cast<std::size_t>(1),
                   imported.Value().library.files.size());
  Assert::AreEqual(std::string("files/standard.lib"),
                   imported.Value().library.files[0].relative_path);
  Assert::IsTrue(
      std::filesystem::is_regular_file(directory / L"files" / L"standard.lib"));
  auto reopened = store.Load(directory);
  Assert::IsTrue(reopened.Ok());
  Assert::AreEqual(std::string("files/standard.lib"),
                   reopened.Value().library.files[0].relative_path);

  std::error_code error;
  std::filesystem::remove_all(directory, error);
}
}
;

}  // namespace designpp::tests

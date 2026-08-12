// Copyright 2026 The Design++ Authors

#include <CppUnitTest.h>
#include <combaseapi.h>
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "designpp/application/project_service.h"
#include "designpp/application/run_store.h"
#include "designpp/core/project.h"

using Microsoft::VisualStudio::CppUnitTestFramework::Assert;

namespace designpp::tests {
namespace {

std::filesystem::path NewProjectTestDirectory() {
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
      (L"designpp-project-test-" + std::wstring(id));
  std::filesystem::create_directories(directory);
  return directory;
}

core::Project SampleProject() {
  core::Project project;
  project.id = "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa";
  project.library_id = "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb";
  project.cell_id = "cccccccc-cccc-cccc-cccc-cccccccccccc";
  project.name = "counter";
  project.top_module = "counter";
  project.created_utc = "2026-08-10T00:00:00Z";
  project.modified_utc = project.created_utc;
  project.cpu_budget = 1;
  project.defines.push_back("SIMULATION=1");
  project.synthesis.liberty_paths.push_back(
      "cells/c/views/constraints/files/std.lib");
  project.synthesis.flatten = true;
  project.timing.corner_name = "slow";
  project.timing.liberty_paths = project.synthesis.liberty_paths;
  project.timing.sdc_path = "cells/c/views/constraints/files/top.sdc";
  project.source_overrides.push_back({"dddddddd-dddd-dddd-dddd-dddddddddddd",
                                      "cells/c/views/v/files/counter.sv", false,
                                      "primary"});
  project.testbench_configurations.push_back(
      {"eeeeeeee-eeee-eeee-eeee-eeeeeeeeeeee",
       "cells/c/views/tb/files/counter_tb.sv", "counter_tb", "icarus", true});
  return project;
}

application::LibraryRecord SampleProjectLibrary(
    const std::filesystem::path& directory) {
  core::Library library;
  library.id = "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb";
  library.name = "library";
  library.created_utc = "2026-08-10T00:00:00Z";
  library.modified_utc = library.created_utc;
  core::Cell cell{"cccccccc-cccc-cccc-cccc-cccccccccccc", "counter", "", {}};
  core::View view{"dddddddd-dddd-dddd-dddd-dddddddddddd",
                  "rtl",
                  "",
                  core::ViewKind::kVerilog,
                  {}};
  view.files.push_back(
      {"cells/cccccccc-cccc-cccc-cccc-cccccccccccc/views/"
       "dddddddd-dddd-dddd-dddd-dddddddddddd/files/counter.sv",
       "primary", 0, library.created_utc});
  cell.views.push_back(std::move(view));
  library.cells.push_back(std::move(cell));
  return {std::move(library), directory, core::LibraryStatus::kReady, {}};
}

}  // namespace

TEST_CLASS(ProjectModelTests){
  public : TEST_METHOD(ValidProjectPassesValidation){
      Assert::IsTrue(core::ValidateProject(SampleProject()).Ok());
}  // namespace designpp::tests

TEST_METHOD(ProjectPathCannotEscapeLibrary) {
  core::Project project = SampleProject();
  project.include_directories.push_back("../outside");
  Assert::IsTrue(core::ValidateProject(project).code ==
                 core::ErrorCode::kCorruptData);
}

TEST_METHOD(TestbenchConfigurationIsValidated) {
  core::Project project = SampleProject();
  project.testbench_configurations.push_back(
      {"dddddddd-dddd-dddd-dddd-dddddddddddd",
       "cells/c/views/tb/files/counter_tb.sv", "counter_tb", "icarus", true});
  Assert::IsTrue(core::ValidateProject(project).Ok());
  project.testbench_configurations.push_back(
      project.testbench_configurations.front());
  Assert::IsTrue(core::ValidateProject(project).code ==
                 core::ErrorCode::kCorruptData);
}
}
;

TEST_CLASS(ProjectStoreTests){
  public : TEST_METHOD(ProjectRoundTripPreservesConfiguration){
      const std::filesystem::path directory = NewProjectTestDirectory();
const std::filesystem::path path = directory / L"project.dpproj";
application::ProjectStore store;
const core::Project project = SampleProject();
Assert::IsTrue(store.Save(project, path, 0).Ok());
auto loaded = store.Load(path);
Assert::IsTrue(loaded.Ok());
Assert::AreEqual(project.name, loaded.Value().name);
Assert::AreEqual(project.top_module, loaded.Value().top_module);
Assert::AreEqual(static_cast<std::size_t>(1),
                 loaded.Value().source_overrides.size());
Assert::AreEqual(static_cast<std::size_t>(1),
                 loaded.Value().testbench_configurations.size());
Assert::AreEqual(std::string("counter_tb"),
                 loaded.Value().testbench_configurations[0].top_module);
Assert::AreEqual(std::string("hdl"),
                 loaded.Value().testbench_configurations[0].runner);
Assert::AreEqual(std::string("vcd"),
                 loaded.Value().testbench_configurations[0].waveform_format);
Assert::IsTrue(loaded.Value().synthesis.flatten);
Assert::AreEqual(std::string("slow"), loaded.Value().timing.corner_name);
Assert::AreEqual(std::string("cells/c/views/constraints/files/top.sdc"),
                 loaded.Value().timing.sdc_path);
Assert::AreEqual(core::Project::kSchemaVersion, loaded.Value().schema_version);
std::error_code error;
std::filesystem::remove_all(directory, error);
}

TEST_METHOD(RevisionConflictPreservesCurrentProject) {
  const std::filesystem::path directory = NewProjectTestDirectory();
  const std::filesystem::path path = directory / L"project.dpproj";
  application::ProjectStore store;
  core::Project project = SampleProject();
  Assert::IsTrue(store.Save(project, path, 0).Ok());
  project.revision = 2;
  Assert::IsTrue(store.Save(project, path, 99).code ==
                 core::ErrorCode::kConflict);
  auto loaded = store.Load(path);
  Assert::IsTrue(loaded.Ok());
  Assert::AreEqual(static_cast<std::uint64_t>(1), loaded.Value().revision);
  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

TEST_METHOD(SchemaOneMigratesInMemoryWithoutOverwritingSource) {
  const std::filesystem::path directory = NewProjectTestDirectory();
  const std::filesystem::path path = directory / L"project.dpproj";
  const std::string schema_one =
      "{\n  \"schema_version\": 1,\n"
      "  \"project_id\": \"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa\",\n"
      "  \"revision\": 1,\n"
      "  \"library_id\": \"bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb\",\n"
      "  \"cell_id\": \"cccccccc-cccc-cccc-cccc-cccccccccccc\",\n"
      "  \"name\": \"counter\",\n  \"top_module\": \"counter\",\n"
      "  \"created_utc\": \"2026-08-10T00:00:00Z\",\n"
      "  \"modified_utc\": \"2026-08-10T00:00:00Z\",\n"
      "  \"source_policy\": \"auto_managed\",\n  \"cpu_budget\": 1,\n"
      "  \"constraint_path\": \"\",\n  \"toolchain_profile_id\": \"\",\n"
      "  \"include_directories\": [],\n  \"defines\": [],\n"
      "  \"parameters\": [],\n  \"source_overrides\": []\n}\n";
  std::ofstream(path, std::ios::binary) << schema_one;
  application::ProjectStore store;
  auto loaded = store.Load(path);
  Assert::IsTrue(loaded.Ok());
  Assert::AreEqual(core::Project::kSchemaVersion,
                   loaded.Value().schema_version);
  std::ifstream input(path, std::ios::binary);
  const std::string unchanged((std::istreambuf_iterator<char>(input)),
                              std::istreambuf_iterator<char>());
  Assert::AreEqual(schema_one, unchanged);
  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

TEST_METHOD(SchemaTwoMigratesTestbenchWaveformConfiguration) {
  const std::filesystem::path directory = NewProjectTestDirectory();
  const std::filesystem::path path = directory / L"project.dpproj";
  const std::string schema_two =
      "{\n  \"schema_version\": 2,\n"
      "  \"project_id\": \"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa\",\n"
      "  \"revision\": 1,\n"
      "  \"library_id\": \"bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb\",\n"
      "  \"cell_id\": \"cccccccc-cccc-cccc-cccc-cccccccccccc\",\n"
      "  \"name\": \"counter\",\n  \"top_module\": \"counter\",\n"
      "  \"created_utc\": \"2026-08-10T00:00:00Z\",\n"
      "  \"modified_utc\": \"2026-08-10T00:00:00Z\",\n"
      "  \"source_policy\": \"auto_managed\",\n  \"cpu_budget\": 1,\n"
      "  \"constraint_path\": \"\",\n  \"toolchain_profile_id\": \"\",\n"
      "  \"include_directories\": [],\n  \"defines\": [],\n"
      "  \"parameters\": [],\n  \"source_overrides\": [],\n"
      "  \"testbench_configurations\": [{\"view_id\": "
      "\"eeeeeeee-eeee-eeee-eeee-eeeeeeeeeeee\", "
      "\"relative_path\": \"cells/c/views/tb/files/counter_tb.sv\", "
      "\"top_module\": \"counter_tb\", \"backend\": \"icarus\", "
      "\"waveform_enabled\": 0}]\n}\n";
  std::ofstream(path, std::ios::binary) << schema_two;
  application::ProjectStore store;
  auto loaded = store.Load(path);
  Assert::IsTrue(loaded.Ok());
  Assert::AreEqual(core::Project::kSchemaVersion,
                   loaded.Value().schema_version);
  Assert::AreEqual(std::string("hdl"),
                   loaded.Value().testbench_configurations[0].runner);
  Assert::AreEqual(std::string("none"),
                   loaded.Value().testbench_configurations[0].waveform_format);
  std::ifstream input(path, std::ios::binary);
  const std::string unchanged((std::istreambuf_iterator<char>(input)),
                              std::istreambuf_iterator<char>());
  Assert::AreEqual(schema_two, unchanged);
  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

TEST_METHOD(SchemaThreeMigratesSynthesisAndTimingDefaults) {
  const std::filesystem::path directory = NewProjectTestDirectory();
  const std::filesystem::path path = directory / L"project.dpproj";
  const std::string schema_three =
      "{\n  \"schema_version\": 3,\n"
      "  \"project_id\": \"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa\",\n"
      "  \"revision\": 1,\n"
      "  \"library_id\": \"bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb\",\n"
      "  \"cell_id\": \"cccccccc-cccc-cccc-cccc-cccccccccccc\",\n"
      "  \"name\": \"counter\",\n  \"top_module\": \"counter\",\n"
      "  \"created_utc\": \"2026-08-10T00:00:00Z\",\n"
      "  \"modified_utc\": \"2026-08-10T00:00:00Z\",\n"
      "  \"source_policy\": \"auto_managed\",\n  \"cpu_budget\": 1,\n"
      "  \"constraint_path\": \"\",\n  \"toolchain_profile_id\": \"\",\n"
      "  \"include_directories\": [],\n  \"defines\": [],\n"
      "  \"parameters\": [],\n  \"source_overrides\": [],\n"
      "  \"testbench_configurations\": []\n}\n";
  std::ofstream(path, std::ios::binary) << schema_three;
  application::ProjectStore store;
  auto loaded = store.Load(path);
  Assert::IsTrue(loaded.Ok());
  Assert::AreEqual(core::Project::kSchemaVersion,
                   loaded.Value().schema_version);
  Assert::IsFalse(loaded.Value().synthesis.flatten);
  Assert::IsTrue(loaded.Value().synthesis.liberty_paths.empty());
  Assert::AreEqual(std::string("typical"), loaded.Value().timing.corner_name);
  Assert::IsTrue(loaded.Value().timing.liberty_paths.empty());
  Assert::IsTrue(loaded.Value().timing.sdc_path.empty());
  std::ifstream input(path, std::ios::binary);
  const std::string unchanged((std::istreambuf_iterator<char>(input)),
                              std::istreambuf_iterator<char>());
  Assert::AreEqual(schema_three, unchanged);
  std::error_code error;
  std::filesystem::remove_all(directory, error);
}
}
;

TEST_CLASS(ProjectServiceTests){
  public : TEST_METHOD(OpenCreatesProjectAndCompetingWriterIsReadOnly){
      const std::filesystem::path directory = NewProjectTestDirectory();
auto library = SampleProjectLibrary(directory);
application::ProjectService service;
auto first = service.OpenOrCreate(library, library.library.cells[0].id);
Assert::IsTrue(first.Ok());
Assert::IsFalse(first.Value().read_only);
auto second = service.OpenOrCreate(library, library.library.cells[0].id);
Assert::IsTrue(second.Ok());
Assert::IsTrue(second.Value().read_only);
std::error_code error;
std::filesystem::remove_all(directory, error);
}

TEST_METHOD(SourceSetReflectsNewManagedFiles) {
  const std::filesystem::path directory = NewProjectTestDirectory();
  auto library = SampleProjectLibrary(directory);
  const auto path = directory / L"cells" /
                    L"cccccccc-cccc-cccc-cccc-cccccccccccc" / L"views" /
                    L"dddddddd-dddd-dddd-dddd-dddddddddddd" / L"files" /
                    L"counter.sv";
  std::filesystem::create_directories(path.parent_path());
  std::ofstream(path) << "module counter; endmodule\n";
  application::ProjectService service;
  auto document = service.OpenOrCreate(library, library.library.cells[0].id);
  Assert::IsTrue(document.Ok());
  auto sources = service.ResolveSources(library, library.library.cells[0].id,
                                        document.Value().project);
  Assert::IsTrue(sources.Ok());
  Assert::AreEqual(static_cast<std::size_t>(1), sources.Value().size());
  Assert::IsTrue(sources.Value()[0].exists);
  std::error_code error;
  std::filesystem::remove_all(directory, error);
}
}
;

TEST_CLASS(RunStoreTests){
  public : TEST_METHOD(RunLifecyclePersistsStatusAndDiagnostics){
      const std::filesystem::path directory = NewProjectTestDirectory();
application::RunStore store;
auto begun =
    store.Begin(directory, SampleProject(), "Lint", "Verilator", "Verilator 5");
Assert::IsTrue(begun.Ok());
application::RunRecord run = std::move(begun).Value();
Assert::IsTrue(store.AppendLog(run, "%Error: test\n").Ok());
core::Diagnostic diagnostic{
    core::DiagnosticSeverity::kError, "WIDTH", "top.sv", 2, 3, "Mismatch"};
Assert::IsTrue(store
                   .Complete(&run, application::RunStatus::kFailed, 1,
                             {diagnostic})
                   .Ok());
auto runs = store.List(directory);
Assert::IsTrue(runs.Ok());
Assert::AreEqual(static_cast<std::size_t>(1), runs.Value().size());
Assert::IsTrue(runs.Value()[0].status == application::RunStatus::kFailed);
std::error_code error;
std::filesystem::remove_all(directory, error);
}

TEST_METHOD(WaveformArtifactRoundTripsInRunManifest) {
  const std::filesystem::path directory = NewProjectTestDirectory();
  application::RunStore store;
  auto begun = store.Begin(directory, SampleProject(), "Simulation",
                           "Icarus Verilog", "12.0");
  Assert::IsTrue(begun.Ok());
  application::RunRecord run = std::move(begun).Value();
  const std::filesystem::path waveform =
      run.directory / L"artifacts" / L"waveform.vcd";
  std::ofstream(waveform, std::ios::binary) << "$date test $end\n";
  Assert::IsTrue(store
                     .Complete(&run, application::RunStatus::kSucceeded, 0, {},
                               {{"waveform", "vcd", "artifacts/waveform.vcd",
                                 16, "input-hash", true}})
                     .Ok());
  auto runs = store.List(directory);
  Assert::IsTrue(runs.Ok());
  Assert::AreEqual(static_cast<std::size_t>(1),
                   runs.Value()[0].artifacts.size());
  Assert::AreEqual(std::string("artifacts/waveform.vcd"),
                   runs.Value()[0].artifacts[0].relative_path);
  Assert::IsTrue(runs.Value()[0].artifacts[0].partial);
  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

TEST_METHOD(RunOutcomeSeparatesProcessAndTestFailure) {
  const std::filesystem::path directory = NewProjectTestDirectory();
  application::RunStore store;
  auto begun =
      store.Begin(directory, SampleProject(), "Simulation", "cocotb", "2.0");
  Assert::IsTrue(begun.Ok());
  application::RunRecord run = std::move(begun).Value();
  const std::filesystem::path summary =
      run.directory / L"reports" / L"simulation-summary.json";
  std::ofstream(summary, std::ios::binary) << "{\"failed\":1}\n";
  application::RunOutcome outcome;
  outcome.process_succeeded = true;
  outcome.result_succeeded = false;
  outcome.summary_relative_path = "reports/simulation-summary.json";
  Assert::IsTrue(
      store.Complete(&run, application::RunStatus::kFailed, 0, {}, {}, outcome)
          .Ok());
  auto runs = store.List(directory);
  Assert::IsTrue(runs.Ok());
  Assert::IsTrue(runs.Value()[0].outcome.process_succeeded);
  Assert::IsFalse(runs.Value()[0].outcome.result_succeeded);
  Assert::AreEqual(std::string("reports/simulation-summary.json"),
                   runs.Value()[0].outcome.summary_relative_path);
  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

TEST_METHOD(SynthesisAndTimingPathsAreValidated) {
  core::Project project = SampleProject();
  project.synthesis.liberty_paths = {"../outside.lib"};
  Assert::IsTrue(core::ValidateProject(project).code ==
                 core::ErrorCode::kCorruptData);
  project = SampleProject();
  project.timing.sdc_path = "../outside.sdc";
  Assert::IsTrue(core::ValidateProject(project).code ==
                 core::ErrorCode::kCorruptData);
}
}
;

}  // namespace designpp::tests

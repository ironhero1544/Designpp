// Copyright 2026 The Design++ Authors

#include <CppUnitTest.h>
#include <combaseapi.h>
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "designpp/application/layout_setup_draft.h"
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
  project.physical_implementation.pdk = "sky130A";
  project.physical_implementation.standard_cell_library = "sky130_fd_sc_hd";
  project.physical_implementation.clock_ports = {"clk"};
  project.physical_implementation.clock_period_ns = "12.5";
  project.physical_implementation.core_utilization_percent = 45;
  project.physical_implementation.placement_density_percent = "55.25";
  project.physical_implementation.die_area = {"0", "0", "200.5", "180"};
  project.physical_implementation.core_area = {"10", "10", "190", "170"};
  project.physical_implementation.tap_cell_distance_um = "14.0";
  project.physical_implementation.pnr_sdc_path =
      "cells/c/views/constraints/files/top.sdc";
  project.physical_implementation.power_distribution.vertical_pitch_um = "40.0";
  project.physical_implementation.power_distribution.horizontal_offset_um =
      "4.0";
  auto& io = project.physical_implementation.io_placement;
  io.algorithm = "annealing";
  io.minimum_distance_um = "1.5";
  io.vertical_length_um = "2.0";
  io.horizontal_thickness_multiplier = "3.0";
  io.vertical_layer = "met3";
  io.unmatched_policy = "both";
  io.north.minimum_distance_um = "2.5";
  io.north.bit_major = true;
  io.north.entries = {"data\\[\\d+\\]", "$2"};
  io.east.entries = {"clk", "rst_n"};
  project.physical_verification.drc_recipe_id =
      "builtin.orfs.sky130hd.klayout-drc";
  project.physical_verification.lvs_recipe_id =
      "builtin.orfs.sky130hd.klayout-lvs";
  project.physical_verification.top_cell = "counter";
  project.physical_verification.power_net = "VDD";
  project.physical_verification.ground_net = "VSS";
  project.physical_verification.parameters_json = "{\"density\":4}";
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

TEST_METHOD(LayoutBlankFieldsRemoveOverrides) {
  application::LayoutSetupDraft draft;
  draft.utilization = " \t";
  draft.values.pdk = " ";
  draft.values.clock_ports = {"clk"};
  draft.values.clock_period_ns = "";
  draft.values.placement_density_percent = " ";
  draft.values.tap_cell_distance_um = "";
  draft.values.power_distribution.vertical_width_um = "\t";
  draft.values.io_placement.vertical_layer = " ";
  auto parsed = application::ValidateLayoutSetupDraft(draft);
  Assert::IsTrue(parsed.Ok());
  Assert::AreEqual(std::uint32_t(40), parsed.Value().core_utilization_percent);
  Assert::AreEqual(std::string("10.0"), parsed.Value().clock_period_ns);
  Assert::IsFalse(parsed.Value().placement_density_percent.has_value());
  Assert::IsFalse(
      parsed.Value().power_distribution.vertical_width_um.has_value());
  Assert::IsFalse(parsed.Value().io_placement.vertical_layer.has_value());
  auto restored = application::MakeLayoutSetupDraft(parsed.Value());
  Assert::IsTrue(restored.utilization.empty());
  Assert::IsTrue(restored.values.clock_period_ns.empty());
}

TEST_METHOD(LayoutInvalidNumbersNeverBecomeAutomatic) {
  for (const auto* text : {"40oops", "-1", "100", "1.5", "99999999999999"}) {
    application::LayoutSetupDraft draft;
    draft.utilization = text;
    Assert::IsFalse(application::ValidateLayoutSetupDraft(draft).Ok());
  }
  application::LayoutSetupDraft draft;
  draft.values.clock_ports = {"clk"};
  draft.values.clock_period_ns = "10oops";
  Assert::IsFalse(application::ValidateLayoutSetupDraft(draft).Ok());
}

TEST_METHOD(LayoutOrfsPlatformSelectionBecomesAnExplicitValue) {
  application::LayoutSetupDraft draft;
  draft.values.backend_id = "orfs";
  draft.values.orfs.platform = "  sky130hd  ";
  draft.values.automatic_fields = {"orfs.platform"};
  auto parsed = application::ValidateLayoutSetupDraft(draft);
  Assert::IsTrue(parsed.Ok());
  Assert::AreEqual(std::string("sky130hd"), parsed.Value().orfs.platform);
  Assert::IsFalse(core::UsesAutomaticValue(parsed.Value(), "orfs.platform"));
}

TEST_METHOD(LayoutAutomaticStateSurvivesSchemaElevenRoundTrip) {
  application::LayoutSetupDraft draft;
  draft.utilization = "";
  auto parsed = application::ValidateLayoutSetupDraft(draft);
  Assert::IsTrue(parsed.Ok());
  auto project = SampleProject();
  project.physical_implementation = parsed.Value();
  const auto directory = NewProjectTestDirectory();
  application::ProjectStore store;
  Assert::IsTrue(store.Save(project, directory / L"project.dpproj", 0).Ok());
  auto loaded = store.Load(directory / L"project.dpproj");
  Assert::IsTrue(loaded.Ok());
  Assert::IsTrue(core::UsesAutomaticValue(
      loaded.Value().physical_implementation, "core_utilization_percent"));
  std::filesystem::remove_all(directory);
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
Assert::AreEqual(std::string("sky130A"),
                 loaded.Value().physical_implementation.pdk);
Assert::AreEqual(std::string("12.5"),
                 loaded.Value().physical_implementation.clock_period_ns);
Assert::AreEqual(
    static_cast<std::uint32_t>(45),
    loaded.Value().physical_implementation.core_utilization_percent);
Assert::AreEqual(
    std::string("55.25"),
    *loaded.Value().physical_implementation.placement_density_percent);
Assert::AreEqual(static_cast<std::size_t>(4),
                 loaded.Value().physical_implementation.die_area.size());
Assert::AreEqual(static_cast<std::size_t>(4),
                 loaded.Value().physical_implementation.core_area.size());
Assert::AreEqual(std::string("14.0"),
                 *loaded.Value().physical_implementation.tap_cell_distance_um);
Assert::AreEqual(
    std::string("40.0"),
    *loaded.Value()
         .physical_implementation.power_distribution.vertical_pitch_um);
Assert::AreEqual(
    std::string("4.0"),
    *loaded.Value()
         .physical_implementation.power_distribution.horizontal_offset_um);
const auto& io = loaded.Value().physical_implementation.io_placement;
Assert::AreEqual(std::string("annealing"), io.algorithm);
Assert::AreEqual(std::string("1.5"), *io.minimum_distance_um);
Assert::AreEqual(std::string("met3"), *io.vertical_layer);
Assert::IsTrue(io.north.bit_major);
Assert::AreEqual(static_cast<std::size_t>(2), io.north.entries.size());
Assert::AreEqual(std::string("$2"), io.north.entries[1]);
Assert::AreEqual(project.physical_verification.drc_recipe_id,
                 loaded.Value().physical_verification.drc_recipe_id);
Assert::AreEqual(std::string("counter"),
                 loaded.Value().physical_verification.top_cell);
Assert::AreEqual(std::string("{\"density\":4}"),
                 loaded.Value().physical_verification.parameters_json);
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
  const std::wstring load_error(loaded.GetStatus().message.begin(),
                                loaded.GetStatus().message.end());
  Assert::IsTrue(loaded.Ok(), load_error.c_str());
  Assert::AreEqual(static_cast<std::uint64_t>(1), loaded.Value().revision);
  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

TEST_METHOD(SchemaFiveOpenLaneMigratesWithoutOverwritingSource) {
  const std::filesystem::path directory = NewProjectTestDirectory();
  const std::filesystem::path path = directory / L"project.dpproj";
  application::ProjectStore store;
  Assert::IsTrue(store.Save(SampleProject(), path, 0).Ok());
  std::ifstream encoded_input(path, std::ios::binary);
  std::string schema_five((std::istreambuf_iterator<char>(encoded_input)),
                          std::istreambuf_iterator<char>());
  const std::size_t schema = schema_five.find("\"schema_version\": 12");
  const std::size_t configuration =
      schema_five.find("\"physical_implementation\"");
  Assert::IsTrue(schema != std::string::npos);
  Assert::IsTrue(configuration != std::string::npos);
  schema_five.replace(configuration,
                      std::string("\"physical_implementation\"").size(),
                      "\"openlane\"");
  schema_five.replace(schema, std::string("\"schema_version\": 12").size(),
                      "\"schema_version\": 5");
  std::ofstream(path, std::ios::binary | std::ios::trunc) << schema_five;

  auto loaded = store.Load(path);
  const std::wstring load_error(loaded.GetStatus().message.begin(),
                                loaded.GetStatus().message.end());
  Assert::IsTrue(loaded.Ok(), load_error.c_str());
  Assert::AreEqual(core::Project::kSchemaVersion,
                   loaded.Value().schema_version);
  Assert::AreEqual(std::string("openlane2"),
                   loaded.Value().physical_implementation.backend_id);
  Assert::AreEqual(std::string("sky130A"),
                   loaded.Value().physical_implementation.pdk);
  std::ifstream unchanged_input(path, std::ios::binary);
  const std::string unchanged((std::istreambuf_iterator<char>(unchanged_input)),
                              std::istreambuf_iterator<char>());
  Assert::AreEqual(schema_five, unchanged);
  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

TEST_METHOD(SchemaSixMigratesPdnDefaultsWithoutOverwritingSource) {
  const std::filesystem::path directory = NewProjectTestDirectory();
  const std::filesystem::path path = directory / L"project.dpproj";
  application::ProjectStore store;
  Assert::IsTrue(store.Save(SampleProject(), path, 0).Ok());
  std::ifstream encoded_input(path, std::ios::binary);
  std::string schema_six((std::istreambuf_iterator<char>(encoded_input)),
                         std::istreambuf_iterator<char>());
  const std::size_t schema = schema_six.find("\"schema_version\": 12");
  Assert::IsTrue(schema != std::string::npos);
  schema_six.replace(schema, std::string("\"schema_version\": 12").size(),
                     "\"schema_version\": 6");
  std::ofstream(path, std::ios::binary | std::ios::trunc) << schema_six;

  auto loaded = store.Load(path);
  Assert::IsTrue(loaded.Ok());
  Assert::AreEqual(core::Project::kSchemaVersion,
                   loaded.Value().schema_version);
  Assert::IsTrue(
      loaded.Value().physical_implementation.power_distribution.multilayer);
  Assert::IsFalse(
      loaded.Value().physical_implementation.power_distribution.core_ring);
  Assert::IsFalse(loaded.Value()
                      .physical_implementation.power_distribution
                      .vertical_pitch_um.has_value());
  std::ifstream unchanged_input(path, std::ios::binary);
  const std::string unchanged((std::istreambuf_iterator<char>(unchanged_input)),
                              std::istreambuf_iterator<char>());
  Assert::AreEqual(schema_six, unchanged);
  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

TEST_METHOD(SchemaSevenMigratesTapCellDefaultWithoutOverwritingSource) {
  const std::filesystem::path directory = NewProjectTestDirectory();
  const std::filesystem::path path = directory / L"project.dpproj";
  application::ProjectStore store;
  Assert::IsTrue(store.Save(SampleProject(), path, 0).Ok());
  std::ifstream encoded_input(path, std::ios::binary);
  std::string schema_seven((std::istreambuf_iterator<char>(encoded_input)),
                           std::istreambuf_iterator<char>());
  const std::size_t schema = schema_seven.find("\"schema_version\": 12");
  const std::string tap_field = "\"tap_cell_distance_um\": \"14.0\", ";
  const std::size_t tap = schema_seven.find(tap_field);
  Assert::IsTrue(schema != std::string::npos);
  Assert::IsTrue(tap != std::string::npos);
  schema_seven.erase(tap, tap_field.size());
  schema_seven.replace(schema, std::string("\"schema_version\": 12").size(),
                       "\"schema_version\": 7");
  std::ofstream(path, std::ios::binary | std::ios::trunc) << schema_seven;

  auto loaded = store.Load(path);
  Assert::IsTrue(loaded.Ok());
  Assert::AreEqual(core::Project::kSchemaVersion,
                   loaded.Value().schema_version);
  Assert::IsFalse(
      loaded.Value().physical_implementation.tap_cell_distance_um.has_value());
  std::ifstream unchanged_input(path, std::ios::binary);
  const std::string unchanged((std::istreambuf_iterator<char>(unchanged_input)),
                              std::istreambuf_iterator<char>());
  Assert::AreEqual(schema_seven, unchanged);
  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

TEST_METHOD(SchemaEightMigratesIoPlacementDefaultsWithoutOverwritingSource) {
  const std::filesystem::path directory = NewProjectTestDirectory();
  const std::filesystem::path path = directory / L"project.dpproj";
  application::ProjectStore store;
  Assert::IsTrue(store.Save(SampleProject(), path, 0).Ok());
  std::ifstream encoded_input(path, std::ios::binary);
  std::string schema_eight((std::istreambuf_iterator<char>(encoded_input)),
                           std::istreambuf_iterator<char>());
  const std::size_t schema = schema_eight.find("\"schema_version\": 12");
  const std::size_t io = schema_eight.find("\"io_placement\"");
  Assert::IsTrue(schema != std::string::npos);
  Assert::IsTrue(io != std::string::npos);
  schema_eight.replace(io, std::string("\"io_placement\"").size(),
                       "\"legacy_io_placement\"");
  schema_eight.replace(schema, std::string("\"schema_version\": 12").size(),
                       "\"schema_version\": 8");
  std::ofstream(path, std::ios::binary | std::ios::trunc) << schema_eight;

  auto loaded = store.Load(path);
  const std::wstring load_error(loaded.GetStatus().message.begin(),
                                loaded.GetStatus().message.end());
  Assert::IsTrue(loaded.Ok(), load_error.c_str());
  Assert::AreEqual(core::Project::kSchemaVersion,
                   loaded.Value().schema_version);
  const auto& migrated = loaded.Value().physical_implementation.io_placement;
  Assert::AreEqual(std::string("matching"), migrated.algorithm);
  Assert::AreEqual(std::string("both"), migrated.unmatched_policy);
  Assert::IsTrue(migrated.north.entries.empty());
  std::ifstream unchanged_input(path, std::ios::binary);
  const std::string unchanged((std::istreambuf_iterator<char>(unchanged_input)),
                              std::istreambuf_iterator<char>());
  Assert::AreEqual(schema_eight, unchanged);
  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

TEST_METHOD(SchemaNineMigratesBackendConfigurationWithoutOverwritingSource) {
  const std::filesystem::path directory = NewProjectTestDirectory();
  const std::filesystem::path path = directory / L"project.dpproj";
  application::ProjectStore store;
  core::Project project = SampleProject();
  project.physical_implementation.orfs.platform = "nangate45";
  project.physical_implementation.orfs.flow_variant = "base";
  project.physical_implementation.orfs.advanced_variables_json =
      "{\"CLOCK_PERIOD\":12}";
  Assert::IsTrue(store.Save(project, path, 0).Ok());
  std::ifstream encoded_input(path, std::ios::binary);
  std::string schema_nine((std::istreambuf_iterator<char>(encoded_input)),
                          std::istreambuf_iterator<char>());
  const std::size_t schema = schema_nine.find("\"schema_version\": 12");
  Assert::IsTrue(schema != std::string::npos);
  schema_nine.replace(schema, std::string("\"schema_version\": 12").size(),
                      "\"schema_version\": 9");
  std::ofstream(path, std::ios::binary | std::ios::trunc) << schema_nine;

  auto loaded = store.Load(path);
  Assert::IsTrue(loaded.Ok());
  Assert::AreEqual(core::Project::kSchemaVersion,
                   loaded.Value().schema_version);
  Assert::AreEqual(std::string("openlane2"),
                   loaded.Value().physical_implementation.backend_id);
  Assert::AreEqual(std::string("sky130hd"),
                   loaded.Value().physical_implementation.orfs.platform);
  Assert::AreEqual(std::string("base"),
                   loaded.Value().physical_implementation.orfs.flow_variant);
  Assert::AreEqual(
      std::string("{}"),
      loaded.Value().physical_implementation.orfs.advanced_variables_json);
  std::ifstream unchanged_input(path, std::ios::binary);
  const std::string unchanged((std::istreambuf_iterator<char>(unchanged_input)),
                              std::istreambuf_iterator<char>());
  Assert::AreEqual(schema_nine, unchanged);
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

TEST_METHOD(OpenLaneRejectsUnsafeSdcAndInvalidPhysicalValues) {
  core::Project project = SampleProject();
  project.physical_implementation.pnr_sdc_path = "../outside.sdc";
  Assert::IsFalse(core::ValidateProject(project).Ok());
  project = SampleProject();
  project.physical_implementation.clock_period_ns = "zero";
  Assert::IsFalse(core::ValidateProject(project).Ok());
  project = SampleProject();
  project.physical_implementation.die_area = {"0", "0", "100"};
  Assert::IsFalse(core::ValidateProject(project).Ok());
  project = SampleProject();
  project.physical_implementation.core_area = {"-1", "10", "90", "90"};
  Assert::IsFalse(core::ValidateProject(project).Ok());
  project = SampleProject();
  project.physical_implementation.die_area.clear();
  Assert::IsFalse(core::ValidateProject(project).Ok());
  project = SampleProject();
  project.physical_implementation.power_distribution.vertical_pitch_um = "0";
  Assert::IsFalse(core::ValidateProject(project).Ok());
  project = SampleProject();
  project.physical_implementation.tap_cell_distance_um = "0";
  Assert::IsFalse(core::ValidateProject(project).Ok());
  project = SampleProject();
  project.physical_implementation.io_placement.algorithm = "unsupported";
  Assert::IsFalse(core::ValidateProject(project).Ok());
  project = SampleProject();
  project.physical_implementation.io_placement.minimum_distance_um = "0";
  Assert::IsFalse(core::ValidateProject(project).Ok());
  project = SampleProject();
  project.physical_implementation.io_placement.north.entries = {"#N"};
  Assert::IsFalse(core::ValidateProject(project).Ok());
  project = SampleProject();
  project.physical_implementation.io_placement.north.entries = {"$0"};
  Assert::IsFalse(core::ValidateProject(project).Ok());
}

TEST_METHOD(PhysicalImplementationSetupCanBeValidatedWithoutProjectIdentity) {
  core::PhysicalImplementationConfiguration configuration;
  configuration.die_area = {"0", "0", "100", "100"};
  Assert::IsTrue(
      core::ValidatePhysicalImplementationConfiguration(configuration).Ok());
  configuration.die_area = {"0", "0", "7.36"};
  Assert::IsFalse(
      core::ValidatePhysicalImplementationConfiguration(configuration).Ok());
}

TEST_METHOD(OrfsConfigurationDoesNotRequireOpenLaneTechnologyFields) {
  core::PhysicalImplementationConfiguration configuration;
  configuration.backend_id = "orfs";
  configuration.pdk.clear();
  configuration.standard_cell_library.clear();
  configuration.orfs.platform = "sky130hd";
  configuration.orfs.flow_variant = "base";
  Assert::IsTrue(
      core::ValidatePhysicalImplementationConfiguration(configuration).Ok());
}

TEST_METHOD(CombinationalPhysicalImplementationDoesNotRequireClockPeriod) {
  core::PhysicalImplementationConfiguration configuration;
  configuration.clock_ports.clear();
  configuration.clock_period_ns.clear();
  Assert::IsTrue(
      core::ValidatePhysicalImplementationConfiguration(configuration).Ok());
  configuration.clock_ports = {"clk"};
  Assert::IsFalse(
      core::ValidatePhysicalImplementationConfiguration(configuration).Ok());
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
  const auto liberty = directory / L"files" / L"standard.lib";
  std::filesystem::create_directories(liberty.parent_path());
  std::ofstream(liberty) << "library(test) {}\n";
  library.library.files.push_back(
      {"files/standard.lib", "support", 17, "2026-08-10T00:00:00Z"});
  application::ProjectService service;
  auto document = service.OpenOrCreate(library, library.library.cells[0].id);
  Assert::IsTrue(document.Ok());
  auto sources = service.ResolveSources(library, library.library.cells[0].id,
                                        document.Value().project);
  Assert::IsTrue(sources.Ok());
  Assert::AreEqual(static_cast<std::size_t>(2), sources.Value().size());
  Assert::AreEqual(std::string("files/standard.lib"),
                   sources.Value()[0].relative_path);
  Assert::IsTrue(sources.Value()[0].exists);
  Assert::IsTrue(sources.Value()[1].exists);
  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

TEST_METHOD(AsyncProjectSnapshotSaveIsCellScoped) {
  const std::filesystem::path directory = NewProjectTestDirectory();
  auto library = SampleProjectLibrary(directory);
  library.library.cells.emplace_back("eeeeeeee-eeee-eeee-eeee-eeeeeeeeeeee",
                                     "other", "", std::vector<core::View>{});
  application::ProjectService service;
  auto first_result =
      service.OpenOrCreate(library, library.library.cells[0].id);
  auto second_result =
      service.OpenOrCreate(library, library.library.cells[1].id);
  Assert::IsTrue(first_result.Ok());
  Assert::IsTrue(second_result.Ok());
  application::ProjectDocument first = std::move(first_result).Value();
  application::ProjectDocument second = std::move(second_result).Value();
  const core::Project original_first = first.project;
  core::Project first_snapshot = first.project;
  first_snapshot.physical_implementation.pdk = "cell_specific_pdk";
  auto saved = service.Save(&first, std::move(first_snapshot));
  Assert::IsTrue(saved.Ok());
  Assert::AreEqual(original_first.physical_implementation.pdk,
                   first.project.physical_implementation.pdk);
  Assert::AreNotEqual(first.project_path.wstring(),
                      second.project_path.wstring());
  Assert::AreEqual(std::string("cell_specific_pdk"),
                   saved.Value().physical_implementation.pdk);
  Assert::AreNotEqual(saved.Value().physical_implementation.pdk,
                      second.project.physical_implementation.pdk);
  application::ProjectStore store;
  auto reloaded_first = store.Load(first.project_path);
  auto reloaded_second = store.Load(second.project_path);
  Assert::IsTrue(reloaded_first.Ok());
  Assert::IsTrue(reloaded_second.Ok());
  Assert::AreEqual(std::string("cell_specific_pdk"),
                   reloaded_first.Value().physical_implementation.pdk);
  Assert::AreNotEqual(reloaded_first.Value().physical_implementation.pdk,
                      reloaded_second.Value().physical_implementation.pdk);
  Assert::AreEqual(first.project.cell_id, reloaded_first.Value().cell_id);
  Assert::AreEqual(second.project.cell_id, reloaded_second.Value().cell_id);
  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

TEST_METHOD(SaveRejectsAProjectPathOwnedByAnotherCell) {
  const std::filesystem::path directory = NewProjectTestDirectory();
  auto library = SampleProjectLibrary(directory);
  library.library.cells.emplace_back("eeeeeeee-eeee-eeee-eeee-eeeeeeeeeeee",
                                     "other", "", std::vector<core::View>{});
  application::ProjectService service;
  auto first_result =
      service.OpenOrCreate(library, library.library.cells[0].id);
  Assert::IsTrue(first_result.Ok());
  application::ProjectDocument first = std::move(first_result).Value();
  const std::filesystem::path first_path = first.project_path;
  first.project_path = directory / L"cells" /
                       L"eeeeeeee-eeee-eeee-eeee-eeeeeeeeeeee" /
                       L"project.dpproj";
  core::Project snapshot = first.project;
  snapshot.physical_implementation.pdk = "other_cell_pdk";
  auto saved = service.Save(&first, std::move(snapshot));
  Assert::IsFalse(saved.Ok());
  Assert::IsTrue(saved.GetStatus().code == core::ErrorCode::kConflict);
  Assert::AreEqual(first_path.wstring(),
                   (directory / L"cells" /
                    L"cccccccc-cccc-cccc-cccc-cccccccccccc" / L"project.dpproj")
                       .wstring());
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

TEST_METHOD(PhysicalVerificationEvidenceRestoresAfterRestart) {
  const std::filesystem::path directory = NewProjectTestDirectory();
  application::RunStore store;
  auto begun = store.Begin(directory, SampleProject(), "physical_verification",
                           "KLayout DRC", "0.30.2");
  Assert::IsTrue(begun.Ok());
  application::RunRecord run = std::move(begun).Value();
  run.source_run_id = "source-layout-run";
  run.environment_id = "orfs-environment";
  run.environment_fingerprint = "environment-sha256";
  Assert::IsTrue(store.AppendLog(run, "KLayout raw output\n").Ok());

  const std::filesystem::path report =
      run.directory / L"reports" / L"drc.lyrdb";
  const std::filesystem::path summary =
      run.directory / L"reports" / L"physical-verification-summary.json";
  const std::filesystem::path inputs =
      run.directory / L"reports" / L"verification-inputs.json";
  std::ofstream(report, std::ios::binary) << "<report-database/>\n";
  std::ofstream(summary, std::ios::binary)
      << "{\"passed\":true,\"violation_count\":0}\n";
  std::ofstream(inputs, std::ios::binary)
      << "{\"source_run_id\":\"source-layout-run\"}\n";
  application::RunOutcome outcome{true, true,
                                  "reports/physical-verification-summary.json"};
  Assert::IsTrue(
      store
          .Complete(
              &run, application::RunStatus::kSucceeded, 0, {},
              {{"verification.report", "lyrdb", "reports/drc.lyrdb", 20,
                "recipe-sha256", false},
               {"verification.summary", "json",
                "reports/physical-verification-summary.json", 39,
                "recipe-sha256", false},
               {"verification.inputs", "json",
                "reports/verification-inputs.json", 38, "input-sha256", false}},
              outcome)
          .Ok());

  application::RunStore restarted_store;
  auto restored = restarted_store.List(directory);
  Assert::IsTrue(restored.Ok());
  Assert::AreEqual<std::size_t>(1, restored.Value().size());
  const auto& saved = restored.Value().front();
  Assert::IsTrue(saved.status == application::RunStatus::kSucceeded);
  Assert::AreEqual(std::string("source-layout-run"), saved.source_run_id);
  Assert::AreEqual(std::string("orfs-environment"), saved.environment_id);
  Assert::AreEqual(std::string("environment-sha256"),
                   saved.environment_fingerprint);
  Assert::IsTrue(saved.outcome.process_succeeded);
  Assert::IsTrue(saved.outcome.result_succeeded);
  Assert::AreEqual<std::size_t>(3, saved.artifacts.size());
  Assert::AreEqual(std::string("input-sha256"), saved.artifacts[2].input_hash);
  std::ifstream raw(saved.directory / L"logs" / L"raw.log", std::ios::binary);
  Assert::AreEqual(std::string("KLayout raw output\n"),
                   std::string((std::istreambuf_iterator<char>(raw)),
                               std::istreambuf_iterator<char>()));
  Assert::IsTrue(std::filesystem::is_regular_file(
      saved.directory / saved.outcome.summary_relative_path));
  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

TEST_METHOD(RecoverInterruptedPersistsAbandonedActiveRuns) {
  const std::filesystem::path directory = NewProjectTestDirectory();
  application::RunStore store;
  auto begun = store.Begin(directory, SampleProject(),
                           "physical_implementation", "openlane2", "2.3.10");
  Assert::IsTrue(begun.Ok());

  Assert::IsTrue(store.RecoverInterrupted(directory).Ok());
  auto runs = store.List(directory);
  Assert::IsTrue(runs.Ok());
  Assert::AreEqual(static_cast<std::size_t>(1), runs.Value().size());
  Assert::IsTrue(runs.Value()[0].status ==
                 application::RunStatus::kInterrupted);
  Assert::IsFalse(runs.Value()[0].finished_utc.empty());

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

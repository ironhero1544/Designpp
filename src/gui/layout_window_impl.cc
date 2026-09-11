// Copyright 2026 The Design++ Authors

#include "designpp/gui/layout_window_impl.h"

#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <utility>

#include "designpp/adapters/orfs_adapter.h"
#include "designpp/application/synthesis_fingerprint.h"
#include "designpp/application/toolchain_environment_service.h"
#include "designpp/application/toolchain_profile_store.h"
#include "designpp/runtime/process_runner.h"

namespace designpp::gui {
namespace {

constexpr wchar_t kWindowClassName[] = L"DesignPlusPlus.LayoutWindow";
constexpr int kRunButton = 7601;
constexpr int kCancelButton = 7602;
constexpr int kResumeButton = 7603;
constexpr int kConfigButton = 7604;
constexpr int kReportsButton = 7605;
constexpr int kArtifactsButton = 7606;
constexpr int kPdkCombo = 7607;
constexpr int kSclCombo = 7608;
constexpr int kStagesButton = 7609;
constexpr int kRunsList = 7610;
constexpr int kPageTabs = 7611;
constexpr int kDrcButton = 7612;
constexpr int kLvsButton = 7613;
constexpr int kVerifyAllButton = 7614;
constexpr int kVerificationCancelButton = 7615;
constexpr int kInitialWindowWidth = 1420;
constexpr int kInitialWindowHeight = 900;
constexpr int kMinimumWindowWidth = 1040;
constexpr int kMinimumWindowHeight = 680;

std::wstring Utf8ToWide(std::string_view text) {
  if (text.empty()) return {};
  const int length =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), nullptr, 0);
  if (length <= 0) return L"[invalid UTF-8]";
  std::wstring result(static_cast<std::size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                      static_cast<int>(text.size()), result.data(), length);
  return result;
}

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

std::wstring ControlText(HWND control) {
  const int length = GetWindowTextLengthW(control);
  std::wstring value(static_cast<std::size_t>(std::max(length, 0)) + 1, L'\0');
  GetWindowTextW(control, value.data(), static_cast<int>(value.size()));
  value.resize(std::wcslen(value.c_str()));
  return value;
}

void SetControlText(HWND control, std::string_view text) {
  const std::wstring wide = Utf8ToWide(text);
  SetWindowTextW(control, wide.c_str());
}

std::vector<std::string> Split(std::string_view value, char delimiter) {
  std::vector<std::string> result;
  std::size_t begin = 0;
  while (begin <= value.size()) {
    const std::size_t end = value.find(delimiter, begin);
    std::string item(value.substr(begin, end - begin));
    const std::size_t first = item.find_first_not_of(" \t\r\n");
    const std::size_t last = item.find_last_not_of(" \t\r\n");
    if (first != std::string::npos) {
      result.push_back(item.substr(first, last - first + 1));
    }
    if (end == std::string_view::npos) break;
    begin = end + 1;
  }
  return result;
}

std::string Join(const std::vector<std::string>& values,
                 std::string_view delimiter) {
  std::ostringstream output;
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (index != 0) output << delimiter;
    output << values[index];
  }
  return output.str();
}

std::optional<std::string> JsonString(std::string_view json,
                                      std::string_view key) {
  const std::string token = "\"" + std::string(key) + "\"";
  const std::size_t key_position = json.find(token);
  if (key_position == std::string_view::npos) return std::nullopt;
  const std::size_t colon = json.find(':', key_position + token.size());
  const std::size_t quote = json.find('"', colon + 1);
  if (colon == std::string_view::npos || quote == std::string_view::npos) {
    return std::nullopt;
  }
  std::string result;
  bool escaped = false;
  for (std::size_t index = quote + 1; index < json.size(); ++index) {
    const char character = json[index];
    if (escaped) {
      result.push_back(character);
      escaped = false;
    } else if (character == '\\') {
      escaped = true;
    } else if (character == '"') {
      return result;
    } else {
      result.push_back(character);
    }
  }
  return std::nullopt;
}

std::optional<double> JsonNumber(std::string_view json, std::string_view key) {
  const std::string token = "\"" + std::string(key) + "\"";
  const std::size_t key_position = json.find(token);
  const std::size_t colon = key_position == std::string_view::npos
                                ? std::string_view::npos
                                : json.find(':', key_position + token.size());
  if (colon == std::string_view::npos) return std::nullopt;
  const char* begin = json.data() + colon + 1;
  char* end = nullptr;
  const double value = std::strtod(begin, &end);
  return end != begin ? std::optional<double>{value} : std::nullopt;
}

std::optional<bool> JsonBool(std::string_view json, std::string_view key) {
  const std::string token = "\"" + std::string(key) + "\"";
  const std::size_t key_position = json.find(token);
  const std::size_t colon = key_position == std::string_view::npos
                                ? std::string_view::npos
                                : json.find(':', key_position + token.size());
  if (colon == std::string_view::npos) return std::nullopt;
  const std::string_view value = json.substr(colon + 1);
  const std::size_t first = value.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) return std::nullopt;
  if (value.substr(first, 4) == "true") return true;
  if (value.substr(first, 5) == "false") return false;
  return std::nullopt;
}

std::optional<std::string> RunSummaryString(const application::RunRecord& run,
                                            std::string_view key) {
  if (run.outcome.summary_relative_path.empty()) return std::nullopt;
  std::ifstream input(run.directory / run.outcome.summary_relative_path,
                      std::ios::binary);
  if (!input) return std::nullopt;
  std::ostringstream output;
  output << input.rdbuf();
  return JsonString(output.str(), key);
}

std::wstring BackendDisplayName(std::string_view backend_id) {
  if (backend_id == "orfs") return L"OpenROAD Flow Scripts";
  if (backend_id == "openlane2") return L"OpenLane 2";
  return Utf8ToWide(backend_id);
}

bool IsActiveFlowState(application::ManagedFlowRunState state) {
  return state == application::ManagedFlowRunState::kProbing ||
         state == application::ManagedFlowRunState::kPreparing ||
         state == application::ManagedFlowRunState::kValidating ||
         state == application::ManagedFlowRunState::kRunning ||
         state == application::ManagedFlowRunState::kCollecting ||
         state == application::ManagedFlowRunState::kCancelling;
}

std::wstring_view FlowStateDisplayName(application::ManagedFlowRunState state) {
  switch (state) {
    case application::ManagedFlowRunState::kProbing:
      return L"Checking backend capability";
    case application::ManagedFlowRunState::kPreparing:
      return L"Preparing inputs";
    case application::ManagedFlowRunState::kValidating:
      return L"Validating configuration";
    case application::ManagedFlowRunState::kRunning:
      return L"Running";
    case application::ManagedFlowRunState::kCollecting:
      return L"Collecting artifacts";
    case application::ManagedFlowRunState::kCancelling:
      return L"Cancelling";
    default:
      return L"Idle";
  }
}

adapters::ManagedFlowMetrics LoadRunMetrics(const application::RunRecord& run) {
  adapters::ManagedFlowMetrics metrics;
  if (run.outcome.summary_relative_path.empty()) return metrics;
  std::ifstream input(run.directory / run.outcome.summary_relative_path,
                      std::ios::binary);
  if (!input) return metrics;
  std::ostringstream output;
  output << input.rdbuf();
  const std::string json = output.str();
  metrics.core_area = JsonNumber(json, "core_area");
  metrics.die_area = JsonNumber(json, "die_area");
  metrics.utilization = JsonNumber(json, "utilization");
  metrics.instance_count = JsonNumber(json, "instance_count");
  metrics.setup_wns = JsonNumber(json, "setup_wns");
  metrics.setup_tns = JsonNumber(json, "setup_tns");
  metrics.hold_wns = JsonNumber(json, "hold_wns");
  metrics.hold_tns = JsonNumber(json, "hold_tns");
  metrics.drc_violations = JsonNumber(json, "drc_violations");
  metrics.lvs_errors = JsonNumber(json, "lvs_errors");
  metrics.global_route_congestion = JsonNumber(json, "global_route_congestion");
  metrics.detailed_route_congestion =
      JsonNumber(json, "detailed_route_congestion");
  metrics.global_route_overflow = JsonNumber(json, "global_route_overflow");
  metrics.detailed_route_overflow = JsonNumber(json, "detailed_route_overflow");
  metrics.routing_violations = JsonNumber(json, "routing_violations");
  metrics.runtime_seconds = JsonNumber(json, "runtime_seconds");
  metrics.peak_memory_mb = JsonNumber(json, "peak_memory_mb");
  return metrics;
}

std::optional<application::ManagedFlowResumeRequest> LoadResume(
    const application::RunRecord& run) {
  const auto backend = [&run]() -> std::optional<std::string> {
    if (run.outcome.summary_relative_path.empty()) return std::nullopt;
    std::ifstream input(run.directory / run.outcome.summary_relative_path,
                        std::ios::binary);
    if (!input) return std::nullopt;
    std::ostringstream output;
    output << input.rdbuf();
    return JsonString(output.str(), "backend_id");
  }();
  const bool is_orfs_success = backend && *backend == "orfs" &&
                               run.status == application::RunStatus::kSucceeded;
  if ((run.stage != "physical_implementation" &&
       run.stage != "physical_design") ||
      (!is_orfs_success && run.status != application::RunStatus::kFailed &&
       run.status != application::RunStatus::kInterrupted) ||
      run.outcome.summary_relative_path.empty()) {
    return std::nullopt;
  }
  std::ifstream input(run.directory / run.outcome.summary_relative_path,
                      std::ios::binary);
  if (!input) return std::nullopt;
  std::ostringstream output;
  output << input.rdbuf();
  const std::string json = output.str();
  auto lineage = JsonString(json, "lineage_id");
  auto step = JsonString(json, "last_step");
  auto fingerprint = JsonString(json, "configuration_fingerprint");
  const auto backend_id = JsonString(json, "backend_id");
  if ((!step || step->empty()) && backend_id && *backend_id == "orfs") {
    step = JsonString(json, "target_stage");
  }
  if (!lineage || lineage->empty() || !step || step->empty() || !fingerprint ||
      fingerprint->empty()) {
    return std::nullopt;
  }
  application::ManagedFlowResumeRequest resume;
  resume.parent_run_id = run.id;
  resume.lineage_id = std::move(*lineage);
  resume.resume_step = std::move(*step);
  resume.configuration_fingerprint = std::move(*fingerprint);
  std::filesystem::path checkpoint =
      run.directory / "artifacts" / "checkpoint-state.json";
  if (backend_id && *backend_id == "orfs") {
    std::filesystem::file_time_type newest_time{};
    checkpoint.clear();
    for (const application::RunArtifact& artifact : run.artifacts) {
      if (artifact.partial ||
          (artifact.format != "odb" && artifact.format != ".odb" &&
           artifact.kind != "checkpoint")) {
        continue;
      }
      const std::filesystem::path candidate =
          run.directory / artifact.relative_path;
      std::error_code error;
      if (!std::filesystem::is_regular_file(candidate, error) || error ||
          std::filesystem::file_size(candidate, error) == 0) {
        continue;
      }
      const auto modified = std::filesystem::last_write_time(candidate, error);
      if (!error && (checkpoint.empty() || modified > newest_time)) {
        checkpoint = candidate;
        newest_time = modified;
      }
    }
  }
  auto hash = application::CalculateFileSha256(checkpoint);
  if (!hash.Ok()) return std::nullopt;
  resume.checkpoint_hash = std::move(hash).Value();
  return resume;
}

void AddColumn(HWND list, int index, int width, const wchar_t* heading) {
  LVCOLUMNW column{};
  column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
  column.iSubItem = index;
  column.cx = width;
  column.pszText = const_cast<wchar_t*>(heading);
  ListView_InsertColumn(list, index, &column);
}

std::wstring Metric(const std::optional<double>& value) {
  if (!value) return L"N/A";
  std::wostringstream output;
  output << std::fixed << std::setprecision(4) << *value;
  return output.str();
}

std::filesystem::path FindGds(const application::RunRecord& run) {
  for (const application::RunArtifact& artifact : run.artifacts) {
    if (artifact.partial || artifact.kind != "final.gds") continue;
    const std::filesystem::path path = run.directory / artifact.relative_path;
    std::error_code error;
    if (std::filesystem::is_regular_file(path, error) &&
        std::filesystem::file_size(path, error) > 0) {
      return path;
    }
  }
  return {};
}

std::filesystem::path FindOdb(const application::RunRecord& run) {
  for (const application::RunArtifact& artifact : run.artifacts) {
    if (artifact.partial || artifact.kind != "final.odb") continue;
    const std::filesystem::path path = run.directory / artifact.relative_path;
    std::error_code error;
    if (std::filesystem::is_regular_file(path, error) && !error &&
        std::filesystem::file_size(path, error) > 0) {
      return path;
    }
  }
  return {};
}

std::filesystem::path FindNetlist(const application::RunRecord& run,
                                  bool extracted) {
  for (const application::RunArtifact& artifact : run.artifacts) {
    if (artifact.partial) continue;
    std::string format = artifact.format;
    std::transform(format.begin(), format.end(), format.begin(),
                   [](unsigned char character) {
                     return static_cast<char>(std::tolower(character));
                   });
    const bool spice = format == "spice" || format == "cdl" ||
                       artifact.relative_path.ends_with(".spice") ||
                       artifact.relative_path.ends_with(".cdl");
    const bool schematic =
        artifact.kind == "netlist.v" || artifact.kind == "power.v" || spice;
    if ((extracted ? spice : schematic)) {
      const std::filesystem::path path = run.directory / artifact.relative_path;
      std::error_code error;
      if (std::filesystem::is_regular_file(path, error) && !error &&
          std::filesystem::file_size(path, error) > 0) {
        return path;
      }
    }
  }
  return {};
}

std::vector<core::VerificationRecipe> BuiltinVerificationRecipes(
    const core::Project& project, const core::ToolchainProfile& profile) {
  std::vector<core::VerificationRecipe> recipes;
  if (project.physical_implementation.backend_id != "orfs" ||
      profile.orfs_root.empty()) {
    return recipes;
  }
  const std::string& platform = project.physical_implementation.orfs.platform;
  if (platform == "asap7" || platform == "sky130hd") {
    core::VerificationRecipe drc;
    drc.id = "builtin.orfs." + platform + ".klayout-drc";
    drc.name = "ORFS " + platform + " KLayout DRC";
    drc.engine = "klayout_drc";
    drc.provider_id = "orfs";
    drc.platform = platform;
    drc.root = profile.orfs_root + "/flow/platforms/" + platform;
    drc.entrypoint = "drc/" + platform + ".lydrc";
    drc.output_format = "lyrdb";
    drc.content_hash =
        "orfs-26q2:036d106273e66855cd5214d49518fd0f0df7de61:" + platform +
        ":drc";
    drc.trusted = true;
    drc.managed = true;
    recipes.push_back(std::move(drc));
  }
  if (platform == "sky130hd") {
    core::VerificationRecipe lvs;
    lvs.id = "builtin.orfs.sky130hd.klayout-lvs";
    lvs.name = "ORFS sky130hd KLayout LVS";
    lvs.engine = "klayout_lvs";
    lvs.provider_id = "orfs";
    lvs.platform = platform;
    lvs.root = profile.orfs_root + "/flow/platforms/sky130hd";
    lvs.entrypoint = "lvs/sky130hd.lylvs";
    lvs.reference_netlist = "cdl/sky130hd.cdl";
    lvs.output_format = "lvsdb";
    lvs.content_hash =
        "orfs-26q2:036d106273e66855cd5214d49518fd0f0df7de61:"
        "sky130hd:lvs";
    lvs.trusted = true;
    lvs.managed = true;
    recipes.push_back(std::move(lvs));
  }
  return recipes;
}

std::filesystem::path FindStageCheckpoint(const application::RunRecord& run,
                                          core::StageId stage) {
  const std::string expected =
      adapters::OrfsAdapter().CheckpointForStage(stage).filename().string();
  const std::string target = adapters::OrfsAdapter().TargetForStage(stage);
  std::filesystem::path newest;
  std::error_code error;
  std::filesystem::file_time_type newest_time{};
  for (const application::RunArtifact& artifact : run.artifacts) {
    if (artifact.partial ||
        (artifact.format != "odb" && artifact.format != ".odb" &&
         artifact.kind != "checkpoint")) {
      continue;
    }
    const std::filesystem::path path = run.directory / artifact.relative_path;
    if (!std::filesystem::is_regular_file(path, error) || error ||
        std::filesystem::file_size(path, error) == 0) {
      continue;
    }
    const std::string filename = path.filename().string();
    if (filename != expected && filename != target + ".odb" &&
        stage != core::StageId::kFinalOutputs)
      continue;
    const auto modified = std::filesystem::last_write_time(path, error);
    if (!error && (newest.empty() || modified > newest_time)) {
      newest = path;
      newest_time = modified;
    }
  }
  return newest;
}

bool SameOrfsLineage(const application::RunRecord& run,
                     const application::RunRecord* selected_run) {
  if (!selected_run || run.tool != "orfs" || selected_run->tool != "orfs") {
    return false;
  }
  const auto run_lineage = RunSummaryString(run, "lineage_id");
  const auto selected_lineage = RunSummaryString(*selected_run, "lineage_id");
  const auto run_fingerprint =
      RunSummaryString(run, "configuration_fingerprint");
  const auto selected_fingerprint =
      RunSummaryString(*selected_run, "configuration_fingerprint");
  return run_lineage && selected_lineage && run_fingerprint &&
         selected_fingerprint && *run_lineage == *selected_lineage &&
         *run_fingerprint == *selected_fingerprint;
}

const application::RunRecord* FindStageRun(
    const std::vector<application::RunRecord>& runs,
    const application::RunRecord* selected_run, core::StageId stage) {
  for (const application::RunRecord& run : runs) {
    if (SameOrfsLineage(run, selected_run) &&
        !FindStageCheckpoint(run, stage).empty()) {
      return &run;
    }
  }
  return nullptr;
}

enum class StageDialogAction {
  kCancel,
  kRunThrough,
  kRebuildFrom,
  kOpenInOpenRoad,
  kReports,
  kArtifacts,
};

constexpr std::array<core::StageId, 6> kPhysicalStages = {
    core::StageId::kSynthesis, core::StageId::kFloorplan,
    core::StageId::kPlacement, core::StageId::kClockTreeSynthesis,
    core::StageId::kRouting,   core::StageId::kFinalOutputs};

struct StageDialogState {
  core::StageId selected_stage = core::StageId::kFinalOutputs;
  StageDialogAction action = StageDialogAction::kCancel;
  bool accepted = false;
  HWND list = nullptr;
  HWND hint = nullptr;
  UINT dpi = 96;
  std::array<std::wstring, 6> labels;
  std::array<bool, 6> checkpoint_available{};
  bool has_resume_candidate = false;
};

constexpr int kStageList = 7840;
constexpr int kStageRunThrough = 7841;
constexpr int kStageRebuild = 7842;
constexpr int kStageOpenRoad = 7843;
constexpr int kStageCancel = 7844;
constexpr int kStageReports = 7845;
constexpr int kStageArtifacts = 7846;

int StageIndex(core::StageId stage) {
  const auto found =
      std::find(kPhysicalStages.begin(), kPhysicalStages.end(), stage);
  return found == kPhysicalStages.end()
             ? static_cast<int>(kPhysicalStages.size() - 1)
             : static_cast<int>(std::distance(kPhysicalStages.begin(), found));
}

std::wstring_view PhysicalStageDisplayName(core::StageId stage);

void UpdateStageDialogSelection(HWND window, StageDialogState* state) {
  if (!window || !state || !state->list) return;
  const int selected = ListView_GetNextItem(state->list, -1, LVNI_SELECTED);
  if (selected < 0 || selected >= static_cast<int>(kPhysicalStages.size())) {
    return;
  }
  state->selected_stage = kPhysicalStages[static_cast<std::size_t>(selected)];
  const bool checkpoint_available =
      state->checkpoint_available[static_cast<std::size_t>(selected)];
  EnableWindow(GetDlgItem(window, kStageRunThrough), TRUE);
  EnableWindow(GetDlgItem(window, kStageRebuild), state->has_resume_candidate);
  EnableWindow(GetDlgItem(window, kStageOpenRoad), checkpoint_available);
  EnableWindow(GetDlgItem(window, kStageReports), checkpoint_available);
  EnableWindow(GetDlgItem(window, kStageArtifacts), checkpoint_available);

  std::wstring detail = state->labels[static_cast<std::size_t>(selected)];
  const std::size_t separator = detail.find(L" — ");
  if (separator != std::wstring::npos) {
    detail = detail.substr(separator + 3);
  }
  const std::wstring hint =
      std::wstring(PhysicalStageDisplayName(state->selected_stage)) +
      (checkpoint_available ? L"  |  Checkpoint ready  |  "
                            : L"  |  No checkpoint yet  |  ") +
      (detail.empty() ? L"Not Run" : detail);
  SetWindowTextW(state->hint, hint.c_str());
}

std::wstring_view PhysicalStageDisplayName(core::StageId stage) {
  switch (stage) {
    case core::StageId::kSynthesis:
      return L"Synthesis";
    case core::StageId::kFloorplan:
      return L"Floorplan";
    case core::StageId::kPlacement:
      return L"Placement";
    case core::StageId::kClockTreeSynthesis:
      return L"CTS";
    case core::StageId::kRouting:
      return L"Routing";
    case core::StageId::kFinalOutputs:
      return L"Final";
    default:
      return L"Stage";
  }
}

std::array<std::wstring, 6> BuildStageLabels(
    const std::vector<application::RunRecord>& runs,
    const application::RunRecord* selected_run) {
  std::array<std::wstring, 6> labels;
  const adapters::OrfsAdapter adapter;
  for (std::size_t index = 0; index < kPhysicalStages.size(); ++index) {
    const core::StageId stage = kPhysicalStages[index];
    const std::string expected =
        adapter.CheckpointForStage(stage).filename().string();
    const std::string target = adapter.TargetForStage(stage);
    std::wstring state = L"Not Run";
    for (const application::RunRecord& run : runs) {
      if (!SameOrfsLineage(run, selected_run) ||
          (run.status != application::RunStatus::kRunning &&
           run.status != application::RunStatus::kSucceeded &&
           run.status != application::RunStatus::kFailed &&
           run.status != application::RunStatus::kInterrupted &&
           run.status != application::RunStatus::kCancelled)) {
        continue;
      }
      bool has_checkpoint = false;
      for (const application::RunArtifact& artifact : run.artifacts) {
        const std::filesystem::path path(artifact.relative_path);
        const std::string filename = path.filename().string();
        if (artifact.format != "odb" && artifact.format != ".odb" &&
            artifact.kind != "checkpoint") {
          continue;
        }
        if (filename == expected || filename == target + ".odb" ||
            (stage == core::StageId::kFinalOutputs &&
             (filename == "final.odb" || filename == "checkpoint.odb"))) {
          has_checkpoint = artifact.size > 0;
          if (has_checkpoint) break;
        }
      }
      const auto run_target = RunSummaryString(run, "target_stage");
      if (!has_checkpoint && run.status != application::RunStatus::kRunning &&
          (!run_target || *run_target != target)) {
        continue;
      }
      switch (run.status) {
        case application::RunStatus::kRunning:
          state = L"Running";
          break;
        case application::RunStatus::kSucceeded:
          state = has_checkpoint ? L"Complete" : L"Stale";
          break;
        case application::RunStatus::kFailed:
        case application::RunStatus::kInterrupted:
        case application::RunStatus::kCancelled:
          state = L"Failed";
          break;
        default:
          break;
      }
      break;
    }
    labels[index] =
        std::wstring(PhysicalStageDisplayName(stage)) + L" — " + state;
  }
  return labels;
}

LRESULT CALLBACK StageDialogProcedure(HWND window, UINT message, WPARAM wparam,
                                      LPARAM lparam) {
  auto* state = reinterpret_cast<StageDialogState*>(
      GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    state = static_cast<StageDialogState*>(create->lpCreateParams);
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
  }
  if (!state) return DefWindowProcW(window, message, wparam, lparam);
  const auto px = [state](int value) {
    return MulDiv(value, static_cast<int>(state->dpi), 96);
  };
  switch (message) {
    case WM_GETMINMAXINFO: {
      auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
      if (limits) {
        limits->ptMinTrackSize.x = px(680);
        limits->ptMinTrackSize.y = px(340);
      }
      return 0;
    }
    case WM_CREATE: {
      state->dpi = GetDpiForWindow(window);
      state->list = CreateWindowExW(
          WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
          WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL |
              LVS_SHOWSELALWAYS,
          0, 0, 0, 0, window,
          reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStageList)),
          GetModuleHandleW(nullptr), nullptr);
      ListView_SetExtendedListViewStyle(state->list, LVS_EX_FULLROWSELECT |
                                                         LVS_EX_GRIDLINES |
                                                         LVS_EX_DOUBLEBUFFER);
      AddColumn(state->list, 0, px(190), L"Stage");
      AddColumn(state->list, 1, px(120), L"Status");
      AddColumn(state->list, 2, px(220), L"Checkpoint");
      for (std::size_t index = 0; index < kPhysicalStages.size(); ++index) {
        const std::wstring label =
            state->labels[index].empty()
                ? std::wstring(PhysicalStageDisplayName(kPhysicalStages[index]))
                : state->labels[index];
        const std::size_t separator = label.find(L" — ");
        std::wstring stage_name =
            separator == std::wstring::npos
                ? std::wstring(PhysicalStageDisplayName(kPhysicalStages[index]))
                : label.substr(0, separator);
        std::wstring status = separator == std::wstring::npos
                                  ? L"Not Run"
                                  : label.substr(separator + 3);
        std::wstring checkpoint = state->checkpoint_available[index]
                                      ? L"Available"
                                      : L"Not generated";
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(index);
        item.pszText = stage_name.data();
        const int row = ListView_InsertItem(state->list, &item);
        if (row >= 0) {
          ListView_SetItemText(state->list, row, 1, status.data());
          ListView_SetItemText(state->list, row, 2, checkpoint.data());
        }
      }
      state->hint = CreateWindowExW(
          0, L"STATIC",
          L"Select a stage to run through, rebuild, inspect, or open in "
          L"OpenROAD.",
          WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, 0, 0, 0, 0, window,
          nullptr, GetModuleHandleW(nullptr), nullptr);
      const auto button = [window](const wchar_t* text, int id) {
        return CreateWindowExW(
            0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0,
            window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
            GetModuleHandleW(nullptr), nullptr);
      };
      button(L"Run Through", kStageRunThrough);
      button(L"Rebuild From", kStageRebuild);
      button(L"Open in OpenROAD", kStageOpenRoad);
      button(L"Reports", kStageReports);
      button(L"Artifacts", kStageArtifacts);
      button(L"Cancel", kStageCancel);
      for (HWND child = GetWindow(window, GW_CHILD); child;
           child = GetWindow(child, GW_HWNDNEXT)) {
        SendMessageW(child, WM_SETFONT,
                     reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),
                     TRUE);
      }
      EnableWindow(GetDlgItem(window, kStageOpenRoad),
                   state->checkpoint_available[static_cast<std::size_t>(
                       StageIndex(state->selected_stage))]);
      ListView_SetItemState(state->list, StageIndex(state->selected_stage),
                            LVIS_SELECTED | LVIS_FOCUSED,
                            LVIS_SELECTED | LVIS_FOCUSED);
      UpdateStageDialogSelection(window, state);
      return 0;
    }
    case WM_SIZE: {
      const int width = LOWORD(lparam);
      const int height = HIWORD(lparam);
      const int button_width = px(128);
      const int button_height = px(30);
      const int first_button_y = height - px(88);
      const int second_button_y = height - px(48);
      MoveWindow(state->hint, px(14), px(12), width - px(28), px(26), TRUE);
      const int list_top = px(48);
      MoveWindow(state->list, px(12), list_top, width - px(24),
                 std::max(px(120), first_button_y - list_top - px(12)), TRUE);
      const int gap = px(6);
      const int row_width = button_width * 3 + gap * 2;
      const int x = width - row_width - px(12);
      for (int row = 0; row < 2; ++row) {
        int button_x = x;
        const int button_y = row == 0 ? first_button_y : second_button_y;
        const std::array<int, 3> ids =
            row == 0 ? std::array<int, 3>{kStageRunThrough, kStageRebuild,
                                          kStageOpenRoad}
                     : std::array<int, 3>{kStageReports, kStageArtifacts,
                                          kStageCancel};
        for (int id : ids) {
          MoveWindow(GetDlgItem(window, id), button_x, button_y, button_width,
                     button_height, TRUE);
          button_x += button_width + gap;
        }
      }
      ListView_SetColumnWidth(state->list, 0, px(190));
      ListView_SetColumnWidth(state->list, 1, px(120));
      const int checkpoint_width =
          std::max(px(130), width - px(24) - px(190) - px(120));
      ListView_SetColumnWidth(state->list, 2, checkpoint_width);
      return 0;
    }
    case WM_NOTIFY: {
      const auto* notification = reinterpret_cast<const NMHDR*>(lparam);
      if (notification && notification->idFrom == kStageList &&
          notification->code == LVN_ITEMCHANGED) {
        const auto* change = reinterpret_cast<const NMLISTVIEW*>(lparam);
        if ((change->uNewState & LVIS_SELECTED) != 0) {
          UpdateStageDialogSelection(window, state);
        }
        return 0;
      }
      break;
    }
    case WM_COMMAND: {
      const int id = LOWORD(wparam);
      if (id == kStageRunThrough || id == kStageRebuild ||
          id == kStageOpenRoad || id == kStageReports ||
          id == kStageArtifacts) {
        state->action = id == kStageRunThrough ? StageDialogAction::kRunThrough
                        : id == kStageRebuild  ? StageDialogAction::kRebuildFrom
                        : id == kStageOpenRoad
                            ? StageDialogAction::kOpenInOpenRoad
                        : id == kStageReports ? StageDialogAction::kReports
                                              : StageDialogAction::kArtifacts;
        state->accepted = true;
        DestroyWindow(window);
        return 0;
      }
      if (id == kStageCancel) {
        DestroyWindow(window);
        return 0;
      }
      break;
    }
    case WM_CLOSE:
      DestroyWindow(window);
      return 0;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

bool ShowStagesDialog(HWND owner, core::StageId* selected_stage,
                      StageDialogAction* action,
                      std::array<std::wstring, 6> labels,
                      std::array<bool, 6> checkpoint_available,
                      bool has_resume_candidate) {
  if (!owner || !selected_stage || !action) return false;
  constexpr wchar_t kClassName[] = L"DesignPlusPlus.LayoutStagesDialog";
  WNDCLASSEXW window_class{sizeof(window_class)};
  window_class.lpfnWndProc = StageDialogProcedure;
  window_class.hInstance = GetModuleHandleW(nullptr);
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  window_class.lpszClassName = kClassName;
  if (!RegisterClassExW(&window_class) &&
      GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return false;
  }
  StageDialogState state;
  state.selected_stage = *selected_stage;
  state.labels = std::move(labels);
  state.checkpoint_available = checkpoint_available;
  state.has_resume_candidate = has_resume_candidate;
  RECT owner_rect{};
  GetWindowRect(owner, &owner_rect);
  const UINT dpi = GetDpiForWindow(owner);
  HMONITOR monitor = MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST);
  MONITORINFO monitor_info{sizeof(monitor_info)};
  GetMonitorInfoW(monitor, &monitor_info);
  const int desired_width = MulDiv(820, dpi, 96);
  const int desired_height = MulDiv(430, dpi, 96);
  const int available_width = static_cast<int>(monitor_info.rcWork.right -
                                               monitor_info.rcWork.left - 24);
  const int available_height = static_cast<int>(monitor_info.rcWork.bottom -
                                                monitor_info.rcWork.top - 24);
  const int width = std::min(desired_width, available_width);
  const int height = std::min(desired_height, available_height);
  const int x = std::clamp(owner_rect.left + 40, monitor_info.rcWork.left,
                           monitor_info.rcWork.right - width);
  const int y = std::clamp(owner_rect.top + 40, monitor_info.rcWork.top,
                           monitor_info.rcWork.bottom - height);
  HWND dialog = CreateWindowExW(
      WS_EX_DLGMODALFRAME, kClassName, L"ORFS Physical Implementation Stages",
      WS_CAPTION | WS_SYSMENU | WS_THICKFRAME, x, y, width, height, owner,
      nullptr, GetModuleHandleW(nullptr), &state);
  if (!dialog) return false;
  EnableWindow(owner, FALSE);
  ShowWindow(dialog, SW_SHOW);
  MSG message{};
  while (IsWindow(dialog) && GetMessageW(&message, nullptr, 0, 0) > 0) {
    if (!IsDialogMessageW(dialog, &message)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }
  EnableWindow(owner, TRUE);
  SetActiveWindow(owner);
  if (state.accepted) {
    *selected_stage = state.selected_stage;
    *action = state.action;
  }
  return state.accepted;
}

}  // namespace

struct LayoutWindowImplementation::EventChannel final {
  std::mutex mutex;
  HWND window = nullptr;
  std::uint64_t generation = 0;
  struct Event {
    std::uint64_t generation = 0;
    bool loaded = false;
    bool saved = false;
    bool generate_after_save = false;
    bool discovered = false;
    bool resume = false;
    core::StageId target_stage = core::StageId::kFinalOutputs;
    bool rebuild_from_stage = false;
    bool full_flow = false;
    bool orfs_discovered = false;
    core::Status status;
    std::shared_ptr<application::ProjectDocument> document;
    std::optional<core::Project> saved_project;
    std::vector<application::ResolvedSource> sources;
    core::ToolchainProfile profile;
    core::ToolchainSettings toolchain_settings;
    std::vector<application::RunRecord> runs;
    std::vector<application::RunRecord> verification_runs;
    std::vector<adapters::ManagedFlowMetrics> run_metrics;
    std::vector<std::optional<application::ManagedFlowResumeRequest>> resumes;
    application::PhysicalImplementationEvent flow;
    application::PhysicalVerificationEvent verification;
    application::LayoutViewerEvent viewer;
    application::OpenRoadViewerEvent openroad_viewer;
    application::OpenLaneDiscoveryResult discovery;
    application::OrfsDiscoveryResult orfs_discovery;
  };
  std::deque<Event> events;
  bool message_posted = false;
};

LayoutWindowImplementation::~LayoutWindowImplementation() { Close(); }

bool LayoutWindowImplementation::Create(
    HINSTANCE instance, const application::WorkspaceOpenRequest& request,
    application::LibraryRecord library, ViewWindowLogCallback central_log,
    ViewWindowLibraryChangedCallback library_changed,
    LayoutJsonEditorCallback json_editor) {
  if (request.library_id != library.library.id) return false;
  const auto cell =
      std::find_if(library.library.cells.begin(), library.library.cells.end(),
                   [&request](const core::Cell& candidate) {
                     return candidate.id == request.cell_id;
                   });
  if (cell == library.library.cells.end()) return false;
  cell_name_ = cell->name;
  const auto view = std::find_if(cell->views.begin(), cell->views.end(),
                                 [&request](const core::View& candidate) {
                                   return candidate.id == request.view_id;
                                 });
  if (view == cell->views.end() || view->kind != core::ViewKind::kLayout) {
    return false;
  }
  instance_ = instance;
  request_ = request;
  view_kind_ = view->kind;
  library_ = std::move(library);
  central_log_ = std::move(central_log);
  library_changed_ = std::move(library_changed);
  json_editor_ = std::move(json_editor);
  event_channel_ = std::make_shared<EventChannel>();
  WNDCLASSEXW window_class{sizeof(window_class)};
  window_class.lpfnWndProc = WindowProcedure;
  window_class.hInstance = instance_;
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  window_class.lpszClassName = kWindowClassName;
  if (!RegisterClassExW(&window_class) &&
      GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return false;
  }
  dpi_ = GetSystemDpi();
  window_ =
      CreateWindowExW(0, kWindowClassName, L"Design++ Layout",
                      WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                      CW_USEDEFAULT, ScaleForDpi(kInitialWindowWidth, dpi_),
                      ScaleForDpi(kInitialWindowHeight, dpi_), nullptr, nullptr,
                      instance_, this);
  if (!window_) return false;
  const std::wstring window_title =
      L"Design++ Layout — " + Utf8ToWide(cell_name_);
  SetWindowTextW(window_, window_title.c_str());
  SetWindowTextW(identity_, (L"Layout — " + Utf8ToWide(cell_name_)).c_str());
  {
    std::scoped_lock lock(event_channel_->mutex);
    event_channel_->window = window_;
    event_channel_->generation = generation_;
  }
  ShowWindow(window_, SW_SHOW);
  UpdateWindow(window_);
  BeginLoad();
  return true;
}

ViewWindowKind LayoutWindowImplementation::Kind() const noexcept {
  return ViewWindowKind::kLayout;
}

bool LayoutWindowImplementation::CanActivate(
    const application::WorkspaceOpenRequest& request,
    core::ViewKind view_kind) const {
  return view_kind == view_kind_ && request.view_id == request_.view_id &&
         MatchesCell(request.library_id, request.cell_id);
}

void LayoutWindowImplementation::Activate(
    const application::WorkspaceOpenRequest& request) {
  if (!CanActivate(request, view_kind_) || !window_) return;
  ShowWindow(window_, SW_RESTORE);
  SetForegroundWindow(window_);
}

bool LayoutWindowImplementation::BelongsToLibrary(
    std::string_view library_id) const {
  return request_.library_id == library_id;
}

bool LayoutWindowImplementation::MatchesCell(std::string_view library_id,
                                             std::string_view cell_id) const {
  return request_.library_id == library_id && request_.cell_id == cell_id;
}

bool LayoutWindowImplementation::IsOpen() const noexcept {
  return window_ != nullptr;
}

void LayoutWindowImplementation::RefreshLibrary(
    application::LibraryRecord library) {
  if (!window_ || !event_channel_ ||
      library.library.id != request_.library_id) {
    return;
  }
  const auto cell =
      std::find_if(library.library.cells.begin(), library.library.cells.end(),
                   [this](const core::Cell& candidate) {
                     return candidate.id == request_.cell_id;
                   });
  if (cell != library.library.cells.end()) {
    cell_name_ = cell->name;
    if (window_) {
      const std::wstring title = L"Design++ Layout — " + Utf8ToWide(cell_name_);
      SetWindowTextW(window_, title.c_str());
      SetControlText(identity_, "Layout — " + cell_name_);
    }
  }
  library_ = std::move(library);
  ++generation_;
  {
    std::scoped_lock lock(event_channel_->mutex);
    // A refresh can race with a worker that already queued an event for the
    // previous Library snapshot.  Drop that event before starting the load
    // for this snapshot; otherwise an old Cell document can be displayed (or
    // edited) while the new cell is still being resolved.
    event_channel_->events.clear();
    event_channel_->message_posted = false;
    event_channel_->generation = generation_;
  }
  flow_service_.Cancel();
  viewer_service_.Cancel();
  openroad_viewer_service_.Cancel();
  discovery_service_.Cancel();
  orfs_discovery_service_.Cancel();
  ClearLoadedCellState();
  BeginLoad();
}

void LayoutWindowImplementation::ClearLoadedCellState() {
  document_.reset();
  sources_.clear();
  sdc_candidates_.clear();
  pdk_candidates_.clear();
  orfs_platform_candidates_.clear();
  profile_ = {};
  toolchain_settings_ = {};
  runs_.clear();
  verification_runs_.clear();
  verification_recipes_.clear();
  run_metrics_.clear();
  resume_candidates_.clear();
  active_run_.reset();
  selected_resume_.reset();
  selected_stage_ = core::StageId::kFinalOutputs;
  metrics_ = {};
  flow_output_utf8_remainder_.clear();
  save_in_progress_ = false;

  if (!window_) return;
  SetControlText(status_, "Loading the selected Cell layout state...");
  SetControlText(summary_, "No layout result for the selected Cell");
  SetControlText(output_, "");
  SendMessageW(stages_list_, LB_RESETCONTENT, 0, 0);
  SendMessageW(pnr_sdc_combo_, CB_RESETCONTENT, 0, 0);
  SendMessageW(signoff_sdc_combo_, CB_RESETCONTENT, 0, 0);
  ApplyState(application::ManagedFlowRunState::kPreparing);
}

bool LayoutWindowImplementation::PrepareClose() {
  if (!setup_.RequestClose()) return false;
  flow_service_.Cancel();
  viewer_service_.Cancel();
  openroad_viewer_service_.Cancel();
  verification_service_.Cancel();
  orfs_discovery_service_.Cancel();
  return true;
}

void LayoutWindowImplementation::Close() {
  if (!window_) return;
  setup_.Close();
  flow_service_.Shutdown();
  viewer_service_.Shutdown();
  openroad_viewer_service_.Shutdown();
  verification_service_.Shutdown();
  discovery_service_.Cancel();
  orfs_discovery_service_.Cancel();
  scheduler_.RequestStop();
  ShutdownChannel();
  DestroyWindow(window_);
  window_ = nullptr;
}

bool LayoutWindowImplementation::TranslateAccelerator(const MSG& message) {
  return setup_.TranslateAccelerator(message);
}

LRESULT CALLBACK LayoutWindowImplementation::WindowProcedure(HWND window,
                                                             UINT message,
                                                             WPARAM wparam,
                                                             LPARAM lparam) {
  LayoutWindowImplementation* self =
      reinterpret_cast<LayoutWindowImplementation*>(
          GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<LayoutWindowImplementation*>(create->lpCreateParams);
    self->window_ = window;
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  }
  return self ? self->HandleMessage(message, wparam, lparam)
              : DefWindowProcW(window, message, wparam, lparam);
}

LRESULT LayoutWindowImplementation::HandleMessage(UINT message, WPARAM wparam,
                                                  LPARAM lparam) {
  switch (message) {
    case WM_CREATE:
      dpi_ = GetDpiForWindow(window_);
      return CreateControls() ? 0 : -1;
    case WM_SIZE:
      LayoutControls(LOWORD(lparam), HIWORD(lparam));
      return 0;
    case WM_PAINT: {
      PAINTSTRUCT paint{};
      BeginPaint(window_, &paint);
      EndPaint(window_, &paint);
      return 0;
    }
    case WM_GETMINMAXINFO: {
      auto* information = reinterpret_cast<MINMAXINFO*>(lparam);
      information->ptMinTrackSize = {ScaleForDpi(kMinimumWindowWidth, dpi_),
                                     ScaleForDpi(kMinimumWindowHeight, dpi_)};
      return 0;
    }
    case WM_COMMAND:
      if (LOWORD(wparam) == kPdkCombo && HIWORD(wparam) == CBN_SELCHANGE) {
        PopulateStandardCellLibraries();
        return 0;
      }
      switch (LOWORD(wparam)) {
        case kRunButton:
          SaveAndStart(false, core::StageId::kFinalOutputs, false, true);
          return 0;
        case kCancelButton:
          flow_service_.Cancel();
          openroad_viewer_service_.Cancel();
          return 0;
        case kResumeButton:
          OpenLayout();
          return 0;
        case kStagesButton:
          ShowStages();
          return 0;
        case kConfigButton:
          EditSetup();
          return 0;
        case kReportsButton:
          ShowReports();
          return 0;
        case kArtifactsButton:
          ShowArtifacts();
          return 0;
        case kDrcButton:
          run_lvs_after_drc_ = false;
          StartVerification(adapters::VerificationCheck::kDrc);
          return 0;
        case kLvsButton:
          run_lvs_after_drc_ = false;
          StartVerification(adapters::VerificationCheck::kLvs);
          return 0;
        case kVerifyAllButton:
          run_lvs_after_drc_ = true;
          StartVerification(adapters::VerificationCheck::kDrc);
          return 0;
        case kVerificationCancelButton:
          run_lvs_after_drc_ = false;
          verification_service_.Cancel();
          return 0;
      }
      break;
    case WM_NOTIFY: {
      const auto* header = reinterpret_cast<NMHDR*>(lparam);
      if (header->hwndFrom == page_tabs_ && header->code == TCN_SELCHANGE) {
        SelectPage(TabCtrl_GetCurSel(page_tabs_));
        return 0;
      }
      if (header->hwndFrom == runs_list_ && header->code == LVN_ITEMCHANGED &&
          !flow_service_.IsActive()) {
        SelectRun(ListView_GetNextItem(runs_list_, -1, LVNI_SELECTED));
      }
      if (header->hwndFrom == verification_list_ && header->code == NM_DBLCLK) {
        OpenVerificationResult(
            ListView_GetNextItem(verification_list_, -1, LVNI_SELECTED));
      }
      return 0;
    }
    case kEventMessage:
      HandleEvents();
      return 0;
    case WM_CLOSE:
      Close();
      return 0;
    case WM_NCDESTROY: {
      const HWND destroyed = window_;
      window_ = nullptr;
      return DefWindowProcW(destroyed, message, wparam, lparam);
    }
  }
  return DefWindowProcW(window_, message, wparam, lparam);
}

bool LayoutWindowImplementation::CreateControls() {
  font_ = CreateUiFont(dpi_);
  const auto create = [this](const wchar_t* cls, const wchar_t* text,
                             DWORD style, int id) {
    HWND control = CreateWindowExW(
        0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, window_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_.Get()),
                 TRUE);
    return control;
  };
  identity_ = create(L"STATIC", L"Layout", SS_LEFT, 0);
  run_button_ =
      create(L"BUTTON", L"Generate / Update Layout", BS_PUSHBUTTON, kRunButton);
  cancel_button_ = create(L"BUTTON", L"Cancel", BS_PUSHBUTTON, kCancelButton);
  resume_button_ =
      create(L"BUTTON", L"Open Layout", BS_PUSHBUTTON, kResumeButton);
  stages_button_ = create(L"BUTTON", L"Stages", BS_PUSHBUTTON, kStagesButton);
  config_button_ = create(L"BUTTON", L"Setup", BS_PUSHBUTTON, kConfigButton);
  reports_button_ =
      create(L"BUTTON", L"Reports", BS_PUSHBUTTON, kReportsButton);
  artifacts_button_ =
      create(L"BUTTON", L"Artifacts", BS_PUSHBUTTON, kArtifactsButton);
  pdk_edit_ = create(WC_COMBOBOXW, L"sky130A",
                     WS_BORDER | CBS_DROPDOWN | WS_VSCROLL, kPdkCombo);
  scl_edit_ = create(WC_COMBOBOXW, L"sky130_fd_sc_hd",
                     WS_BORDER | CBS_DROPDOWN | WS_VSCROLL, kSclCombo);
  clocks_edit_ = create(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, 0);
  period_edit_ = create(L"EDIT", L"10.0", WS_BORDER | ES_AUTOHSCROLL, 0);
  utilization_edit_ =
      create(L"EDIT", L"40", WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL, 0);
  density_edit_ = create(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, 0);
  die_area_edit_ = create(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, 0);
  pnr_sdc_combo_ = create(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL, 0);
  signoff_sdc_combo_ =
      create(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL, 0);
  advanced_edit_ =
      create(L"EDIT", L"{}",
             WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL, 0);
  stages_list_ = create(L"LISTBOX", L"", WS_BORDER | WS_VSCROLL, 0);
  page_tabs_ = create(WC_TABCONTROLW, L"", WS_TABSTOP, kPageTabs);
  for (const wchar_t* label : {L"Summary", L"Verification", L"Runs"}) {
    TCITEMW item{};
    item.mask = TCIF_TEXT;
    item.pszText = const_cast<wchar_t*>(label);
    TabCtrl_InsertItem(page_tabs_, TabCtrl_GetItemCount(page_tabs_), &item);
  }
  summary_ = create(L"EDIT", L"No compatible layout result",
                    WS_BORDER | ES_MULTILINE | ES_READONLY | WS_VSCROLL, 0);
  runs_list_ = create(
      WC_LISTVIEWW, L"",
      WS_BORDER | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, kRunsList);
  ListView_SetExtendedListViewStyle(runs_list_, LVS_EX_FULLROWSELECT |
                                                    LVS_EX_GRIDLINES |
                                                    LVS_EX_DOUBLEBUFFER);
  AddColumn(runs_list_, 0, 150, L"Started");
  AddColumn(runs_list_, 1, 95, L"Status");
  AddColumn(runs_list_, 2, 190, L"Run ID");
  verification_source_ =
      create(L"STATIC", L"Select a final Layout Run to verify", SS_LEFT, 0);
  drc_button_ = create(L"BUTTON", L"Run DRC", BS_PUSHBUTTON, kDrcButton);
  lvs_button_ = create(L"BUTTON", L"Run LVS", BS_PUSHBUTTON, kLvsButton);
  verify_all_button_ =
      create(L"BUTTON", L"Run All Checks", BS_PUSHBUTTON, kVerifyAllButton);
  verification_cancel_button_ =
      create(L"BUTTON", L"Cancel Verification", BS_PUSHBUTTON,
             kVerificationCancelButton);
  verification_list_ =
      create(WC_LISTVIEWW, L"",
             WS_BORDER | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, 0);
  ListView_SetExtendedListViewStyle(
      verification_list_,
      LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
  AddColumn(verification_list_, 0, 90, L"Check");
  AddColumn(verification_list_, 1, 230, L"Recipe");
  AddColumn(verification_list_, 2, 100, L"Status");
  AddColumn(verification_list_, 3, 100, L"Violations");
  AddColumn(verification_list_, 4, 190, L"Source Run");
  verification_detail_ = create(
      L"EDIT", L"No physical verification result",
      WS_BORDER | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, 0);
  output_ = create(
      L"EDIT", L"",
      WS_BORDER | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, 0);
  status_ = create(L"STATIC", L"Loading layout state...", SS_LEFT, 0);
  EnableWindow(cancel_button_, FALSE);
  EnableWindow(resume_button_, FALSE);
  EnableWindow(verification_cancel_button_, FALSE);
  for (HWND hidden :
       {pdk_edit_, scl_edit_, clocks_edit_, period_edit_, utilization_edit_,
        density_edit_, die_area_edit_, pnr_sdc_combo_, signoff_sdc_combo_,
        advanced_edit_, stages_list_, output_}) {
    ShowWindow(hidden, SW_HIDE);
  }
  for (HWND hidden : {runs_list_, verification_source_, verification_list_,
                      verification_detail_, drc_button_, lvs_button_,
                      verify_all_button_, verification_cancel_button_}) {
    ShowWindow(hidden, SW_HIDE);
  }
  return identity_ && run_button_ && cancel_button_ && resume_button_ &&
         stages_button_ && config_button_ && reports_button_ &&
         artifacts_button_ && pdk_edit_ && scl_edit_ && clocks_edit_ &&
         period_edit_ && utilization_edit_ && density_edit_ && die_area_edit_ &&
         pnr_sdc_combo_ && signoff_sdc_combo_ && advanced_edit_ &&
         stages_list_ && page_tabs_ && summary_ && runs_list_ && output_ &&
         status_ && verification_source_ && verification_list_ &&
         verification_detail_ && drc_button_ && lvs_button_ &&
         verify_all_button_ && verification_cancel_button_;
}

void LayoutWindowImplementation::LayoutControls(int width, int height) {
  const auto px = [this](int value) {
    return MulDiv(value, static_cast<int>(dpi_), 96);
  };
  const int gap = px(4);
  const int header = px(44);
  MoveWindow(identity_, px(8), px(9), px(210), px(26), TRUE);
  HWND buttons[] = {run_button_,      cancel_button_, resume_button_,
                    stages_button_,   config_button_, reports_button_,
                    artifacts_button_};
  int x = px(220);
  const int button_width = std::max(px(100), (width - x - px(8) - gap * 6) / 7);
  for (HWND button : buttons) {
    MoveWindow(button, x, px(6), button_width, px(31), TRUE);
    x += button_width + gap;
  }
  const int bottom = height - px(28);
  MoveWindow(page_tabs_, px(8), header, width - px(16), bottom - header, TRUE);
  RECT page{px(18), header + px(34), width - px(18), bottom - px(10)};
  if (selected_page_ == 0) {
    MoveWindow(summary_, page.left, page.top, page.right - page.left,
               page.bottom - page.top, TRUE);
  } else if (selected_page_ == 2) {
    MoveWindow(runs_list_, page.left, page.top, page.right - page.left,
               page.bottom - page.top, TRUE);
  } else {
    const int verification_button_width = px(130);
    MoveWindow(verification_source_, page.left, page.top,
               page.right - page.left, px(24), TRUE);
    int button_x = page.left;
    for (HWND button : {drc_button_, lvs_button_, verify_all_button_,
                        verification_cancel_button_}) {
      MoveWindow(button, button_x, page.top + px(28), verification_button_width,
                 px(30), TRUE);
      button_x += verification_button_width + gap;
    }
    const int list_top = page.top + px(64);
    const int page_bottom = static_cast<int>(page.bottom);
    const int page_width = static_cast<int>(page.right - page.left);
    const int list_height = std::max(px(120), (page_bottom - list_top) / 2);
    MoveWindow(verification_list_, page.left, list_top, page_width, list_height,
               TRUE);
    MoveWindow(verification_detail_, page.left, list_top + list_height + gap,
               page_width,
               std::max(px(70), page_bottom - list_top - list_height - gap),
               TRUE);
  }
  MoveWindow(status_, 0, height - px(22), width, px(22), TRUE);
}

void LayoutWindowImplementation::BeginLoad() {
  ApplyState(application::ManagedFlowRunState::kPreparing);
  const auto channel = event_channel_;
  const auto library = library_;
  const std::string cell_id = request_.cell_id;
  const std::uint64_t generation = generation_;
  static_cast<void>(scheduler_.Submit([channel, library, cell_id,
                                       generation](std::stop_token token) {
    if (token.stop_requested()) return;
    EventChannel::Event event;
    event.generation = generation;
    event.loaded = true;
    application::ProjectService projects;
    auto opened = projects.OpenOrCreate(library, cell_id);
    if (!opened.Ok()) {
      event.status = opened.GetStatus();
    } else {
      event.document = std::make_shared<application::ProjectDocument>(
          std::move(opened).Value());
      auto sources =
          projects.ResolveSources(library, cell_id, event.document->project);
      if (!sources.Ok()) {
        event.status = sources.GetStatus();
      } else {
        event.sources = std::move(sources).Value();
        application::ToolchainProfileStore profiles;
        auto settings = profiles.LoadOrCreateDefaults();
        if (!settings.Ok()) {
          event.status = settings.GetStatus();
        } else {
          const core::ToolchainSettings& values = settings.Value();
          event.toolchain_settings = values;
          // A project may pin a toolchain profile for this cell.  The
          // selected profile is only the fallback for legacy projects that
          // have no binding; it must not silently override an explicit cell
          // choice.
          const std::string profile_id =
              event.document->project.toolchain_profile_id.value_or(
                  values.selected_profile_id);
          const auto selected = std::find_if(
              values.profiles.begin(), values.profiles.end(),
              [&profile_id](const core::ToolchainProfile& profile) {
                return profile.id == profile_id;
              });
          if (selected == values.profiles.end()) {
            event.status = {
                core::ErrorCode::kNotFound,
                event.document->project.toolchain_profile_id
                    ? "Toolchain Profile pinned by this cell is unavailable"
                    : "Selected Toolchain Profile is unavailable",
                0};
          } else {
            event.profile = *selected;
            application::RunStore store;
            const std::filesystem::path cell_directory =
                library.directory / L"cells" / Utf8ToWide(cell_id);
            const core::Status recovery =
                store.RecoverInterrupted(cell_directory);
            if (!recovery.Ok()) {
              event.status = recovery;
            } else {
              auto runs = store.List(cell_directory);
              if (runs.Ok()) {
                std::vector<application::RunRecord> loaded_runs =
                    std::move(runs).Value();
                for (application::RunRecord& run : loaded_runs) {
                  // A cell owns its run store, but keep the project identity
                  // check here as a second boundary. It prevents a stale or
                  // copied run directory from making another cell's layout
                  // configuration appear in this window.
                  if (run.project_id != event.document->project.id) {
                    continue;
                  }
                  if (run.stage != "physical_implementation" &&
                      run.stage != "physical_design") {
                    if (run.stage == "physical_verification") {
                      event.verification_runs.push_back(std::move(run));
                    }
                    continue;
                  }
                  event.resumes.push_back(LoadResume(run));
                  event.run_metrics.push_back(LoadRunMetrics(run));
                  event.runs.push_back(std::move(run));
                }
              } else {
                event.status = runs.GetStatus();
              }
            }
          }
        }
      }
    }
    HWND target = nullptr;
    bool post = false;
    {
      std::scoped_lock lock(channel->mutex);
      if (channel->generation != generation || !channel->window) return;
      channel->events.push_back(std::move(event));
      target = channel->window;
      post = !channel->message_posted;
      channel->message_posted = true;
    }
    if (post) PostMessageW(target, kEventMessage, 0, 0);
  }));
}

void LayoutWindowImplementation::HandleEvents() {
  std::deque<EventChannel::Event> events;
  {
    std::scoped_lock lock(event_channel_->mutex);
    events.swap(event_channel_->events);
    event_channel_->message_posted = false;
  }
  for (EventChannel::Event& event : events) {
    if (event.generation != generation_) continue;
    if (event.loaded) {
      if (!event.status.Ok()) {
        SetControlText(status_, event.status.message);
        EnableWindow(run_button_, FALSE);
        continue;
      }
      if (!event.document ||
          event.document->project.library_id != request_.library_id ||
          event.document->project.cell_id != request_.cell_id ||
          event.document->project_path !=
              (library_.directory / L"cells" / Utf8ToWide(request_.cell_id) /
               L"project.dpproj")) {
        // Never publish a document resolved for another Cell, even if a
        // stale worker event survived a Library refresh.  The cell identity
        // and storage path are part of the UI session, not just a display
        // label.
        SetControlText(status_,
                       "Loaded project belongs to a different Cell or path");
        EnableWindow(run_button_, FALSE);
        continue;
      }
      document_ = std::move(event.document);
      sources_ = std::move(event.sources);
      profile_ = std::move(event.profile);
      toolchain_settings_ = std::move(event.toolchain_settings);
      runs_ = std::move(event.runs);
      verification_runs_ = std::move(event.verification_runs);
      run_metrics_ = std::move(event.run_metrics);
      resume_candidates_ = std::move(event.resumes);
      PopulateConfiguration();
      PopulateRuns();
      if (!runs_.empty()) SelectRun(0);
      PopulateVerification();
      const std::wstring ready_status = L"Cell " + Utf8ToWide(cell_name_) +
                                        L" is ready to generate or "
                                        L"update the layout";
      SetWindowTextW(status_, ready_status.c_str());
      ApplyState(application::ManagedFlowRunState::kIdle);
      const auto channel = event_channel_;
      const std::uint64_t generation = generation_;
      core::Status discovery_started = discovery_service_.Start(
          profile_, generation,
          [channel, generation](application::OpenLaneDiscoveryResult result) {
            HWND target = nullptr;
            bool post = false;
            {
              std::scoped_lock lock(channel->mutex);
              if (channel->generation != generation || !channel->window) return;
              EventChannel::Event event;
              event.generation = generation;
              event.discovered = true;
              event.discovery = std::move(result);
              channel->events.push_back(std::move(event));
              target = channel->window;
              post = !channel->message_posted;
              channel->message_posted = true;
            }
            if (post) PostMessageW(target, kEventMessage, 0, 0);
          });
      if (!discovery_started.Ok()) {
        SetControlText(status_, discovery_started.message);
      }
      if (!profile_.orfs_root.empty()) {
        const core::Status orfs_started = orfs_discovery_service_.Start(
            profile_, generation,
            [channel, generation](application::OrfsDiscoveryResult result) {
              HWND target = nullptr;
              bool post = false;
              {
                std::scoped_lock lock(channel->mutex);
                if (channel->generation != generation || !channel->window) {
                  return;
                }
                EventChannel::Event event;
                event.generation = generation;
                event.orfs_discovered = true;
                event.orfs_discovery = std::move(result);
                channel->events.push_back(std::move(event));
                target = channel->window;
                post = !channel->message_posted;
                channel->message_posted = true;
              }
              if (post) PostMessageW(target, kEventMessage, 0, 0);
            });
        if (!orfs_started.Ok()) SetControlText(status_, orfs_started.message);
      }
      continue;
    }
    if (event.discovered) {
      if (event.discovery.status.Ok()) {
        pdk_candidates_ = std::move(event.discovery.candidates);
        std::vector<std::string> pdks;
        for (const auto& candidate : pdk_candidates_) {
          if (std::find(pdks.begin(), pdks.end(), candidate.pdk) ==
              pdks.end()) {
            pdks.push_back(candidate.pdk);
          }
        }
        const std::wstring selected_pdk = ControlText(pdk_edit_);
        SendMessageW(pdk_edit_, CB_RESETCONTENT, 0, 0);
        for (const std::string& pdk : pdks) {
          const std::wstring value = Utf8ToWide(pdk);
          SendMessageW(pdk_edit_, CB_ADDSTRING, 0,
                       reinterpret_cast<LPARAM>(value.c_str()));
        }
        SetWindowTextW(pdk_edit_, selected_pdk.c_str());
        PopulateStandardCellLibraries();
      } else {
        SetControlText(status_, event.discovery.status.message);
      }
      continue;
    }
    if (event.orfs_discovered) {
      if (event.orfs_discovery.status.Ok()) {
        orfs_platform_candidates_ = std::move(event.orfs_discovery.candidates);
        const bool has_selected = std::any_of(
            orfs_platform_candidates_.begin(), orfs_platform_candidates_.end(),
            [this](const adapters::OrfsPlatformCandidate& candidate) {
              return candidate.runnable &&
                     candidate.name ==
                         document_->project.physical_implementation.orfs
                             .platform;
            });
        if (!has_selected) {
          SetWindowTextW(
              status_,
              L"The configured ORFS platform is not installed or incomplete");
        }
      }
      continue;
    }
    if (event.saved) {
      save_in_progress_ = false;
      if (!event.generate_after_save) {
        const core::Status result =
            event.status.Ok() &&
                    (!event.document || event.document != document_ ||
                     !event.saved_project)
                ? core::Status{core::ErrorCode::kInvalidArgument,
                               "Project save returned an invalid Cell snapshot",
                               0}
                : event.status;
        setup_.Saved(result, event.saved_project
                                 ? event.saved_project->physical_implementation
                                 : core::PhysicalImplementationConfiguration{});
      }
      if (!event.status.Ok()) {
        SetControlText(status_, event.status.message);
        ApplyState(application::ManagedFlowRunState::kFailed);
      } else if (!event.document || event.document != document_ ||
                 !event.saved_project) {
        SetWindowTextW(status_,
                       L"Project save returned an invalid cell snapshot");
        ApplyState(application::ManagedFlowRunState::kFailed);
      } else {
        // Publish the saved snapshot only on the owning UI thread. The
        // worker never mutates document_, so another cell/window cannot see a
        // transient draft or a partially updated configuration.
        document_->project = std::move(*event.saved_project);
        if (event.generate_after_save) {
          StartPreparedRun(event.resume, event.target_stage,
                           event.rebuild_from_stage, event.full_flow);
        } else {
          PopulateConfiguration();
          ApplyState(application::ManagedFlowRunState::kIdle);
          const std::wstring saved_status =
              L"Layout setup saved for Cell " + Utf8ToWide(cell_name_);
          SetWindowTextW(status_, saved_status.c_str());
          if (central_log_) {
            central_log_(L"[Layout Setup] Cell " + Utf8ToWide(cell_name_) +
                         L" saved to " + document_->project_path.wstring() +
                         L"\r\n");
          }
        }
      }
      continue;
    }
    if (event.viewer.completed || !event.viewer.output.empty()) {
      if (!event.viewer.output.empty() && central_log_) {
        central_log_(Utf8ToWide(event.viewer.output));
      }
      if (event.viewer.completed)
        SetControlText(status_, event.viewer.status.message);
      continue;
    }
    if (event.openroad_viewer.completed ||
        !event.openroad_viewer.output.empty()) {
      if (!event.openroad_viewer.output.empty() && central_log_) {
        central_log_(Utf8ToWide(event.openroad_viewer.output));
      }
      if (event.openroad_viewer.completed) {
        SetControlText(status_, event.openroad_viewer.status.message);
      }
      continue;
    }
    if (event.verification.completed || !event.verification.output.empty() ||
        event.verification.state !=
            application::PhysicalVerificationState::kIdle) {
      ApplyVerificationEvent(event.verification);
      continue;
    }
    const application::PhysicalImplementationEvent& flow = event.flow;
    if (flow.kind ==
        application::PhysicalImplementationEventKind::kStateChanged) {
      ApplyState(flow.state);
      if (IsActiveFlowState(flow.state) && document_) {
        std::wostringstream summary;
        summary << L"Layout Generation\r\n\r\nBackend: "
                << BackendDisplayName(
                       document_->project.physical_implementation.backend_id)
                << L"\r\nStatus: " << FlowStateDisplayName(flow.state)
                << L"\r\n\r\nCompleted Runs remain in the history table; "
                   L"the current Run is added when it finishes.";
        SetWindowTextW(summary_, summary.str().c_str());
      }
    } else if (flow.kind ==
               application::PhysicalImplementationEventKind::kOutput) {
      const std::string complete_output = runtime::TakeCompleteUtf8Chunk(
          flow.output, &flow_output_utf8_remainder_);
      if (!complete_output.empty()) {
        const std::wstring display_output = Utf8ToWide(complete_output);
        AppendOutput(display_output);
        if (central_log_) central_log_(display_output);
      }
    } else if (flow.kind ==
               application::PhysicalImplementationEventKind::kProgress) {
      const std::wstring step = Utf8ToWide(flow.progress.step_id);
      SendMessageW(stages_list_, LB_ADDSTRING, 0,
                   reinterpret_cast<LPARAM>(step.c_str()));
      SetControlText(status_, flow.progress.message);
      if (document_) {
        std::wostringstream summary;
        summary << L"Layout Generation\r\n\r\nBackend: "
                << BackendDisplayName(
                       document_->project.physical_implementation.backend_id)
                << L"\r\nStatus: Running\r\nCurrent step: " << step
                << L"\r\n\r\nCompleted Runs remain in the history table; "
                   L"the current Run is added when it finishes.";
        SetWindowTextW(summary_, summary.str().c_str());
      }
    } else if (flow.kind ==
               application::PhysicalImplementationEventKind::kCompleted) {
      if (!flow_output_utf8_remainder_.empty()) {
        AppendOutput(L"[truncated UTF-8 output]");
        if (central_log_) central_log_(L"[truncated UTF-8 output]");
        flow_output_utf8_remainder_.clear();
      }
      active_run_ = flow.run;
      metrics_ = flow.metrics;
      ApplyState(flow.state);
      std::wostringstream summary;
      summary
          << L"Layout Summary\r\n\r\n"
          << L"Generated: "
          << (flow.run ? Utf8ToWide(flow.run->finished_utc) : L"N/A")
          << L"\r\nBackend: "
          << BackendDisplayName(
                 flow.run
                     ? flow.run->tool
                     : document_->project.physical_implementation.backend_id)
          << L"\r\nBackend result: " << (flow.status.Ok() ? L"PASS" : L"FAILED")
          << L"\r\n"
          << L"Die area: " << Metric(metrics_.die_area) << L"\r\n"
          << L"Core area: " << Metric(metrics_.core_area) << L"\r\n"
          << L"Utilization: " << Metric(metrics_.utilization) << L"\r\n"
          << L"Instances: " << Metric(metrics_.instance_count) << L"\r\n"
          << L"Setup WNS/TNS: " << Metric(metrics_.setup_wns) << L" / "
          << Metric(metrics_.setup_tns) << L"\r\n"
          << L"Hold WNS/TNS: " << Metric(metrics_.hold_wns) << L" / "
          << Metric(metrics_.hold_tns) << L"\r\n"
          << L"Wire length: " << Metric(metrics_.wire_length) << L"\r\n"
          << L"Congestion global/detailed: "
          << Metric(metrics_.global_route_congestion) << L" / "
          << Metric(metrics_.detailed_route_congestion) << L"\r\n"
          << L"Overflow global/detailed: "
          << Metric(metrics_.global_route_overflow) << L" / "
          << Metric(metrics_.detailed_route_overflow) << L"\r\n"
          << L"Routing violations: " << Metric(metrics_.routing_violations)
          << L"\r\n"
          << L"Runtime/peak memory: " << Metric(metrics_.runtime_seconds)
          << L" s / " << Metric(metrics_.peak_memory_mb) << L" MB\r\n"
          << L"DRC/LVS: " << Metric(metrics_.drc_violations) << L" / "
          << Metric(metrics_.lvs_errors) << L"\r\n"
          << L"Timing setup/hold WNS (ns): " << Metric(metrics_.setup_wns)
          << L" / " << Metric(metrics_.hold_wns);
      if (!flow.status.Ok()) {
        summary << L"\r\n\r\nFailure stage: "
                << Utf8ToWide(flow.failure_stage.empty() ? "unknown"
                                                         : flow.failure_stage)
                << L"\r\nError code: "
                << Utf8ToWide(flow.failure_code.empty() ? "unknown"
                                                        : flow.failure_code)
                << L"\r\nReason: " << Utf8ToWide(flow.status.message);
        if (flow.run) summary << L"\r\nRun ID: " << Utf8ToWide(flow.run->id);
      }
      SetWindowTextW(summary_, summary.str().c_str());
      SetControlText(status_, flow.reused
                                  ? "Compatible GDS is already up to date"
                              : flow.status.Ok() ? "Layout generation completed"
                                                 : flow.status.message);
      MergeCompletedRun(flow.run);
    }
  }
}

void LayoutWindowImplementation::PopulateStandardCellLibraries() {
  const std::string selected_pdk = WideToUtf8(ControlText(pdk_edit_));
  const std::wstring selected_library = ControlText(scl_edit_);
  SendMessageW(scl_edit_, CB_RESETCONTENT, 0, 0);
  for (const auto& candidate : pdk_candidates_) {
    if (candidate.pdk != selected_pdk) continue;
    const std::wstring value = Utf8ToWide(candidate.standard_cell_library);
    SendMessageW(scl_edit_, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(value.c_str()));
  }
  SetWindowTextW(scl_edit_, selected_library.c_str());
}

void LayoutWindowImplementation::PopulateConfiguration() {
  if (!document_) return;
  const core::PhysicalImplementationConfiguration& config =
      document_->project.physical_implementation;
  SetControlText(pdk_edit_, config.pdk);
  SetControlText(scl_edit_, config.standard_cell_library);
  SetControlText(clocks_edit_, Join(config.clock_ports, ";"));
  SetControlText(period_edit_, config.clock_period_ns);
  SetControlText(utilization_edit_,
                 std::to_string(config.core_utilization_percent));
  SetControlText(density_edit_, config.placement_density_percent.value_or(""));
  SetControlText(die_area_edit_, Join(config.die_area, ","));
  SetControlText(advanced_edit_, config.advanced_overrides_json);
  SendMessageW(pnr_sdc_combo_, CB_RESETCONTENT, 0, 0);
  SendMessageW(signoff_sdc_combo_, CB_RESETCONTENT, 0, 0);
  SendMessageW(pnr_sdc_combo_, CB_ADDSTRING, 0,
               reinterpret_cast<LPARAM>(L"(None)"));
  SendMessageW(signoff_sdc_combo_, CB_ADDSTRING, 0,
               reinterpret_cast<LPARAM>(L"(None)"));
  sdc_candidates_.clear();
  for (const application::ResolvedSource& source : sources_) {
    std::wstring extension = source.windows_path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   ::towlower);
    if (!source.exists || source.view_kind != core::ViewKind::kConstraints ||
        extension != L".sdc") {
      continue;
    }
    sdc_candidates_.push_back(&source);
    const std::wstring label = Utf8ToWide(source.relative_path);
    SendMessageW(pnr_sdc_combo_, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(label.c_str()));
    SendMessageW(signoff_sdc_combo_, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(label.c_str()));
  }
  auto select = [this](HWND combo, const std::string& relative) {
    int index = 0;
    for (std::size_t candidate = 0; candidate < sdc_candidates_.size();
         ++candidate) {
      if (sdc_candidates_[candidate]->relative_path == relative) {
        index = static_cast<int>(candidate + 1);
        break;
      }
    }
    SendMessageW(combo, CB_SETCURSEL, index, 0);
  };
  select(pnr_sdc_combo_, config.pnr_sdc_path);
  select(signoff_sdc_combo_, config.signoff_sdc_path);
}

void LayoutWindowImplementation::EditSetup() {
  if (!document_) {
    SetControlText(status_, "The selected Cell project is still loading");
    return;
  }
  if (document_->read_only) {
    SetControlText(status_,
                   "This Cell is read-only; close the other owner before "
                   "saving setup");
    return;
  }
  if (flow_service_.IsActive() || save_in_progress_) {
    SetControlText(status_, "Wait for the active Cell operation to finish");
    return;
  }
  if (document_->project.library_id != request_.library_id ||
      document_->project.cell_id != request_.cell_id) {
    SetControlText(status_, "Project identity does not match this Cell");
    return;
  }
  core::PhysicalImplementationConfiguration configuration =
      document_->project.physical_implementation;
  setup_.Open(window_, instance_, configuration, sdc_candidates_,
              orfs_platform_candidates_, json_editor_,
              [this](const core::PhysicalImplementationConfiguration& value) {
                SaveSetup(value);
              });
}

void LayoutWindowImplementation::SaveSetup(
    core::PhysicalImplementationConfiguration configuration) {
  const auto failed = [&](std::string message) {
    core::Status status{core::ErrorCode::kInvalidArgument, std::move(message),
                        0};
    setup_.Saved(status, {});
    SetControlText(status_, status.message);
  };
  if (!document_) {
    failed("The selected Cell project is still loading");
    return;
  }
  if (document_->read_only) {
    failed("This Cell is read-only; close the other owner before saving setup");
    return;
  }
  if (flow_service_.IsActive() || save_in_progress_) {
    failed("Wait for the active Cell operation to finish");
    return;
  }
  if (document_->project.library_id != request_.library_id ||
      document_->project.cell_id != request_.cell_id) {
    failed("Project identity does not match this Cell");
    return;
  }
  core::Project project = document_->project;
  project.physical_implementation = std::move(configuration);
  save_in_progress_ = true;
  ApplyState(application::ManagedFlowRunState::kPreparing);
  const auto channel = event_channel_;
  const auto document = document_;
  const std::uint64_t generation = generation_;
  const bool queued =
      scheduler_.Submit([channel, document, project = std::move(project),
                         generation](std::stop_token token) mutable {
        EventChannel::Event event;
        event.generation = generation;
        event.saved = true;
        event.generate_after_save = false;
        event.document = document;
        if (token.stop_requested()) {
          event.status = {core::ErrorCode::kCancelled,
                          "Layout setup save cancelled", 0};
        } else {
          application::ProjectService service;
          auto saved = service.Save(event.document.get(), std::move(project));
          if (saved.Ok()) {
            event.saved_project = std::move(saved).Value();
          } else {
            event.status = saved.GetStatus();
          }
        }
        HWND target = nullptr;
        bool post = false;
        {
          std::scoped_lock lock(channel->mutex);
          if (channel->generation != generation || !channel->window) return;
          channel->events.push_back(std::move(event));
          target = channel->window;
          post = !channel->message_posted;
          channel->message_posted = true;
        }
        if (post) PostMessageW(target, kEventMessage, 0, 0);
      });
  if (!queued) {
    save_in_progress_ = false;
    setup_.Saved({core::ErrorCode::kInvalidArgument,
                  "Layout setup save queue is unavailable", 0},
                 {});
    SetWindowTextW(status_, L"Layout setup save queue is unavailable");
    ApplyState(application::ManagedFlowRunState::kFailed);
  }
}

void LayoutWindowImplementation::SaveAndStart(bool resume,
                                              core::StageId target_stage,
                                              bool rebuild_from_stage,
                                              bool full_flow) {
  if (!document_ || document_->read_only || flow_service_.IsActive() ||
      save_in_progress_) {
    return;
  }
  if (document_->project.library_id != request_.library_id ||
      document_->project.cell_id != request_.cell_id) {
    SetControlText(status_, "Project identity does not match this Cell");
    return;
  }
  discovery_service_.Cancel();
  if (resume && !selected_resume_) {
    SetWindowTextW(status_, L"Select a compatible failed Run to resume");
    return;
  }
  core::Project project = document_->project;
  core::Status validation = core::ValidateProject(project);
  adapters::OpenLane2Adapter openlane_adapter;
  if (validation.Ok()) {
    validation =
        project.physical_implementation.backend_id == "orfs"
            ? adapters::OrfsAdapter().ValidateAdvancedVariables(
                  project.physical_implementation.orfs.advanced_variables_json)
            : openlane_adapter.ValidateAdvancedOverrides(
                  project.physical_implementation.advanced_overrides_json);
  }
  if (!validation.Ok()) {
    SetControlText(status_, validation.message);
    return;
  }
  StartPreparedRun(resume, target_stage, rebuild_from_stage, full_flow);
}

void LayoutWindowImplementation::StartPreparedRun(bool resume,
                                                  core::StageId target_stage,
                                                  bool rebuild_from_stage,
                                                  bool full_flow) {
  if (!document_ || document_->project.library_id != request_.library_id ||
      document_->project.cell_id != request_.cell_id) {
    SetControlText(status_, "Project identity does not match this Cell");
    ApplyState(application::ManagedFlowRunState::kFailed);
    return;
  }
  flow_output_utf8_remainder_.clear();
  application::ManagedFlowRunRequest run_request;
  run_request.project = document_->project;
  run_request.profile = profile_;
  run_request.sources = sources_;
  run_request.library_directory = library_.directory;
  run_request.cell_directory =
      library_.directory / L"cells" / Utf8ToWide(request_.cell_id);
  const std::string provider_id =
      document_->project.physical_implementation.backend_id == "orfs"
          ? "orfs"
          : "openlane2";
  application::ToolchainInventoryService inventory;
  auto environment =
      inventory.Resolve(toolchain_settings_, profile_, provider_id);
  if (environment.Ok()) {
    run_request.environment_id = environment.Value().installation_id;
    run_request.environment_fingerprint = environment.Value().fingerprint;
  } else {
    const std::string root =
        provider_id == "orfs" ? profile_.orfs_root : profile_.openlane_root;
    auto custom_fingerprint = application::CalculateSha256(
        "custom:" + profile_.id + ":" + provider_id + ":" + root);
    run_request.environment_id = "custom." + profile_.id + "." + provider_id;
    run_request.environment_fingerprint =
        custom_fingerprint.Ok() ? custom_fingerprint.Value() : root;
  }
  run_request.generation = generation_;
  run_request.target_stage = target_stage;
  run_request.full_flow = full_flow;
  if (rebuild_from_stage) run_request.rebuild_from_stage = target_stage;
  if (resume) run_request.resume = selected_resume_;
  ListView_SetItemState(runs_list_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
  active_run_.reset();
  selected_resume_.reset();
  std::wostringstream running_summary;
  running_summary
      << L"Layout Generation\r\n\r\nBackend: "
      << BackendDisplayName(
             document_->project.physical_implementation.backend_id)
      << L"\r\nStatus: Starting\r\n\r\nCompleted Runs remain in the history "
         L"table; the current Run is added when it finishes.";
  SetWindowTextW(summary_, running_summary.str().c_str());
  const auto channel = event_channel_;
  const std::uint64_t generation = generation_;
  core::Status started = flow_service_.EnsureLayout(
      std::move(run_request),
      [channel, generation](application::PhysicalImplementationEvent flow) {
        HWND target = nullptr;
        bool post = false;
        {
          std::scoped_lock lock(channel->mutex);
          if (channel->generation != generation || !channel->window) return;
          if (flow.kind ==
                  application::PhysicalImplementationEventKind::kOutput &&
              !channel->events.empty() &&
              channel->events.back().flow.kind ==
                  application::PhysicalImplementationEventKind::kOutput) {
            std::string& pending = channel->events.back().flow.output;
            constexpr std::size_t kMaximumPendingOutput = 1024 * 1024;
            if (pending.size() < kMaximumPendingOutput) {
              pending.append(flow.output, 0,
                             std::min(flow.output.size(),
                                      kMaximumPendingOutput - pending.size()));
            }
          } else {
            EventChannel::Event event;
            event.generation = generation;
            event.flow = std::move(flow);
            channel->events.push_back(std::move(event));
          }
          target = channel->window;
          post = !channel->message_posted;
          channel->message_posted = true;
        }
        if (post) PostMessageW(target, kEventMessage, 0, 0);
      });
  if (!started.Ok()) {
    SetControlText(status_, started.message);
    ApplyState(application::ManagedFlowRunState::kFailed);
  }
}

void LayoutWindowImplementation::ApplyState(
    application::ManagedFlowRunState state) {
  const bool active = IsActiveFlowState(state);
  EnableWindow(run_button_, !active && document_ && !document_->read_only);
  EnableWindow(cancel_button_, active);
  EnableWindow(runs_list_, !active);
  EnableWindow(
      stages_button_,
      !active && document_ != nullptr &&
          document_->project.physical_implementation.backend_id == "orfs");
  EnableWindow(config_button_, !active && !save_in_progress_ && document_ &&
                                   !document_->read_only);
  EnableWindow(resume_button_, !active && active_run_ != nullptr &&
                                   !FindGds(*active_run_).empty());
}

void LayoutWindowImplementation::AppendOutput(std::wstring_view text) {
  constexpr int kMaximumVisibleLogCharacters = 2 * 1024 * 1024;
  int length = GetWindowTextLengthW(output_);
  if (length > kMaximumVisibleLogCharacters) {
    SetWindowTextW(output_,
                   L"[Earlier output remains available in raw.log]\r\n");
    length = GetWindowTextLengthW(output_);
  }
  SendMessageW(output_, EM_SETSEL, length, length);
  SendMessageW(output_, EM_REPLACESEL, FALSE,
               reinterpret_cast<LPARAM>(std::wstring(text).c_str()));
}

void LayoutWindowImplementation::MergeCompletedRun(
    const std::shared_ptr<application::RunRecord>& completed_run) {
  if (!completed_run) return;

  const auto existing = std::find_if(runs_.begin(), runs_.end(),
                                     [&completed_run](const auto& run) {
                                       return run.id == completed_run->id;
                                     });
  const std::optional<application::ManagedFlowResumeRequest> resume =
      LoadResume(*completed_run);
  std::size_t selected_index = 0;
  if (existing == runs_.end()) {
    runs_.insert(runs_.begin(), *completed_run);
    run_metrics_.insert(run_metrics_.begin(), metrics_);
    resume_candidates_.insert(resume_candidates_.begin(), resume);
  } else {
    selected_index =
        static_cast<std::size_t>(std::distance(runs_.begin(), existing));
    *existing = *completed_run;
    if (selected_index < run_metrics_.size()) {
      run_metrics_[selected_index] = metrics_;
    }
    if (selected_index < resume_candidates_.size()) {
      resume_candidates_[selected_index] = resume;
    }
  }
  active_run_ = completed_run;
  selected_resume_ = resume;
  PopulateRuns();
  if (selected_index < runs_.size()) {
    const int row = static_cast<int>(selected_index);
    ListView_SetItemState(runs_list_, row, LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(runs_list_, row, FALSE);
  }
  ApplyState(flow_service_.IsActive()
                 ? application::ManagedFlowRunState::kRunning
                 : application::ManagedFlowRunState::kIdle);
}

void LayoutWindowImplementation::PopulateRuns() {
  ListView_DeleteAllItems(runs_list_);
  for (std::size_t index = 0; index < runs_.size(); ++index) {
    std::wstring values[] = {
        Utf8ToWide(runs_[index].started_utc),
        Utf8ToWide(application::RunStatusName(runs_[index].status)),
        Utf8ToWide(runs_[index].id)};
    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = static_cast<int>(index);
    item.pszText = values[0].data();
    const int row = ListView_InsertItem(runs_list_, &item);
    for (int column = 1; column < 3 && row >= 0; ++column) {
      ListView_SetItemText(runs_list_, row, column, values[column].data());
    }
  }
}

void LayoutWindowImplementation::SelectPage(int index) {
  selected_page_ = std::clamp(index, 0, 2);
  ShowWindow(summary_, selected_page_ == 0 ? SW_SHOW : SW_HIDE);
  ShowWindow(runs_list_, selected_page_ == 2 ? SW_SHOW : SW_HIDE);
  const int verification_visibility = selected_page_ == 1 ? SW_SHOW : SW_HIDE;
  for (HWND control : {verification_source_, verification_list_,
                       verification_detail_, drc_button_, lvs_button_,
                       verify_all_button_, verification_cancel_button_}) {
    ShowWindow(control, verification_visibility);
  }
  RECT client{};
  GetClientRect(window_, &client);
  LayoutControls(client.right, client.bottom);
}

void LayoutWindowImplementation::PopulateVerification() {
  verification_recipes_ =
      document_ ? BuiltinVerificationRecipes(document_->project, profile_)
                : std::vector<core::VerificationRecipe>{};
  if (document_) {
    const std::string backend =
        document_->project.physical_implementation.backend_id;
    const std::string platform =
        backend == "orfs"
            ? document_->project.physical_implementation.orfs.platform
            : document_->project.physical_implementation.pdk;
    for (const core::VerificationRecipe& recipe :
         toolchain_settings_.verification_recipes) {
      if ((recipe.provider_id.empty() || recipe.provider_id == backend) &&
          (recipe.platform.empty() || recipe.platform == platform)) {
        verification_recipes_.push_back(recipe);
      }
    }
  }
  ListView_DeleteAllItems(verification_list_);
  for (std::size_t index = 0; index < verification_runs_.size(); ++index) {
    const application::RunRecord& run = verification_runs_[index];
    const std::string check =
        RunSummaryString(run, "check").value_or("unknown");
    const std::string recipe =
        RunSummaryString(run, "recipe_id").value_or(run.tool);
    std::optional<double> violations;
    std::optional<bool> parsed;
    std::optional<bool> passed;
    if (!run.outcome.summary_relative_path.empty()) {
      const std::filesystem::path summary_path =
          run.directory / run.outcome.summary_relative_path;
      std::ifstream input(summary_path, std::ios::binary);
      const std::string summary((std::istreambuf_iterator<char>(input)), {});
      violations = JsonNumber(summary, "violation_count");
      parsed = JsonBool(summary, "parsed");
      passed = JsonBool(summary, "passed");
    }
    const bool stale = active_run_ && run.source_run_id != active_run_->id;
    const bool has_violations = run.status == application::RunStatus::kFailed &&
                                parsed.value_or(false) &&
                                !passed.value_or(true);
    std::wstring values[] = {
        Utf8ToWide(check), Utf8ToWide(recipe),
        stale            ? L"Stale"
        : has_violations ? L"Violations"
                         : Utf8ToWide(application::RunStatusName(run.status)),
        violations ? std::to_wstring(static_cast<std::uint64_t>(*violations))
                   : L"-",
        Utf8ToWide(run.source_run_id)};
    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = static_cast<int>(index);
    item.pszText = values[0].data();
    const int row = ListView_InsertItem(verification_list_, &item);
    for (int column = 1; column < 5 && row >= 0; ++column) {
      ListView_SetItemText(verification_list_, row, column,
                           values[column].data());
    }
  }
  const bool source_ready = active_run_ && !FindGds(*active_run_).empty();
  const auto selected_recipe = [this](adapters::VerificationCheck check)
      -> const core::VerificationRecipe* {
    const std::string configured =
        check == adapters::VerificationCheck::kDrc
            ? document_->project.physical_verification.drc_recipe_id
            : document_->project.physical_verification.lvs_recipe_id;
    auto matches = [check](const core::VerificationRecipe& recipe) {
      return recipe.trusted && (check == adapters::VerificationCheck::kDrc
                                    ? recipe.engine.ends_with("drc")
                                    : recipe.engine.ends_with("lvs"));
    };
    if (!configured.empty()) {
      const auto found = std::find_if(
          verification_recipes_.begin(), verification_recipes_.end(),
          [&configured, &matches](const core::VerificationRecipe& recipe) {
            return recipe.id == configured && matches(recipe);
          });
      return found == verification_recipes_.end() ? nullptr : &*found;
    }
    const auto found = std::find_if(verification_recipes_.begin(),
                                    verification_recipes_.end(), matches);
    return found == verification_recipes_.end() ? nullptr : &*found;
  };
  const core::VerificationRecipe* drc_recipe =
      document_ ? selected_recipe(adapters::VerificationCheck::kDrc) : nullptr;
  const bool drc_ready = source_ready && drc_recipe != nullptr;
  const core::VerificationRecipe* lvs_recipe =
      document_ ? selected_recipe(adapters::VerificationCheck::kLvs) : nullptr;
  const bool lvs_requires_spice =
      lvs_recipe != nullptr && lvs_recipe->engine == "klayout_lvs";
  const std::filesystem::path schematic =
      active_run_ ? FindNetlist(*active_run_, lvs_requires_spice)
                  : std::filesystem::path{};
  const std::filesystem::path source_netlist =
      active_run_ ? FindNetlist(*active_run_, false) : std::filesystem::path{};
  const std::filesystem::path odb =
      active_run_ ? FindOdb(*active_run_) : std::filesystem::path{};
  const std::filesystem::path extracted =
      active_run_ ? FindNetlist(*active_run_, true) : std::filesystem::path{};
  const bool lvs_ready =
      source_ready && lvs_recipe != nullptr &&
      (lvs_requires_spice ? (!odb.empty() && !source_netlist.empty())
                          : !schematic.empty()) &&
      (lvs_recipe->engine != "netgen_lvs" || !extracted.empty());
  EnableWindow(drc_button_, drc_ready && !verification_service_.IsActive());
  EnableWindow(lvs_button_, lvs_ready && !verification_service_.IsActive());
  EnableWindow(verify_all_button_,
               drc_ready && lvs_ready && !verification_service_.IsActive());
  EnableWindow(verification_cancel_button_, verification_service_.IsActive());
  const std::wstring source =
      source_ready
          ? L"Verification source Layout Run: " + Utf8ToWide(active_run_->id)
          : L"Select a Run with a complete final GDS";
  SetWindowTextW(verification_source_, source.c_str());
  if (source_ready && !lvs_ready) {
    if (lvs_recipe == nullptr &&
        document_->project.physical_implementation.backend_id == "orfs" &&
        document_->project.physical_implementation.orfs.platform == "asap7") {
      SetWindowTextW(
          verification_detail_,
          L"LVS is unavailable for ORFS ASAP7: no validated LVS recipe is "
          L"registered. DRC results remain available.");
    } else if (lvs_recipe == nullptr) {
      SetWindowTextW(
          verification_detail_,
          L"LVS is unavailable because no compatible trusted rule is "
          L"registered for this backend/platform.");
    } else if (lvs_requires_spice && odb.empty()) {
      SetWindowTextW(
          verification_detail_,
          L"LVS requires the final ODB from the same Layout Run to generate "
          L"the CDL input.");
    } else if (lvs_requires_spice && source_netlist.empty()) {
      SetWindowTextW(
          verification_detail_,
          L"LVS requires the final netlist from the same Layout Run as input "
          L"evidence.");
    } else if (schematic.empty()) {
      SetWindowTextW(verification_detail_,
                     L"LVS requires a same-Layout-Run schematic/CDL artifact.");
    } else if (lvs_recipe->engine == "netgen_lvs" && extracted.empty()) {
      SetWindowTextW(
          verification_detail_,
          L"LVS requires an extracted netlist from the same Layout Run.");
    }
  }
}

void LayoutWindowImplementation::StartVerification(
    adapters::VerificationCheck check) {
  if (!document_ || !active_run_ || verification_service_.IsActive()) return;
  const std::string configured =
      check == adapters::VerificationCheck::kDrc
          ? document_->project.physical_verification.drc_recipe_id
          : document_->project.physical_verification.lvs_recipe_id;
  const auto recipe =
      std::find_if(verification_recipes_.begin(), verification_recipes_.end(),
                   [check, &configured](const core::VerificationRecipe& value) {
                     return (configured.empty() || configured == value.id) &&
                            value.trusted &&
                            (check == adapters::VerificationCheck::kDrc
                                 ? value.engine.ends_with("drc")
                                 : value.engine.ends_with("lvs"));
                   });
  if (recipe == verification_recipes_.end()) {
    SetWindowTextW(verification_detail_,
                   L"No trusted compatible verification rule is available.");
    return;
  }
  application::PhysicalVerificationRequest request;
  request.project = document_->project;
  request.profile = profile_;
  request.recipe = *recipe;
  request.check = check;
  request.source_run = *active_run_;
  request.cell_directory =
      library_.directory / L"cells" / Utf8ToWide(request_.cell_id);
  request.gds_path = FindGds(*active_run_);
  request.odb_path = FindOdb(*active_run_);
  request.source_netlist_path = FindNetlist(*active_run_, false);
  request.schematic_path =
      recipe->engine == "klayout_lvs"
          ? std::filesystem::path{}
          : FindNetlist(*active_run_, check == adapters::VerificationCheck::kLvs);
  request.extracted_path = FindNetlist(*active_run_, true);
  request.environment_id = active_run_->environment_id;
  request.environment_fingerprint = active_run_->environment_fingerprint;
  request.generation = generation_;
  const auto channel = event_channel_;
  const std::uint64_t generation = generation_;
  const core::Status started = verification_service_.Start(
      std::move(request),
      [channel, generation](application::PhysicalVerificationEvent result) {
        HWND target = nullptr;
        bool post = false;
        {
          std::scoped_lock lock(channel->mutex);
          if (channel->generation != generation || !channel->window) return;
          EventChannel::Event event;
          event.generation = generation;
          event.verification = std::move(result);
          channel->events.push_back(std::move(event));
          target = channel->window;
          post = !channel->message_posted;
          channel->message_posted = true;
        }
        if (post) PostMessageW(target, kEventMessage, 0, 0);
      });
  if (!started.Ok()) {
    run_lvs_after_drc_ = false;
    SetControlText(verification_detail_, started.message);
  } else {
    PopulateVerification();
  }
}

void LayoutWindowImplementation::ApplyVerificationEvent(
    const application::PhysicalVerificationEvent& event) {
  if (!event.output.empty() && central_log_) {
    central_log_(Utf8ToWide(event.output));
  }
  if (!event.completed) {
    EnableWindow(drc_button_, FALSE);
    EnableWindow(lvs_button_, FALSE);
    EnableWindow(verify_all_button_, FALSE);
    EnableWindow(verification_cancel_button_, TRUE);
    const wchar_t* message = L"Physical verification is running...";
    if (event.state == application::PhysicalVerificationState::kCdlGenerating) {
      message = L"Generating design CDL from the final ODB...";
    } else if (event.state ==
               application::PhysicalVerificationState::kCdlCombining) {
      message = L"Combining the design CDL with the platform cell model...";
    } else if (event.state == application::PhysicalVerificationState::kRunning) {
      message = L"Running LVS...";
    }
    SetWindowTextW(verification_detail_, message);
    return;
  }
  if (event.run) {
    verification_runs_.insert(verification_runs_.begin(), *event.run);
  }
  std::wostringstream detail;
  if (!event.status.Ok()) {
    detail << L"Verification failed: " << Utf8ToWide(event.status.message);
  } else {
    detail << (event.result.passed ? L"PASS" : L"VIOLATIONS")
           << L"\r\nViolation count: " << event.result.violation_count
           << L"\r\n"
           << Utf8ToWide(event.result.detail);
  }
  SetWindowTextW(verification_detail_, detail.str().c_str());
  const bool continue_with_lvs = run_lvs_after_drc_;
  run_lvs_after_drc_ = false;
  PopulateVerification();
  if (continue_with_lvs) StartVerification(adapters::VerificationCheck::kLvs);
}

void LayoutWindowImplementation::OpenVerificationResult(int index) {
  if (!active_run_ || index < 0 ||
      static_cast<std::size_t>(index) >= verification_runs_.size() ||
      viewer_service_.IsActive()) {
    return;
  }
  const application::RunRecord& verification_run = verification_runs_[index];
  if (verification_run.source_run_id != active_run_->id) {
    SetWindowTextW(verification_detail_,
                   L"This result belongs to another Layout Run.");
    return;
  }
  std::filesystem::path marker_database;
  for (const application::RunArtifact& artifact : verification_run.artifacts) {
    if (artifact.kind == "verification.report" &&
        (artifact.format == "lyrdb" || artifact.format == "lvsdb")) {
      marker_database = verification_run.directory / artifact.relative_path;
      break;
    }
  }
  if (marker_database.empty()) {
    ShellExecuteW(window_, L"open",
                  (verification_run.directory / L"reports").c_str(), nullptr,
                  nullptr, SW_SHOWNORMAL);
    return;
  }
  application::LayoutViewerRequest request;
  request.profile = profile_;
  request.gds_path = FindGds(*active_run_);
  request.generation = generation_;
  request.marker_database_path = marker_database;
  const auto channel = event_channel_;
  const std::uint64_t generation = generation_;
  const core::Status started = viewer_service_.Open(
      std::move(request),
      [channel, generation](application::LayoutViewerEvent viewer) {
        HWND target = nullptr;
        bool post = false;
        {
          std::scoped_lock lock(channel->mutex);
          if (channel->generation != generation || !channel->window) return;
          EventChannel::Event event;
          event.generation = generation;
          event.viewer = std::move(viewer);
          channel->events.push_back(std::move(event));
          target = channel->window;
          post = !channel->message_posted;
          channel->message_posted = true;
        }
        if (post) PostMessageW(target, kEventMessage, 0, 0);
      });
  if (!started.Ok()) SetControlText(verification_detail_, started.message);
}

void LayoutWindowImplementation::SelectRun(int index) {
  selected_resume_.reset();
  active_run_.reset();
  if (index >= 0 && static_cast<std::size_t>(index) < runs_.size()) {
    active_run_ = std::make_shared<application::RunRecord>(runs_[index]);
    if (static_cast<std::size_t>(index) < run_metrics_.size()) {
      metrics_ = run_metrics_[static_cast<std::size_t>(index)];
      std::wostringstream summary;
      summary << L"Layout Summary\r\n\r\nGenerated: "
              << Utf8ToWide(active_run_->finished_utc) << L"\r\nBackend: "
              << BackendDisplayName(active_run_->tool)
              << L"\r\nBackend result: "
              << (active_run_->outcome.result_succeeded ? L"PASS" : L"FAILED")
              << L"\r\nDie area: " << Metric(metrics_.die_area)
              << L"\r\nCore area: " << Metric(metrics_.core_area)
              << L"\r\nUtilization: " << Metric(metrics_.utilization)
              << L"\r\nCongestion global/detailed: "
              << Metric(metrics_.global_route_congestion) << L" / "
              << Metric(metrics_.detailed_route_congestion) << L"\r\nDRC/LVS: "
              << Metric(metrics_.drc_violations) << L" / "
              << Metric(metrics_.lvs_errors)
              << L"\r\nTiming setup/hold WNS (ns): "
              << Metric(metrics_.setup_wns) << L" / "
              << Metric(metrics_.hold_wns);
      const auto failure_stage =
          RunSummaryString(*active_run_, "failure_stage");
      const auto failure_message =
          RunSummaryString(*active_run_, "failure_message");
      const auto failure_code = RunSummaryString(*active_run_, "failure_code");
      if (failure_stage || failure_message) {
        summary << L"\r\n\r\nFailure stage: "
                << Utf8ToWide(failure_stage.value_or("unknown"))
                << L"\r\nError code: "
                << Utf8ToWide(failure_code.value_or("unknown"))
                << L"\r\nReason: "
                << Utf8ToWide(failure_message.value_or("Unknown failure"))
                << L"\r\nRun ID: " << Utf8ToWide(active_run_->id);
      }
      SetWindowTextW(summary_, summary.str().c_str());
    }
    if (static_cast<std::size_t>(index) < resume_candidates_.size()) {
      selected_resume_ = resume_candidates_[index];
    }
  }
  ApplyState(flow_service_.IsActive()
                 ? application::ManagedFlowRunState::kRunning
                 : application::ManagedFlowRunState::kIdle);
}

void LayoutWindowImplementation::ShowStages() {
  if (!document_ || flow_service_.IsActive()) return;
  if (document_->project.physical_implementation.backend_id != "orfs") {
    SetWindowTextW(status_, L"Stages is available only for the ORFS backend");
    return;
  }
  StageDialogAction action = StageDialogAction::kCancel;
  core::StageId stage = selected_stage_;
  std::array<bool, 6> checkpoint_available{};
  for (std::size_t index = 0; index < kPhysicalStages.size(); ++index) {
    checkpoint_available[index] =
        FindStageRun(runs_, active_run_.get(), kPhysicalStages[index]) !=
        nullptr;
  }
  if (!ShowStagesDialog(window_, &stage, &action,
                        BuildStageLabels(runs_, active_run_.get()),
                        checkpoint_available, selected_resume_.has_value())) {
    return;
  }
  selected_stage_ = stage;
  const application::RunRecord* stage_run =
      FindStageRun(runs_, active_run_.get(), selected_stage_);
  switch (action) {
    case StageDialogAction::kRunThrough:
      SaveAndStart(false, selected_stage_, false);
      break;
    case StageDialogAction::kRebuildFrom:
      if (!selected_resume_) {
        SetWindowTextW(status_,
                       L"Select a failed ORFS Run with a checkpoint first");
        return;
      }
      SaveAndStart(true, selected_stage_, true);
      break;
    case StageDialogAction::kOpenInOpenRoad:
      if (!stage_run) {
        SetWindowTextW(status_, L"Select a Run with a checkpoint first");
        return;
      }
      {
        const std::filesystem::path odb =
            FindStageCheckpoint(*stage_run, selected_stage_);
        if (odb.empty()) {
          SetWindowTextW(status_,
                         L"The selected stage has no valid ODB checkpoint");
          return;
        }
        const auto channel = event_channel_;
        const std::uint64_t generation = generation_;
        application::OpenRoadViewerRequest request{
            profile_, odb,
            adapters::OrfsAdapter().GuiTargetForStage(selected_stage_),
            generation};
        request.orfs_root = profile_.orfs_root;
        request.flow_variant =
            document_->project.physical_implementation.orfs.flow_variant;
        request.config_path = stage_run->directory / "artifacts" /
                              std::filesystem::path("config.mk");
        if (const auto lineage = RunSummaryString(*stage_run, "lineage_id")) {
          request.backend_workspace =
              ".designpp/runs/orfs/" + document_->project.id + "/" + *lineage;
        }
        const core::Status started = openroad_viewer_service_.Open(
            std::move(request),
            [channel, generation](application::OpenRoadViewerEvent viewer) {
              HWND target = nullptr;
              bool post = false;
              {
                std::scoped_lock lock(channel->mutex);
                if (channel->generation != generation || !channel->window)
                  return;
                EventChannel::Event event;
                event.generation = generation;
                event.openroad_viewer = std::move(viewer);
                channel->events.push_back(std::move(event));
                target = channel->window;
                post = !channel->message_posted;
                channel->message_posted = true;
              }
              if (post) PostMessageW(target, kEventMessage, 0, 0);
            });
        if (!started.Ok()) SetControlText(status_, started.message);
      }
      break;
    case StageDialogAction::kReports:
      if (stage_run) {
        ShellExecuteW(window_, L"open",
                      (stage_run->directory / "reports").c_str(), nullptr,
                      nullptr, SW_SHOWNORMAL);
      }
      break;
    case StageDialogAction::kArtifacts:
      if (stage_run) {
        ShellExecuteW(window_, L"open",
                      (stage_run->directory / "artifacts").c_str(), nullptr,
                      nullptr, SW_SHOWNORMAL);
      }
      break;
    case StageDialogAction::kCancel:
      break;
  }
}

void LayoutWindowImplementation::ShowReports() {
  if (!active_run_) return;
  const std::filesystem::path path = active_run_->directory / "reports";
  ShellExecuteW(window_, L"open", path.c_str(), nullptr, nullptr,
                SW_SHOWNORMAL);
}

void LayoutWindowImplementation::ShowArtifacts() {
  if (!active_run_) return;
  const std::filesystem::path path = active_run_->directory / "artifacts";
  ShellExecuteW(window_, L"open", path.c_str(), nullptr, nullptr,
                SW_SHOWNORMAL);
}

void LayoutWindowImplementation::OpenLayout() {
  if (!active_run_ || viewer_service_.IsActive()) return;
  std::filesystem::path gds = FindGds(*active_run_);
  if (gds.empty()) {
    SetWindowTextW(status_, L"Generate or update the layout before opening it");
    return;
  }
  const auto channel = event_channel_;
  const std::uint64_t generation = generation_;
  application::LayoutViewerRequest request{profile_, std::move(gds),
                                           generation};
  const core::Status started = viewer_service_.Open(
      std::move(request),
      [channel, generation](application::LayoutViewerEvent viewer) {
        HWND target = nullptr;
        bool post = false;
        {
          std::scoped_lock lock(channel->mutex);
          if (channel->generation != generation || !channel->window) return;
          EventChannel::Event event;
          event.generation = generation;
          event.viewer = std::move(viewer);
          channel->events.push_back(std::move(event));
          target = channel->window;
          post = !channel->message_posted;
          channel->message_posted = true;
        }
        if (post) PostMessageW(target, kEventMessage, 0, 0);
      });
  if (!started.Ok()) SetControlText(status_, started.message);
}

void LayoutWindowImplementation::ShutdownChannel() {
  if (!event_channel_) return;
  std::scoped_lock lock(event_channel_->mutex);
  event_channel_->window = nullptr;
  event_channel_->events.clear();
}

}  // namespace designpp::gui

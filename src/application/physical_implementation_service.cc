// Copyright 2026 The Design++ Authors

#include "designpp/application/physical_implementation_service.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <optional>
#include <sstream>
#include <utility>

#include "designpp/application/prepared_physical_inputs.h"
#include "designpp/application/synthesis_fingerprint.h"

namespace designpp::application {
namespace {

std::filesystem::path Utf8Path(std::string_view text) {
  std::u8string value;
  value.reserve(text.size());
  for (char character : text) value.push_back(static_cast<char8_t>(character));
  return std::filesystem::path(value);
}

bool IsPhysicalRun(const RunRecord& run) {
  return run.stage == "physical_implementation" ||
         run.stage == "physical_design";
}

std::filesystem::path FindGds(const RunRecord& run) {
  for (const RunArtifact& artifact : run.artifacts) {
    if (artifact.partial) continue;
    if (artifact.kind == "final.gds" || artifact.format == ".gds" ||
        artifact.format == "gds") {
      const std::filesystem::path path =
          run.directory / std::filesystem::path(artifact.relative_path);
      std::error_code error;
      if (std::filesystem::is_regular_file(path, error) &&
          std::filesystem::file_size(path, error) > 0) {
        return path;
      }
    }
  }
  return {};
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
  const std::size_t end = json.find('"', quote + 1);
  if (end == std::string_view::npos) return std::nullopt;
  return std::string(json.substr(quote + 1, end - quote - 1));
}

std::optional<std::uint64_t> JsonUnsigned(std::string_view json,
                                          std::string_view key) {
  const std::string token = "\"" + std::string(key) + "\"";
  const std::size_t key_position = json.find(token);
  if (key_position == std::string_view::npos) return std::nullopt;
  const std::size_t colon = json.find(':', key_position + token.size());
  if (colon == std::string_view::npos) return std::nullopt;
  const std::size_t begin = json.find_first_of("0123456789", colon + 1);
  if (begin == std::string_view::npos) return std::nullopt;
  const std::size_t end = json.find_first_not_of("0123456789", begin);
  std::uint64_t value = 0;
  const char* first = json.data() + begin;
  const char* last =
      json.data() + (end == std::string_view::npos ? json.size() : end);
  const auto parsed = std::from_chars(first, last, value);
  return parsed.ec == std::errc{} && parsed.ptr == last
             ? std::optional<std::uint64_t>{value}
             : std::nullopt;
}

std::optional<std::string> ReadSummary(const RunRecord& run) {
  if (run.outcome.summary_relative_path.empty()) return std::nullopt;
  std::ifstream input(run.directory / run.outcome.summary_relative_path,
                      std::ios::binary);
  if (!input) return std::nullopt;
  std::ostringstream output;
  output << input.rdbuf();
  return output.str();
}

std::filesystem::path FindCheckpointArtifact(const RunRecord& run) {
  std::filesystem::path newest;
  std::error_code error;
  std::filesystem::file_time_type newest_time{};
  for (const RunArtifact& artifact : run.artifacts) {
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
    const auto modified = std::filesystem::last_write_time(path, error);
    if (!error && (newest.empty() || modified > newest_time)) {
      newest = path;
      newest_time = modified;
    }
  }
  return newest;
}

std::string OrfsTargetForStage(core::StageId stage) {
  switch (stage) {
    case core::StageId::kSynthesis:
      return "synth";
    case core::StageId::kFloorplan:
      return "floorplan";
    case core::StageId::kPlacement:
      return "place";
    case core::StageId::kClockTreeSynthesis:
      return "cts";
    case core::StageId::kRouting:
      return "route";
    case core::StageId::kFinalOutputs:
      return "finish";
    default:
      return {};
  }
}

std::string ExpectedOrfsTarget(const ManagedFlowRunRequest& request) {
  return request.full_flow &&
                 request.target_stage == core::StageId::kFinalOutputs
             ? "all"
             : OrfsTargetForStage(request.target_stage);
}

int OrfsStageRank(std::string_view target) {
  if (target == "synth") return 0;
  if (target == "floorplan") return 1;
  if (target == "place") return 2;
  if (target == "cts") return 3;
  if (target == "route") return 4;
  if (target == "finish" || target == "all") return 5;
  return -1;
}

bool SummaryTargetMatches(const RunRecord& run,
                          const ManagedFlowRunRequest& request) {
  if (request.project.physical_implementation.backend_id != "orfs") {
    return request.target_stage == core::StageId::kFinalOutputs;
  }
  const auto summary = ReadSummary(run);
  if (!summary) return false;
  const auto target = JsonString(*summary, "target_stage");
  return target && *target == ExpectedOrfsTarget(request);
}

bool InputsAreOlderThan(const ManagedFlowRunRequest& request,
                        const std::filesystem::path& artifact) {
  std::error_code error;
  const auto artifact_time = std::filesystem::last_write_time(artifact, error);
  if (error) return false;
  for (const ResolvedSource& source : request.sources) {
    if (!source.enabled || !source.exists) continue;
    const auto source_time =
        std::filesystem::last_write_time(source.windows_path, error);
    if (error || source_time > artifact_time) return false;
  }
  for (const std::string& include : request.project.include_directories) {
    const std::filesystem::path directory =
        request.library_directory / Utf8Path(include);
    std::filesystem::recursive_directory_iterator iterator(
        directory, std::filesystem::directory_options::skip_permission_denied,
        error);
    if (error) return false;
    for (const auto& entry : iterator) {
      if (!entry.is_regular_file(error) || error) {
        if (error) return false;
        continue;
      }
      const auto include_time = entry.last_write_time(error);
      if (error || include_time > artifact_time) return false;
    }
  }
  const std::string sdc =
      request.project.physical_implementation.pnr_sdc_path.empty()
          ? request.project.physical_implementation.signoff_sdc_path
          : request.project.physical_implementation.pnr_sdc_path;
  if (!sdc.empty()) {
    const auto sdc_time = std::filesystem::last_write_time(
        request.library_directory / Utf8Path(sdc), error);
    if (error || sdc_time > artifact_time) return false;
  }
  return true;
}

bool SummaryMatches(const RunRecord& run, const ManagedFlowRunRequest& request,
                    std::string* fingerprint) {
  if (run.project_id != request.project.id) return false;
  const auto summary = ReadSummary(run);
  if (!summary) return false;
  const auto revision = JsonUnsigned(*summary, "project_revision");
  const auto backend = JsonString(*summary, "backend_id");
  const auto library_id = JsonString(*summary, "library_id");
  const auto cell_id = JsonString(*summary, "cell_id");
  const auto value = JsonString(*summary, "configuration_fingerprint");
  if (!backend ||
      *backend != request.project.physical_implementation.backend_id ||
      !value || value->empty()) {
    return false;
  }
  if (*backend != "orfs" &&
      (!revision || *revision != request.project.revision)) {
    return false;
  }
  // Identity fields were added after the first physical-flow summaries.  A
  // present field must match the selected cell; an old summary remains
  // eligible only when its RunStore/project identity still matches below.
  if ((library_id && *library_id != request.project.library_id) ||
      (cell_id && *cell_id != request.project.cell_id)) {
    return false;
  }
  if (*backend == "orfs") {
    const auto tool_version = JsonString(*summary, "tool_version");
    const auto contract = JsonString(*summary, "configuration_contract_hex");
    if (!tool_version || !contract ||
        *contract != EncodePhysicalImplementationContract(
                         BuildPhysicalImplementationConfigurationContract(
                             request, *tool_version))) {
      return false;
    }
    auto prepared =
        request.prepared_inputs &&
                request.prepared_inputs->tool_version == *tool_version
            ? core::Result<std::shared_ptr<const PreparedPhysicalInputs>>(
                  request.prepared_inputs)
            : PrepareOrfsPhysicalInputs(request, *tool_version);
    if (!prepared.Ok() || prepared.Value()->fingerprint != *value) {
      return false;
    }
  }
  *fingerprint = *value;
  return true;
}

}  // namespace

std::string BuildPhysicalImplementationConfigurationContract(
    const ManagedFlowRunRequest& request, std::string_view tool_version) {
  std::ostringstream output;
  const auto append = [&output](std::string_view name, std::string_view value) {
    output << name.size() << ':' << name << value.size() << ':' << value
           << '\n';
  };
  const auto append_optional = [&append](
                                   std::string_view name,
                                   const std::optional<std::string>& value) {
    append(name, value.value_or(""));
  };
  const auto append_vector = [&append](std::string_view name,
                                       const std::vector<std::string>& values) {
    const std::string count_name = std::string(name) + ".count";
    append(count_name, std::to_string(values.size()));
    for (const std::string& value : values) append(name, value);
  };
  const auto configuration =
      core::ResolvePhysicalDefaults(request.project.physical_implementation);
  append("contract_version", "4");
  append("project_id", request.project.id);
  append("library_id", request.project.library_id);
  append("cell_id", request.project.cell_id);
  append("backend_id", configuration.backend_id);
  append("tool_version", tool_version);
  append("profile_id", request.profile.id);
  append("wsl_distribution", request.profile.wsl_distribution);
  append("top_module", request.project.top_module);
  if (configuration.backend_id != "orfs" ||
      configuration.die_area.size() != 4 ||
      configuration.core_area.size() != 4) {
    append("core_utilization_percent",
           std::to_string(configuration.core_utilization_percent));
  }
  append_optional("placement_density_percent",
                  configuration.placement_density_percent);
  append_vector("die_area", configuration.die_area);
  append_vector("core_area", configuration.core_area);
  if (configuration.backend_id == "orfs") {
    append("managed_sdc_path", configuration.pnr_sdc_path.empty()
                                   ? configuration.signoff_sdc_path
                                   : configuration.pnr_sdc_path);
    append("orfs_root", request.profile.orfs_root);
    append("orfs_mode", request.profile.orfs_mode);
    append("orfs_bundle_id", request.profile.orfs_bundle_id);
    append("orfs_platform", configuration.orfs.platform);
    append("orfs_flow_variant", configuration.orfs.flow_variant);
    append("orfs_advanced_variables",
           configuration.orfs.advanced_variables_json);
  } else {
    append("pnr_sdc_path", configuration.pnr_sdc_path);
    append("signoff_sdc_path", configuration.signoff_sdc_path);
    append_vector("automatic_fields", configuration.automatic_fields);
    append("openlane_root", request.profile.openlane_root);
    append("pdk_root", request.profile.pdk_root);
    append("pdk", configuration.pdk);
    append("standard_cell_library", configuration.standard_cell_library);
    append("clock_period_ns", configuration.clock_period_ns);
    append_vector("clock_port", configuration.clock_ports);
    append_optional("tap_cell_distance_um", configuration.tap_cell_distance_um);
    append("openlane_advanced_overrides",
           configuration.advanced_overrides_json);

    const core::PowerDistributionConfiguration& pdn =
        configuration.power_distribution;
    append("pdn_multilayer", pdn.multilayer ? "1" : "0");
    append("pdn_core_ring", pdn.core_ring ? "1" : "0");
    append("pdn_enable_rails", pdn.enable_rails ? "1" : "0");
    append_optional("pdn_vertical_width_um", pdn.vertical_width_um);
    append_optional("pdn_horizontal_width_um", pdn.horizontal_width_um);
    append_optional("pdn_vertical_spacing_um", pdn.vertical_spacing_um);
    append_optional("pdn_horizontal_spacing_um", pdn.horizontal_spacing_um);
    append_optional("pdn_vertical_pitch_um", pdn.vertical_pitch_um);
    append_optional("pdn_horizontal_pitch_um", pdn.horizontal_pitch_um);
    append_optional("pdn_vertical_offset_um", pdn.vertical_offset_um);
    append_optional("pdn_horizontal_offset_um", pdn.horizontal_offset_um);

    const core::IoPlacementConfiguration& io = configuration.io_placement;
    append("io_algorithm", io.algorithm);
    append_optional("io_minimum_distance_um", io.minimum_distance_um);
    append_optional("io_vertical_length_um", io.vertical_length_um);
    append_optional("io_horizontal_length_um", io.horizontal_length_um);
    append_optional("io_vertical_thickness_multiplier",
                    io.vertical_thickness_multiplier);
    append_optional("io_horizontal_thickness_multiplier",
                    io.horizontal_thickness_multiplier);
    append_optional("io_vertical_extension_um", io.vertical_extension_um);
    append_optional("io_horizontal_extension_um", io.horizontal_extension_um);
    append_optional("io_vertical_layer", io.vertical_layer);
    append_optional("io_horizontal_layer", io.horizontal_layer);
    append("io_unmatched_policy", io.unmatched_policy);
    const auto append_side = [&](std::string_view side,
                                 const core::IoPinSideConfiguration& value) {
      const std::string side_name(side);
      append(side_name + ".bit_major", value.bit_major ? "1" : "0");
      append_optional(side_name + ".minimum_distance_um",
                      value.minimum_distance_um);
      append_vector(side_name + ".entry", value.entries);
    };
    append_side("io_north", io.north);
    append_side("io_south", io.south);
    append_side("io_east", io.east);
    append_side("io_west", io.west);
  }

  append_vector("include_directory", request.project.include_directories);
  append_vector("define", request.project.defines);
  append_vector("parameter", request.project.parameters);
  for (const ResolvedSource& source : request.sources) {
    append("source_path", source.relative_path);
    append("source_enabled", source.enabled ? "1" : "0");
    append("source_kind", std::to_string(static_cast<int>(source.view_kind)));
  }
  return output.str();
}

std::string EncodePhysicalImplementationContract(std::string_view contract) {
  constexpr char kHex[] = "0123456789abcdef";
  std::string encoded;
  encoded.reserve(contract.size() * 2);
  for (const unsigned char value : contract) {
    encoded.push_back(kHex[value >> 4]);
    encoded.push_back(kHex[value & 0x0f]);
  }
  return encoded;
}

std::optional<ManagedFlowResumeRequest> BuildPhysicalImplementationResume(
    const RunRecord& run, const ManagedFlowRunRequest& request) {
  if (run.status != RunStatus::kFailed &&
      run.status != RunStatus::kInterrupted) {
    return std::nullopt;
  }
  std::string fingerprint;
  if (!SummaryMatches(run, request, &fingerprint)) return std::nullopt;
  const auto summary = ReadSummary(run);
  const auto lineage = JsonString(*summary, "lineage_id");
  const auto backend = JsonString(*summary, "backend_id");
  if (backend && *backend == "orfs") {
    const auto target = JsonString(*summary, "target_stage");
    const int source_rank = target ? OrfsStageRank(*target) : -1;
    const int requested_rank =
        OrfsStageRank(OrfsTargetForStage(request.target_stage));
    // A failed ORFS target may have produced a valid checkpoint for that
    // target (or an earlier one).  Allow a later Run Through request to use
    // it, while never attempting to resume from a checkpoint beyond the
    // requested target.
    if (source_rank < 0 || requested_rank < 0 || source_rank > requested_rank) {
      return std::nullopt;
    }
  }
  auto step = JsonString(*summary, "last_step");
  if ((!step || step->empty()) && backend && *backend == "orfs") {
    step = JsonString(*summary, "target_stage");
  }
  if (!lineage || lineage->empty() || !step || step->empty()) {
    return std::nullopt;
  }
  const std::filesystem::path checkpoint =
      backend && *backend == "orfs"
          ? FindCheckpointArtifact(run)
          : run.directory / "artifacts" / "checkpoint-state.json";
  if (checkpoint.empty()) return std::nullopt;
  auto hash = CalculateFileSha256(checkpoint);
  if (!hash.Ok()) return std::nullopt;
  return ManagedFlowResumeRequest{run.id, *lineage, *step, fingerprint,
                                  std::move(hash).Value()};
}

// Returns a checkpoint from an earlier successful ORFS target so a later
// Run Through request can continue the same backend lineage.  The target is
// intentionally kept out of the ORFS input fingerprint; only the requested
// make target changes between these attempts.
std::optional<ManagedFlowResumeRequest> BuildOrfsStageResume(
    const RunRecord& run, const ManagedFlowRunRequest& request) {
  if (request.project.physical_implementation.backend_id != "orfs" ||
      run.status != RunStatus::kSucceeded || !run.outcome.result_succeeded) {
    return std::nullopt;
  }
  std::string fingerprint;
  if (!SummaryMatches(run, request, &fingerprint)) return std::nullopt;
  const auto summary = ReadSummary(run);
  if (!summary) return std::nullopt;
  const auto source_target = JsonString(*summary, "target_stage");
  const int source_rank = source_target ? OrfsStageRank(*source_target) : -1;
  const int target_rank =
      OrfsStageRank(OrfsTargetForStage(request.target_stage));
  if (source_rank < 0 || target_rank <= source_rank) return std::nullopt;
  const auto lineage = JsonString(*summary, "lineage_id");
  if (!lineage || lineage->empty()) return std::nullopt;
  auto step = JsonString(*summary, "last_step");
  if (!step || step->empty()) step = source_target;
  if (!step || step->empty()) return std::nullopt;
  const std::filesystem::path checkpoint = FindCheckpointArtifact(run);
  if (checkpoint.empty()) return std::nullopt;
  auto hash = CalculateFileSha256(checkpoint);
  if (!hash.Ok()) return std::nullopt;
  return ManagedFlowResumeRequest{run.id, *lineage, *step, fingerprint,
                                  std::move(hash).Value()};
}

PhysicalImplementationService::PhysicalImplementationService(
    runtime::ExecutionProvider* provider)
    : managed_flow_(provider) {}

PhysicalImplementationService::~PhysicalImplementationService() { Shutdown(); }

core::Status PhysicalImplementationService::EnsureLayout(
    ManagedFlowRunRequest request, PhysicalImplementationEventSink sink) {
  if (!sink || request.generation == 0) {
    return {core::ErrorCode::kInvalidArgument,
            "Layout request identity is incomplete", 0};
  }
  PhysicalImplementationEventSink current_sink;
  {
    std::scoped_lock lock(mutex_);
    if (shutdown_ || active_) {
      return {core::ErrorCode::kConflict,
              "A physical implementation request is already active", 0};
    }
    active_ = true;
    terminal_delivered_ = false;
    generation_ = request.generation;
    operation_stop_source_ = std::stop_source{};
    sink_ = std::move(sink);
    current_sink = sink_;
  }
  PhysicalImplementationEvent checking;
  checking.generation = request.generation;
  checking.state = ManagedFlowRunState::kPreparing;
  current_sink(std::move(checking));
  std::stop_token operation_stop_token;
  {
    std::scoped_lock lock(mutex_);
    operation_stop_token = operation_stop_source_.get_token();
  }
  const bool queued = scheduler_.Submit(
      [this, request = std::move(request),
       operation_stop_token](std::stop_token scheduler_stop_token) mutable {
        PhysicalImplementationEventSink sink;
        {
          std::scoped_lock lock(mutex_);
          sink = sink_;
        }
        InspectAndStart(std::move(request), std::move(sink),
                        scheduler_stop_token, operation_stop_token);
      });
  if (!queued) {
    std::scoped_lock lock(mutex_);
    active_ = false;
    sink_ = {};
    return {core::ErrorCode::kConflict,
            "Layout preparation queue is unavailable", 0};
  }
  return core::Status::Success();
}

void PhysicalImplementationService::InspectAndStart(
    ManagedFlowRunRequest request, PhysicalImplementationEventSink sink,
    std::stop_token scheduler_stop_token,
    std::stop_token operation_stop_token) {
  const auto complete_cancelled =
      [this, &request](PhysicalImplementationEventSink sink) {
        PhysicalImplementationEvent event;
        event.kind = PhysicalImplementationEventKind::kCompleted;
        event.state = ManagedFlowRunState::kCancelled;
        event.generation = request.generation;
        event.status = {core::ErrorCode::kCancelled,
                        "Physical implementation cancelled", 0};
        {
          std::scoped_lock lock(mutex_);
          if (shutdown_ || generation_ != request.generation ||
              terminal_delivered_) {
            return;
          }
          terminal_delivered_ = true;
          active_ = false;
        }
        if (sink) sink(std::move(event));
      };
  if (scheduler_stop_token.stop_requested() ||
      operation_stop_token.stop_requested()) {
    complete_cancelled(std::move(sink));
    return;
  }
  RunStore store;
  auto listed = store.List(request.cell_directory);
  if (listed.Ok()) {
    std::vector<RunRecord> runs = std::move(listed).Value();
    if (request.project.physical_implementation.backend_id == "orfs" &&
        !request.prepared_inputs) {
      for (const RunRecord& run : runs) {
        if (!IsPhysicalRun(run)) continue;
        const auto summary = ReadSummary(run);
        if (!summary) continue;
        const auto backend = JsonString(*summary, "backend_id");
        const auto tool_version = JsonString(*summary, "tool_version");
        if (!backend || *backend != "orfs" || !tool_version) continue;
        auto prepared = PrepareOrfsPhysicalInputs(request, *tool_version);
        if (prepared.Ok())
          request.prepared_inputs = std::move(prepared).Value();
        break;
      }
    }
    for (const RunRecord& run : runs) {
      if (!IsPhysicalRun(run)) continue;
      if (run.status == RunStatus::kSucceeded && run.outcome.result_succeeded &&
          SummaryTargetMatches(run, request)) {
        const std::filesystem::path gds = FindGds(run);
        const std::filesystem::path reusable =
            request.project.physical_implementation.backend_id == "orfs"
                ? FindCheckpointArtifact(run)
                : gds;
        std::string fingerprint;
        const bool orfs_managed_environment =
            request.project.physical_implementation.backend_id == "orfs" &&
            request.profile.orfs_mode == "managed" &&
            !request.profile.orfs_bundle_id.empty();
        const bool content_compatible =
            orfs_managed_environment ||
            (request.project.physical_implementation.backend_id != "orfs" &&
             InputsAreOlderThan(request, reusable));
        if (!reusable.empty() && content_compatible &&
            SummaryMatches(run, request, &fingerprint)) {
          PhysicalImplementationEvent event;
          event.kind = PhysicalImplementationEventKind::kCompleted;
          event.state = ManagedFlowRunState::kSucceeded;
          event.generation = request.generation;
          event.status = core::Status::Success();
          event.run = std::make_shared<RunRecord>(run);
          event.gds_path = gds;
          event.reused = true;
          {
            std::scoped_lock lock(mutex_);
            if (shutdown_ || generation_ != request.generation ||
                terminal_delivered_) {
              return;
            }
            terminal_delivered_ = true;
            active_ = false;
          }
          sink(std::move(event));
          return;
        }
      }
      if (!request.resume &&
          request.project.physical_implementation.backend_id == "orfs") {
        request.resume = BuildOrfsStageResume(run, request);
      }
      if (!request.resume) {
        request.resume = BuildPhysicalImplementationResume(run, request);
      }
      if (request.resume) break;
    }
  }
  if (scheduler_stop_token.stop_requested() ||
      operation_stop_token.stop_requested()) {
    complete_cancelled(std::move(sink));
    return;
  }
  {
    std::scoped_lock lock(mutex_);
    if (shutdown_ || generation_ != request.generation || terminal_delivered_) {
      return;
    }
  }
  const core::Status started = managed_flow_.Start(
      request,
      [this](ManagedFlowRunEvent event) { Forward(std::move(event)); });
  if (started.Ok() && operation_stop_token.stop_requested()) {
    managed_flow_.Cancel();
    return;
  }
  if (!started.Ok()) {
    PhysicalImplementationEvent event;
    event.kind = PhysicalImplementationEventKind::kCompleted;
    event.state = ManagedFlowRunState::kFailed;
    event.generation = request.generation;
    event.status = started;
    PhysicalImplementationEventSink current_sink;
    {
      std::scoped_lock lock(mutex_);
      if (terminal_delivered_) return;
      terminal_delivered_ = true;
      active_ = false;
      current_sink = sink_;
    }
    if (current_sink) current_sink(std::move(event));
  }
}

void PhysicalImplementationService::Forward(ManagedFlowRunEvent source) {
  PhysicalImplementationEvent event;
  event.kind = source.kind == ManagedFlowRunEventKind::kOutput
                   ? PhysicalImplementationEventKind::kOutput
               : source.kind == ManagedFlowRunEventKind::kProgress
                   ? PhysicalImplementationEventKind::kProgress
               : source.kind == ManagedFlowRunEventKind::kCompleted
                   ? PhysicalImplementationEventKind::kCompleted
                   : PhysicalImplementationEventKind::kStateChanged;
  event.state = source.state;
  event.generation = source.generation;
  event.status = std::move(source.status);
  event.output = std::move(source.output);
  event.progress = std::move(source.progress);
  event.metrics = std::move(source.metrics);
  event.diagnostics = std::move(source.diagnostics);
  event.failure_stage = std::move(source.failure_stage);
  event.failure_code = std::move(source.failure_code);
  event.run = std::move(source.run);
  if (event.run) event.gds_path = FindGds(*event.run);
  PhysicalImplementationEventSink sink;
  {
    std::scoped_lock lock(mutex_);
    if (shutdown_ || source.generation != generation_ || terminal_delivered_) {
      return;
    }
    if (event.kind == PhysicalImplementationEventKind::kCompleted) {
      terminal_delivered_ = true;
      active_ = false;
    }
    sink = sink_;
  }
  if (sink) sink(std::move(event));
}

void PhysicalImplementationService::Cancel() noexcept {
  PhysicalImplementationEventSink sink;
  PhysicalImplementationEvent event;
  bool cancel_managed_flow = false;
  {
    std::scoped_lock lock(mutex_);
    if (!active_ || terminal_delivered_) return;
    operation_stop_source_.request_stop();
    if (managed_flow_.IsActive()) {
      cancel_managed_flow = true;
    } else {
      terminal_delivered_ = true;
      active_ = false;
      sink = sink_;
      event.kind = PhysicalImplementationEventKind::kCompleted;
      event.state = ManagedFlowRunState::kCancelled;
      event.generation = generation_;
      event.status = {core::ErrorCode::kCancelled,
                      "Physical implementation cancelled", 0};
    }
  }
  if (cancel_managed_flow) {
    managed_flow_.Cancel();
    return;
  }
  if (sink) sink(std::move(event));
}

void PhysicalImplementationService::Shutdown() noexcept {
  {
    std::scoped_lock lock(mutex_);
    if (shutdown_) return;
    shutdown_ = true;
    active_ = false;
    operation_stop_source_.request_stop();
    sink_ = {};
  }
  managed_flow_.Shutdown();
  scheduler_.RequestStop();
}

bool PhysicalImplementationService::IsActive() const {
  std::scoped_lock lock(mutex_);
  return active_;
}

}  // namespace designpp::application

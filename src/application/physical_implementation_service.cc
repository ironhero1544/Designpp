// Copyright 2026 The Design++ Authors

#include "designpp/application/physical_implementation_service.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <optional>
#include <sstream>
#include <utility>

#include "designpp/application/synthesis_fingerprint.h"

namespace designpp::application {
namespace {

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
  return true;
}

bool SummaryMatches(const RunRecord& run, const ManagedFlowRunRequest& request,
                    std::string* fingerprint) {
  const auto summary = ReadSummary(run);
  if (!summary) return false;
  const auto revision = JsonUnsigned(*summary, "project_revision");
  const auto backend = JsonString(*summary, "backend_id");
  const auto value = JsonString(*summary, "configuration_fingerprint");
  if (!revision || *revision != request.project.revision || !backend ||
      *backend != request.project.physical_implementation.backend_id ||
      !value || value->empty()) {
    return false;
  }
  *fingerprint = *value;
  return true;
}

}  // namespace

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
  const auto step = JsonString(*summary, "last_step");
  if (!lineage || lineage->empty() || !step || step->empty()) {
    return std::nullopt;
  }
  const std::filesystem::path checkpoint =
      run.directory / "artifacts" / "checkpoint-state.json";
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
    sink_ = std::move(sink);
    current_sink = sink_;
  }
  PhysicalImplementationEvent checking;
  checking.generation = request.generation;
  checking.state = ManagedFlowRunState::kPreparing;
  current_sink(std::move(checking));
  const bool queued = scheduler_.Submit(
      [this, request = std::move(request)](std::stop_token stop_token) mutable {
        PhysicalImplementationEventSink sink;
        {
          std::scoped_lock lock(mutex_);
          sink = sink_;
        }
        InspectAndStart(std::move(request), std::move(sink), stop_token);
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
    std::stop_token stop_token) {
  if (stop_token.stop_requested()) return;
  RunStore store;
  auto listed = store.List(request.cell_directory);
  if (listed.Ok()) {
    std::vector<RunRecord> runs = std::move(listed).Value();
    std::reverse(runs.begin(), runs.end());
    for (const RunRecord& run : runs) {
      if (!IsPhysicalRun(run)) continue;
      if (run.status == RunStatus::kSucceeded && run.outcome.result_succeeded) {
        const std::filesystem::path gds = FindGds(run);
        std::string fingerprint;
        if (!gds.empty() && SummaryMatches(run, request, &fingerprint) &&
            InputsAreOlderThan(request, gds)) {
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
      if (!request.resume) {
        request.resume = BuildPhysicalImplementationResume(run, request);
      }
      if (request.resume) break;
    }
  }
  {
    std::scoped_lock lock(mutex_);
    if (shutdown_ || generation_ != request.generation || terminal_delivered_) {
      return;
    }
  }
  const core::Status started = managed_flow_.Start(
      std::move(request),
      [this](ManagedFlowRunEvent event) { Forward(std::move(event)); });
  if (!started.Ok()) {
    PhysicalImplementationEvent event;
    event.kind = PhysicalImplementationEventKind::kCompleted;
    event.state = ManagedFlowRunState::kFailed;
    event.generation = generation_;
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
  {
    std::scoped_lock lock(mutex_);
    if (!active_ || terminal_delivered_) return;
    if (managed_flow_.IsActive()) {
      managed_flow_.Cancel();
      return;
    }
    terminal_delivered_ = true;
    active_ = false;
    sink = sink_;
    event.kind = PhysicalImplementationEventKind::kCompleted;
    event.state = ManagedFlowRunState::kCancelled;
    event.generation = generation_;
    event.status = {core::ErrorCode::kCancelled,
                    "Physical implementation cancelled", 0};
  }
  if (sink) sink(std::move(event));
}

void PhysicalImplementationService::Shutdown() noexcept {
  {
    std::scoped_lock lock(mutex_);
    if (shutdown_) return;
    shutdown_ = true;
    active_ = false;
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

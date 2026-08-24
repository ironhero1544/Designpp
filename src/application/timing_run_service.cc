// Copyright 2026 The Design++ Authors

#include "designpp/application/timing_run_service.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <mutex>
#include <sstream>
#include <utility>

#include "designpp/application/synthesis_fingerprint.h"
#include "designpp/runtime/path_mapper.h"
#include "designpp/runtime/resource_coordinator.h"
#include "designpp/runtime/task_scheduler.h"

namespace designpp::application {
namespace {

std::string EscapeJson(std::string_view value) {
  std::string result;
  for (const char character : value) {
    if (character == '\\' || character == '"') result.push_back('\\');
    result.push_back(character);
  }
  return result;
}

bool CopyFileSafe(const std::filesystem::path& source,
                  const std::filesystem::path& destination) {
  std::error_code error;
  std::filesystem::create_directories(destination.parent_path(), error);
  if (error) return false;
  std::filesystem::copy_file(source, destination,
                             std::filesystem::copy_options::overwrite_existing,
                             error);
  return !error;
}

bool AddArtifact(const std::shared_ptr<RunRecord>& run, std::string kind,
                 std::string format, const std::filesystem::path& path,
                 std::string input_hash, bool partial,
                 std::vector<RunArtifact>* artifacts) {
  std::error_code error;
  const std::uintmax_t size = std::filesystem::file_size(path, error);
  if (error || size == 0) return false;
  const std::filesystem::path relative =
      std::filesystem::relative(path, run->directory, error);
  if (error || relative.empty()) return false;
  artifacts->push_back({std::move(kind), std::move(format),
                        relative.generic_string(), size, std::move(input_hash),
                        partial});
  return true;
}

core::Result<std::filesystem::path> CreateStagingDirectory(
    std::string_view run_id) {
  wchar_t temporary[MAX_PATH]{};
  const DWORD length =
      GetTempPathW(static_cast<DWORD>(std::size(temporary)), temporary);
  if (length == 0 || length >= std::size(temporary)) {
    return core::Status{core::ErrorCode::kIoError,
                        "Cannot resolve timing staging directory",
                        GetLastError()};
  }
  for (const wchar_t character : std::wstring_view(temporary, length)) {
    if (character > 0x7f) {
      return core::Status{core::ErrorCode::kInvalidArgument,
                          "OpenSTA staging path must be ASCII", 0};
    }
  }
  std::wstring id;
  for (const char character : run_id) {
    if (!std::isalnum(static_cast<unsigned char>(character)) &&
        character != '-') {
      return core::Status{core::ErrorCode::kInvalidArgument,
                          "Timing run id is invalid", 0};
    }
    id.push_back(static_cast<wchar_t>(character));
  }
  const std::filesystem::path directory =
      std::filesystem::path(temporary) / L"DesignPlusPlus" / L"opensta" / id;
  std::error_code error;
  if (!std::filesystem::create_directories(directory, error) && error) {
    return core::Status{core::ErrorCode::kIoError,
                        "Cannot create timing staging directory",
                        static_cast<unsigned long>(error.value())};
  }
  return directory;
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  std::ostringstream contents;
  contents << input.rdbuf();
  return input || input.eof() ? contents.str() : std::string{};
}

std::string ExtractToolVersion(std::string_view output) {
  std::istringstream lines{std::string(output)};
  std::string line;
  std::string version;
  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!line.empty() && !line.starts_with("warning:")) version = line;
  }
  return version.empty() ? "OpenSTA" : version;
}

bool ContainsJsonString(std::string_view json, std::string_view key,
                        std::string_view value) {
  return json.find("\"" + std::string(key) + "\":\"" + EscapeJson(value) +
                   "\"") != std::string_view::npos;
}

const RunArtifact* FindArtifact(const RunRecord& run, std::string_view kind,
                                std::string_view format) {
  const auto found = std::find_if(
      run.artifacts.begin(), run.artifacts.end(), [&](const auto& artifact) {
        return artifact.kind == kind && artifact.format == format &&
               !artifact.partial;
      });
  return found == run.artifacts.end() ? nullptr : &*found;
}

core::Result<std::shared_ptr<RunRecord>> FindCompatibleSynthesisRun(
    const TimingRunRequest& request,
    const SynthesisFingerprint& expected_fingerprint,
    const RunStore& run_store) {
  if (request.project.timing.sdc_path.empty()) {
    return core::Status{
        core::ErrorCode::kNotFound,
        "Add and select a managed SDC file in the Constraints View", 0};
  }
  const auto sdc = std::find_if(
      request.sources.begin(), request.sources.end(), [&](const auto& source) {
        return source.relative_path == request.project.timing.sdc_path &&
               source.view_kind == core::ViewKind::kConstraints;
      });
  std::error_code sdc_error;
  if (sdc == request.sources.end() || !sdc->exists ||
      std::filesystem::file_size(sdc->windows_path, sdc_error) == 0 ||
      sdc_error) {
    return core::Status{
        core::ErrorCode::kInvalidArgument,
        "The selected managed SDC is missing or empty; define timing clocks "
        "and constraints first",
        static_cast<unsigned long>(sdc_error.value())};
  }
  adapters::OpenStaAdapter adapter;
  const core::Status sdc_status =
      adapter.ValidateSdcText(ReadFile(sdc->windows_path));
  if (!sdc_status.Ok()) return sdc_status;
  if (request.project.timing.liberty_paths.empty()) {
    return core::Status{core::ErrorCode::kNotFound,
                        "Add and select at least one managed Liberty file", 0};
  }
  if (request.project.synthesis.liberty_paths.empty()) {
    return core::Status{core::ErrorCode::kConflict,
                        "Run Synthesis again with the Timing Liberty selection",
                        0};
  }
  if (request.project.timing.liberty_paths !=
      request.project.synthesis.liberty_paths) {
    return core::Status{
        core::ErrorCode::kConflict,
        "Timing Liberty must exactly match mapped synthesis Liberty", 0};
  }
  auto listed = run_store.List(request.cell_directory);
  if (!listed.Ok()) return listed.GetStatus();
  for (const RunRecord& candidate : listed.Value()) {
    if (candidate.stage != "Synthesis" || candidate.tool != "Yosys" ||
        candidate.status != RunStatus::kSucceeded ||
        !candidate.outcome.process_succeeded ||
        !candidate.outcome.result_succeeded) {
      continue;
    }
    const RunArtifact* netlist = FindArtifact(candidate, "netlist", "verilog");
    const RunArtifact* summary = FindArtifact(candidate, "summary", "json");
    if (netlist == nullptr || summary == nullptr) continue;
    const std::string summary_text =
        ReadFile(candidate.directory / summary->relative_path);
    if (!ContainsJsonString(summary_text, "top_module",
                            request.project.top_module) ||
        !ContainsJsonString(summary_text, "configuration_sha256",
                            expected_fingerprint.configuration_sha256)) {
      continue;
    }
    return std::make_shared<RunRecord>(candidate);
  }
  return core::Status{core::ErrorCode::kNotFound,
                      "No compatible mapped Yosys synthesis run exists", 0};
}

}  // namespace

core::Result<std::shared_ptr<RunRecord>> ResolveCompatibleSynthesisRun(
    const TimingRunRequest& request) {
  auto fingerprint = CalculateSynthesisFingerprint(
      request.project, request.sources, request.library_directory);
  if (!fingerprint.Ok()) return fingerprint.GetStatus();
  return FindCompatibleSynthesisRun(request, fingerprint.Value(), RunStore());
}

struct TimingRunService::Implementation final
    : std::enable_shared_from_this<Implementation> {
  explicit Implementation(runtime::ExecutionProvider* execution_provider)
      : provider(execution_provider), scheduler(1) {}

  void Emit(TimingRunEvent event) {
    TimingRunEventSink callback;
    {
      std::scoped_lock lock(mutex);
      if (shutdown || !sink) return;
      callback = sink;
    }
    callback(std::move(event));
  }

  void EmitState(TimingRunState new_state) {
    {
      std::scoped_lock lock(mutex);
      state = new_state;
    }
    TimingRunEvent event;
    event.kind = TimingRunEventKind::kStateChanged;
    event.state = new_state;
    event.generation = request.generation;
    Emit(std::move(event));
  }

  void Finish(core::Status status, runtime::ProcessResult result,
              std::shared_ptr<RunRecord> completed_run,
              std::vector<core::Diagnostic> diagnostics,
              adapters::TimingMetrics metrics = {},
              std::string script_text = {}) {
    TimingRunEventSink callback;
    TimingRunEvent event;
    {
      std::scoped_lock lock(mutex);
      if (shutdown || terminal_delivered) return;
      terminal_delivered = true;
      active = false;
      cpu_lease.reset();
      state = result.cancelled ? TimingRunState::kCancelled
              : status.Ok()    ? TimingRunState::kSucceeded
                               : TimingRunState::kFailed;
      event.kind = TimingRunEventKind::kCompleted;
      event.state = state;
      event.generation = request.generation;
      event.status = std::move(status);
      event.process_result = std::move(result);
      event.run = std::move(completed_run);
      event.synthesis_run = synthesis_run;
      event.diagnostics = std::move(diagnostics);
      event.metrics = std::move(metrics);
      event.script_text = std::move(script_text);
      callback = sink;
    }
    if (callback) callback(std::move(event));
  }

  void StartProbe() {
    const std::uint64_t generation = request.generation;
    const auto self = shared_from_this();
    auto started = provider->Start(
        adapter.BuildProbeCommand(),
        [self, generation](std::string output) {
          TimingRunEvent event;
          event.kind = TimingRunEventKind::kOutput;
          event.state = TimingRunState::kProbing;
          event.generation = generation;
          event.output = std::move(output);
          self->Emit(std::move(event));
        },
        [self, generation](runtime::ProcessResult result) mutable {
          {
            std::scoped_lock lock(self->mutex);
            if (self->shutdown || !self->active || self->completion_claimed ||
                self->request.generation != generation) {
              return;
            }
            self->completion_claimed = true;
          }
          if (result.cancelled || result.exit_code != 0) {
            self->Finish(
                result.cancelled
                    ? core::Status{core::ErrorCode::kCancelled,
                                   "Timing analysis cancelled", 0}
                    : core::Status{core::ErrorCode::kNotFound,
                                   "OpenSTA capability probe failed", 0},
                std::move(result), nullptr, {});
            return;
          }
          std::string version = ExtractToolVersion(result.output);
          {
            std::scoped_lock lock(self->mutex);
            self->completion_claimed = false;
          }
          self->EmitState(TimingRunState::kPreparing);
          const bool accepted = self->scheduler.Submit(
              [self, generation,
               version = std::move(version)](std::stop_token token) mutable {
                self->Prepare(generation, std::move(version), token);
              });
          if (!accepted) {
            self->Finish({core::ErrorCode::kConflict,
                          "Timing preparation queue is unavailable", 0},
                         {}, nullptr, {});
          }
        });
    if (!started.Ok()) {
      Finish(started.status, {}, nullptr, {});
      return;
    }
    std::scoped_lock lock(mutex);
    handle = std::move(started.handle);
    if (cancellation_requested && handle) handle->Cancel();
  }

  core::Result<std::shared_ptr<RunRecord>> FindCompatibleSynthesis(
      const SynthesisFingerprint& expected_fingerprint) {
    return FindCompatibleSynthesisRun(request, expected_fingerprint, run_store);
  }

  const ResolvedSource* FindManagedConstraint(std::string_view relative_path,
                                              bool liberty) const {
    const auto found = std::find_if(
        request.sources.begin(), request.sources.end(),
        [&](const auto& source) {
          const bool managed_constraints =
              source.view_kind == core::ViewKind::kConstraints;
          const bool legacy_liberty =
              liberty && (source.view_kind == core::ViewKind::kSynthesis ||
                          source.view_kind == core::ViewKind::kTiming);
          if (!source.exists || (!managed_constraints && !legacy_liberty) ||
              source.relative_path != relative_path) {
            return false;
          }
          std::string extension = source.windows_path.extension().string();
          std::transform(extension.begin(), extension.end(), extension.begin(),
                         [](unsigned char value) {
                           return static_cast<char>(std::tolower(value));
                         });
          return liberty ? extension == ".lib" || extension == ".liberty"
                         : extension == ".sdc";
        });
    return found == request.sources.end() ? nullptr : &*found;
  }

  void Prepare(std::uint64_t generation, std::string version,
               std::stop_token token) {
    bool cancelled = token.stop_requested();
    {
      std::scoped_lock lock(mutex);
      cancelled = cancelled || shutdown || cancellation_requested ||
                  request.generation != generation;
    }
    if (cancelled) {
      runtime::ProcessResult result;
      result.cancelled = true;
      Finish({core::ErrorCode::kCancelled, "Timing analysis cancelled", 0},
             std::move(result), nullptr, {});
      return;
    }
    auto fingerprint_result = CalculateSynthesisFingerprint(
        request.project, request.sources, request.library_directory);
    if (!fingerprint_result.Ok()) {
      Finish(fingerprint_result.GetStatus(), {}, nullptr, {});
      return;
    }
    fingerprint = std::move(fingerprint_result).Value();
    auto compatible = FindCompatibleSynthesis(fingerprint);
    if (!compatible.Ok()) {
      Finish(compatible.GetStatus(), {}, nullptr, {});
      return;
    }
    synthesis_run = std::move(compatible).Value();
    const RunArtifact* netlist_artifact =
        FindArtifact(*synthesis_run, "netlist", "verilog");
    const ResolvedSource* sdc =
        FindManagedConstraint(request.project.timing.sdc_path, false);
    if (sdc == nullptr) {
      Finish({core::ErrorCode::kNotFound,
              "Timing SDC is not a managed Constraints file", 0},
             {}, nullptr, {});
      return;
    }
    std::vector<const ResolvedSource*> liberty_sources;
    for (const std::string& path : request.project.timing.liberty_paths) {
      const ResolvedSource* liberty = FindManagedConstraint(path, true);
      if (liberty == nullptr) {
        Finish({core::ErrorCode::kNotFound,
                "Timing Liberty is not a managed Constraints file", 0},
               {}, nullptr, {});
        return;
      }
      liberty_sources.push_back(liberty);
    }
    auto acquired = resource_coordinator.AcquireCpu(
        request.project.cpu_budget, operation_stop_source.get_token());
    if (!acquired.Ok()) {
      Finish(acquired.GetStatus(), {}, nullptr, {});
      return;
    }
    cpu_lease =
        std::make_unique<runtime::CpuTokenLease>(std::move(acquired).Value());
    auto begun = run_store.Begin(request.cell_directory, request.project,
                                 "StaticTimingAnalysis", "OpenSTA", version);
    if (!begun.Ok()) {
      Finish(begun.GetStatus(), {}, nullptr, {});
      return;
    }
    run = std::make_shared<RunRecord>(std::move(begun).Value());
    auto staged = CreateStagingDirectory(run->id);
    if (!staged.Ok()) {
      Finish(staged.GetStatus(), {}, run, {});
      return;
    }
    staging_directory = std::move(staged).Value();
    const std::filesystem::path staged_netlist =
        staging_directory / L"netlist.v";
    const std::filesystem::path staged_sdc =
        staging_directory / L"constraints.sdc";
    if (!CopyFileSafe(
            synthesis_run->directory / netlist_artifact->relative_path,
            staged_netlist) ||
        !CopyFileSafe(sdc->windows_path, staged_sdc)) {
      CompletePreparationFailure("Cannot stage timing netlist or SDC");
      return;
    }
    adapters::TimingRequest adapter_request;
    adapter_request.top_module = request.project.top_module;
    adapter_request.corner_name = request.project.timing.corner_name;
    adapter_request.netlist_path = staged_netlist;
    adapter_request.sdc_path = staged_sdc;
    adapter_request.artifact_directory = staging_directory;
    adapter_request.cpu_threads = request.project.cpu_budget;
    for (std::size_t index = 0; index < liberty_sources.size(); ++index) {
      const std::filesystem::path target =
          staging_directory / (L"liberty-" + std::to_wstring(index) + L".lib");
      if (!CopyFileSafe(liberty_sources[index]->windows_path, target)) {
        CompletePreparationFailure("Cannot stage timing Liberty");
        return;
      }
      adapter_request.liberty_files.push_back(target);
    }
    runtime::PathMapper mapper;
    auto built = adapter.BuildPlan(adapter_request, mapper);
    if (!built.Ok()) {
      CompletePreparationFailure(built.GetStatus().message);
      return;
    }
    plan = std::move(built).Value();
    std::ofstream script(plan.script_path, std::ios::binary | std::ios::trunc);
    script << plan.script_text;
    script.flush();
    if (!script) {
      CompletePreparationFailure("Cannot write OpenSTA timing script");
      return;
    }
    EmitState(TimingRunState::kRunning);
    StartExecution(generation);
  }

  void CompletePreparationFailure(std::string message) {
    std::vector<core::Diagnostic> diagnostics;
    if (run) {
      static_cast<void>(
          run_store.Complete(run.get(), RunStatus::kFailed, 0, diagnostics));
    }
    Finish({core::ErrorCode::kIoError, std::move(message), 0}, {}, run, {});
  }

  void StartExecution(std::uint64_t generation) {
    const auto self = shared_from_this();
    auto started = provider->Start(
        plan.execute,
        [self, generation](std::string output) {
          static_cast<void>(self->run_store.AppendLog(*self->run, output));
          TimingRunEvent event;
          event.kind = TimingRunEventKind::kOutput;
          event.state = TimingRunState::kRunning;
          event.generation = generation;
          event.output = std::move(output);
          self->Emit(std::move(event));
        },
        [self, generation](runtime::ProcessResult result) mutable {
          {
            std::scoped_lock lock(self->mutex);
            if (self->shutdown || !self->active || self->completion_claimed ||
                self->request.generation != generation) {
              return;
            }
            self->completion_claimed = true;
          }
          const bool accepted = self->scheduler.Submit(
              [self, generation,
               result = std::move(result)](std::stop_token token) mutable {
                self->Finalize(generation, std::move(result), token);
              });
          if (!accepted) {
            self->Finish({core::ErrorCode::kConflict,
                          "Timing result queue is unavailable", 0},
                         std::move(result), self->run, {});
          }
        });
    if (!started.Ok()) {
      CompletePreparationFailure(started.status.message);
      return;
    }
    std::scoped_lock lock(mutex);
    handle = std::move(started.handle);
    if (cancellation_requested && handle) handle->Cancel();
  }

  void Finalize(std::uint64_t generation, runtime::ProcessResult result,
                std::stop_token token) {
    if (generation != request.generation) return;
    std::vector<core::Diagnostic> diagnostics =
        adapter.ParseDiagnostics(result.output);
    std::vector<RunArtifact> artifacts;
    const bool partial =
        result.cancelled || result.exit_code != 0 || token.stop_requested();
    const std::filesystem::path artifact_directory =
        run->directory / L"artifacts";
    const std::filesystem::path final_script =
        artifact_directory / L"timing.tcl";
    const std::filesystem::path final_report =
        artifact_directory / L"timing.rpt";
    const bool copied_script = CopyFileSafe(plan.script_path, final_script);
    const bool copied_report = CopyFileSafe(plan.report_path, final_report);
    auto netlist_digest = CalculateFileSha256(staging_directory / L"netlist.v");
    const std::string netlist_hash =
        netlist_digest.Ok() ? std::move(netlist_digest).Value() : std::string{};
    const bool has_script =
        copied_script && AddArtifact(run, "script", "tcl", final_script,
                                     netlist_hash, partial, &artifacts);
    const bool has_report =
        copied_report && AddArtifact(run, "report", "text", final_report,
                                     netlist_hash, partial, &artifacts);
    adapters::TimingMetrics metrics;
    core::Status parse_status = core::Status::Success();
    if (!has_script || !has_report) {
      parse_status = {core::ErrorCode::kNotFound,
                      "OpenSTA required artifact is missing or empty", 0};
    } else if (result.exit_code == 0 && !result.cancelled) {
      auto parsed = adapter.ParseReport(result.output, ReadFile(final_report),
                                        request.project.timing.corner_name);
      if (!parsed.Ok()) {
        parse_status = parsed.GetStatus();
      } else {
        metrics = std::move(parsed).Value();
      }
    }
    for (const adapters::TimingViolation& violation : metrics.violations) {
      core::Diagnostic diagnostic;
      diagnostic.severity = core::DiagnosticSeverity::kError;
      if (violation.check_type == "removal") {
        diagnostic.code = "TIMING-REMOVAL";
      } else if (violation.check_type == "recovery") {
        diagnostic.code = "TIMING-RECOVERY";
      } else if (violation.check_type == "hold") {
        diagnostic.code = "TIMING-HOLD";
      } else {
        diagnostic.code = "TIMING-SETUP";
      }
      diagnostic.message = violation.startpoint + " -> " + violation.endpoint +
                           " slack " + std::to_string(violation.slack) + " ns";
      diagnostics.push_back(std::move(diagnostic));
    }
    const bool process_succeeded =
        result.started && !result.cancelled && result.exit_code == 0;
    std::error_code log_error;
    const std::filesystem::path raw_log = run->directory / L"logs" / L"raw.log";
    if ((!std::filesystem::exists(raw_log, log_error) ||
         std::filesystem::file_size(raw_log, log_error) == 0) &&
        !result.output.empty()) {
      static_cast<void>(run_store.AppendLog(*run, result.output));
    }
    bool analysis_complete = process_succeeded && parse_status.Ok() &&
                             metrics.setup.has_paths && metrics.hold.has_paths;
    std::filesystem::path summary_path =
        run->directory / L"reports" / L"timing-summary.json";
    if (analysis_complete) {
      std::ofstream summary(summary_path, std::ios::binary | std::ios::trunc);
      summary << "{\"schema_version\":2,\"corner\":\""
              << EscapeJson(request.project.timing.corner_name)
              << "\",\"tool_version\":\"" << EscapeJson(run->tool_version)
              << "\",\"synthesis_run_id\":\"" << EscapeJson(synthesis_run->id)
              << "\",\"netlist_sha256\":\"" << netlist_hash
              << "\",\"setup_wns\":" << metrics.setup.wns
              << ",\"setup_tns\":" << metrics.setup.tns
              << ",\"hold_wns\":" << metrics.hold.wns
              << ",\"hold_tns\":" << metrics.hold.tns
              << ",\"setup_violations\":" << metrics.setup.violation_count
              << ",\"hold_violations\":" << metrics.hold.violation_count
              << ",\"setup_check_violations\":"
              << metrics.violation_counts.setup
              << ",\"hold_check_violations\":" << metrics.violation_counts.hold
              << ",\"recovery_violations\":"
              << metrics.violation_counts.recovery
              << ",\"removal_violations\":" << metrics.violation_counts.removal
              << ",\"unknown_violations\":" << metrics.violation_counts.unknown
              << ",\"truncated\":"
              << (metrics.violations_truncated ? "true" : "false")
              << ",\"configuration_sha256\":\""
              << EscapeJson(fingerprint.configuration_sha256)
              << "\",\"liberty_files\":[";
      for (std::size_t index = 0; index < fingerprint.liberty_files.size();
           ++index) {
        const LibertyFingerprint& liberty = fingerprint.liberty_files[index];
        summary << (index == 0 ? "" : ",") << "{\"relative_path\":\""
                << EscapeJson(liberty.relative_path) << "\",\"sha256\":\""
                << liberty.sha256 << "\"}";
      }
      auto sdc_hash =
          CalculateFileSha256(staging_directory / L"constraints.sdc");
      summary << "],\"sdc_relative_path\":\""
              << EscapeJson(request.project.timing.sdc_path)
              << "\",\"sdc_sha256\":\""
              << (sdc_hash.Ok() ? std::move(sdc_hash).Value() : std::string{})
              << "\"}";
      summary.flush();
      analysis_complete =
          summary && AddArtifact(run, "summary", "json", summary_path,
                                 netlist_hash, false, &artifacts);
    }
    const bool timing_passed = analysis_complete && metrics.Passed();
    const RunStatus run_status = result.cancelled ? RunStatus::kCancelled
                                 : timing_passed  ? RunStatus::kSucceeded
                                                  : RunStatus::kFailed;
    RunOutcome outcome;
    outcome.process_succeeded = process_succeeded;
    outcome.result_succeeded = timing_passed;
    if (analysis_complete)
      outcome.summary_relative_path = "reports/timing-summary.json";
    static_cast<void>(run_store.Complete(run.get(), run_status,
                                         result.exit_code, diagnostics,
                                         std::move(artifacts), outcome));
    core::Status status =
        result.cancelled     ? core::Status{core::ErrorCode::kCancelled,
                                            "Timing analysis cancelled", 0}
        : !parse_status.Ok() ? parse_status
        : !analysis_complete
            ? core::Status{core::ErrorCode::kCorruptData,
                           "OpenSTA did not analyze setup and hold paths", 0}
        : timing_passed ? core::Status::Success()
                        : core::Status{core::ErrorCode::kConflict,
                                       "Timing constraints are violated", 0};
    Finish(std::move(status), std::move(result), run, std::move(diagnostics),
           metrics, plan.script_text);
    // Keep the staging directory when a generated artifact could not be copied.
    // It is the only recoverable copy available for diagnosing that failure.
    if (!staging_directory.empty() && copied_script && copied_report) {
      std::error_code error;
      std::filesystem::remove_all(staging_directory, error);
    }
  }

  runtime::ExecutionProvider* provider = nullptr;
  mutable std::mutex mutex;
  runtime::TaskScheduler scheduler;
  runtime::ResourceCoordinator resource_coordinator;
  RunStore run_store;
  adapters::OpenStaAdapter adapter;
  TimingRunRequest request;
  TimingRunEventSink sink;
  TimingRunState state = TimingRunState::kIdle;
  std::unique_ptr<runtime::ExecutionHandle> handle;
  std::unique_ptr<runtime::CpuTokenLease> cpu_lease;
  std::shared_ptr<RunRecord> run;
  std::shared_ptr<RunRecord> synthesis_run;
  adapters::TimingPlan plan;
  SynthesisFingerprint fingerprint;
  std::filesystem::path staging_directory;
  bool active = false;
  bool cancellation_requested = false;
  bool completion_claimed = false;
  bool terminal_delivered = false;
  bool shutdown = false;
  std::stop_source operation_stop_source;
};

TimingRunService::TimingRunService(runtime::ExecutionProvider* provider)
    : implementation_(std::make_shared<Implementation>(provider)) {}

TimingRunService::~TimingRunService() { Shutdown(); }

core::Status TimingRunService::Start(TimingRunRequest request,
                                     TimingRunEventSink sink) {
  if (!implementation_ || !implementation_->provider || !sink ||
      request.generation == 0 || request.library_directory.empty() ||
      request.cell_directory.empty()) {
    return {core::ErrorCode::kInvalidArgument,
            "Timing run request is incomplete", 0};
  }
  {
    std::scoped_lock lock(implementation_->mutex);
    if (implementation_->shutdown) {
      return {core::ErrorCode::kCancelled, "Timing service is shutting down",
              0};
    }
    if (implementation_->active) {
      return {core::ErrorCode::kConflict, "A timing analysis is already active",
              0};
    }
    implementation_->request = std::move(request);
    implementation_->sink = std::move(sink);
    implementation_->state = TimingRunState::kProbing;
    implementation_->handle.reset();
    implementation_->cpu_lease.reset();
    implementation_->run.reset();
    implementation_->synthesis_run.reset();
    implementation_->plan = {};
    implementation_->fingerprint = {};
    implementation_->staging_directory.clear();
    implementation_->active = true;
    implementation_->cancellation_requested = false;
    implementation_->completion_claimed = false;
    implementation_->terminal_delivered = false;
    implementation_->operation_stop_source = std::stop_source{};
  }
  implementation_->EmitState(TimingRunState::kProbing);
  implementation_->StartProbe();
  return core::Status::Success();
}

void TimingRunService::Cancel() noexcept {
  if (!implementation_) return;
  std::scoped_lock lock(implementation_->mutex);
  if (!implementation_->active) return;
  implementation_->cancellation_requested = true;
  implementation_->operation_stop_source.request_stop();
  implementation_->state = TimingRunState::kCancelling;
  if (implementation_->handle) implementation_->handle->Cancel();
}

void TimingRunService::Shutdown() noexcept {
  if (!implementation_) return;
  {
    std::scoped_lock lock(implementation_->mutex);
    if (implementation_->shutdown) return;
    implementation_->shutdown = true;
    implementation_->active = false;
    implementation_->operation_stop_source.request_stop();
    if (implementation_->handle) implementation_->handle->Cancel();
    implementation_->sink = {};
  }
  implementation_->scheduler.RequestStop();
}

TimingRunState TimingRunService::State() const {
  if (!implementation_) return TimingRunState::kIdle;
  std::scoped_lock lock(implementation_->mutex);
  return implementation_->state;
}

bool TimingRunService::IsActive() const {
  if (!implementation_) return false;
  std::scoped_lock lock(implementation_->mutex);
  return implementation_->active;
}

}  // namespace designpp::application

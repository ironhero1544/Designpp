// Copyright 2026 The Design++ Authors

#include "designpp/application/synthesis_run_service.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <mutex>
#include <sstream>
#include <utility>
#include <vector>

#include "designpp/application/synthesis_fingerprint.h"
#include "designpp/runtime/path_mapper.h"
#include "designpp/runtime/resource_coordinator.h"
#include "designpp/runtime/task_scheduler.h"

namespace designpp::application {
namespace {

std::string EscapeJson(std::string_view value) {
  std::string result;
  result.reserve(value.size());
  for (const char character : value) {
    if (character == '\\' || character == '"') result.push_back('\\');
    result.push_back(character);
  }
  return result;
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

core::Result<std::filesystem::path> CreateAsciiStagingDirectory(
    std::string_view run_id) {
  wchar_t temporary_root[MAX_PATH]{};
  const DWORD length = GetTempPathW(
      static_cast<DWORD>(std::size(temporary_root)), temporary_root);
  if (length == 0 || length >= std::size(temporary_root)) {
    return core::Status{core::ErrorCode::kIoError,
                        "Cannot resolve the Windows temporary directory",
                        GetLastError()};
  }
  const std::filesystem::path root(temporary_root);
  if (std::any_of(root.native().begin(), root.native().end(),
                  [](wchar_t character) { return character > 0x7f; })) {
    return core::Status{
        core::ErrorCode::kInvalidArgument,
        "Yosys requires an ASCII Windows temporary-directory path", 0};
  }
  std::wstring wide_id;
  wide_id.reserve(run_id.size());
  for (const char character : run_id) {
    if (!(std::isalnum(static_cast<unsigned char>(character)) ||
          character == '-')) {
      return core::Status{core::ErrorCode::kInvalidArgument,
                          "Yosys run identity is invalid", 0};
    }
    wide_id.push_back(static_cast<unsigned char>(character));
  }
  const std::filesystem::path directory =
      root / L"DesignPlusPlus" / L"yosys" / wide_id;
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) {
    return core::Status{core::ErrorCode::kIoError,
                        "Cannot create the Yosys staging directory",
                        static_cast<unsigned long>(error.value())};
  }
  return directory;
}

bool CopyIfPresent(const std::filesystem::path& source,
                   const std::filesystem::path& destination) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(source, error) || error) return false;
  std::filesystem::copy_file(source, destination,
                             std::filesystem::copy_options::overwrite_existing,
                             error);
  return !error;
}

}  // namespace

struct SynthesisRunService::Implementation final
    : public std::enable_shared_from_this<Implementation> {
  explicit Implementation(runtime::ExecutionProvider* execution_provider)
      : provider(execution_provider), scheduler(1) {}

  void Emit(SynthesisRunEvent event) {
    SynthesisRunEventSink event_sink;
    {
      std::scoped_lock lock(mutex);
      if (shutdown || event.generation != request.generation) return;
      event_sink = sink;
    }
    if (event_sink) event_sink(std::move(event));
  }

  void EmitState(SynthesisRunState next_state) {
    SynthesisRunEvent event;
    {
      std::scoped_lock lock(mutex);
      if (shutdown || terminal_delivered) return;
      state = next_state;
      event.kind = SynthesisRunEventKind::kStateChanged;
      event.state = state;
      event.generation = request.generation;
    }
    Emit(std::move(event));
  }

  void Finish(core::Status status, runtime::ProcessResult process_result,
              std::shared_ptr<RunRecord> completed_run,
              std::vector<core::Diagnostic> diagnostics,
              adapters::SynthesisMetrics metrics, std::string script_text,
              core::SchematicModel gate_schematic = {},
              core::SchematicModel readable_schematic = {},
              core::Status readable_schematic_status = {}) {
    SynthesisRunEvent event;
    SynthesisRunEventSink event_sink;
    {
      std::scoped_lock lock(mutex);
      if (shutdown || terminal_delivered) return;
      terminal_delivered = true;
      active = false;
      state =
          process_result.cancelled || status.code == core::ErrorCode::kCancelled
              ? SynthesisRunState::kCancelled
          : status.Ok() ? SynthesisRunState::kSucceeded
                        : SynthesisRunState::kFailed;
      cpu_lease.reset();
      event.kind = SynthesisRunEventKind::kCompleted;
      event.state = state;
      event.generation = request.generation;
      event.status = std::move(status);
      event.process_result = std::move(process_result);
      event.run = std::move(completed_run);
      event.diagnostics = std::move(diagnostics);
      event.metrics = metrics;
      event.gate_schematic = std::move(gate_schematic);
      event.readable_schematic = std::move(readable_schematic);
      event.readable_schematic_status = std::move(readable_schematic_status);
      event.script_text = std::move(script_text);
      event_sink = sink;
    }
    if (event_sink) event_sink(std::move(event));
  }

  void StartProbe() {
    const std::shared_ptr<Implementation> self = shared_from_this();
    const std::uint64_t generation = request.generation;
    runtime::ExecutionStartResult started = provider->Start(
        adapter.BuildProbeCommand(),
        [self, generation](std::string output) {
          SynthesisRunEvent event;
          event.kind = SynthesisRunEventKind::kOutput;
          event.state = SynthesisRunState::kProbing;
          event.generation = generation;
          event.output = std::move(output);
          self->Emit(std::move(event));
        },
        [self, generation](runtime::ProcessResult result) {
          {
            std::scoped_lock lock(self->mutex);
            if (self->shutdown || !self->active || self->terminal_delivered ||
                self->request.generation != generation ||
                (self->state != SynthesisRunState::kProbing &&
                 self->state != SynthesisRunState::kCancelling)) {
              return;
            }
          }
          if (!result.started || result.cancelled || result.exit_code != 0) {
            core::Status status =
                result.cancelled
                    ? core::Status{core::ErrorCode::kCancelled,
                                   "Synthesis cancelled", 0}
                    : core::Status{core::ErrorCode::kNotFound,
                                   "Yosys capability probe failed", 0};
            self->Finish(std::move(status), std::move(result), nullptr, {}, {},
                         {});
            return;
          }
          std::string version = result.output;
          const std::size_t newline = version.find_first_of("\r\n");
          if (newline != std::string::npos) version.resize(newline);
          self->EmitState(SynthesisRunState::kPreparing);
          const bool accepted = self->scheduler.Submit(
              [self, generation, version = std::move(version)](
                  std::stop_token stop_token) mutable {
                self->Prepare(generation, std::move(version), stop_token);
              });
          if (!accepted) {
            self->Finish({core::ErrorCode::kConflict,
                          "Synthesis preparation queue is unavailable", 0},
                         {}, nullptr, {}, {}, {});
          }
        });
    if (!started.Ok()) {
      Finish(started.status, {}, nullptr, {}, {}, {});
      return;
    }
    std::shared_ptr<runtime::ExecutionHandle> handle_to_cancel;
    {
      std::scoped_lock lock(mutex);
      handle =
          std::shared_ptr<runtime::ExecutionHandle>(std::move(started.handle));
      if (cancellation_requested) handle_to_cancel = handle;
    }
    if (handle_to_cancel != nullptr) handle_to_cancel->Cancel();
  }

  void Prepare(std::uint64_t generation, std::string tool_version,
               std::stop_token stop_token) {
    bool cancelled = false;
    {
      std::scoped_lock lock(mutex);
      cancelled = shutdown || stop_token.stop_requested() || !active ||
                  request.generation != generation || cancellation_requested;
    }
    if (cancelled) {
      runtime::ProcessResult result;
      result.cancelled = true;
      Finish({core::ErrorCode::kCancelled, "Synthesis cancelled", 0},
             std::move(result), nullptr, {}, {}, {});
      return;
    }
    auto acquired = resource_coordinator.AcquireCpu(
        request.project.cpu_budget, operation_stop_source.get_token());
    if (!acquired.Ok()) {
      Finish(acquired.GetStatus(), {}, nullptr, {}, {}, {});
      return;
    }
    {
      std::scoped_lock lock(mutex);
      cpu_lease =
          std::make_unique<runtime::CpuTokenLease>(std::move(acquired).Value());
    }
    auto begun = run_store.Begin(request.cell_directory, request.project,
                                 "Synthesis", "Yosys", tool_version);
    if (!begun.Ok()) {
      Finish(begun.GetStatus(), {}, nullptr, {}, {}, {});
      return;
    }
    auto prepared_run = std::make_shared<RunRecord>(std::move(begun).Value());
    auto staging = CreateAsciiStagingDirectory(prepared_run->id);
    if (!staging.Ok()) {
      CompleteFailedRun(prepared_run, staging.GetStatus());
      return;
    }
    adapters::SynthesisRequest adapter_request;
    adapter_request.project = request.project;
    adapter_request.sources = request.sources;
    adapter_request.flatten = request.project.synthesis.flatten;
    adapter_request.artifact_directory = staging.Value();
    for (const std::string& path : request.project.synthesis.liberty_paths) {
      adapter_request.liberty_files.push_back(request.library_directory / path);
    }
    runtime::PathMapper mapper;
    auto built = adapter.BuildPlan(adapter_request, mapper);
    if (!built.Ok()) {
      std::error_code error;
      std::filesystem::remove_all(staging.Value(), error);
      CompleteFailedRun(prepared_run, built.GetStatus());
      return;
    }
    adapters::SynthesisPlan prepared_plan = std::move(built).Value();
    auto hashed = CalculateSynthesisFingerprint(
        request.project, request.sources, request.library_directory);
    if (!hashed.Ok()) {
      std::error_code error;
      std::filesystem::remove_all(staging.Value(), error);
      CompleteFailedRun(prepared_run, hashed.GetStatus());
      return;
    }
    std::ofstream script(prepared_plan.script_path,
                         std::ios::binary | std::ios::trunc);
    script << prepared_plan.script_text;
    script.flush();
    if (!script) {
      std::error_code error;
      std::filesystem::remove_all(staging.Value(), error);
      CompleteFailedRun(prepared_run,
                        {core::ErrorCode::kIoError,
                         "Cannot write the Yosys synthesis script", 0});
      return;
    }
    cancelled = false;
    {
      std::scoped_lock lock(mutex);
      cancelled = shutdown || cancellation_requested || !active;
      if (!cancelled) {
        run = prepared_run;
        plan = prepared_plan;
        fingerprint = std::move(hashed).Value();
        input_hash = fingerprint.configuration_sha256;
        staging_directory = std::move(staging).Value();
      }
    }
    if (cancelled) {
      runtime::ProcessResult result;
      result.cancelled = true;
      std::vector<core::Diagnostic> diagnostics;
      static_cast<void>(run_store.Complete(
          prepared_run.get(), RunStatus::kCancelled, 0, diagnostics));
      std::error_code error;
      std::filesystem::remove_all(staging.Value(), error);
      Finish({core::ErrorCode::kCancelled, "Synthesis cancelled", 0},
             std::move(result), prepared_run, {}, {},
             prepared_plan.script_text);
      return;
    }
    EmitState(SynthesisRunState::kRunning);
    StartExecution(generation);
  }

  void CompleteFailedRun(const std::shared_ptr<RunRecord>& failed_run,
                         core::Status status) {
    std::vector<core::Diagnostic> diagnostics;
    std::vector<RunArtifact> artifacts;
    std::filesystem::path staged_script;
    std::filesystem::path staging;
    std::string hash;
    std::string script_text;
    {
      std::scoped_lock lock(mutex);
      staged_script = plan.script_path;
      staging = staging_directory;
      hash = input_hash;
      script_text = plan.script_text;
    }
    if (!staged_script.empty()) {
      const std::filesystem::path final_script =
          failed_run->directory / L"artifacts" / L"synthesis.ys";
      if (CopyIfPresent(staged_script, final_script)) {
        static_cast<void>(AddArtifact(failed_run, "script", "yosys",
                                      final_script, hash, true, &artifacts));
      }
    }
    static_cast<void>(run_store.Complete(failed_run.get(), RunStatus::kFailed,
                                         0, diagnostics, std::move(artifacts)));
    if (!staging.empty()) {
      std::error_code error;
      std::filesystem::remove_all(staging, error);
    }
    Finish(std::move(status), {}, failed_run, {}, {}, std::move(script_text));
  }

  void StartExecution(std::uint64_t generation) {
    std::shared_ptr<RunRecord> executing_run;
    adapters::SynthesisPlan executing_plan;
    {
      std::scoped_lock lock(mutex);
      executing_run = run;
      executing_plan = plan;
    }
    const std::shared_ptr<Implementation> self = shared_from_this();
    runtime::ExecutionStartResult started = provider->Start(
        executing_plan.execute,
        [self, generation, executing_run](std::string output) {
          static_cast<void>(self->run_store.AppendLog(*executing_run, output));
          SynthesisRunEvent event;
          event.kind = SynthesisRunEventKind::kOutput;
          event.state = SynthesisRunState::kRunning;
          event.generation = generation;
          event.output = std::move(output);
          self->Emit(std::move(event));
        },
        [self, generation](runtime::ProcessResult result) {
          {
            std::scoped_lock lock(self->mutex);
            if (self->shutdown || !self->active || self->terminal_delivered ||
                self->completion_claimed ||
                self->request.generation != generation ||
                (self->state != SynthesisRunState::kRunning &&
                 self->state != SynthesisRunState::kCancelling)) {
              return;
            }
            self->completion_claimed = true;
          }
          const bool accepted = self->scheduler.Submit(
              [self, generation,
               result = std::move(result)](std::stop_token stop_token) mutable {
                self->Finalize(generation, std::move(result), stop_token);
              });
          if (!accepted) {
            self->Finish({core::ErrorCode::kConflict,
                          "Synthesis result queue is unavailable", 0},
                         std::move(result), self->run, {}, {},
                         self->plan.script_text);
          }
        });
    if (!started.Ok()) {
      CompleteFailedRun(executing_run, started.status);
      return;
    }
    std::shared_ptr<runtime::ExecutionHandle> handle_to_cancel;
    {
      std::scoped_lock lock(mutex);
      handle =
          std::shared_ptr<runtime::ExecutionHandle>(std::move(started.handle));
      if (cancellation_requested) handle_to_cancel = handle;
    }
    if (handle_to_cancel != nullptr) handle_to_cancel->Cancel();
  }

  void Finalize(std::uint64_t generation, runtime::ProcessResult result,
                std::stop_token stop_token) {
    std::shared_ptr<RunRecord> completed_run;
    adapters::SynthesisPlan completed_plan;
    std::string completed_hash;
    std::filesystem::path completed_staging_directory;
    core::Project project;
    SynthesisFingerprint completed_fingerprint;
    {
      std::scoped_lock lock(mutex);
      if (shutdown || request.generation != generation) return;
      completed_run = run;
      completed_plan = plan;
      completed_hash = input_hash;
      completed_staging_directory = staging_directory;
      project = request.project;
      completed_fingerprint = fingerprint;
    }
    std::vector<core::Diagnostic> diagnostics =
        adapter.ParseDiagnostics(result.output);
    std::vector<RunArtifact> artifacts;
    const bool partial = !result.started || result.cancelled ||
                         result.exit_code != 0 || stop_token.stop_requested();
    const std::filesystem::path final_artifact_directory =
        completed_run->directory / L"artifacts";
    adapters::SynthesisPlan final_plan = completed_plan;
    final_plan.script_path = final_artifact_directory / L"synthesis.ys";
    final_plan.structural_schematic_path =
        final_artifact_directory / L"schematic-structural.json";
    final_plan.json_netlist_path = final_artifact_directory / L"netlist.json";
    final_plan.verilog_netlist_path = final_artifact_directory / L"netlist.v";
    final_plan.statistics_path = final_artifact_directory / L"statistics.json";
    final_plan.report_path = final_artifact_directory / L"synthesis.rpt";
    bool staging_copy_failed = false;
    const auto copy_staged = [&staging_copy_failed](
                                 const std::filesystem::path& source,
                                 const std::filesystem::path& destination) {
      std::error_code error;
      const bool exists = std::filesystem::is_regular_file(source, error);
      if (error || !exists) return;
      if (!CopyIfPresent(source, destination)) staging_copy_failed = true;
    };
    copy_staged(completed_plan.script_path, final_plan.script_path);
    copy_staged(completed_plan.structural_schematic_path,
                final_plan.structural_schematic_path);
    copy_staged(completed_plan.json_netlist_path, final_plan.json_netlist_path);
    copy_staged(completed_plan.verilog_netlist_path,
                final_plan.verilog_netlist_path);
    copy_staged(completed_plan.statistics_path, final_plan.statistics_path);
    copy_staged(completed_plan.report_path, final_plan.report_path);
    const bool has_script =
        AddArtifact(completed_run, "script", "yosys", final_plan.script_path,
                    completed_hash, partial, &artifacts);
    const bool has_structural =
        AddArtifact(completed_run, "schematic", "yosys-structural-json",
                    final_plan.structural_schematic_path, completed_hash,
                    partial, &artifacts);
    const bool has_json = AddArtifact(completed_run, "netlist", "json",
                                      final_plan.json_netlist_path,
                                      completed_hash, partial, &artifacts);
    const bool has_verilog = AddArtifact(completed_run, "netlist", "verilog",
                                         final_plan.verilog_netlist_path,
                                         completed_hash, partial, &artifacts);
    const bool has_statistics = AddArtifact(
        completed_run, "statistics", "json", final_plan.statistics_path,
        completed_hash, partial, &artifacts);
    const bool has_report =
        AddArtifact(completed_run, "report", "text", final_plan.report_path,
                    completed_hash, partial, &artifacts);
    core::Status parse_status = core::Status::Success();
    if (staging_copy_failed) {
      parse_status = {core::ErrorCode::kIoError,
                      "Cannot copy Yosys artifacts from ASCII staging", 0};
    }
    adapters::SynthesisMetrics metrics;
    core::SchematicModel gate_schematic;
    core::SchematicModel readable_schematic;
    core::Status readable_schematic_status = core::Status::Success();
    if (parse_status.Ok() && !has_statistics) {
      parse_status = {core::ErrorCode::kNotFound,
                      "Yosys statistics artifact is missing or empty", 0};
    } else if (parse_status.Ok()) {
      std::ifstream statistics(final_plan.statistics_path, std::ios::binary);
      std::ostringstream contents;
      contents << statistics.rdbuf();
      if (!statistics && !statistics.eof()) {
        parse_status = {core::ErrorCode::kIoError,
                        "Cannot read Yosys statistics artifact", 0};
      } else {
        auto parsed = adapter.ParseStatistics(contents.str());
        if (!parsed.Ok()) {
          parse_status = parsed.GetStatus();
        } else {
          metrics = std::move(parsed).Value();
        }
      }
    }
    if (has_json) {
      std::ifstream netlist(final_plan.json_netlist_path, std::ios::binary);
      std::ostringstream contents;
      contents << netlist.rdbuf();
      if (netlist || netlist.eof()) {
        auto parsed = adapter.ParseNetlist(contents.str(), project.top_module);
        if (parsed.Ok()) gate_schematic = std::move(parsed).Value();
      }
    }
    if (has_structural) {
      std::ifstream structural(final_plan.structural_schematic_path,
                               std::ios::binary);
      std::ostringstream contents;
      contents << structural.rdbuf();
      if (!structural && !structural.eof()) {
        readable_schematic_status = {
            core::ErrorCode::kIoError,
            "Cannot read structural schematic artifact", 0};
      } else {
        auto parsed = adapter.ParseNetlist(contents.str(), project.top_module);
        if (parsed.Ok()) {
          readable_schematic = std::move(parsed).Value();
        } else {
          readable_schematic_status = parsed.GetStatus();
        }
      }
    } else {
      readable_schematic_status = {
          core::ErrorCode::kNotFound,
          "Structural schematic artifact is unavailable", 0};
    }
    if (!readable_schematic_status.Ok()) {
      core::Diagnostic diagnostic;
      diagnostic.severity = core::DiagnosticSeverity::kWarning;
      diagnostic.code = "SCHEMATIC-READABLE";
      diagnostic.message = readable_schematic_status.message;
      diagnostics.push_back(std::move(diagnostic));
    }
    const bool process_succeeded =
        result.started && !result.cancelled && result.exit_code == 0;
    bool result_succeeded = process_succeeded && has_script && has_json &&
                            has_verilog && has_statistics && has_report &&
                            parse_status.Ok();
    std::filesystem::path summary_path;
    if (result_succeeded) {
      summary_path =
          completed_run->directory / L"reports" / L"synthesis-summary.json";
      std::ofstream summary(summary_path, std::ios::binary | std::ios::trunc);
      summary << "{\"schema_version\":2,\"top_module\":\""
              << EscapeJson(project.top_module)
              << "\",\"cell_count\":" << metrics.cell_count << ",\"area\":";
      if (metrics.has_area) {
        summary << metrics.area;
      } else {
        summary << "null";
      }
      summary << ",\"tool_version\":\""
              << EscapeJson(completed_run->tool_version) << "\",\"flatten\":"
              << (project.synthesis.flatten ? "true" : "false")
              << ",\"configuration_sha256\":\""
              << EscapeJson(completed_fingerprint.configuration_sha256)
              << "\",\"liberty_files\":[";
      for (std::size_t index = 0;
           index < completed_fingerprint.liberty_files.size(); ++index) {
        const LibertyFingerprint& liberty =
            completed_fingerprint.liberty_files[index];
        summary << (index == 0 ? "" : ",") << "{\"relative_path\":\""
                << EscapeJson(liberty.relative_path) << "\",\"sha256\":\""
                << EscapeJson(liberty.sha256) << "\"}";
      }
      summary << "]"
              << ",\"gate_cells\":" << gate_schematic.nodes.size()
              << ",\"readable_cells\":" << readable_schematic.nodes.size()
              << "}";
      summary.flush();
      if (!summary ||
          !AddArtifact(completed_run, "summary", "json", summary_path,
                       completed_hash, false, &artifacts)) {
        parse_status = {core::ErrorCode::kIoError,
                        "Cannot preserve synthesis summary", 0};
        result_succeeded = false;
      }
    }
    const RunStatus run_status = result.cancelled   ? RunStatus::kCancelled
                                 : result_succeeded ? RunStatus::kSucceeded
                                                    : RunStatus::kFailed;
    RunOutcome outcome;
    outcome.process_succeeded = process_succeeded;
    outcome.result_succeeded = result_succeeded;
    if (result_succeeded) {
      outcome.summary_relative_path = "reports/synthesis-summary.json";
    }
    const core::Status stored = run_store.Complete(
        completed_run.get(), run_status, result.exit_code, diagnostics,
        std::move(artifacts), std::move(outcome));
    core::Status status =
        !stored.Ok()       ? stored
        : result.cancelled ? core::Status{core::ErrorCode::kCancelled,
                                          "Synthesis cancelled", 0}
        : result_succeeded ? core::Status::Success()
        : !parse_status.Ok()
            ? parse_status
            : core::Status{core::ErrorCode::kIoError,
                           "Yosys did not produce all required artifacts", 0};
    Finish(std::move(status), std::move(result), completed_run,
           std::move(diagnostics), metrics, completed_plan.script_text,
           std::move(gate_schematic), std::move(readable_schematic),
           std::move(readable_schematic_status));
    if (!staging_copy_failed && !completed_staging_directory.empty()) {
      std::error_code error;
      std::filesystem::remove_all(completed_staging_directory, error);
    }
  }

  runtime::ExecutionProvider* provider = nullptr;
  mutable std::mutex mutex;
  runtime::TaskScheduler scheduler;
  runtime::ResourceCoordinator resource_coordinator;
  RunStore run_store;
  adapters::YosysAdapter adapter;
  SynthesisRunRequest request;
  SynthesisRunEventSink sink;
  SynthesisRunState state = SynthesisRunState::kIdle;
  std::shared_ptr<runtime::ExecutionHandle> handle;
  std::unique_ptr<runtime::CpuTokenLease> cpu_lease;
  std::shared_ptr<RunRecord> run;
  adapters::SynthesisPlan plan;
  SynthesisFingerprint fingerprint;
  std::string input_hash;
  std::filesystem::path staging_directory;
  bool active = false;
  bool cancellation_requested = false;
  bool completion_claimed = false;
  bool terminal_delivered = false;
  bool shutdown = false;
  std::stop_source operation_stop_source;
};

SynthesisRunService::SynthesisRunService(runtime::ExecutionProvider* provider)
    : implementation_(std::make_shared<Implementation>(provider)) {}

SynthesisRunService::~SynthesisRunService() { Shutdown(); }

core::Status SynthesisRunService::Start(SynthesisRunRequest request,
                                        SynthesisRunEventSink sink) {
  const std::shared_ptr<Implementation> implementation = implementation_;
  if (implementation == nullptr || implementation->provider == nullptr) {
    return {core::ErrorCode::kInvalidArgument,
            "Synthesis execution provider is unavailable", 0};
  }
  if (request.library_directory.empty() || request.cell_directory.empty() ||
      request.generation == 0) {
    return {core::ErrorCode::kInvalidArgument,
            "Synthesis run identity is incomplete", 0};
  }
  {
    std::scoped_lock lock(implementation->mutex);
    if (implementation->shutdown) {
      return {core::ErrorCode::kCancelled, "Synthesis service is shutting down",
              0};
    }
    if (implementation->active) {
      return {core::ErrorCode::kConflict,
              "A synthesis operation is already active", 0};
    }
    implementation->request = std::move(request);
    implementation->sink = std::move(sink);
    implementation->state = SynthesisRunState::kProbing;
    implementation->handle.reset();
    implementation->cpu_lease.reset();
    implementation->run.reset();
    implementation->plan = {};
    implementation->fingerprint = {};
    implementation->input_hash.clear();
    implementation->staging_directory.clear();
    implementation->active = true;
    implementation->cancellation_requested = false;
    implementation->completion_claimed = false;
    implementation->terminal_delivered = false;
    implementation->operation_stop_source = std::stop_source{};
  }
  implementation->EmitState(SynthesisRunState::kProbing);
  implementation->StartProbe();
  return core::Status::Success();
}

void SynthesisRunService::Cancel() noexcept {
  const std::shared_ptr<Implementation> implementation = implementation_;
  if (implementation == nullptr) return;
  std::shared_ptr<runtime::ExecutionHandle> handle;
  {
    std::scoped_lock lock(implementation->mutex);
    if (!implementation->active || implementation->terminal_delivered) return;
    implementation->cancellation_requested = true;
    implementation->operation_stop_source.request_stop();
    implementation->state = SynthesisRunState::kCancelling;
    handle = implementation->handle;
  }
  implementation->EmitState(SynthesisRunState::kCancelling);
  if (handle != nullptr) handle->Cancel();
}

void SynthesisRunService::Shutdown() noexcept {
  const std::shared_ptr<Implementation> implementation = implementation_;
  if (implementation == nullptr) return;
  std::shared_ptr<runtime::ExecutionHandle> handle;
  {
    std::scoped_lock lock(implementation->mutex);
    if (implementation->shutdown) return;
    implementation->shutdown = true;
    implementation->active = false;
    implementation->operation_stop_source.request_stop();
    handle = implementation->handle;
  }
  if (handle != nullptr) handle->Cancel();
  implementation->scheduler.RequestStop();
}

SynthesisRunState SynthesisRunService::State() const {
  std::scoped_lock lock(implementation_->mutex);
  return implementation_->state;
}

bool SynthesisRunService::IsActive() const {
  std::scoped_lock lock(implementation_->mutex);
  return implementation_->active;
}

}  // namespace designpp::application

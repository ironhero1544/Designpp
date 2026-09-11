// Copyright 2026 The Design++ Authors

#include "designpp/application/managed_flow_run_service.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <mutex>
#include <sstream>
#include <utility>

#include "designpp/adapters/managed_flow_adapter_factory.h"
#include "designpp/application/orfs_managed_flow_run_service.h"
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
    if (character == '\n') {
      result += "\\n";
    } else if (character != '\r') {
      result.push_back(character);
    }
  }
  return result;
}

std::wstring Utf8ToWide(std::string_view text) {
  if (text.empty()) return {};
  const int length =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), nullptr, 0);
  if (length <= 0) return {};
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

core::Result<std::filesystem::path> CreateStagingDirectory(
    std::string_view run_id) {
  wchar_t temporary_root[MAX_PATH]{};
  const DWORD length = GetTempPathW(
      static_cast<DWORD>(std::size(temporary_root)), temporary_root);
  if (length == 0 || length >= std::size(temporary_root)) {
    return core::Status{core::ErrorCode::kIoError,
                        "Cannot resolve the Windows temporary directory",
                        GetLastError()};
  }
  std::wstring id;
  for (const char character : run_id) {
    if (!std::isalnum(static_cast<unsigned char>(character)) &&
        character != '-') {
      return core::Status{core::ErrorCode::kInvalidArgument,
                          "OpenLane run identity is invalid", 0};
    }
    id.push_back(static_cast<unsigned char>(character));
  }
  const std::filesystem::path directory =
      std::filesystem::path(temporary_root) / L"DesignPlusPlus" / L"openlane" /
      id;
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  return error ? core::Result<std::filesystem::path>(
                     core::Status{core::ErrorCode::kIoError,
                                  "Cannot create OpenLane staging directory",
                                  static_cast<unsigned long>(error.value())})
               : core::Result<std::filesystem::path>(directory);
}

core::Status CopyFile(const std::filesystem::path& source,
                      const std::filesystem::path& destination) {
  std::error_code error;
  std::filesystem::create_directories(destination.parent_path(), error);
  if (!error) {
    std::filesystem::copy_file(
        source, destination, std::filesystem::copy_options::overwrite_existing,
        error);
  }
  return error ? core::Status{core::ErrorCode::kIoError,
                              "Cannot stage an OpenLane input",
                              static_cast<unsigned long>(error.value())}
               : core::Status::Success();
}

core::Result<std::string> ReadFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return core::Status{core::ErrorCode::kNotFound,
                        "OpenLane output file is missing", 0};
  }
  std::ostringstream output;
  output << input.rdbuf();
  return output.str();
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

std::string ProbeVersion(std::string_view output) {
  const std::size_t marker = output.find("OpenLane v");
  if (marker == std::string_view::npos) return "OpenLane 2";
  const std::size_t end = output.find_first_of("\r\n", marker);
  return std::string(output.substr(marker, end - marker));
}

void AppendOptionalNumber(std::ostream* output, std::string_view name,
                          const std::optional<double>& value) {
  *output << ",\"" << name << "\":";
  if (value) {
    *output << *value;
  } else {
    *output << "null";
  }
}

void CopyPartialReports(const std::filesystem::path& source_root,
                        const std::filesystem::path& destination_root,
                        const std::shared_ptr<RunRecord>& run,
                        std::string_view fingerprint,
                        std::vector<RunArtifact>* artifacts) {
  constexpr std::size_t kMaximumFiles = 256;
  constexpr std::uintmax_t kMaximumBytes = 256ULL * 1024ULL * 1024ULL;
  std::size_t copied_files = 0;
  std::uintmax_t copied_bytes = 0;
  std::error_code error;
  for (std::filesystem::recursive_directory_iterator
           iterator(source_root, error),
       end;
       !error && iterator != end && copied_files < kMaximumFiles;
       iterator.increment(error)) {
    if (!iterator->is_regular_file()) continue;
    std::string extension = iterator->path().extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](char character) {
                     return static_cast<char>(
                         std::tolower(static_cast<unsigned char>(character)));
                   });
    if (extension != ".log" && extension != ".rpt" && extension != ".json" &&
        extension != ".csv") {
      continue;
    }
    const std::uintmax_t size = iterator->file_size(error);
    if (error) break;
    if (copied_bytes + size > kMaximumBytes) break;
    const std::filesystem::path relative =
        std::filesystem::relative(iterator->path(), source_root, error);
    if (error || relative.empty()) break;
    const std::filesystem::path destination = destination_root / relative;
    core::Status copied = CopyFile(iterator->path(), destination);
    if (!copied.Ok()) continue;
    AddArtifact(run, "partial-output", extension, destination,
                std::string(fingerprint), true, artifacts);
    copied_bytes += size;
    ++copied_files;
  }
}

bool IsPhysicalVerificationReport(const std::filesystem::path& relative_path) {
  std::string path = relative_path.generic_string();
  std::transform(path.begin(), path.end(), path.begin(), [](char character) {
    return static_cast<char>(
        std::tolower(static_cast<unsigned char>(character)));
  });
  std::string extension = relative_path.extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](char character) {
                   return static_cast<char>(
                       std::tolower(static_cast<unsigned char>(character)));
                 });
  const bool report_extension = extension == ".rpt" || extension == ".report" ||
                                extension == ".txt" || extension == ".xml" ||
                                extension == ".log" || extension == ".drc" ||
                                extension == ".lvs" || extension.empty();
  if (!report_extension) return false;
  constexpr std::array<std::string_view, 8> kReportSteps = {
      "magic.drc",  "magic-drc",  "magic_drc",   "netgen.lvs",
      "netgen-lvs", "netgen_lvs", "klayout.drc", "klayout.xor"};
  return std::any_of(kReportSteps.begin(), kReportSteps.end(),
                     [&path](std::string_view step) {
                       return path.find(step) != std::string::npos;
                     });
}

void CopyPhysicalVerificationReports(
    const std::filesystem::path& source_root,
    const std::filesystem::path& destination_root,
    const std::shared_ptr<RunRecord>& run, std::string_view fingerprint,
    bool partial, std::vector<RunArtifact>* artifacts) {
  constexpr std::size_t kMaximumFiles = 256;
  constexpr std::uintmax_t kMaximumBytes = 256ULL * 1024ULL * 1024ULL;
  std::size_t copied_files = 0;
  std::uintmax_t copied_bytes = 0;
  std::error_code error;
  for (std::filesystem::recursive_directory_iterator
           iterator(source_root, error),
       end;
       !error && iterator != end && copied_files < kMaximumFiles;
       iterator.increment(error)) {
    if (!iterator->is_regular_file()) continue;
    const std::filesystem::path relative =
        std::filesystem::relative(iterator->path(), source_root, error);
    if (error || relative.empty()) break;
    if (!IsPhysicalVerificationReport(relative)) continue;
    const std::uintmax_t size = iterator->file_size(error);
    if (error) break;
    if (size == 0 || copied_bytes + size > kMaximumBytes) continue;
    const std::filesystem::path destination = destination_root / relative;
    const core::Status copied = CopyFile(iterator->path(), destination);
    if (!copied.Ok()) continue;
    std::string format = destination.extension().string();
    if (!format.empty() && format.front() == '.') format.erase(0, 1);
    if (format.empty()) format = "text";
    AddArtifact(run, "report", std::move(format), destination,
                std::string(fingerprint), partial, artifacts);
    copied_bytes += size;
    ++copied_files;
  }
}

}  // namespace

struct ManagedFlowRunService::Implementation final
    : public std::enable_shared_from_this<Implementation> {
  explicit Implementation(runtime::ExecutionProvider* execution_provider)
      : provider(execution_provider),
        scheduler(1),
        orfs_service(
            std::make_unique<OrfsManagedFlowRunService>(execution_provider)) {}

  void Emit(ManagedFlowRunEvent event) {
    ManagedFlowRunEventSink event_sink;
    {
      std::scoped_lock lock(mutex);
      if (shutdown || event.generation != request.generation) return;
      event_sink = sink;
    }
    if (event_sink) event_sink(std::move(event));
  }

  void EmitState(ManagedFlowRunState next_state) {
    ManagedFlowRunEvent event;
    {
      std::scoped_lock lock(mutex);
      if (shutdown || terminal_delivered) return;
      state = next_state;
      event.kind = ManagedFlowRunEventKind::kStateChanged;
      event.state = state;
      event.generation = request.generation;
    }
    Emit(std::move(event));
  }

  void EmitOutput(std::string output, ManagedFlowRunState output_state) {
    if (run) {
      const core::Status ignored = run_store.AppendLog(*run, output);
      (void)ignored;
    }
    ManagedFlowRunEvent event;
    event.kind = ManagedFlowRunEventKind::kOutput;
    event.state = output_state;
    event.generation = request.generation;
    event.output = output;
    Emit(std::move(event));
    std::istringstream lines{std::move(output)};
    std::string line;
    while (std::getline(lines, line)) {
      auto progress = adapter.ParseProgress(line);
      if (!progress) continue;
      last_step = progress->step_id;
      if (step_ids.empty() || step_ids.back() != progress->step_id) {
        step_ids.push_back(progress->step_id);
      }
      ManagedFlowRunEvent progress_event;
      progress_event.kind = ManagedFlowRunEventKind::kProgress;
      progress_event.state = output_state;
      progress_event.generation = request.generation;
      progress_event.progress = std::move(*progress);
      Emit(std::move(progress_event));
    }
  }

  void Finish(core::Status status, runtime::ProcessResult result,
              std::vector<core::Diagnostic> diagnostics,
              adapters::ManagedFlowMetrics completed_metrics = {}) {
    ManagedFlowRunEvent event;
    ManagedFlowRunEventSink event_sink;
    std::shared_ptr<runtime::ExecutionHandle> completed_handle;
    {
      std::scoped_lock lock(mutex);
      if (shutdown || terminal_delivered) return;
      terminal_delivered = true;
      active = false;
      cpu_lease.reset();
      state = result.cancelled || status.code == core::ErrorCode::kCancelled
                  ? ManagedFlowRunState::kCancelled
              : status.Ok() ? ManagedFlowRunState::kSucceeded
                            : ManagedFlowRunState::kFailed;
      event.kind = ManagedFlowRunEventKind::kCompleted;
      event.state = state;
      event.generation = request.generation;
      event.status = std::move(status);
      event.process_result = std::move(result);
      event.run = run;
      event.diagnostics = std::move(diagnostics);
      event.metrics = std::move(completed_metrics);
      event.configuration_fingerprint = fingerprint;
      event.lineage_id = lineage_id;
      event_sink = sink;
      completed_handle = std::move(handle);
    }
    QueueHandleCleanup(std::move(completed_handle));
    if (event_sink) event_sink(std::move(event));
  }

  void StartProbe() {
    const auto self = shared_from_this();
    const std::uint64_t generation = request.generation;
    runtime::WslCommand command = adapter.BuildProbeCommand(request.profile);
    runtime::ExecutionStartResult started = provider->Start(
        command,
        [self](std::string output) {
          self->probe_output += output;
          self->EmitOutput(std::move(output), ManagedFlowRunState::kProbing);
        },
        [self, generation](runtime::ProcessResult result) {
          if (!self->AcceptCallback(generation,
                                    ManagedFlowRunState::kProbing)) {
            return;
          }
          if (!result.started || result.cancelled || result.exit_code != 0 ||
              result.output.find("2.3.10") == std::string::npos) {
            self->Finish(
                result.cancelled
                    ? core::Status{core::ErrorCode::kCancelled,
                                   "OpenLane flow cancelled", 0}
                    : core::Status{core::ErrorCode::kNotFound,
                                   "OpenLane 2.3.10 capability probe failed",
                                   0},
                std::move(result), {});
            return;
          }
          self->tool_version = ProbeVersion(result.output);
          self->EmitState(ManagedFlowRunState::kPreparing);
          const bool queued = self->scheduler.Submit(
              [self, generation](std::stop_token stop_token) {
                self->Prepare(generation, stop_token);
              });
          if (!queued) {
            self->Finish({core::ErrorCode::kConflict,
                          "OpenLane preparation queue is unavailable", 0},
                         {}, {});
          }
        });
    if (!started.Ok()) {
      Finish(started.status, {}, {});
      return;
    }
    StoreHandle(std::move(started.handle));
  }

  bool AcceptCallback(std::uint64_t generation,
                      ManagedFlowRunState expected) const {
    std::scoped_lock lock(mutex);
    return !shutdown && active && !terminal_delivered &&
           request.generation == generation &&
           (state == expected || state == ManagedFlowRunState::kCancelling);
  }

  void StoreHandle(std::unique_ptr<runtime::ExecutionHandle> next) {
    bool cancel = false;
    std::shared_ptr<runtime::ExecutionHandle> previous;
    std::shared_ptr<runtime::ExecutionHandle> current;
    {
      std::scoped_lock lock(mutex);
      if (shutdown || terminal_delivered) {
        previous = std::shared_ptr<runtime::ExecutionHandle>(std::move(next));
      } else {
        previous = std::move(handle);
        handle = std::shared_ptr<runtime::ExecutionHandle>(std::move(next));
      }
      cancel = cancellation_requested;
      current = handle;
    }
    QueueHandleCleanup(std::move(previous));
    if (cancel && current) current->Cancel();
  }

  void QueueHandleCleanup(
      std::shared_ptr<runtime::ExecutionHandle> completed_handle) {
    if (!completed_handle) return;
    auto holder = std::make_shared<std::shared_ptr<runtime::ExecutionHandle>>(
        std::move(completed_handle));
    const bool queued =
        cleanup_scheduler.Submit([holder](std::stop_token) mutable {
          // Destruction on this worker may join the process callback thread.
          // It must never happen from inside that callback itself.
          auto handle = std::move(*holder);
          holder.reset();
          static_cast<void>(handle->IsRunning());
        });
    if (!queued) {
      std::scoped_lock lock(mutex);
      deferred_handle_cleanup.push_back(std::move(*holder));
    }
  }

  void Prepare(std::uint64_t generation, std::stop_token stop_token) {
    if (stop_token.stop_requested() || operation_stop_source.stop_requested()) {
      Finish({core::ErrorCode::kCancelled, "OpenLane flow cancelled", 0}, {},
             {});
      return;
    }
    auto begun =
        run_store.Begin(request.cell_directory, request.project,
                        "physical_implementation", "openlane2", tool_version);
    if (!begun.Ok()) {
      Finish(begun.GetStatus(), {}, {});
      return;
    }
    run = std::make_shared<RunRecord>(std::move(begun).Value());
    run->environment_id = request.environment_id;
    run->environment_fingerprint = request.environment_fingerprint;
    if (!probe_output.empty()) {
      const core::Status ignored = run_store.AppendLog(*run, probe_output);
      (void)ignored;
    }
    auto staging = CreateStagingDirectory(run->id);
    if (!staging.Ok()) {
      CompletePreparationFailure(staging.GetStatus());
      return;
    }
    staging_directory = std::move(staging).Value();
    std::error_code error;
    std::filesystem::create_directories(staging_directory / "src", error);
    std::filesystem::create_directories(staging_directory / "constraints",
                                        error);
    std::filesystem::create_directories(staging_directory / "include", error);
    if (error) {
      CompletePreparationFailure({core::ErrorCode::kIoError,
                                  "Cannot prepare OpenLane staging folders",
                                  static_cast<unsigned long>(error.value())});
      return;
    }

    adapters::OpenLaneRequest adapter_request;
    adapter_request.profile = request.profile;
    adapter_request.configuration = request.project.physical_implementation;
    adapter_request.top_module = request.project.top_module;
    adapter_request.defines = request.project.defines;
    adapter_request.parameters = request.project.parameters;
    adapter_request.cpu_threads = request.project.cpu_budget;
    std::size_t source_index = 0;
    std::ostringstream fingerprint_manifest;
    // Keep the cell identity in the input fingerprint.  RunStore is normally
    // already cell-local, but this prevents a copied or repaired run directory
    // from becoming compatible with a different cell that happens to have the
    // same RTL and physical settings.
    fingerprint_manifest
        << "project:" << request.project.id << '\n'
        << "library:" << request.project.library_id << '\n'
        << "cell:" << request.project.cell_id << '\n'
        << request.project.top_module << '\n'
        << request.profile.openlane_root << '\n'
        << request.profile.pdk_root << '\n'
        << "environment:" << request.environment_id << ':'
        << request.environment_fingerprint << '\n'
        << request.project.physical_implementation.pdk << '\n'
        << request.project.physical_implementation.standard_cell_library
        << '\n';
    for (const ResolvedSource& source : request.sources) {
      if (!source.enabled || !source.exists ||
          source.view_kind != core::ViewKind::kVerilog) {
        continue;
      }
      std::filesystem::path extension = source.windows_path.extension();
      if (extension.empty()) extension = ".sv";
      const std::filesystem::path target =
          staging_directory / "src" /
          ("source_" + std::to_string(source_index++) + extension.string());
      core::Status copied = CopyFile(source.windows_path, target);
      if (!copied.Ok()) {
        CompletePreparationFailure(std::move(copied));
        return;
      }
      auto hash = CalculateFileSha256(target);
      if (!hash.Ok()) {
        CompletePreparationFailure(hash.GetStatus());
        return;
      }
      fingerprint_manifest << source.relative_path << ':' << hash.Value()
                           << '\n';
      adapter_request.sources.push_back(
          {source.relative_path, target.filename()});
    }
    if (adapter_request.sources.empty()) {
      CompletePreparationFailure({core::ErrorCode::kInvalidArgument,
                                  "No enabled RTL is available for OpenLane",
                                  0});
      return;
    }
    for (std::size_t index = 0;
         index < request.project.include_directories.size(); ++index) {
      const std::filesystem::path source =
          request.library_directory /
          std::filesystem::path(request.project.include_directories[index]);
      const std::filesystem::path target =
          staging_directory / "include" / ("include_" + std::to_string(index));
      std::filesystem::copy(
          source, target,
          std::filesystem::copy_options::recursive |
              std::filesystem::copy_options::overwrite_existing,
          error);
      if (error) {
        CompletePreparationFailure(
            {core::ErrorCode::kIoError,
             "Cannot stage an OpenLane include directory",
             static_cast<unsigned long>(error.value())});
        return;
      }
      adapter_request.include_directories.push_back(
          request.project.include_directories[index]);
      std::vector<std::filesystem::path> include_files;
      for (std::filesystem::recursive_directory_iterator
               iterator(target, error),
           end;
           !error && iterator != end; iterator.increment(error)) {
        if (iterator->is_regular_file())
          include_files.push_back(iterator->path());
      }
      if (error) {
        CompletePreparationFailure(
            {core::ErrorCode::kIoError,
             "Cannot fingerprint an OpenLane include directory",
             static_cast<unsigned long>(error.value())});
        return;
      }
      std::sort(include_files.begin(), include_files.end());
      for (const auto& include_file : include_files) {
        auto include_hash = CalculateFileSha256(include_file);
        if (!include_hash.Ok()) {
          CompletePreparationFailure(include_hash.GetStatus());
          return;
        }
        fingerprint_manifest
            << "include:"
            << std::filesystem::relative(include_file, target).generic_string()
            << ':' << include_hash.Value() << '\n';
      }
    }
    auto stage_sdc = [this](const std::string& relative, std::string_view name,
                            std::filesystem::path* output) {
      if (relative.empty()) return core::Status::Success();
      const std::filesystem::path source =
          request.library_directory / std::filesystem::path(relative);
      const std::filesystem::path target =
          staging_directory / "constraints" / std::filesystem::path(name);
      core::Status status = CopyFile(source, target);
      if (status.Ok()) *output = target.filename();
      return status;
    };
    core::Status sdc_status =
        stage_sdc(request.project.physical_implementation.pnr_sdc_path,
                  "pnr.sdc", &adapter_request.pnr_sdc);
    if (sdc_status.Ok()) {
      sdc_status =
          stage_sdc(request.project.physical_implementation.signoff_sdc_path,
                    "signoff.sdc", &adapter_request.signoff_sdc);
    }
    if (!sdc_status.Ok()) {
      CompletePreparationFailure(std::move(sdc_status));
      return;
    }
    auto pin_order = adapter.BuildPinOrderConfiguration(
        request.project.physical_implementation);
    if (!pin_order.Ok()) {
      CompletePreparationFailure(pin_order.GetStatus());
      return;
    }
    if (!pin_order.Value().empty()) {
      const std::filesystem::path pin_order_path =
          staging_directory / "constraints" / "pin_order.cfg";
      std::ofstream pin_order_output(pin_order_path,
                                     std::ios::binary | std::ios::trunc);
      pin_order_output << pin_order.Value();
      pin_order_output.close();
      if (!pin_order_output) {
        CompletePreparationFailure(
            {core::ErrorCode::kIoError,
             "Cannot stage the OpenLane pin-order configuration", 0});
        return;
      }
      adapter_request.pin_order_cfg = pin_order_path.filename();
      fingerprint_manifest << "pin_order:\n" << pin_order.Value();
    }
    for (const std::string& define : request.project.defines) {
      fingerprint_manifest << "define:" << define << '\n';
    }
    for (const std::string& parameter : request.project.parameters) {
      fingerprint_manifest << "parameter:" << parameter << '\n';
    }
    for (const std::string& clock :
         request.project.physical_implementation.clock_ports) {
      fingerprint_manifest << "clock:" << clock << '\n';
    }
    fingerprint_manifest
        << "period:" << request.project.physical_implementation.clock_period_ns
        << '\n'
        << "utilization:"
        << request.project.physical_implementation.core_utilization_percent
        << '\n';
    if (request.project.physical_implementation.placement_density_percent) {
      fingerprint_manifest
          << "density:"
          << *request.project.physical_implementation.placement_density_percent
          << '\n';
    }
    for (const std::string& coordinate :
         request.project.physical_implementation.die_area) {
      fingerprint_manifest << "die:" << coordinate << '\n';
    }
    for (const std::string& coordinate :
         request.project.physical_implementation.core_area) {
      fingerprint_manifest << "core:" << coordinate << '\n';
    }
    fingerprint_manifest << "tap_cell_distance:"
                         << request.project.physical_implementation
                                .tap_cell_distance_um.value_or("")
                         << '\n';
    const core::PowerDistributionConfiguration& pdn =
        request.project.physical_implementation.power_distribution;
    fingerprint_manifest << "pdn_multilayer:" << pdn.multilayer << '\n'
                         << "pdn_core_ring:" << pdn.core_ring << '\n'
                         << "pdn_enable_rails:" << pdn.enable_rails << '\n';
    const auto fingerprint_pdn = [&fingerprint_manifest](
                                     std::string_view name,
                                     const std::optional<std::string>& value) {
      fingerprint_manifest << name << ':' << value.value_or("") << '\n';
    };
    fingerprint_pdn("pdn_vwidth", pdn.vertical_width_um);
    fingerprint_pdn("pdn_hwidth", pdn.horizontal_width_um);
    fingerprint_pdn("pdn_vspacing", pdn.vertical_spacing_um);
    fingerprint_pdn("pdn_hspacing", pdn.horizontal_spacing_um);
    fingerprint_pdn("pdn_vpitch", pdn.vertical_pitch_um);
    fingerprint_pdn("pdn_hpitch", pdn.horizontal_pitch_um);
    fingerprint_pdn("pdn_voffset", pdn.vertical_offset_um);
    fingerprint_pdn("pdn_hoffset", pdn.horizontal_offset_um);
    const core::IoPlacementConfiguration& io =
        request.project.physical_implementation.io_placement;
    fingerprint_manifest << "io_algorithm:" << io.algorithm << '\n'
                         << "io_unmatched:" << io.unmatched_policy << '\n';
    const auto fingerprint_io = [&fingerprint_manifest](
                                    std::string_view name,
                                    const std::optional<std::string>& value) {
      fingerprint_manifest << name << ':' << value.value_or("") << '\n';
    };
    fingerprint_io("io_min_distance", io.minimum_distance_um);
    fingerprint_io("io_vlength", io.vertical_length_um);
    fingerprint_io("io_hlength", io.horizontal_length_um);
    fingerprint_io("io_vthickness", io.vertical_thickness_multiplier);
    fingerprint_io("io_hthickness", io.horizontal_thickness_multiplier);
    fingerprint_io("io_vextend", io.vertical_extension_um);
    fingerprint_io("io_hextend", io.horizontal_extension_um);
    fingerprint_io("io_vlayer", io.vertical_layer);
    fingerprint_io("io_hlayer", io.horizontal_layer);
    for (const auto& [name, path] :
         std::array<std::pair<std::string_view, std::filesystem::path>, 2>{
             {{"pnr_sdc", adapter_request.pnr_sdc},
              {"signoff_sdc", adapter_request.signoff_sdc}}}) {
      if (path.empty()) continue;
      auto sdc_hash =
          CalculateFileSha256(staging_directory / "constraints" / path);
      if (!sdc_hash.Ok()) {
        CompletePreparationFailure(sdc_hash.GetStatus());
        return;
      }
      fingerprint_manifest << name << ':' << sdc_hash.Value() << '\n';
    }

    const std::filesystem::path fingerprint_path =
        staging_directory / "fingerprint.txt";
    {
      std::ofstream manifest(fingerprint_path, std::ios::binary);
      manifest
          << fingerprint_manifest.str()
          << request.project.physical_implementation.advanced_overrides_json;
    }
    auto calculated = CalculateFileSha256(fingerprint_path);
    if (!calculated.Ok()) {
      CompletePreparationFailure(calculated.GetStatus());
      return;
    }
    fingerprint = std::move(calculated).Value();
    if (request.resume &&
        request.resume->configuration_fingerprint != fingerprint) {
      CompletePreparationFailure(
          {core::ErrorCode::kConflict,
           "OpenLane resume fingerprint no longer matches project inputs", 0});
      return;
    }
    lineage_id = request.resume ? request.resume->lineage_id : run->id;
    adapter_request.resume_step =
        request.resume ? request.resume->resume_step : std::string{};
    adapter_request.checkpoint_hash =
        request.resume ? request.resume->checkpoint_hash : std::string{};
    adapter_request.backend_workspace =
        ".designpp/runs/openlane/" + request.project.id + "/" + lineage_id;
    runtime::PathMapper mapper;
    auto mapped = mapper.WindowsToWsl(staging_directory);
    if (!mapped.Ok()) {
      CompletePreparationFailure(mapped.GetStatus());
      return;
    }
    adapter_request.staging_workspace = WideToUtf8(mapped.Value());
    auto built = adapter.BuildPlan(adapter_request);
    if (!built.Ok()) {
      CompletePreparationFailure(built.GetStatus());
      return;
    }
    plan = std::move(built).Value();
    {
      std::ofstream config(staging_directory / "config.json", std::ios::binary);
      config << plan.config_json;
      if (!config) {
        CompletePreparationFailure({core::ErrorCode::kIoError,
                                    "Cannot write OpenLane config.json", 0});
        return;
      }
    }
    auto lease = resource_coordinator.AcquireCpu(
        request.project.cpu_budget, operation_stop_source.get_token());
    if (!lease.Ok()) {
      CompletePreparationFailure(lease.GetStatus());
      return;
    }
    cpu_lease =
        std::make_unique<runtime::CpuTokenLease>(std::move(lease).Value());
    if (request.resume) {
      StartFlow(generation);
    } else {
      StartValidation(generation);
    }
  }

  void CompletePreparationFailure(core::Status status) {
    std::vector<core::Diagnostic> diagnostics;
    if (run) {
      core::Diagnostic diagnostic;
      diagnostic.severity = core::DiagnosticSeverity::kError;
      diagnostic.code = "OPENLANE-PREPARE";
      diagnostic.message = status.message;
      diagnostics.push_back(diagnostic);
      std::vector<RunArtifact> artifacts;
      const std::filesystem::path artifact_directory =
          run->directory / "artifacts";
      const std::filesystem::path reports_directory =
          run->directory / "reports";
      std::error_code error;
      std::filesystem::create_directories(artifact_directory, error);
      std::filesystem::create_directories(reports_directory, error);
      const auto preserve = [this, &artifacts](
                                const std::filesystem::path& source,
                                std::string_view name) {
        if (source.empty() || !CopyFile(source, run->directory / "artifacts" /
                                                    std::filesystem::path(name))
                                   .Ok()) {
          return;
        }
        const std::filesystem::path destination =
            run->directory / "artifacts" / std::filesystem::path(name);
        AddArtifact(run, std::string(name), destination.extension().string(),
                    destination, fingerprint, true, &artifacts);
      };
      if (!staging_directory.empty()) {
        preserve(staging_directory / "config.json", "config.json");
        preserve(staging_directory / "fingerprint.txt", "fingerprint.txt");
        CopyPartialReports(staging_directory / "backend",
                           artifact_directory / "step-logs", run, fingerprint,
                           &artifacts);
        CopyPhysicalVerificationReports(staging_directory / "backend",
                                        reports_directory / "openlane", run,
                                        fingerprint, true, &artifacts);
      }
      const core::Status ignored =
          run_store.Complete(run.get(), RunStatus::kFailed, 0, diagnostics,
                             std::move(artifacts), {});
      (void)ignored;
    }
    Finish(std::move(status), {}, std::move(diagnostics));
  }

  void StartValidation(std::uint64_t generation) {
    EmitState(ManagedFlowRunState::kValidating);
    const auto self = shared_from_this();
    runtime::ExecutionStartResult started = provider->Start(
        plan.validate,
        [self](std::string output) {
          self->EmitOutput(std::move(output), ManagedFlowRunState::kValidating);
        },
        [self, generation](runtime::ProcessResult result) {
          if (!self->AcceptCallback(generation,
                                    ManagedFlowRunState::kValidating)) {
            return;
          }
          if (!result.started || result.cancelled || result.exit_code != 0) {
            self->Collect(std::move(result));
            return;
          }
          self->StartFlow(generation);
        });
    if (!started.Ok()) {
      CompletePreparationFailure(started.status);
      return;
    }
    StoreHandle(std::move(started.handle));
  }

  void StartFlow(std::uint64_t generation) {
    EmitState(ManagedFlowRunState::kRunning);
    const auto self = shared_from_this();
    runtime::ExecutionStartResult started = provider->Start(
        plan.execute,
        [self](std::string output) {
          self->EmitOutput(std::move(output), ManagedFlowRunState::kRunning);
        },
        [self, generation](runtime::ProcessResult result) {
          if (!self->AcceptCallback(generation,
                                    ManagedFlowRunState::kRunning)) {
            return;
          }
          self->Collect(std::move(result));
        });
    if (!started.Ok()) {
      CompletePreparationFailure(started.status);
      return;
    }
    StoreHandle(std::move(started.handle));
  }

  void Collect(runtime::ProcessResult flow_result) {
    EmitState(ManagedFlowRunState::kCollecting);
    process_result = std::move(flow_result);
    runtime::PathMapper mapper;
    const std::filesystem::path collected = staging_directory / "backend";
    std::error_code error;
    std::filesystem::create_directories(collected, error);
    auto mapped = mapper.WindowsToWsl(collected);
    if (!mapped.Ok()) {
      Finalize(mapped.GetStatus());
      return;
    }
    runtime::WslCommand copy;
    copy.program = L"/bin/bash";
    copy.arguments = {
        L"-lc",
        L"workspace=\"$HOME/$1\"; destination=\"$2\"; "
        L"if [ -d \"$workspace/runs/designpp\" ]; then "
        L"cp -R \"$workspace/runs/designpp\"/. \"$destination\"/; fi",
        L"designpp-openlane-collect",
        Utf8ToWide(".designpp/runs/openlane/" + request.project.id + "/" +
                   lineage_id),
        mapped.Value()};
    if (!request.profile.wsl_distribution.empty()) {
      copy.distribution = Utf8ToWide(request.profile.wsl_distribution);
    }
    const auto self = shared_from_this();
    const std::uint64_t generation = request.generation;
    runtime::ExecutionStartResult started = provider->Start(
        copy,
        [self](std::string output) {
          self->EmitOutput(std::move(output), ManagedFlowRunState::kCollecting);
        },
        [self, generation](runtime::ProcessResult result) {
          if (!self->AcceptCallback(generation,
                                    ManagedFlowRunState::kCollecting)) {
            return;
          }
          self->Finalize(result.started && result.exit_code == 0
                             ? core::Status::Success()
                             : core::Status{core::ErrorCode::kIoError,
                                            "Cannot collect OpenLane outputs",
                                            0});
        });
    if (!started.Ok()) {
      Finalize(started.status);
      return;
    }
    StoreHandle(std::move(started.handle));
  }

  void Finalize(core::Status collection_status) {
    const auto self = shared_from_this();
    const std::uint64_t generation = request.generation;
    const bool queued = scheduler.Submit(
        [self, generation, collection_status = std::move(collection_status)](
            std::stop_token) mutable {
          self->FinalizeOnWorker(generation, std::move(collection_status));
        });
    if (!queued) {
      CompletePreparationFailure({core::ErrorCode::kConflict,
                                  "OpenLane finalization queue is unavailable",
                                  0});
    }
  }

  void FinalizeOnWorker(std::uint64_t generation,
                        core::Status collection_status) {
    if (!AcceptCallback(generation, ManagedFlowRunState::kCollecting)) return;
    std::vector<core::Diagnostic> diagnostics =
        adapter.ParseDiagnostics(process_result.output);
    std::vector<RunArtifact> artifacts;
    adapters::ManagedFlowMetrics metrics;
    core::Status result_status = collection_status;
    const std::filesystem::path artifact_directory =
        run->directory / "artifacts";
    const std::filesystem::path reports_directory = run->directory / "reports";
    std::error_code error;
    std::filesystem::create_directories(artifact_directory, error);
    std::filesystem::create_directories(reports_directory, error);
    CopyFile(staging_directory / "config.json",
             artifact_directory / "config.json");
    AddArtifact(run, "configuration", "json",
                artifact_directory / "config.json", fingerprint, false,
                &artifacts);

    std::istringstream completed_output(process_result.output);
    std::string completed_line;
    while (std::getline(completed_output, completed_line)) {
      auto progress = adapter.ParseProgress(completed_line);
      if (!progress) continue;
      last_step = progress->step_id;
      if (step_ids.empty() || step_ids.back() != progress->step_id) {
        step_ids.push_back(progress->step_id);
      }
    }
    const adapters::ManagedFlowArtifactSet set =
        adapter.DiscoverAvailableArtifacts(staging_directory / "backend");
    const core::Status final_artifact_status =
        adapter.ValidateFinalArtifacts(set);
    bool artifacts_complete = final_artifact_status.Ok();
    {
      const std::array<std::pair<const std::filesystem::path*, const char*>, 13>
          copies = {{{&set.resolved_config, "resolved.json"},
                     {&set.final_state, "state_out.json"},
                     {&set.metrics_json, "metrics.json"},
                     {&set.metrics_csv, "metrics.csv"},
                     {&set.gds, "final.gds"},
                     {&set.def, "final.def"},
                     {&set.lef, "final.lef"},
                     {&set.odb, "final.odb"},
                     {&set.gate_netlist, "netlist.v"},
                     {&set.power_netlist, "power.v"},
                     {&set.sdf, "final.sdf"},
                     {&set.spef, "final.spef"},
                     {&set.final_state, "checkpoint-state.json"}}};
      for (const auto& [source, name] : copies) {
        if (source->empty()) continue;
        const std::filesystem::path destination = artifact_directory / name;
        const core::Status copied = CopyFile(*source, destination);
        if (!copied.Ok()) continue;
        AddArtifact(run, name, destination.extension().string(), destination,
                    fingerprint, !artifacts_complete, &artifacts);
      }
      auto metrics_text = ReadFile(set.metrics_json);
      if (metrics_text.Ok()) {
        auto parsed = adapter.ParseMetrics(metrics_text.Value());
        if (parsed.Ok()) {
          metrics = std::move(parsed).Value();
        } else {
          result_status = parsed.GetStatus();
          artifacts_complete = false;
        }
      }
    }
    CopyPartialReports(staging_directory / "backend",
                       artifact_directory / "step-logs", run, fingerprint,
                       &artifacts);
    CopyPhysicalVerificationReports(
        staging_directory / "backend", reports_directory / "openlane", run,
        fingerprint,
        !artifacts_complete || process_result.exit_code != 0 ||
            process_result.cancelled,
        &artifacts);
    if (!final_artifact_status.Ok() && process_result.exit_code == 0 &&
        !process_result.cancelled) {
      result_status = final_artifact_status;
    }

    const bool process_succeeded = process_result.started &&
                                   !process_result.cancelled &&
                                   process_result.exit_code == 0;
    const bool result_succeeded = process_succeeded && artifacts_complete &&
                                  metrics.Passed() && collection_status.Ok();
    std::string checkpoint_hash;
    auto hashed_checkpoint =
        CalculateFileSha256(artifact_directory / "checkpoint-state.json");
    if (hashed_checkpoint.Ok()) {
      checkpoint_hash = std::move(hashed_checkpoint).Value();
    }
    const std::filesystem::path summary_path =
        reports_directory / "openlane-summary.json";
    {
      std::ofstream summary(summary_path, std::ios::binary);
      summary << "{\"schema_version\":1,\"tool\":\"openlane2\""
              << ",\"tool_version\":\"" << EscapeJson(tool_version) << '"'
              << ",\"project_id\":\"" << EscapeJson(request.project.id)
              << "\",\"library_id\":\""
              << EscapeJson(request.project.library_id) << "\",\"cell_id\":\""
              << EscapeJson(request.project.cell_id)
              << "\",\"project_revision\":" << request.project.revision
              << ",\"backend_id\":\""
              << EscapeJson(request.project.physical_implementation.backend_id)
              << "\",\"lineage_id\":\"" << EscapeJson(lineage_id)
              << "\",\"parent_run_id\":\""
              << EscapeJson(request.resume ? request.resume->parent_run_id : "")
              << "\",\"resume_step\":\""
              << EscapeJson(request.resume ? request.resume->resume_step : "")
              << "\",\"checkpoint_hash\":\"" << EscapeJson(checkpoint_hash)
              << "\",\"configuration_fingerprint\":\"" << fingerprint
              << "\",\"last_step\":\"" << EscapeJson(last_step)
              << "\",\"process_succeeded\":"
              << (process_succeeded ? "true" : "false")
              << ",\"result_succeeded\":"
              << (result_succeeded ? "true" : "false") << ",\"steps\":[";
      for (std::size_t index = 0; index < step_ids.size(); ++index) {
        if (index != 0) summary << ',';
        summary << '"' << EscapeJson(step_ids[index]) << '"';
      }
      summary << ']';
      AppendOptionalNumber(&summary, "core_area", metrics.core_area);
      AppendOptionalNumber(&summary, "die_area", metrics.die_area);
      AppendOptionalNumber(&summary, "utilization", metrics.utilization);
      AppendOptionalNumber(&summary, "instance_count", metrics.instance_count);
      AppendOptionalNumber(&summary, "setup_wns", metrics.setup_wns);
      AppendOptionalNumber(&summary, "setup_tns", metrics.setup_tns);
      AppendOptionalNumber(&summary, "hold_wns", metrics.hold_wns);
      AppendOptionalNumber(&summary, "hold_tns", metrics.hold_tns);
      AppendOptionalNumber(&summary, "worst_setup_skew",
                           metrics.worst_setup_skew);
      AppendOptionalNumber(&summary, "worst_hold_skew",
                           metrics.worst_hold_skew);
      AppendOptionalNumber(&summary, "wire_length", metrics.wire_length);
      AppendOptionalNumber(&summary, "global_route_congestion",
                           metrics.global_route_congestion);
      AppendOptionalNumber(&summary, "detailed_route_congestion",
                           metrics.detailed_route_congestion);
      AppendOptionalNumber(&summary, "global_route_overflow",
                           metrics.global_route_overflow);
      AppendOptionalNumber(&summary, "detailed_route_overflow",
                           metrics.detailed_route_overflow);
      AppendOptionalNumber(&summary, "routing_violations",
                           metrics.routing_violations);
      AppendOptionalNumber(&summary, "runtime_seconds",
                           metrics.runtime_seconds);
      AppendOptionalNumber(&summary, "peak_memory_mb", metrics.peak_memory_mb);
      AppendOptionalNumber(&summary, "antenna_violations",
                           metrics.antenna_violations);
      AppendOptionalNumber(&summary, "drc_violations", metrics.drc_violations);
      AppendOptionalNumber(&summary, "xor_violations", metrics.xor_violations);
      AppendOptionalNumber(&summary, "lvs_errors", metrics.lvs_errors);
      summary << ",\"metrics_raw_count\":" << metrics.raw.size() << "}";
    }
    AddArtifact(run, "summary", "json", summary_path, fingerprint, false,
                &artifacts);
    const std::filesystem::path checkpoint_path =
        reports_directory / "backend-checkpoint.json";
    {
      std::ofstream checkpoint(checkpoint_path, std::ios::binary);
      checkpoint << "{\"lineage_id\":\"" << EscapeJson(lineage_id)
                 << "\",\"last_step\":\"" << EscapeJson(last_step)
                 << "\",\"configuration_fingerprint\":\"" << fingerprint
                 << "\",\"checkpoint_hash\":\"" << EscapeJson(checkpoint_hash)
                 << "\"}";
    }
    AddArtifact(run, "checkpoint", "json", checkpoint_path, fingerprint,
                !result_succeeded, &artifacts);

    RunOutcome outcome;
    outcome.process_succeeded = process_succeeded;
    outcome.result_succeeded = result_succeeded;
    outcome.summary_relative_path = "reports/openlane-summary.json";
    const RunStatus run_status = process_result.cancelled
                                     ? RunStatus::kCancelled
                                 : result_succeeded ? RunStatus::kSucceeded
                                                    : RunStatus::kFailed;
    const core::Status stored = run_store.Complete(
        run.get(), run_status, process_result.exit_code, diagnostics,
        std::move(artifacts), std::move(outcome));
    if (!stored.Ok()) result_status = stored;
    if (process_result.cancelled) {
      result_status = {core::ErrorCode::kCancelled, "OpenLane flow cancelled",
                       0};
    } else if (result_succeeded) {
      result_status = core::Status::Success();
    } else if (result_status.Ok()) {
      result_status = {core::ErrorCode::kIoError,
                       process_succeeded
                           ? "OpenLane physical checks or artifacts failed"
                           : "OpenLane Classic flow failed",
                       0};
    }
    Finish(std::move(result_status), std::move(process_result),
           std::move(diagnostics), std::move(metrics));
  }

  runtime::ExecutionProvider* provider = nullptr;
  mutable std::mutex mutex;
  runtime::TaskScheduler scheduler;
  runtime::TaskScheduler cleanup_scheduler{1};
  runtime::ResourceCoordinator resource_coordinator;
  RunStore run_store;
  adapters::OpenLane2Adapter adapter;
  ManagedFlowRunRequest request;
  ManagedFlowRunEventSink sink;
  ManagedFlowRunState state = ManagedFlowRunState::kIdle;
  std::shared_ptr<runtime::ExecutionHandle> handle;
  std::vector<std::shared_ptr<runtime::ExecutionHandle>>
      deferred_handle_cleanup;
  std::unique_ptr<runtime::CpuTokenLease> cpu_lease;
  std::shared_ptr<RunRecord> run;
  adapters::OpenLanePlan plan;
  runtime::ProcessResult process_result;
  std::filesystem::path staging_directory;
  std::string probe_output;
  std::string tool_version;
  std::string fingerprint;
  std::string lineage_id;
  std::string last_step;
  std::vector<std::string> step_ids;
  std::unique_ptr<OrfsManagedFlowRunService> orfs_service;
  bool delegated_orfs = false;
  bool active = false;
  bool cancellation_requested = false;
  bool terminal_delivered = false;
  bool shutdown = false;
  std::stop_source operation_stop_source;
};

ManagedFlowRunService::ManagedFlowRunService(
    runtime::ExecutionProvider* provider)
    : implementation_(std::make_shared<Implementation>(provider)) {}

ManagedFlowRunService::~ManagedFlowRunService() { Shutdown(); }

core::Status ManagedFlowRunService::Start(ManagedFlowRunRequest request,
                                          ManagedFlowRunEventSink sink) {
  const auto implementation = implementation_;
  if (implementation == nullptr || implementation->provider == nullptr ||
      !sink || request.library_directory.empty() ||
      request.cell_directory.empty() || request.generation == 0) {
    return {core::ErrorCode::kInvalidArgument,
            "Managed flow run identity is incomplete", 0};
  }
  core::Status validation = core::ValidateProject(request.project);
  if (!validation.Ok()) return validation;
  const std::string backend_id =
      request.project.physical_implementation.backend_id;
  if (!adapters::CreateManagedFlowAdapter(backend_id)) {
    return {core::ErrorCode::kInvalidArgument,
            "The physical implementation backend is not registered", 0};
  }
  if (backend_id == "orfs") {
    {
      std::scoped_lock lock(implementation->mutex);
      if (implementation->shutdown) {
        return {core::ErrorCode::kCancelled,
                "Managed flow service is shutting down", 0};
      }
      if (implementation->active ||
          (implementation->orfs_service != nullptr &&
           implementation->orfs_service->IsActive())) {
        return {core::ErrorCode::kConflict, "A managed flow is already active",
                0};
      }
      implementation->delegated_orfs = true;
    }
    const core::Status started = implementation->orfs_service->Start(
        std::move(request), std::move(sink));
    if (!started.Ok()) {
      std::scoped_lock lock(implementation->mutex);
      implementation->delegated_orfs = false;
    }
    return started;
  }
  if (implementation->orfs_service != nullptr &&
      implementation->orfs_service->IsActive()) {
    return {core::ErrorCode::kConflict, "A managed flow is already active", 0};
  }
  {
    std::scoped_lock lock(implementation->mutex);
    if (implementation->shutdown) {
      return {core::ErrorCode::kCancelled,
              "Managed flow service is shutting down", 0};
    }
    if (implementation->active) {
      return {core::ErrorCode::kConflict, "A managed flow is already active",
              0};
    }
    implementation->request = std::move(request);
    implementation->sink = std::move(sink);
    implementation->state = ManagedFlowRunState::kProbing;
    implementation->handle.reset();
    implementation->cpu_lease.reset();
    implementation->run.reset();
    implementation->plan = {};
    implementation->staging_directory.clear();
    implementation->probe_output.clear();
    implementation->tool_version.clear();
    implementation->fingerprint.clear();
    implementation->lineage_id.clear();
    implementation->last_step.clear();
    implementation->step_ids.clear();
    implementation->active = true;
    implementation->cancellation_requested = false;
    implementation->terminal_delivered = false;
    implementation->operation_stop_source = std::stop_source{};
    implementation->delegated_orfs = false;
  }
  implementation->EmitState(ManagedFlowRunState::kProbing);
  implementation->StartProbe();
  return core::Status::Success();
}

void ManagedFlowRunService::Cancel() noexcept {
  const auto implementation = implementation_;
  if (!implementation) return;
  bool delegated_orfs = false;
  {
    std::scoped_lock lock(implementation->mutex);
    delegated_orfs = implementation->delegated_orfs;
  }
  if (delegated_orfs) {
    if (implementation->orfs_service) implementation->orfs_service->Cancel();
    return;
  }
  std::shared_ptr<runtime::ExecutionHandle> handle;
  {
    std::scoped_lock lock(implementation->mutex);
    if (!implementation->active || implementation->terminal_delivered) return;
    implementation->cancellation_requested = true;
    implementation->state = ManagedFlowRunState::kCancelling;
    implementation->operation_stop_source.request_stop();
    // Keep the handle alive after releasing the service mutex.  Completion
    // may concurrently replace and retire the active handle; retaining a
    // shared ownership here prevents Cancel from dereferencing a destroyed
    // process object in that race.
    handle = implementation->handle;
  }
  if (handle) handle->Cancel();
  implementation->EmitState(ManagedFlowRunState::kCancelling);
}

void ManagedFlowRunService::Shutdown() noexcept {
  const auto implementation = implementation_;
  if (!implementation) return;
  std::shared_ptr<runtime::ExecutionHandle> active_handle;
  std::vector<std::shared_ptr<runtime::ExecutionHandle>> deferred_handles;
  {
    std::scoped_lock lock(implementation->mutex);
    if (implementation->shutdown) return;
    implementation->shutdown = true;
    implementation->active = false;
    implementation->operation_stop_source.request_stop();
    active_handle = std::move(implementation->handle);
    deferred_handles.swap(implementation->deferred_handle_cleanup);
  }
  if (implementation->orfs_service) implementation->orfs_service->Shutdown();
  if (active_handle) active_handle->Cancel();
  implementation->scheduler.RequestStop();
  active_handle.reset();
  implementation->cleanup_scheduler.RequestStop();
  deferred_handles.clear();
}

ManagedFlowRunState ManagedFlowRunService::State() const {
  const auto implementation = implementation_;
  if (!implementation) return ManagedFlowRunState::kIdle;
  bool delegated_orfs = false;
  {
    std::scoped_lock lock(implementation->mutex);
    delegated_orfs = implementation->delegated_orfs;
    if (!delegated_orfs) return implementation->state;
  }
  return implementation->orfs_service ? implementation->orfs_service->State()
                                      : ManagedFlowRunState::kIdle;
}

bool ManagedFlowRunService::IsActive() const {
  const auto implementation = implementation_;
  if (!implementation) return false;
  bool delegated_orfs = false;
  {
    std::scoped_lock lock(implementation->mutex);
    delegated_orfs = implementation->delegated_orfs;
    if (!delegated_orfs) return implementation->active;
  }
  return implementation->orfs_service != nullptr &&
         implementation->orfs_service->IsActive();
}

}  // namespace designpp::application

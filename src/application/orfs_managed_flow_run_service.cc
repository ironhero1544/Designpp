// Copyright 2026 The Design++ Authors

#include "designpp/application/orfs_managed_flow_run_service.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <mutex>
#include <sstream>
#include <utility>
#include <vector>

#include "designpp/adapters/orfs_adapter.h"
#include "designpp/application/physical_implementation_service.h"
#include "designpp/application/prepared_physical_inputs.h"
#include "designpp/application/synthesis_fingerprint.h"
#include "designpp/application/toolchain_environment_service.h"
#include "designpp/runtime/path_mapper.h"
#include "designpp/runtime/resource_coordinator.h"
#include "designpp/runtime/task_scheduler.h"

namespace designpp::application {
namespace {

std::wstring Utf8ToWide(std::string_view text);

class LineageLease final {
 public:
  LineageLease() = default;
  LineageLease(const LineageLease&) = delete;
  LineageLease& operator=(const LineageLease&) = delete;

  ~LineageLease() {
    if (acquired_ && mutex_) ReleaseMutex(mutex_);
    if (mutex_) CloseHandle(mutex_);
  }

  [[nodiscard]] static core::Result<std::unique_ptr<LineageLease>> Acquire(
      std::string_view project_id, std::string_view lineage_id,
      std::stop_token stop_token) {
    std::string identity;
    identity.reserve(project_id.size() + lineage_id.size() + 1);
    identity.append(project_id);
    identity.push_back('.');
    identity.append(lineage_id);
    for (char& character : identity) {
      if (!std::isalnum(static_cast<unsigned char>(character)) &&
          character != '-' && character != '_') {
        character = '_';
      }
    }
    if (identity.size() > 180) identity.resize(180);
    const std::wstring name =
        L"Local\\DesignPlusPlus.OrfsLineage." + Utf8ToWide(identity);
    auto lease = std::make_unique<LineageLease>();
    lease->mutex_ = CreateMutexW(nullptr, FALSE, name.c_str());
    if (!lease->mutex_) {
      return core::Status{core::ErrorCode::kIoError,
                          "Cannot create the ORFS lineage lease",
                          GetLastError()};
    }
    while (!stop_token.stop_requested()) {
      const DWORD wait = WaitForSingleObject(lease->mutex_, 50);
      if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) {
        lease->acquired_ = true;
        return lease;
      }
      if (wait != WAIT_TIMEOUT) {
        return core::Status{core::ErrorCode::kIoError,
                            "Cannot acquire the ORFS lineage lease",
                            GetLastError()};
      }
    }
    return core::Status{core::ErrorCode::kCancelled,
                        "ORFS lineage lease acquisition was cancelled", 0};
  }

 private:
  HANDLE mutex_ = nullptr;
  bool acquired_ = false;
};

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

std::string EscapeJson(std::string_view value) {
  std::string result;
  for (char character : value) {
    if (character == '\\' || character == '"') result.push_back('\\');
    if (character == '\n') {
      result += "\\n";
    } else if (character != '\r') {
      result.push_back(character);
    }
  }
  return result;
}

bool IsNixLockNoticeContinuation(std::string_view line) {
  return line.empty() || line.front() == ' ' || line.front() == '\t' ||
         line.starts_with("\xE2\x80\xA2");
}

std::string FilterNixLockNotice(std::string_view chunk, bool* suppressing,
                                std::string* partial_line) {
  constexpr std::string_view kNotice =
      "warning: not writing modified lock file of flake";
  partial_line->append(chunk);
  std::string visible;
  std::size_t begin = 0;
  while (true) {
    const std::size_t newline = partial_line->find('\n', begin);
    if (newline == std::string::npos) break;
    std::string_view line(*partial_line);
    line = line.substr(begin, newline - begin);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.starts_with(kNotice)) {
      *suppressing = true;
    } else if (*suppressing && IsNixLockNoticeContinuation(line)) {
      // Nix lists every temporary flake override below the notice. These
      // entries are deterministic environment wiring, not flow diagnostics.
    } else {
      *suppressing = false;
      visible.append(partial_line->data() + begin, newline - begin + 1);
    }
    begin = newline + 1;
  }
  partial_line->erase(0, begin);
  if (partial_line->empty()) return visible;
  const std::string_view trailing(*partial_line);
  if (*suppressing && IsNixLockNoticeContinuation(trailing)) return visible;
  if (!*suppressing && kNotice.starts_with(trailing)) return visible;
  *suppressing = false;
  visible.append(*partial_line);
  partial_line->clear();
  return visible;
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
                          "ORFS run identity is invalid", 0};
    }
    id.push_back(static_cast<unsigned char>(character));
  }
  const std::filesystem::path directory =
      std::filesystem::path(temporary_root) / L"DesignPlusPlus" / L"orfs" / id;
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  return error ? core::Result<std::filesystem::path>(
                     core::Status{core::ErrorCode::kIoError,
                                  "Cannot create ORFS staging directory",
                                  static_cast<unsigned long>(error.value())})
               : core::Result<std::filesystem::path>(directory);
}

core::Status CopyFile(const std::filesystem::path& source,
                      const std::filesystem::path& destination) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(source, error) || error) {
    return {core::ErrorCode::kNotFound, "ORFS input file is missing", 0};
  }
  std::filesystem::create_directories(destination.parent_path(), error);
  if (!error) {
    std::filesystem::copy_file(
        source, destination, std::filesystem::copy_options::overwrite_existing,
        error);
  }
  return error ? core::Status{core::ErrorCode::kIoError,
                              "Cannot stage an ORFS input",
                              static_cast<unsigned long>(error.value())}
               : core::Status::Success();
}

core::Result<std::string> ReadFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return core::Status{core::ErrorCode::kNotFound,
                        "ORFS output file is missing", 0};
  }
  std::ostringstream output;
  output << input.rdbuf();
  return output.str();
}

bool DirectoryContainsText(const std::filesystem::path& root,
                           std::string_view expected) {
  constexpr std::size_t kMaximumFiles = 512;
  constexpr std::uintmax_t kMaximumBytes = 64ULL * 1024ULL * 1024ULL;
  constexpr std::size_t kChunkSize = 64ULL * 1024ULL;
  if (expected.empty()) return false;

  std::size_t visited_files = 0;
  std::uintmax_t visited_bytes = 0;
  std::error_code error;
  for (std::filesystem::recursive_directory_iterator iterator(
           root, std::filesystem::directory_options::skip_permission_denied,
           error),
       end;
       !error && iterator != end && visited_files < kMaximumFiles;
       iterator.increment(error)) {
    if (!iterator->is_regular_file(error) || error) continue;
    const std::uintmax_t file_size = iterator->file_size(error);
    if (error || file_size == 0 || visited_bytes + file_size > kMaximumBytes) {
      continue;
    }
    ++visited_files;
    visited_bytes += file_size;

    std::ifstream input(iterator->path(), std::ios::binary);
    if (!input) continue;
    std::string buffer(kChunkSize, '\0');
    std::string overlap;
    while (input) {
      input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
      const std::streamsize count = input.gcount();
      if (count <= 0) break;
      std::string searchable = overlap;
      searchable.append(buffer.data(), static_cast<std::size_t>(count));
      if (searchable.find(expected) != std::string::npos) return true;
      const std::size_t retained =
          std::min(expected.size() - 1, searchable.size());
      overlap.assign(searchable.end() - retained, searchable.end());
    }
  }
  return false;
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

void AppendOptionalNumber(std::ostream* output, std::string_view name,
                          const std::optional<double>& value) {
  *output << ",\"" << name << "\":";
  if (value) {
    *output << *value;
  } else {
    *output << "null";
  }
}

bool MetricsPasses(const adapters::ManagedFlowMetrics& metrics) {
  const auto nonnegative = [](const std::optional<double>& value) {
    return !value || *value >= 0.0;
  };
  const auto nonpositive = [](const std::optional<double>& value) {
    return !value || *value <= 0.0;
  };
  return nonnegative(metrics.setup_wns) && nonnegative(metrics.setup_tns) &&
         nonnegative(metrics.hold_wns) && nonnegative(metrics.hold_tns) &&
         nonpositive(metrics.antenna_violations) &&
         nonpositive(metrics.slew_violations) &&
         nonpositive(metrics.capacitance_violations) &&
         nonpositive(metrics.fanout_violations) &&
         nonpositive(metrics.routing_violations) &&
         nonpositive(metrics.drc_violations) &&
         nonpositive(metrics.xor_violations) && nonpositive(metrics.lvs_errors);
}

void NormalizeTimingMetricsToNanoseconds(
    std::string_view source_unit, adapters::ManagedFlowMetrics* metrics) {
  double scale = 1.0;
  if (source_unit == "ps") {
    scale = 1.0e-3;
  } else if (source_unit == "fs") {
    scale = 1.0e-6;
  } else if (source_unit == "us") {
    scale = 1.0e3;
  } else if (source_unit == "ms") {
    scale = 1.0e6;
  } else if (source_unit == "s") {
    scale = 1.0e9;
  }
  const auto apply = [scale](std::optional<double>* value) {
    if (*value) **value *= scale;
  };
  apply(&metrics->setup_wns);
  apply(&metrics->setup_tns);
  apply(&metrics->hold_wns);
  apply(&metrics->hold_tns);
  apply(&metrics->worst_setup_skew);
  apply(&metrics->worst_hold_skew);
}

std::string ProbeVersion(std::string_view output) {
  const std::size_t marker = output.find("DESIGNPP_ORFS_READY");
  if (marker == std::string_view::npos) return "ORFS";
  std::istringstream lines{std::string(output.substr(marker))};
  std::string line;
  std::getline(lines, line);
  std::getline(lines, line);
  if (line.size() > 40) line.resize(40);
  return line.empty() ? "ORFS" : "ORFS " + line;
}

std::string ProbeToolMode(std::string_view output) {
  constexpr std::string_view kMarker = "DESIGNPP_ORFS_TOOL_MODE=";
  const std::size_t begin = output.find(kMarker);
  if (begin == std::string_view::npos) return {};
  const std::size_t value_begin = begin + kMarker.size();
  const std::size_t end = output.find_first_of("\r\n", value_begin);
  return std::string(output.substr(value_begin, end - value_begin));
}

std::string ProbeFailureMessage(const runtime::ProcessResult& result) {
  std::string detail = result.output;
  if (detail.empty()) detail = WideToUtf8(result.error_message);
  while (!detail.empty() &&
         std::isspace(static_cast<unsigned char>(detail.back()))) {
    detail.pop_back();
  }
  constexpr std::size_t kMaximumDetail = 512;
  if (detail.size() > kMaximumDetail) {
    detail = "..." + detail.substr(detail.size() - kMaximumDetail + 3);
  }
  std::string message = "ORFS capability probe failed";
  if (result.cancelled) {
    message += " (cancelled)";
  } else if (!result.started) {
    message += " (process did not start)";
  } else {
    message += " (exit code " + std::to_string(result.exit_code) + ")";
  }
  if (!detail.empty()) message += ": " + detail;
  return message;
}

void CopyOutputs(const std::filesystem::path& source_root,
                 const std::filesystem::path& destination_root,
                 const std::shared_ptr<RunRecord>& run,
                 std::string_view fingerprint, bool partial,
                 std::vector<RunArtifact>* artifacts) {
  constexpr std::size_t kMaximumFiles = 512;
  constexpr std::uintmax_t kMaximumBytes = 512ULL * 1024ULL * 1024ULL;
  std::size_t copied_files = 0;
  std::uintmax_t copied_bytes = 0;
  std::error_code error;
  for (std::filesystem::recursive_directory_iterator iterator(
           source_root,
           std::filesystem::directory_options::skip_permission_denied, error),
       end;
       !error && iterator != end && copied_files < kMaximumFiles;
       iterator.increment(error)) {
    if (!iterator->is_regular_file(error) || error) continue;
    const std::filesystem::path relative =
        std::filesystem::relative(iterator->path(), source_root, error);
    if (error || relative.empty()) continue;
    const std::string extension = [&relative] {
      std::string value = relative.extension().string();
      std::transform(value.begin(), value.end(), value.begin(), [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      });
      return value;
    }();
    if (extension != ".log" && extension != ".rpt" && extension != ".json" &&
        extension != ".csv" && extension != ".odb" && extension != ".def" &&
        extension != ".lef" && extension != ".gds" && extension != ".v" &&
        extension != ".sdf" && extension != ".spef") {
      continue;
    }
    const std::uintmax_t size = iterator->file_size(error);
    if (error || size == 0 || copied_bytes + size > kMaximumBytes) continue;
    const std::filesystem::path destination = destination_root / relative;
    if (!CopyFile(iterator->path(), destination).Ok()) continue;
    std::string format = extension.empty() ? "file" : extension.substr(1);
    AddArtifact(run, "orfs-output", std::move(format), destination,
                std::string(fingerprint), partial, artifacts);
    copied_bytes += size;
    ++copied_files;
  }
}

}  // namespace

struct OrfsManagedFlowRunService::Implementation final
    : public std::enable_shared_from_this<Implementation> {
  explicit Implementation(runtime::ExecutionProvider* execution_provider)
      : provider(execution_provider), scheduler(1), cleanup_scheduler(1) {}

  void Emit(ManagedFlowRunEvent event) {
    ManagedFlowRunEventSink current;
    {
      std::scoped_lock lock(mutex);
      if (shutdown || event.generation != request.generation) return;
      current = sink;
    }
    if (current) current(std::move(event));
  }

  void EmitState(ManagedFlowRunState next) {
    {
      std::scoped_lock lock(mutex);
      if (shutdown || terminal_delivered) return;
      state = next;
    }
    ManagedFlowRunEvent event;
    event.kind = ManagedFlowRunEventKind::kStateChanged;
    event.state = next;
    event.generation = request.generation;
    Emit(std::move(event));
  }

  void EmitOutput(std::string output, ManagedFlowRunState output_state) {
    std::shared_ptr<RunRecord> current_run;
    std::string visible_output;
    {
      std::scoped_lock lock(mutex);
      current_run = run;
      visible_output = FilterNixLockNotice(output, &suppressing_nix_lock_notice,
                                           &nix_notice_partial_line);
    }
    if (current_run) {
      const core::Status ignored = run_store.AppendLog(*current_run, output);
      (void)ignored;
    }
    if (!visible_output.empty()) {
      ManagedFlowRunEvent event;
      event.kind = ManagedFlowRunEventKind::kOutput;
      event.state = output_state;
      event.generation = request.generation;
      event.output = std::move(visible_output);
      Emit(std::move(event));
    }
    std::istringstream lines{output};
    std::string line;
    while (std::getline(lines, line)) {
      const auto progress = adapter.ParseProgress(line);
      if (!progress) continue;
      {
        std::scoped_lock lock(mutex);
        last_step = progress->step_id;
      }
      ManagedFlowRunEvent progress_event;
      progress_event.kind = ManagedFlowRunEventKind::kProgress;
      progress_event.state = output_state;
      progress_event.generation = request.generation;
      progress_event.progress = *progress;
      Emit(std::move(progress_event));
    }
  }

  bool AcceptCallback(std::uint64_t callback_generation,
                      ManagedFlowRunState expected) const {
    std::scoped_lock lock(mutex);
    return !shutdown && active && !terminal_delivered &&
           request.generation == callback_generation &&
           (state == expected || state == ManagedFlowRunState::kCancelling);
  }

  void StoreHandle(std::unique_ptr<runtime::ExecutionHandle> next) {
    bool cancel = false;
    std::shared_ptr<runtime::ExecutionHandle> old;
    std::shared_ptr<runtime::ExecutionHandle> current;
    {
      std::scoped_lock lock(mutex);
      if (shutdown || terminal_delivered) {
        old = std::shared_ptr<runtime::ExecutionHandle>(std::move(next));
      } else {
        old = std::move(handle);
        handle = std::shared_ptr<runtime::ExecutionHandle>(std::move(next));
      }
      cancel = cancellation_requested;
      current = handle;
    }
    QueueHandleCleanup(std::move(old));
    if (cancel) {
      if (current) current->Cancel();
    }
  }

  void QueueHandleCleanup(
      std::shared_ptr<runtime::ExecutionHandle> completed_handle) {
    if (!completed_handle) return;
    auto holder = std::make_shared<std::shared_ptr<runtime::ExecutionHandle>>(
        std::move(completed_handle));
    const bool queued =
        cleanup_scheduler.Submit([holder](std::stop_token) mutable {
          // Destruction may join a process callback thread, so keep it off
          // the callback that just delivered completion.
          auto handle = std::move(*holder);
          holder.reset();
          static_cast<void>(handle->IsRunning());
        });
    if (!queued) {
      std::scoped_lock lock(mutex);
      deferred_handle_cleanup.push_back(std::move(*holder));
    }
  }

  void Finish(core::Status status, runtime::ProcessResult result,
              std::vector<core::Diagnostic> diagnostics,
              adapters::ManagedFlowMetrics metrics = {},
              std::string failure_stage = {}, std::string failure_code = {}) {
    ManagedFlowRunEvent event;
    ManagedFlowRunEventSink current;
    std::shared_ptr<runtime::ExecutionHandle> old;
    {
      std::scoped_lock lock(mutex);
      if (shutdown || terminal_delivered) return;
      terminal_delivered = true;
      active = false;
      cpu_lease.reset();
      lineage_lease.reset();
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
      event.metrics = std::move(metrics);
      event.failure_stage = std::move(failure_stage);
      event.failure_code = std::move(failure_code);
      event.configuration_fingerprint = fingerprint;
      event.lineage_id = lineage_id;
      current = sink;
      old = std::move(handle);
    }
    QueueHandleCleanup(std::move(old));
    if (current) current(std::move(event));
  }

  void StartProbe() {
    const auto self = shared_from_this();
    const std::uint64_t callback_generation = request.generation;
    const runtime::WslCommand command =
        adapter.BuildProbeCommand(request.profile);
    runtime::ExecutionStartResult started = provider->Start(
        command,
        [self](std::string output) {
          {
            std::scoped_lock lock(self->mutex);
            if (self->shutdown || self->terminal_delivered) return;
            self->probe_output += output;
          }
          self->EmitOutput(std::move(output), ManagedFlowRunState::kProbing);
        },
        [self, callback_generation](runtime::ProcessResult result) {
          if (!self->AcceptCallback(callback_generation,
                                    ManagedFlowRunState::kProbing)) {
            return;
          }
          const auto compatibility =
              ProbeToolchainCompatibility("orfs", result);
          if (!compatibility.Ok()) {
            self->Finish(
                result.cancelled ? core::Status{core::ErrorCode::kCancelled,
                                                "ORFS probe cancelled", 0}
                                 : compatibility.GetStatus(),
                std::move(result), {}, {}, "capability_probe", "ORFS-PROBE");
            return;
          }
          if (!self->request.environment_fingerprint.empty() &&
              self->request.environment_fingerprint !=
                  compatibility.Value().environment_fingerprint) {
            self->Finish(
                {core::ErrorCode::kExternalModification,
                 "Selected ORFS environment changed; recheck it in Tool Check",
                 0},
                std::move(result), {}, {}, "capability_probe", "ORFS-PROBE");
            return;
          }
          self->request.environment_fingerprint =
              compatibility.Value().environment_fingerprint;
          if (self->request.environment_id.empty()) {
            self->request.environment_id = compatibility.Value().bundle_id;
          }
          {
            std::scoped_lock lock(self->mutex);
            self->probe_output = result.output;
          }
          self->tool_version = ProbeVersion(result.output);
          self->tool_mode = ProbeToolMode(result.output);
          self->EmitState(ManagedFlowRunState::kPreparing);
          const bool queued = self->scheduler.Submit(
              [self, callback_generation](std::stop_token token) {
                self->Prepare(callback_generation, token);
              });
          if (!queued) {
            self->Finish({core::ErrorCode::kConflict,
                          "ORFS preparation queue is unavailable", 0},
                         {}, {}, {}, "input_preparation", "ORFS-PREPARE");
          }
        });
    if (!started.Ok()) {
      Finish(started.status, {}, {}, {}, "capability_probe", "ORFS-PROBE");
      return;
    }
    StoreHandle(std::move(started.handle));
  }

  void Prepare(std::uint64_t callback_generation, std::stop_token token) {
    if (token.stop_requested() || operation_stop_source.stop_requested()) {
      Finish({core::ErrorCode::kCancelled, "ORFS flow cancelled", 0}, {}, {},
             {}, "cancelled", "ORFS-CANCELLED");
      return;
    }
    auto begun =
        run_store.Begin(request.cell_directory, request.project,
                        "physical_implementation", "orfs", tool_version);
    if (!begun.Ok()) {
      Finish(begun.GetStatus(), {}, {}, {}, "input_preparation",
             "ORFS-PREPARE");
      return;
    }
    run = std::make_shared<RunRecord>(std::move(begun).Value());
    run->environment_id = request.environment_id;
    run->environment_fingerprint = request.environment_fingerprint;
    std::string captured_probe_output;
    {
      std::scoped_lock lock(mutex);
      captured_probe_output = probe_output;
    }
    if (!captured_probe_output.empty()) {
      const core::Status ignored =
          run_store.AppendLog(*run, captured_probe_output);
      (void)ignored;
    }
    auto staging = CreateStagingDirectory(run->id);
    if (!staging.Ok()) {
      CompletePreparationFailure(staging.GetStatus());
      return;
    }
    staging_directory = std::move(staging).Value();
    std::error_code error;
    for (const char* folder : {"src", "include", "constraints"}) {
      std::filesystem::create_directories(staging_directory / folder, error);
      if (error) {
        CompletePreparationFailure({core::ErrorCode::kIoError,
                                    "Cannot prepare ORFS staging folders",
                                    static_cast<unsigned long>(error.value())});
        return;
      }
    }

    adapters::OrfsRequest adapter_request;
    adapter_request.profile = request.profile;
    adapter_request.configuration = request.project.physical_implementation;
    adapter_request.top_module = request.project.top_module;
    adapter_request.defines = request.project.defines;
    adapter_request.parameters = request.project.parameters;
    adapter_request.cpu_threads = request.project.cpu_budget;
    adapter_request.target_stage = request.target_stage;
    adapter_request.full_flow = request.full_flow;
    adapter_request.tool_mode = tool_mode;
    auto prepared_result =
        request.prepared_inputs &&
                request.prepared_inputs->tool_version == tool_version
            ? core::Result<std::shared_ptr<const PreparedPhysicalInputs>>(
                  request.prepared_inputs)
            : PrepareOrfsPhysicalInputs(request, tool_version);
    if (!prepared_result.Ok()) {
      CompletePreparationFailure(prepared_result.GetStatus());
      return;
    }
    request.prepared_inputs = std::move(prepared_result).Value();
    const PreparedPhysicalInputs& prepared = *request.prepared_inputs;
    configuration_contract_hex = prepared.configuration_contract_hex;
    for (const PreparedPhysicalFile& source : prepared.rtl_sources) {
      const std::filesystem::path target =
          staging_directory / "src" / source.staged_path;
      std::filesystem::create_directories(target.parent_path(), error);
      std::ofstream output(target, std::ios::binary | std::ios::trunc);
      output << source.contents;
      if (!output) {
        CompletePreparationFailure({core::ErrorCode::kIoError,
                                    "Cannot stage an ORFS RTL snapshot", 0});
        return;
      }
      adapter_request.sources.push_back(
          {source.relative_path, source.staged_path});
    }
    for (std::size_t index = 0; index < prepared.include_directories.size();
         ++index) {
      const std::filesystem::path target =
          staging_directory / "include" / ("include_" + std::to_string(index));
      for (const PreparedPhysicalFile& file :
           prepared.include_directories[index].files) {
        const std::filesystem::path destination = target / file.staged_path;
        std::filesystem::create_directories(destination.parent_path(), error);
        std::ofstream output(destination, std::ios::binary | std::ios::trunc);
        output << file.contents;
        if (!output) {
          CompletePreparationFailure({core::ErrorCode::kIoError,
                                      "Cannot stage an ORFS include snapshot",
                                      0});
          return;
        }
      }
      adapter_request.include_directories.push_back(
          prepared.include_directories[index].configured_path);
    }
    const std::filesystem::path sdc_target =
        staging_directory / "constraints" / "constraints.sdc";
    std::ofstream staged_sdc(sdc_target, std::ios::binary | std::ios::trunc);
    staged_sdc << prepared.sdc_contents;
    if (!staged_sdc) {
      CompletePreparationFailure(
          {core::ErrorCode::kIoError, "Cannot stage the prepared ORFS SDC", 0});
      return;
    }
    sdc_auto_generated = prepared.sdc_auto_generated;
    sdc_provenance = prepared.sdc_provenance;
    unconstrained_warning = prepared.unconstrained_warning;
    adapter_request.sdc_path = sdc_target.filename();
    adapter_request.sdc_time_unit = prepared.sdc_time_unit;
    adapter_request.effective_clock_period_ns =
        prepared.effective_clock_period_text.empty()
            ? std::optional<std::string>{}
            : std::optional<std::string>{prepared.effective_clock_period_text};

    const std::filesystem::path fingerprint_path =
        staging_directory / "fingerprint.txt";
    {
      std::ofstream output(fingerprint_path,
                           std::ios::binary | std::ios::trunc);
      output << prepared.fingerprint_manifest;
    }
    fingerprint = prepared.fingerprint;
    if (request.resume &&
        request.resume->configuration_fingerprint != fingerprint) {
      if (request.rebuild_from_stage) {
        CompletePreparationFailure(
            {core::ErrorCode::kConflict,
             "ORFS rebuild fingerprint no longer matches project inputs", 0});
        return;
      }
      EmitOutput(
          "Existing ORFS checkpoint is stale; starting a new "
          "lineage.\n",
          ManagedFlowRunState::kPreparing);
      request.resume.reset();
    }
    lineage_id = request.resume && !request.rebuild_from_stage
                     ? request.resume->lineage_id
                     : run->id;
    if (request.rebuild_from_stage && request.resume) {
      adapter_request.parent_backend_workspace = ".designpp/runs/orfs/" +
                                                 request.project.id + "/" +
                                                 request.resume->lineage_id;
    }
    adapter_request.resume_step =
        request.resume ? request.resume->resume_step : std::string{};
    adapter_request.checkpoint_hash =
        request.resume ? request.resume->checkpoint_hash : std::string{};
    adapter_request.backend_workspace =
        ".designpp/runs/orfs/" + request.project.id + "/" + lineage_id;
    auto exclusive_lineage = LineageLease::Acquire(
        request.project.id, lineage_id, operation_stop_source.get_token());
    if (!exclusive_lineage.Ok()) {
      CompletePreparationFailure(exclusive_lineage.GetStatus());
      return;
    }
    lineage_lease = std::move(exclusive_lineage).Value();
    auto mapped = runtime::PathMapper().WindowsToWsl(staging_directory);
    if (!mapped.Ok()) {
      CompletePreparationFailure(mapped.GetStatus());
      return;
    }
    adapter_request.staging_workspace = WideToUtf8(mapped.Value());
    auto lease = resource_coordinator.AcquireCpu(
        request.project.cpu_budget, operation_stop_source.get_token());
    if (!lease.Ok()) {
      CompletePreparationFailure(lease.GetStatus());
      return;
    }
    cpu_lease =
        std::make_unique<runtime::CpuTokenLease>(std::move(lease).Value());
    adapter_request.cpu_threads = cpu_lease->token_count();
    auto built = adapter.BuildPlan(adapter_request);
    if (!built.Ok()) {
      CompletePreparationFailure(built.GetStatus());
      return;
    }
    plan = std::move(built).Value();
    {
      std::ofstream config(staging_directory / "config.mk",
                           std::ios::binary | std::ios::trunc);
      config << plan.config_makefile;
      if (!config) {
        CompletePreparationFailure(
            {core::ErrorCode::kIoError, "Cannot write ORFS config.mk", 0});
        return;
      }
    }
    {
      std::ofstream wrapper(staging_directory / "designpp-openroad-wrapper.sh",
                            std::ios::binary | std::ios::trunc);
      wrapper << plan.openroad_wrapper;
      if (!wrapper) {
        CompletePreparationFailure(
            {core::ErrorCode::kIoError,
             "Cannot write the ORFS OpenROAD unit wrapper", 0});
        return;
      }
    }
    {
      std::ofstream init(staging_directory / "designpp-openroad-init.tcl",
                         std::ios::binary | std::ios::trunc);
      init << plan.openroad_init;
      if (!init) {
        CompletePreparationFailure(
            {core::ErrorCode::kIoError,
             "Cannot write the ORFS OpenROAD unit initialization", 0});
        return;
      }
    }
    if (request.resume) {
      StartFlow(callback_generation);
    } else {
      StartValidation(callback_generation);
    }
  }

  void CompletePreparationFailure(core::Status status) {
    std::vector<core::Diagnostic> diagnostics;
    core::Diagnostic diagnostic;
    diagnostic.severity = core::DiagnosticSeverity::kError;
    diagnostic.code = "ORFS-PREPARE";
    diagnostic.message = status.message;
    diagnostics.push_back(std::move(diagnostic));
    if (run) {
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
        preserve(staging_directory / "config.mk", "config.mk");
        preserve(staging_directory / "constraints" / "constraints.sdc",
                 "constraints.sdc");
        preserve(staging_directory / "fingerprint.txt", "fingerprint.txt");
        preserve(staging_directory / "designpp-openroad-wrapper.sh",
                 "designpp-openroad-wrapper.sh");
        preserve(staging_directory / "designpp-openroad-init.tcl",
                 "designpp-openroad-init.tcl");
        CopyOutputs(staging_directory / "backend", reports_directory / "orfs",
                    run, fingerprint, true, &artifacts);
      }
      const std::filesystem::path summary_path =
          reports_directory / "orfs-summary.json";
      {
        std::ofstream summary(summary_path, std::ios::binary | std::ios::trunc);
        summary << "{\"schema_version\":2,\"tool\":\"orfs\","
                   "\"project_id\":\""
                << EscapeJson(request.project.id) << "\",\"library_id\":\""
                << EscapeJson(request.project.library_id) << "\",\"cell_id\":\""
                << EscapeJson(request.project.cell_id)
                << "\",\"project_revision\":" << request.project.revision
                << ",\"backend_id\":\"orfs\",\"failure_stage\":"
                   "\"input_preparation\",\"failure_code\":\"ORFS-PREPARE\","
                   "\"failure_message\":\""
                << EscapeJson(status.message)
                << "\",\"configuration_fingerprint\":\""
                << EscapeJson(fingerprint)
                << "\",\"configuration_contract_hex\":\""
                << configuration_contract_hex
                << "\",\"process_succeeded\":false,"
                   "\"result_succeeded\":false}";
      }
      AddArtifact(run, "summary", "json", summary_path, fingerprint, false,
                  &artifacts);
      RunOutcome outcome;
      outcome.summary_relative_path = "reports/orfs-summary.json";
      const core::Status ignored =
          run_store.Complete(run.get(), RunStatus::kFailed, 0, diagnostics,
                             std::move(artifacts), std::move(outcome));
      (void)ignored;
    }
    Finish(std::move(status), {}, std::move(diagnostics), {},
           "input_preparation", "ORFS-PREPARE");
  }

  void StartValidation(std::uint64_t callback_generation) {
    EmitState(ManagedFlowRunState::kValidating);
    const auto self = shared_from_this();
    runtime::ExecutionStartResult started = provider->Start(
        plan.validate,
        [self](std::string output) {
          self->EmitOutput(std::move(output), ManagedFlowRunState::kValidating);
        },
        [self, callback_generation](runtime::ProcessResult result) {
          if (!self->AcceptCallback(callback_generation,
                                    ManagedFlowRunState::kValidating)) {
            return;
          }
          if (!result.started || result.cancelled || result.exit_code != 0) {
            self->Collect(std::move(result));
            return;
          }
          self->StartFlow(callback_generation);
        });
    if (!started.Ok()) {
      CompletePreparationFailure(started.status);
      return;
    }
    StoreHandle(std::move(started.handle));
  }

  void StartFlow(std::uint64_t callback_generation) {
    EmitState(ManagedFlowRunState::kRunning);
    const auto self = shared_from_this();
    runtime::ExecutionStartResult started = provider->Start(
        plan.execute,
        [self](std::string output) {
          self->EmitOutput(std::move(output), ManagedFlowRunState::kRunning);
        },
        [self, callback_generation](runtime::ProcessResult result) {
          if (!self->AcceptCallback(callback_generation,
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
    std::error_code error;
    const std::filesystem::path collected = staging_directory / "backend";
    std::filesystem::create_directories(collected, error);
    auto mapped = runtime::PathMapper().WindowsToWsl(collected);
    if (!mapped.Ok()) {
      Finalize(mapped.GetStatus());
      return;
    }
    runtime::WslCommand copy;
    copy.program = L"/bin/bash";
    copy.arguments = {L"-lc",
                      L"workspace=\"$HOME/$1\"; destination=\"$2\"; "
                      L"if [ -d \"$workspace\" ]; then cp -R \"$workspace\"/. "
                      L"\"$destination\"/; fi",
                      L"designpp-orfs-collect",
                      Utf8ToWide(adapter_request_workspace()), mapped.Value()};
    if (!request.profile.wsl_distribution.empty())
      copy.distribution = Utf8ToWide(request.profile.wsl_distribution);
    const auto self = shared_from_this();
    const std::uint64_t callback_generation = request.generation;
    runtime::ExecutionStartResult started = provider->Start(
        copy,
        [self](std::string output) {
          self->EmitOutput(std::move(output), ManagedFlowRunState::kCollecting);
        },
        [self, callback_generation](runtime::ProcessResult result) {
          if (!self->AcceptCallback(callback_generation,
                                    ManagedFlowRunState::kCollecting)) {
            return;
          }
          self->Finalize(result.started && result.exit_code == 0
                             ? core::Status::Success()
                             : core::Status{core::ErrorCode::kIoError,
                                            "Cannot collect ORFS outputs", 0});
        });
    if (!started.Ok()) {
      Finalize(started.status);
      return;
    }
    StoreHandle(std::move(started.handle));
  }

  std::string adapter_request_workspace() const {
    return ".designpp/runs/orfs/" + request.project.id + "/" + lineage_id;
  }

  void Finalize(core::Status collection_status) {
    const auto self = shared_from_this();
    const std::uint64_t callback_generation = request.generation;
    const bool queued = scheduler.Submit([self, callback_generation,
                                          collection_status =
                                              std::move(collection_status)](
                                             std::stop_token) mutable {
      self->FinalizeOnWorker(callback_generation, std::move(collection_status));
    });
    if (!queued) {
      CompletePreparationFailure({core::ErrorCode::kConflict,
                                  "ORFS finalization queue unavailable", 0});
    }
  }

  void FinalizeOnWorker(std::uint64_t callback_generation,
                        core::Status collection_status) {
    if (!AcceptCallback(callback_generation, ManagedFlowRunState::kCollecting))
      return;
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
    const bool final_target =
        request.target_stage == core::StageId::kFinalOutputs;
    const auto copy_named = [this, &artifacts](
                                const std::filesystem::path& source,
                                std::string_view name, bool partial) {
      if (source.empty()) return;
      const std::filesystem::path destination =
          run->directory / "artifacts" / std::filesystem::path(name);
      if (!CopyFile(source, destination).Ok()) return;
      AddArtifact(run, std::string(name), destination.extension().string(),
                  destination, fingerprint, partial, &artifacts);
    };
    adapters::ManagedFlowArtifactSet available =
        adapter.DiscoverAvailableArtifacts(staging_directory / "backend");
    // Discovery is intentionally recursive because ORFS places outputs below
    // platform/design/variant directories.  For an intermediate target,
    // however, accepting the newest arbitrary ODB could accidentally treat a
    // downstream checkpoint as the requested prerequisite.  Pin validation to
    // the exact checkpoint contract for that target.
    const auto exact_checkpoint = [this](core::StageId stage) {
      const std::string expected =
          adapter.CheckpointForStage(stage).filename().string();
      std::error_code checkpoint_error;
      for (std::filesystem::recursive_directory_iterator iterator(
               staging_directory / "backend",
               std::filesystem::directory_options::skip_permission_denied,
               checkpoint_error),
           end;
           !checkpoint_error && iterator != end;
           iterator.increment(checkpoint_error)) {
        if (!iterator->is_regular_file(checkpoint_error) || checkpoint_error)
          continue;
        if (iterator->path().filename().string() != expected) continue;
        if (iterator->file_size(checkpoint_error) == 0 || checkpoint_error)
          continue;
        return iterator->path();
      }
      return std::filesystem::path{};
    };
    const std::filesystem::path requested_checkpoint =
        exact_checkpoint(request.target_stage);
    if (!requested_checkpoint.empty()) {
      available.odb = requested_checkpoint;
    } else if (!final_target) {
      available.odb.clear();
    }
    const core::Status artifact_status =
        adapter.ValidateStageArtifacts(available, request.target_stage);
    const bool artifacts_complete = artifact_status.Ok();
    copy_named(staging_directory / "config.mk", "config.mk", false);
    copy_named(staging_directory / "constraints" / "constraints.sdc",
               "constraints.sdc", false);
    copy_named(staging_directory / "designpp-openroad-wrapper.sh",
               "designpp-openroad-wrapper.sh", false);
    copy_named(staging_directory / "designpp-openroad-init.tcl",
               "designpp-openroad-init.tcl", false);
    if (final_target) {
      copy_named(available.gds, "final.gds", !artifacts_complete);
      copy_named(available.def, "final.def", !artifacts_complete);
      copy_named(available.lef, "final.lef", !artifacts_complete);
      copy_named(available.odb, "final.odb", !artifacts_complete);
      // A failed finish may still leave a valid routed ODB.  Preserve it as
      // an independent checkpoint so the next attempt can resume the exact
      // backend lineage without treating the incomplete final view as valid.
      copy_named(available.odb, "checkpoint.odb", false);
      copy_named(available.gate_netlist, "netlist.v", !artifacts_complete);
      copy_named(available.power_netlist, "power.v", !artifacts_complete);
      copy_named(available.sdf, "final.sdf", !artifacts_complete);
      copy_named(available.spef, "final.spef", !artifacts_complete);
    } else {
      const std::filesystem::path checkpoint =
          run->directory / "artifacts" /
          (adapter.TargetForStage(request.target_stage) + ".odb");
      if (!available.odb.empty() && CopyFile(available.odb, checkpoint).Ok()) {
        AddArtifact(run, "checkpoint", "odb", checkpoint, fingerprint,
                    !artifacts_complete, &artifacts);
      }
    }
    CopyOutputs(staging_directory / "backend", reports_directory / "orfs", run,
                fingerprint,
                !artifacts_complete || process_result.exit_code != 0 ||
                    process_result.cancelled,
                &artifacts);
    bool metrics_succeeded = false;
    auto metrics_text = ReadFile(available.metrics_json);
    if (metrics_text.Ok()) {
      auto parsed = adapter.ParseMetrics(metrics_text.Value());
      if (parsed.Ok()) {
        metrics = std::move(parsed).Value();
        if (request.prepared_inputs) {
          NormalizeTimingMetricsToNanoseconds(
              request.prepared_inputs->sdc_time_unit, &metrics);
        }
        metrics_succeeded = true;
      } else {
        result_status = parsed.GetStatus();
      }
    } else {
      result_status = metrics_text.GetStatus();
    }
    if (!artifact_status.Ok() && process_result.exit_code == 0 &&
        !process_result.cancelled) {
      result_status = artifact_status;
    }
    const bool process_succeeded = process_result.started &&
                                   !process_result.cancelled &&
                                   process_result.exit_code == 0;
    const std::string expected_unit_marker =
        "DESIGNPP_OPENROAD_SDC_TIME_UNIT=" +
        request.prepared_inputs->sdc_time_unit;
    const bool sdc_unit_contract_observed = DirectoryContainsText(
        staging_directory / "backend" / "logs", expected_unit_marker);
    if (process_succeeded && !sdc_unit_contract_observed) {
      core::Diagnostic diagnostic;
      diagnostic.severity = core::DiagnosticSeverity::kError;
      diagnostic.code = "ORFS-SDC-UNIT";
      diagnostic.message =
          "ORFS completed without applying the configured SDC time unit; "
          "the OpenROAD wrapper or execution contract was bypassed";
      diagnostics.push_back(std::move(diagnostic));
      result_status = {core::ErrorCode::kCorruptData,
                       "ORFS did not apply the configured SDC time unit", 0};
    }
    const bool result_succeeded =
        process_succeeded && artifacts_complete && metrics_succeeded &&
        sdc_unit_contract_observed &&
        (!final_target || MetricsPasses(metrics)) && collection_status.Ok();
    std::string failure_stage;
    std::string failure_code;
    std::string failure_message;
    if (!result_succeeded) {
      if (!process_succeeded) {
        failure_stage = last_step.empty() ? "backend_execution" : last_step;
        failure_message = "ORFS process did not complete successfully";
      } else if (!collection_status.Ok()) {
        failure_stage = "artifact_collection";
        failure_message = collection_status.message;
      } else if (!sdc_unit_contract_observed) {
        failure_stage = "timing_unit_validation";
        failure_code = "ORFS-SDC-UNIT";
        failure_message = result_status.message;
      } else if (!artifacts_complete) {
        failure_stage = "artifact_validation";
        failure_message = artifact_status.message;
      } else if (!metrics_succeeded) {
        failure_stage = "metrics_parsing";
        failure_message = result_status.message;
      } else {
        failure_stage = "signoff_checks";
        failure_message = "ORFS timing or physical checks failed";
      }
      if (failure_code.empty()) failure_code = "ORFS-FLOW";
    }
    const std::filesystem::path summary_path =
        reports_directory / "orfs-summary.json";
    std::string checkpoint_hash;
    if (!available.odb.empty()) {
      auto hash = CalculateFileSha256(available.odb);
      if (hash.Ok()) checkpoint_hash = std::move(hash).Value();
    }
    {
      std::ofstream summary(summary_path, std::ios::binary | std::ios::trunc);
      summary
          << "{\"schema_version\":2,\"tool\":\"orfs\",\"tool_version\":\""
          << EscapeJson(tool_version) << "\",\"project_id\":\""
          << EscapeJson(request.project.id) << "\",\"library_id\":\""
          << EscapeJson(request.project.library_id) << "\",\"cell_id\":\""
          << EscapeJson(request.project.cell_id)
          << "\",\"project_revision\":" << request.project.revision
          << ",\"backend_id\":\"orfs\",\"target_stage\":\""
          << EscapeJson(plan.target) << "\",\"lineage_id\":\""
          << EscapeJson(lineage_id) << "\",\"last_step\":\""
          << EscapeJson(last_step) << "\",\"parent_run_id\":\""
          << EscapeJson(request.resume ? request.resume->parent_run_id : "")
          << "\",\"resume_step\":\""
          << EscapeJson(request.resume ? request.resume->resume_step : "")
          << "\",\"parent_lineage_id\":\""
          << EscapeJson(request.rebuild_from_stage && request.resume
                            ? request.resume->lineage_id
                            : "")
          << "\",\"rebuild_stage\":\""
          << EscapeJson(request.rebuild_from_stage ? plan.target : "")
          << "\",\"checkpoint_hash\":\"" << EscapeJson(checkpoint_hash)
          << "\",\"configuration_contract_hex\":\""
          << configuration_contract_hex << "\",\"configuration_fingerprint\":\""
          << EscapeJson(fingerprint) << "\",\"process_succeeded\":"
          << (process_succeeded ? "true" : "false")
          << ",\"result_succeeded\":" << (result_succeeded ? "true" : "false")
          << ",\"failure_stage\":\"" << EscapeJson(failure_stage)
          << "\",\"failure_code\":\"" << EscapeJson(failure_code)
          << "\",\"failure_message\":\"" << EscapeJson(failure_message) << "\""
          << ",\"platform\":\""
          << EscapeJson(request.project.physical_implementation.orfs.platform)
          << "\",\"flow_variant\":\""
          << EscapeJson(
                 request.project.physical_implementation.orfs.flow_variant)
          << "\",\"sdc_auto_generated\":"
          << (sdc_auto_generated ? "true" : "false") << ",\"sdc_provenance\":\""
          << EscapeJson(sdc_provenance) << "\",\"unconstrained_warning\":"
          << (unconstrained_warning ? "true" : "false")
          << ",\"sdc_unit_contract_observed\":"
          << (sdc_unit_contract_observed ? "true" : "false")
          << ",\"metrics\":{}";
      if (request.prepared_inputs) {
        summary << ",\"sdc_time_unit\":\""
                << EscapeJson(request.prepared_inputs->sdc_time_unit)
                << "\",\"effective_clock_period_ns\":";
        if (request.prepared_inputs->effective_clock_period_ns) {
          summary << *request.prepared_inputs->effective_clock_period_ns;
        } else {
          summary << "null";
        }
        summary << ",\"abc_clock_selection\":\""
                << (request.prepared_inputs->effective_clock_period_ns
                        ? "shortest_valid_clock"
                        : "no_literal_clock")
                << "\"";
        summary << ",\"clocks\":[";
        for (std::size_t index = 0;
             index < request.prepared_inputs->clocks.size(); ++index) {
          if (index != 0) summary << ',';
          const PreparedPhysicalClock& clock =
              request.prepared_inputs->clocks[index];
          summary << "{\"name\":\"" << EscapeJson(clock.name)
                  << "\",\"target_port\":\"" << EscapeJson(clock.target_port)
                  << "\",\"period_ns\":" << clock.period_ns << '}';
        }
        summary << ']';
      }
      AppendOptionalNumber(&summary, "core_area", metrics.core_area);
      AppendOptionalNumber(&summary, "die_area", metrics.die_area);
      AppendOptionalNumber(&summary, "utilization", metrics.utilization);
      AppendOptionalNumber(&summary, "instance_count", metrics.instance_count);
      AppendOptionalNumber(&summary, "setup_wns", metrics.setup_wns);
      AppendOptionalNumber(&summary, "setup_tns", metrics.setup_tns);
      AppendOptionalNumber(&summary, "hold_wns", metrics.hold_wns);
      AppendOptionalNumber(&summary, "hold_tns", metrics.hold_tns);
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
      summary << ",\"metrics_raw_entries\":[";
      for (std::size_t index = 0; index < metrics.raw_entries.size(); ++index) {
        if (index != 0) summary << ',';
        const auto& [key, value] = metrics.raw_entries[index];
        summary << "{\"key\":\"" << EscapeJson(key) << "\",\"value\":" << value
                << '}';
      }
      summary << ']';
      summary << ",\"metrics_raw_count\":" << metrics.raw_entries.size() << "}";
    }
    AddArtifact(run, "summary", "json", summary_path, fingerprint, false,
                &artifacts);
    const std::filesystem::path checkpoint_manifest =
        reports_directory / "backend-checkpoint.json";
    {
      std::ofstream output(checkpoint_manifest,
                           std::ios::binary | std::ios::trunc);
      output << "{\"backend\":\"orfs\",\"lineage_id\":\""
             << EscapeJson(lineage_id) << "\",\"target_stage\":\""
             << EscapeJson(plan.target) << "\",\"configuration_fingerprint\":\""
             << EscapeJson(fingerprint) << "\",\"checkpoint_hash\":\""
             << EscapeJson(checkpoint_hash) << "\",\"parent_lineage_id\":\""
             << EscapeJson(request.rebuild_from_stage && request.resume
                               ? request.resume->lineage_id
                               : "")
             << "\",\"configuration_contract_hex\":\""
             << configuration_contract_hex << "\"}";
    }
    AddArtifact(run, "checkpoint-manifest", "json", checkpoint_manifest,
                fingerprint, !result_succeeded, &artifacts);
    RunOutcome outcome;
    outcome.process_succeeded = process_succeeded;
    outcome.result_succeeded = result_succeeded;
    outcome.summary_relative_path = "reports/orfs-summary.json";
    const RunStatus run_status = process_result.cancelled
                                     ? RunStatus::kCancelled
                                 : result_succeeded ? RunStatus::kSucceeded
                                                    : RunStatus::kFailed;
    const core::Status stored = run_store.Complete(
        run.get(), run_status, process_result.exit_code, diagnostics,
        std::move(artifacts), std::move(outcome));
    if (!stored.Ok()) result_status = stored;
    if (process_result.cancelled) {
      result_status = {core::ErrorCode::kCancelled, "ORFS flow cancelled", 0};
    } else if (result_succeeded) {
      result_status = core::Status::Success();
    } else if (result_status.Ok()) {
      result_status = {core::ErrorCode::kIoError,
                       process_succeeded
                           ? "ORFS stage artifacts or metrics failed"
                           : "ORFS flow failed",
                       0};
    }
    Finish(std::move(result_status), std::move(process_result),
           std::move(diagnostics), std::move(metrics), std::move(failure_stage),
           std::move(failure_code));
  }

  runtime::ExecutionProvider* provider = nullptr;
  mutable std::mutex mutex;
  runtime::TaskScheduler scheduler;
  runtime::TaskScheduler cleanup_scheduler;
  runtime::ResourceCoordinator resource_coordinator;
  RunStore run_store;
  adapters::OrfsAdapter adapter;
  ManagedFlowRunRequest request;
  ManagedFlowRunEventSink sink;
  ManagedFlowRunState state = ManagedFlowRunState::kIdle;
  std::shared_ptr<runtime::ExecutionHandle> handle;
  std::vector<std::shared_ptr<runtime::ExecutionHandle>>
      deferred_handle_cleanup;
  std::unique_ptr<runtime::CpuTokenLease> cpu_lease;
  std::unique_ptr<LineageLease> lineage_lease;
  std::shared_ptr<RunRecord> run;
  adapters::OrfsPlan plan;
  runtime::ProcessResult process_result;
  std::filesystem::path staging_directory;
  std::string probe_output;
  std::string tool_version;
  std::string tool_mode;
  std::string fingerprint;
  std::string configuration_contract_hex;
  std::string lineage_id;
  std::string last_step;
  bool sdc_auto_generated = false;
  bool unconstrained_warning = false;
  std::string sdc_provenance;
  bool suppressing_nix_lock_notice = false;
  std::string nix_notice_partial_line;
  bool active = false;
  bool cancellation_requested = false;
  bool terminal_delivered = false;
  bool shutdown = false;
  std::stop_source operation_stop_source;
};

OrfsManagedFlowRunService::OrfsManagedFlowRunService(
    runtime::ExecutionProvider* provider)
    : implementation_(std::make_shared<Implementation>(provider)) {}

OrfsManagedFlowRunService::~OrfsManagedFlowRunService() { Shutdown(); }

core::Status OrfsManagedFlowRunService::Start(ManagedFlowRunRequest request,
                                              ManagedFlowRunEventSink sink) {
  const auto implementation = implementation_;
  if (!implementation || !implementation->provider || !sink ||
      request.generation == 0 || request.library_directory.empty() ||
      request.cell_directory.empty()) {
    return {core::ErrorCode::kInvalidArgument,
            "ORFS run identity is incomplete", 0};
  }
  const core::Status validation = core::ValidateProject(request.project);
  if (!validation.Ok()) return validation;
  {
    std::scoped_lock lock(implementation->mutex);
    if (implementation->shutdown || implementation->active) {
      return {core::ErrorCode::kConflict,
              "An ORFS flow is already active or shutting down", 0};
    }
    implementation->request = std::move(request);
    implementation->sink = std::move(sink);
    implementation->state = ManagedFlowRunState::kProbing;
    implementation->handle.reset();
    implementation->cpu_lease.reset();
    implementation->lineage_lease.reset();
    implementation->run.reset();
    implementation->staging_directory.clear();
    implementation->probe_output.clear();
    implementation->tool_version.clear();
    implementation->tool_mode.clear();
    implementation->fingerprint.clear();
    implementation->configuration_contract_hex.clear();
    implementation->lineage_id.clear();
    implementation->last_step.clear();
    implementation->sdc_auto_generated = false;
    implementation->unconstrained_warning = false;
    implementation->sdc_provenance.clear();
    implementation->suppressing_nix_lock_notice = false;
    implementation->nix_notice_partial_line.clear();
    implementation->active = true;
    implementation->cancellation_requested = false;
    implementation->terminal_delivered = false;
    implementation->operation_stop_source = std::stop_source{};
  }
  implementation->EmitState(ManagedFlowRunState::kProbing);
  implementation->StartProbe();
  return core::Status::Success();
}

void OrfsManagedFlowRunService::Cancel() noexcept {
  const auto implementation = implementation_;
  if (!implementation) return;
  std::shared_ptr<runtime::ExecutionHandle> handle;
  {
    std::scoped_lock lock(implementation->mutex);
    if (!implementation->active || implementation->terminal_delivered) return;
    implementation->cancellation_requested = true;
    implementation->state = ManagedFlowRunState::kCancelling;
    implementation->operation_stop_source.request_stop();
    handle = implementation->handle;
  }
  if (handle) handle->Cancel();
  implementation->EmitState(ManagedFlowRunState::kCancelling);
}

void OrfsManagedFlowRunService::Shutdown() noexcept {
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
    implementation->sink = {};
  }
  if (active_handle) active_handle->Cancel();
  implementation->scheduler.RequestStop();
  implementation->cleanup_scheduler.RequestStop();
  deferred_handles.clear();
}

ManagedFlowRunState OrfsManagedFlowRunService::State() const {
  const auto implementation = implementation_;
  if (!implementation) return ManagedFlowRunState::kIdle;
  std::scoped_lock lock(implementation->mutex);
  return implementation->state;
}

bool OrfsManagedFlowRunService::IsActive() const {
  const auto implementation = implementation_;
  if (!implementation) return false;
  std::scoped_lock lock(implementation->mutex);
  return implementation->active;
}

}  // namespace designpp::application

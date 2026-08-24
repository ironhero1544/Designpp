// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_APPLICATION_TIMING_RUN_SERVICE_H_
#define DESIGNPP_APPLICATION_TIMING_RUN_SERVICE_H_

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "designpp/adapters/opensta_adapter.h"
#include "designpp/application/project_service.h"
#include "designpp/application/run_store.h"
#include "designpp/core/diagnostic.h"
#include "designpp/core/status.h"
#include "designpp/runtime/execution_provider.h"

namespace designpp::application {

enum class TimingRunState {
  kIdle,
  kProbing,
  kPreparing,
  kRunning,
  kCancelling,
  kSucceeded,
  kFailed,
  kCancelled,
};

enum class TimingRunEventKind { kStateChanged, kOutput, kCompleted };

struct TimingRunRequest {
  core::Project project;
  std::vector<ResolvedSource> sources;
  std::filesystem::path library_directory;
  std::filesystem::path cell_directory;
  std::uint64_t generation = 0;
};

struct TimingRunEvent {
  TimingRunEventKind kind = TimingRunEventKind::kStateChanged;
  TimingRunState state = TimingRunState::kIdle;
  std::uint64_t generation = 0;
  std::string output;
  core::Status status;
  runtime::ProcessResult process_result;
  std::shared_ptr<RunRecord> run;
  std::shared_ptr<RunRecord> synthesis_run;
  std::vector<core::Diagnostic> diagnostics;
  adapters::TimingMetrics metrics;
  std::string script_text;
};

using TimingRunEventSink = std::function<void(TimingRunEvent)>;

// Resolves the newest synthesis run that is safe to use for timing. This reads
// and hashes managed files and therefore must run on a worker thread.
[[nodiscard]] core::Result<std::shared_ptr<RunRecord>>
ResolveCompatibleSynthesisRun(const TimingRunRequest& request);

// Runs one OpenSTA analysis asynchronously and delivers terminal completion
// exactly once. Event sinks execute outside internal locks and must marshal to
// their owning GUI thread.
class TimingRunService final {
 public:
  explicit TimingRunService(runtime::ExecutionProvider* provider);
  TimingRunService(const TimingRunService&) = delete;
  TimingRunService& operator=(const TimingRunService&) = delete;
  ~TimingRunService();

  [[nodiscard]] core::Status Start(TimingRunRequest request,
                                   TimingRunEventSink sink);
  void Cancel() noexcept;
  void Shutdown() noexcept;
  [[nodiscard]] TimingRunState State() const;
  [[nodiscard]] bool IsActive() const;

 private:
  struct Implementation;
  std::shared_ptr<Implementation> implementation_;
};

}  // namespace designpp::application

#endif  // DESIGNPP_APPLICATION_TIMING_RUN_SERVICE_H_

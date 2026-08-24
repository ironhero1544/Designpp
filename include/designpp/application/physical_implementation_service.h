// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_APPLICATION_PHYSICAL_IMPLEMENTATION_SERVICE_H_
#define DESIGNPP_APPLICATION_PHYSICAL_IMPLEMENTATION_SERVICE_H_

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "designpp/application/managed_flow_run_service.h"
#include "designpp/runtime/task_scheduler.h"

namespace designpp::application {

enum class PhysicalImplementationEventKind {
  kStateChanged,
  kOutput,
  kProgress,
  kCompleted,
};

struct PhysicalImplementationEvent {
  PhysicalImplementationEventKind kind =
      PhysicalImplementationEventKind::kStateChanged;
  ManagedFlowRunState state = ManagedFlowRunState::kIdle;
  std::uint64_t generation = 0;
  core::Status status;
  std::string output;
  adapters::ManagedFlowProgress progress;
  adapters::ManagedFlowMetrics metrics;
  std::shared_ptr<RunRecord> run;
  std::filesystem::path gds_path;
  bool reused = false;
};

using PhysicalImplementationEventSink =
    std::function<void(PhysicalImplementationEvent)>;

// Returns a verified checkpoint resume request for a failed or interrupted
// physical implementation run whose inputs still match the current request.
[[nodiscard]] std::optional<ManagedFlowResumeRequest>
BuildPhysicalImplementationResume(const RunRecord& run,
                                  const ManagedFlowRunRequest& request);

// Provides the user-facing EnsureLayout operation while keeping the selected
// managed RTL-to-GDS backend and its checkpoint details outside the GUI.
class PhysicalImplementationService final {
 public:
  explicit PhysicalImplementationService(runtime::ExecutionProvider* provider);
  PhysicalImplementationService(const PhysicalImplementationService&) = delete;
  PhysicalImplementationService& operator=(
      const PhysicalImplementationService&) = delete;
  ~PhysicalImplementationService();

  [[nodiscard]] core::Status EnsureLayout(ManagedFlowRunRequest request,
                                          PhysicalImplementationEventSink sink);
  void Cancel() noexcept;
  void Shutdown() noexcept;
  [[nodiscard]] bool IsActive() const;

 private:
  void InspectAndStart(ManagedFlowRunRequest request,
                       PhysicalImplementationEventSink sink,
                       std::stop_token stop_token);
  void Forward(ManagedFlowRunEvent event);

  mutable std::mutex mutex_;
  ManagedFlowRunService managed_flow_;
  runtime::TaskScheduler scheduler_{1};
  PhysicalImplementationEventSink sink_;
  std::uint64_t generation_ = 0;
  bool active_ = false;
  bool terminal_delivered_ = false;
  bool shutdown_ = false;
};

}  // namespace designpp::application

#endif  // DESIGNPP_APPLICATION_PHYSICAL_IMPLEMENTATION_SERVICE_H_

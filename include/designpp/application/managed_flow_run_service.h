// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_APPLICATION_MANAGED_FLOW_RUN_SERVICE_H_
#define DESIGNPP_APPLICATION_MANAGED_FLOW_RUN_SERVICE_H_

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "designpp/adapters/openlane2_adapter.h"
#include "designpp/application/project_service.h"
#include "designpp/application/run_store.h"
#include "designpp/core/toolchain_profile.h"
#include "designpp/runtime/execution_provider.h"

namespace designpp::application {

struct PreparedPhysicalInputs;

enum class ManagedFlowRunState {
  kIdle,
  kProbing,
  kPreparing,
  kValidating,
  kRunning,
  kCollecting,
  kCancelling,
  kSucceeded,
  kFailed,
  kCancelled,
};

enum class ManagedFlowRunEventKind {
  kStateChanged,
  kOutput,
  kProgress,
  kCompleted,
};

struct ManagedFlowResumeRequest {
  std::string parent_run_id;
  std::string lineage_id;
  std::string resume_step;
  std::string configuration_fingerprint;
  std::string checkpoint_hash;
};

struct ManagedFlowRunRequest {
  core::Project project;
  core::ToolchainProfile profile;
  std::vector<ResolvedSource> sources;
  std::filesystem::path library_directory;
  std::filesystem::path cell_directory;
  std::uint64_t generation = 0;
  // The backend target for this attempt.  Layout requests use the final
  // output stage; the stage dialog can select an intermediate checkpoint.
  core::StageId target_stage = core::StageId::kFinalOutputs;
  // Generate/Update Layout requests use the backend's complete flow target.
  // Stage-dialog requests leave this false so Final maps to the incremental
  // finish target instead of the complete all target.
  bool full_flow = false;
  // A rebuild request creates a child backend lineage seeded only with
  // prerequisites before this stage.  The original lineage is preserved.
  std::optional<core::StageId> rebuild_from_stage;
  std::optional<ManagedFlowResumeRequest> resume;
  std::shared_ptr<const PreparedPhysicalInputs> prepared_inputs;
  // Immutable environment identity resolved by Tool Check before execution.
  std::string environment_id;
  std::string environment_fingerprint;
};

struct ManagedFlowRunEvent {
  ManagedFlowRunEventKind kind = ManagedFlowRunEventKind::kStateChanged;
  ManagedFlowRunState state = ManagedFlowRunState::kIdle;
  std::uint64_t generation = 0;
  std::string output;
  core::Status status;
  runtime::ProcessResult process_result;
  std::shared_ptr<RunRecord> run;
  std::vector<core::Diagnostic> diagnostics;
  adapters::ManagedFlowProgress progress;
  adapters::ManagedFlowMetrics metrics;
  std::string failure_stage;
  std::string failure_code;
  std::string configuration_fingerprint;
  std::string lineage_id;
};

using ManagedFlowRunEventSink = std::function<void(ManagedFlowRunEvent)>;

// Owns a managed RTL-to-GDS run. Completion is delivered exactly once and
// event sinks run outside service locks.
class ManagedFlowRunService final {
 public:
  explicit ManagedFlowRunService(runtime::ExecutionProvider* provider);
  ManagedFlowRunService(const ManagedFlowRunService&) = delete;
  ManagedFlowRunService& operator=(const ManagedFlowRunService&) = delete;
  ~ManagedFlowRunService();

  [[nodiscard]] core::Status Start(ManagedFlowRunRequest request,
                                   ManagedFlowRunEventSink sink);
  void Cancel() noexcept;
  void Shutdown() noexcept;
  [[nodiscard]] ManagedFlowRunState State() const;
  [[nodiscard]] bool IsActive() const;

 private:
  struct Implementation;
  std::shared_ptr<Implementation> implementation_;
};

}  // namespace designpp::application

#endif  // DESIGNPP_APPLICATION_MANAGED_FLOW_RUN_SERVICE_H_

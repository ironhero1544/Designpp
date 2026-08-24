// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_APPLICATION_SYNTHESIS_RUN_SERVICE_H_
#define DESIGNPP_APPLICATION_SYNTHESIS_RUN_SERVICE_H_

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "designpp/adapters/yosys_adapter.h"
#include "designpp/application/project_service.h"
#include "designpp/application/run_store.h"
#include "designpp/core/diagnostic.h"
#include "designpp/core/schematic.h"
#include "designpp/core/status.h"
#include "designpp/runtime/execution_provider.h"

namespace designpp::application {

enum class SynthesisRunState {
  kIdle,
  kProbing,
  kPreparing,
  kRunning,
  kCancelling,
  kSucceeded,
  kFailed,
  kCancelled,
};

enum class SynthesisRunEventKind {
  kStateChanged,
  kOutput,
  kCompleted,
};

struct SynthesisRunRequest {
  core::Project project;
  std::vector<ResolvedSource> sources;
  std::filesystem::path library_directory;
  std::filesystem::path cell_directory;
  std::uint64_t generation = 0;
};

struct SynthesisRunEvent {
  SynthesisRunEventKind kind = SynthesisRunEventKind::kStateChanged;
  SynthesisRunState state = SynthesisRunState::kIdle;
  std::uint64_t generation = 0;
  std::string output;
  core::Status status;
  runtime::ProcessResult process_result;
  std::shared_ptr<RunRecord> run;
  std::vector<core::Diagnostic> diagnostics;
  adapters::SynthesisMetrics metrics;
  core::SchematicModel gate_schematic;
  core::SchematicModel readable_schematic;
  core::Status readable_schematic_status;
  std::string script_text;
};

using SynthesisRunEventSink = std::function<void(SynthesisRunEvent)>;

// Owns one asynchronous Yosys run, including probing, CPU quota, run storage,
// artifact validation, parsing, cancellation, and exactly-once completion.
// Event sinks may run on worker threads and must marshal to their GUI thread.
class SynthesisRunService final {
 public:
  explicit SynthesisRunService(runtime::ExecutionProvider* provider);
  SynthesisRunService(const SynthesisRunService&) = delete;
  SynthesisRunService& operator=(const SynthesisRunService&) = delete;
  ~SynthesisRunService();

  [[nodiscard]] core::Status Start(SynthesisRunRequest request,
                                   SynthesisRunEventSink sink);
  void Cancel() noexcept;
  void Shutdown() noexcept;
  [[nodiscard]] SynthesisRunState State() const;
  [[nodiscard]] bool IsActive() const;

 private:
  struct Implementation;
  std::shared_ptr<Implementation> implementation_;
};

}  // namespace designpp::application

#endif  // DESIGNPP_APPLICATION_SYNTHESIS_RUN_SERVICE_H_

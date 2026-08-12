// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_APPLICATION_TESTBENCH_EXECUTION_SERVICE_H_
#define DESIGNPP_APPLICATION_TESTBENCH_EXECUTION_SERVICE_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "designpp/core/status.h"
#include "designpp/runtime/execution_provider.h"

namespace designpp::application {

enum class TestbenchOperation {
  kNone,
  kSimulation,
  kDebug,
};

enum class TestbenchExecutionState {
  kIdle,
  kProbing,
  kCompiling,
  kRunning,
  kCancelling,
  kCompleted,
  kFailed,
  kCancelled,
};

enum class TestbenchExecutionEventKind {
  kOutput,
  kCompleted,
};

struct TestbenchExecutionSnapshot {
  TestbenchOperation operation = TestbenchOperation::kNone;
  TestbenchExecutionState state = TestbenchExecutionState::kIdle;
  std::uint64_t generation = 0;
  bool active = false;
};

struct TestbenchExecutionEvent {
  TestbenchExecutionEventKind kind = TestbenchExecutionEventKind::kOutput;
  TestbenchOperation operation = TestbenchOperation::kNone;
  std::uint64_t generation = 0;
  std::string output;
  runtime::ProcessResult result;
};

using TestbenchExecutionEventSink =
    std::function<void(TestbenchExecutionEvent)>;

// Serializes one simulation or debug process for a Workspace. Callbacks may run
// on a process worker; the sink must marshal events to its presentation thread.
class TestbenchExecutionService final {
 public:
  explicit TestbenchExecutionService(runtime::ExecutionProvider* provider);
  TestbenchExecutionService(const TestbenchExecutionService&) = delete;
  TestbenchExecutionService& operator=(const TestbenchExecutionService&) =
      delete;
  ~TestbenchExecutionService();

  [[nodiscard]] core::Status Start(TestbenchOperation operation,
                                   TestbenchExecutionState initial_state,
                                   std::uint64_t generation,
                                   const runtime::WslCommand& command,
                                   TestbenchExecutionEventSink sink);
  [[nodiscard]] runtime::InputWriteResult WriteInput(std::string bytes);
  void Cancel() noexcept;
  void Shutdown() noexcept;
  [[nodiscard]] TestbenchExecutionSnapshot Snapshot() const;

 private:
  struct Implementation;
  std::shared_ptr<Implementation> implementation_;
};

}  // namespace designpp::application

#endif  // DESIGNPP_APPLICATION_TESTBENCH_EXECUTION_SERVICE_H_

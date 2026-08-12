// Copyright 2026 The Design++ Authors

#include "designpp/application/testbench_execution_service.h"

#include <mutex>
#include <utility>

namespace designpp::application {

struct TestbenchExecutionService::Implementation final {
  explicit Implementation(runtime::ExecutionProvider* provider)
      : provider(provider) {}

  runtime::ExecutionProvider* provider = nullptr;
  mutable std::mutex mutex;
  std::unique_ptr<runtime::ExecutionHandle> handle;
  TestbenchExecutionSnapshot snapshot;
  TestbenchExecutionEventSink sink;
  bool terminal_delivered = false;
  bool cancellation_requested = false;
  bool shutdown = false;
};

TestbenchExecutionService::TestbenchExecutionService(
    runtime::ExecutionProvider* provider)
    : implementation_(std::make_shared<Implementation>(provider)) {}

TestbenchExecutionService::~TestbenchExecutionService() { Shutdown(); }

core::Status TestbenchExecutionService::Start(
    TestbenchOperation operation, TestbenchExecutionState initial_state,
    std::uint64_t generation, const runtime::WslCommand& command,
    TestbenchExecutionEventSink sink) {
  const std::shared_ptr<Implementation> implementation = implementation_;
  if (implementation == nullptr || implementation->provider == nullptr) {
    return {core::ErrorCode::kInvalidArgument,
            "Testbench execution provider is unavailable", 0};
  }
  {
    std::scoped_lock lock(implementation->mutex);
    if (implementation->shutdown) {
      return {core::ErrorCode::kCancelled, "Workspace is shutting down", 0};
    }
    if (implementation->snapshot.active) {
      return {core::ErrorCode::kConflict,
              "A testbench operation is already active", 0};
    }
    implementation->handle.reset();
    implementation->snapshot = {operation, initial_state, generation, true};
    implementation->sink = std::move(sink);
    implementation->terminal_delivered = false;
    implementation->cancellation_requested = false;
  }

  runtime::ExecutionStartResult started = implementation->provider->Start(
      command,
      [implementation, operation, generation](std::string output) {
        TestbenchExecutionEventSink sink;
        {
          std::scoped_lock lock(implementation->mutex);
          if (implementation->shutdown || !implementation->snapshot.active ||
              implementation->snapshot.operation != operation ||
              implementation->snapshot.generation != generation) {
            return;
          }
          sink = implementation->sink;
        }
        if (sink) {
          TestbenchExecutionEvent event;
          event.kind = TestbenchExecutionEventKind::kOutput;
          event.operation = operation;
          event.generation = generation;
          event.output = std::move(output);
          sink(std::move(event));
        }
      },
      [implementation, operation, generation](runtime::ProcessResult result) {
        TestbenchExecutionEventSink sink;
        {
          std::scoped_lock lock(implementation->mutex);
          if (implementation->shutdown || implementation->terminal_delivered ||
              !implementation->snapshot.active ||
              implementation->snapshot.operation != operation ||
              implementation->snapshot.generation != generation) {
            return;
          }
          implementation->terminal_delivered = true;
          implementation->snapshot.active = false;
          implementation->snapshot.state =
              result.cancelled ? TestbenchExecutionState::kCancelled
              : result.started && result.exit_code == 0
                  ? TestbenchExecutionState::kCompleted
                  : TestbenchExecutionState::kFailed;
          sink = implementation->sink;
        }
        if (sink) {
          TestbenchExecutionEvent event;
          event.kind = TestbenchExecutionEventKind::kCompleted;
          event.operation = operation;
          event.generation = generation;
          event.result = std::move(result);
          sink(std::move(event));
        }
      });
  if (!started.Ok()) {
    std::scoped_lock lock(implementation->mutex);
    implementation->snapshot.active = false;
    implementation->snapshot.state = TestbenchExecutionState::kFailed;
    return started.status;
  }

  bool cancel = false;
  {
    std::scoped_lock lock(implementation->mutex);
    if (implementation->snapshot.active &&
        implementation->snapshot.generation == generation) {
      implementation->handle = std::move(started.handle);
      cancel = implementation->cancellation_requested;
    }
  }
  if (cancel) Cancel();
  return core::Status::Success();
}

runtime::InputWriteResult TestbenchExecutionService::WriteInput(
    std::string bytes) {
  std::scoped_lock lock(implementation_->mutex);
  if (!implementation_->snapshot.active || implementation_->handle == nullptr) {
    return runtime::InputWriteResult::kClosed;
  }
  return implementation_->handle->WriteInput(std::move(bytes));
}

void TestbenchExecutionService::Cancel() noexcept {
  const std::shared_ptr<Implementation> implementation = implementation_;
  if (implementation == nullptr) return;
  runtime::ExecutionHandle* handle = nullptr;
  {
    std::scoped_lock lock(implementation->mutex);
    if (!implementation->snapshot.active) return;
    implementation->cancellation_requested = true;
    implementation->snapshot.state = TestbenchExecutionState::kCancelling;
    handle = implementation->handle.get();
  }
  if (handle != nullptr) handle->Cancel();
}

void TestbenchExecutionService::Shutdown() noexcept {
  const std::shared_ptr<Implementation> implementation = implementation_;
  if (implementation == nullptr) return;
  runtime::ExecutionHandle* handle = nullptr;
  {
    std::scoped_lock lock(implementation->mutex);
    implementation->shutdown = true;
    handle = implementation->handle.get();
  }
  if (handle != nullptr) handle->Cancel();
}

TestbenchExecutionSnapshot TestbenchExecutionService::Snapshot() const {
  std::scoped_lock lock(implementation_->mutex);
  return implementation_->snapshot;
}

}  // namespace designpp::application

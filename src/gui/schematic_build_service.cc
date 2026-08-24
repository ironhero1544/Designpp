// Copyright 2026 The Design++ Authors

#include "designpp/gui/schematic_build_service.h"

#include <mutex>
#include <utility>

#include "designpp/runtime/resource_coordinator.h"
#include "designpp/runtime/task_scheduler.h"

namespace designpp::gui {

struct SchematicBuildService::Implementation final
    : public std::enable_shared_from_this<Implementation> {
  Implementation() : scheduler(1) {}

  void Finish(SchematicBuildEvent event) {
    SchematicBuildEventSink event_sink;
    {
      std::scoped_lock lock(mutex);
      if (shutdown || terminal_delivered || event.generation != generation) {
        return;
      }
      terminal_delivered = true;
      active = false;
      event_sink = sink;
      cpu_lease.reset();
    }
    if (event_sink) event_sink(std::move(event));
  }

  mutable std::mutex mutex;
  runtime::TaskScheduler scheduler;
  runtime::ResourceCoordinator resource_coordinator;
  std::unique_ptr<runtime::CpuTokenLease> cpu_lease;
  SchematicBuildEventSink sink;
  std::uint64_t generation = 0;
  bool active = false;
  bool cancelled = false;
  bool shutdown = false;
  bool terminal_delivered = false;
  std::stop_source operation_stop_source;
};

SchematicBuildService::SchematicBuildService()
    : implementation_(std::make_shared<Implementation>()) {}

SchematicBuildService::~SchematicBuildService() { Shutdown(); }

core::Status SchematicBuildService::Start(std::uint64_t generation,
                                          SchematicBuildRequest request,
                                          SchematicBuildEventSink sink) {
  const std::shared_ptr<Implementation> implementation = implementation_;
  {
    std::scoped_lock lock(implementation->mutex);
    if (implementation->shutdown) {
      return {core::ErrorCode::kConflict,
              "Schematic build service is shut down", 0};
    }
    if (implementation->active) {
      return {core::ErrorCode::kConflict,
              "A schematic scene is already being built", 0};
    }
    implementation->generation = generation;
    implementation->sink = std::move(sink);
    implementation->active = true;
    implementation->cancelled = false;
    implementation->terminal_delivered = false;
    implementation->operation_stop_source = std::stop_source{};
  }
  const bool accepted = implementation->scheduler.Submit(
      [implementation, generation,
       request = std::move(request)](std::stop_token stop_token) mutable {
        auto acquired = implementation->resource_coordinator.AcquireCpu(
            1, implementation->operation_stop_source.get_token());
        if (!acquired.Ok()) {
          implementation->Finish({generation, acquired.GetStatus(), nullptr});
          return;
        }
        bool cancelled_before_build = false;
        {
          std::scoped_lock lock(implementation->mutex);
          cancelled_before_build = implementation->shutdown ||
                                   implementation->cancelled ||
                                   stop_token.stop_requested();
          if (!cancelled_before_build) {
            implementation->cpu_lease =
                std::make_unique<runtime::CpuTokenLease>(
                    std::move(acquired).Value());
          }
        }
        if (cancelled_before_build) {
          implementation->Finish(
              {generation,
               {core::ErrorCode::kCancelled, "Schematic build cancelled", 0},
               nullptr});
          return;
        }
        const auto cancelled = [implementation, generation, stop_token] {
          std::scoped_lock lock(implementation->mutex);
          return implementation->shutdown || implementation->cancelled ||
                 implementation->generation != generation ||
                 stop_token.stop_requested();
        };
        auto built =
            SchematicSceneBuilder().Build(std::move(request), cancelled);
        if (!built.Ok()) {
          implementation->Finish({generation, built.GetStatus(), nullptr});
          return;
        }
        implementation->Finish(
            {generation, core::Status::Success(),
             std::make_shared<const SchematicScene>(std::move(built).Value())});
      });
  if (!accepted) {
    std::scoped_lock lock(implementation->mutex);
    implementation->active = false;
    implementation->terminal_delivered = true;
    return {core::ErrorCode::kConflict, "Schematic build queue is unavailable",
            0};
  }
  return core::Status::Success();
}

void SchematicBuildService::Cancel() noexcept {
  std::scoped_lock lock(implementation_->mutex);
  implementation_->cancelled = true;
  implementation_->operation_stop_source.request_stop();
}

void SchematicBuildService::Shutdown() noexcept {
  const std::shared_ptr<Implementation> implementation = implementation_;
  {
    std::scoped_lock lock(implementation->mutex);
    if (implementation->shutdown) return;
    implementation->shutdown = true;
    implementation->cancelled = true;
    implementation->operation_stop_source.request_stop();
    implementation->sink = {};
  }
  implementation->scheduler.RequestStop();
}

}  // namespace designpp::gui

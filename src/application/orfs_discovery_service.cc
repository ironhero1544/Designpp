// Copyright 2026 The Design++ Authors

#include "designpp/application/orfs_discovery_service.h"

#include <algorithm>
#include <utility>

namespace designpp::application {

OrfsDiscoveryService::OrfsDiscoveryService(runtime::ExecutionProvider* provider)
    : provider_(provider), live_(std::make_shared<std::atomic_bool>(true)) {}

OrfsDiscoveryService::~OrfsDiscoveryService() { Cancel(); }

core::Status OrfsDiscoveryService::Start(const core::ToolchainProfile& profile,
                                         std::uint64_t generation,
                                         OrfsDiscoverySink sink) {
  if (provider_ == nullptr || generation == 0 || profile.orfs_root.empty() ||
      !sink) {
    return {core::ErrorCode::kInvalidArgument,
            "ORFS platform discovery input is incomplete", 0};
  }
  Cancel();
  live_ = std::make_shared<std::atomic_bool>(true);
  const auto live = live_;
  adapters::OrfsAdapter adapter;
  const runtime::WslCommand command =
      adapter.BuildPlatformDiscoveryCommand(profile);
  runtime::ExecutionStartResult started = provider_->Start(
      command, [](std::string) {},
      [live, generation, sink = std::move(sink),
       adapter](runtime::ProcessResult result) mutable {
        if (!live->load() || !sink) return;
        OrfsDiscoveryResult discovery;
        discovery.generation = generation;
        if (!result.started || result.cancelled || result.exit_code != 0) {
          discovery.status =
              result.cancelled
                  ? core::Status{core::ErrorCode::kCancelled,
                                 "ORFS platform discovery cancelled", 0}
                  : core::Status{core::ErrorCode::kNotFound,
                                 "ORFS platform directory cannot be explored",
                                 0};
        } else {
          auto parsed = adapter.ParsePlatformDiscovery(result.output);
          if (!parsed.Ok()) {
            discovery.status = parsed.GetStatus();
          } else {
            discovery.candidates = std::move(parsed).Value();
            discovery.status = discovery.candidates.empty()
                                   ? core::Status{core::ErrorCode::kNotFound,
                                                  "No ORFS platforms found", 0}
                                   : core::Status::Success();
          }
        }
        sink(std::move(discovery));
      });
  if (!started.Ok()) return started.status;
  handle_ = std::move(started.handle);
  return core::Status::Success();
}

void OrfsDiscoveryService::Cancel() noexcept {
  if (live_) live_->store(false);
  if (handle_) handle_->Cancel();
  handle_.reset();
}

}  // namespace designpp::application

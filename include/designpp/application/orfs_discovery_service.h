// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_APPLICATION_ORFS_DISCOVERY_SERVICE_H_
#define DESIGNPP_APPLICATION_ORFS_DISCOVERY_SERVICE_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "designpp/adapters/orfs_adapter.h"
#include "designpp/core/toolchain_profile.h"
#include "designpp/runtime/execution_provider.h"

namespace designpp::application {

struct OrfsDiscoveryResult {
  std::uint64_t generation = 0;
  core::Status status;
  std::vector<adapters::OrfsPlatformCandidate> candidates;
};

using OrfsDiscoverySink = std::function<void(OrfsDiscoveryResult)>;

// Discovers ORFS platforms on a worker-owned WSL process.  The checkout is
// read-only and no platform is considered selectable until its contract files
// are present.
class OrfsDiscoveryService final {
 public:
  explicit OrfsDiscoveryService(runtime::ExecutionProvider* provider);
  OrfsDiscoveryService(const OrfsDiscoveryService&) = delete;
  OrfsDiscoveryService& operator=(const OrfsDiscoveryService&) = delete;
  ~OrfsDiscoveryService();

  [[nodiscard]] core::Status Start(const core::ToolchainProfile& profile,
                                   std::uint64_t generation,
                                   OrfsDiscoverySink sink);
  void Cancel() noexcept;

 private:
  runtime::ExecutionProvider* provider_ = nullptr;
  std::unique_ptr<runtime::ExecutionHandle> handle_;
  std::shared_ptr<std::atomic_bool> live_;
};

}  // namespace designpp::application

#endif  // DESIGNPP_APPLICATION_ORFS_DISCOVERY_SERVICE_H_

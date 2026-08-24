// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_APPLICATION_OPENLANE_DISCOVERY_SERVICE_H_
#define DESIGNPP_APPLICATION_OPENLANE_DISCOVERY_SERVICE_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "designpp/core/status.h"
#include "designpp/core/toolchain_profile.h"
#include "designpp/runtime/execution_provider.h"

namespace designpp::application {

struct OpenLanePdkCandidate {
  std::string pdk;
  std::string standard_cell_library;
};

struct OpenLaneDiscoveryResult {
  std::uint64_t generation = 0;
  core::Status status;
  std::vector<OpenLanePdkCandidate> candidates;
};

using OpenLaneDiscoverySink = std::function<void(OpenLaneDiscoveryResult)>;

class OpenLaneDiscoveryService final {
 public:
  explicit OpenLaneDiscoveryService(runtime::ExecutionProvider* provider);
  OpenLaneDiscoveryService(const OpenLaneDiscoveryService&) = delete;
  OpenLaneDiscoveryService& operator=(const OpenLaneDiscoveryService&) = delete;
  ~OpenLaneDiscoveryService();

  [[nodiscard]] core::Status Start(const core::ToolchainProfile& profile,
                                   std::uint64_t generation,
                                   OpenLaneDiscoverySink sink);
  void Cancel() noexcept;

 private:
  runtime::ExecutionProvider* provider_ = nullptr;
  std::unique_ptr<runtime::ExecutionHandle> handle_;
  std::shared_ptr<std::atomic_bool> live_;
};

}  // namespace designpp::application

#endif  // DESIGNPP_APPLICATION_OPENLANE_DISCOVERY_SERVICE_H_

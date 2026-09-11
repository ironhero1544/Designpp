// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_APPLICATION_TOOLCHAIN_ENVIRONMENT_SERVICE_H_
#define DESIGNPP_APPLICATION_TOOLCHAIN_ENVIRONMENT_SERVICE_H_

#include <string>
#include <string_view>

#include "designpp/application/toolchain_profile_store.h"
#include "designpp/core/status.h"
#include "designpp/core/toolchain_profile.h"
#include "designpp/runtime/wsl_executor.h"

namespace designpp::application {

struct ResolvedToolchainEnvironment {
  std::string installation_id;
  std::string provider_id;
  std::string bundle_id;
  std::string version;
  std::string root;
  std::string executable;
  std::string fingerprint;
  std::string wsl_distribution;
  bool verified = false;
};

enum class ToolInventoryState {
  kUnchecked,
  kReady,
  kPreparationRequired,
  kMissing,
  kIncompatible,
};

struct ToolInventoryResult {
  ToolInventoryState state = ToolInventoryState::kUnchecked;
  ResolvedToolchainEnvironment environment;
  std::string summary;
};

class ToolchainInventoryService final {
 public:
  [[nodiscard]] core::Result<ResolvedToolchainEnvironment> Resolve(
      const core::ToolchainSettings& settings,
      const core::ToolchainProfile& profile,
      std::string_view provider_id) const;
  [[nodiscard]] ToolInventoryResult Inspect(
      const core::ToolchainSettings& settings,
      const core::ToolchainProfile& profile,
      std::string_view provider_id) const;
  [[nodiscard]] runtime::WslCommand BuildVersionProbe(
      const ResolvedToolchainEnvironment& environment) const;
};

struct ToolchainAdoptionRequest {
  std::string profile_id;
  core::InstalledToolchainEnvironment environment;
};

// Registers already prepared environments and atomically changes the active
// reference. It never downloads, builds, updates, or deletes a toolchain.
class ToolchainPreparationService final {
 public:
  explicit ToolchainPreparationService(ToolchainProfileStore* store);

  [[nodiscard]] core::Result<ResolvedToolchainEnvironment> Adopt(
      ToolchainAdoptionRequest request);
  [[nodiscard]] core::Status Rollback(std::string_view profile_id,
                                      std::string_view provider_id);

 private:
  ToolchainProfileStore* store_ = nullptr;
};

}  // namespace designpp::application

#endif  // DESIGNPP_APPLICATION_TOOLCHAIN_ENVIRONMENT_SERVICE_H_

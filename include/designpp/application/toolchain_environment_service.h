// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_APPLICATION_TOOLCHAIN_ENVIRONMENT_SERVICE_H_
#define DESIGNPP_APPLICATION_TOOLCHAIN_ENVIRONMENT_SERVICE_H_

#include <stop_token>
#include <string>
#include <string_view>

#include "designpp/application/toolchain_profile_store.h"
#include "designpp/core/status.h"
#include "designpp/core/toolchain_compatibility.h"
#include "designpp/core/toolchain_profile.h"
#include "designpp/runtime/execution_provider.h"
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
  std::string command_contract_id;
  std::string framework_revision;
  std::string lock_hash;
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
  core::ToolchainCompatibilityEvidence evidence;
  bool activate = true;
  std::uint64_t expected_revision = 0;
};

enum class ToolchainManagementAction {
  kInspectConfigured,
  kRegisterPrepared,
  kActivatePrepared,
  kRollback,
};

// Blocking worker-only operation; cancellation and process-handle destruction
// occur on the caller's worker, never on an execution callback or GUI thread.
[[nodiscard]] core::Result<ResolvedToolchainEnvironment> ManageToolchain(
    ToolchainProfileStore& store, runtime::ExecutionProvider& provider,
    std::string_view provider_id, std::string_view bundle_id,
    ToolchainManagementAction action, std::stop_token stop);

// Parses the bounded, versioned probe protocol. A successful process without
// complete compatibility evidence is never a successful compatibility probe.
[[nodiscard]] core::Result<core::ToolchainCompatibilityEvidence>
ProbeToolchainCompatibility(std::string_view provider_id,
                            const runtime::ProcessResult& result);

// Registers already prepared environments and atomically changes the active
// reference. It never downloads, builds, updates, or deletes a toolchain.
class ToolchainPreparationService final {
 public:
  explicit ToolchainPreparationService(ToolchainProfileStore* store);

  [[nodiscard]] core::Result<ResolvedToolchainEnvironment> Adopt(
      ToolchainAdoptionRequest request);
  [[nodiscard]] core::Result<ResolvedToolchainEnvironment> Register(
      ToolchainAdoptionRequest request);
  [[nodiscard]] core::Status Rollback(
      std::string_view profile_id, std::string_view provider_id,
      const core::ToolchainCompatibilityEvidence& evidence);

 private:
  ToolchainProfileStore* store_ = nullptr;
};

}  // namespace designpp::application

#endif  // DESIGNPP_APPLICATION_TOOLCHAIN_ENVIRONMENT_SERVICE_H_

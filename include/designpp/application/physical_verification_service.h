// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_APPLICATION_PHYSICAL_VERIFICATION_SERVICE_H_
#define DESIGNPP_APPLICATION_PHYSICAL_VERIFICATION_SERVICE_H_

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "designpp/adapters/physical_verification_adapter.h"
#include "designpp/application/run_store.h"
#include "designpp/core/project.h"
#include "designpp/core/toolchain_profile.h"
#include "designpp/runtime/execution_provider.h"

namespace designpp::application {

enum class PhysicalVerificationState {
  kIdle,
  kPreparing,
  kProbing,
  kCdlModelValidating,
  kCdlGenerating,
  kCdlCombining,
  kRunning,
  kCollecting,
  kSucceeded,
  kViolated,
  kFailed,
  kCancelled,
};

// Frozen input contract for one explicit verification Run. Its hashes are
// written before tool execution and prevent a later Setup change from being
// mistaken for the source Layout Run used by this verification.
struct ResolvedVerificationContext {
  static constexpr std::uint32_t kContractVersion = 3;

  std::uint32_t contract_version = kContractVersion;
  std::string source_run_id;
  std::string platform;
  std::string gds_sha256;
  std::string odb_sha256;
  std::string source_netlist_sha256;
  std::string recipe_sha256;
  std::string environment_id;
  std::string environment_fingerprint;
};

struct PhysicalVerificationRequest {
  core::Project project;
  core::ToolchainProfile profile;
  core::VerificationRecipe recipe;
  adapters::VerificationCheck check = adapters::VerificationCheck::kDrc;
  RunRecord source_run;
  std::filesystem::path cell_directory;
  std::filesystem::path gds_path;
  std::filesystem::path odb_path;
  std::filesystem::path source_netlist_path;
  std::filesystem::path schematic_path;
  std::filesystem::path extracted_path;
  std::string environment_id;
  std::string environment_fingerprint;
  ResolvedVerificationContext resolved_context;
  std::uint64_t generation = 0;
};

struct PhysicalVerificationEvent {
  PhysicalVerificationState state = PhysicalVerificationState::kIdle;
  std::uint64_t generation = 0;
  core::Status status;
  std::string output;
  adapters::VerificationResult result;
  std::shared_ptr<RunRecord> run;
  bool completed = false;
};

using PhysicalVerificationEventSink =
    std::function<void(PhysicalVerificationEvent)>;

// Resolves managed verification recipes from the source Layout Run contract.
// Callers must provide backend and platform from that Run, never current Setup.
[[nodiscard]] std::vector<core::VerificationRecipe>
ResolveManagedVerificationRecipes(const core::ToolchainProfile& profile,
                                  std::string_view backend,
                                  std::string_view platform);

struct VerificationCapability {
  bool drc_ready = false;
  bool lvs_ready = false;
  std::string drc_status;
  std::string lvs_status;
};

// Intersects discovered platform files with the managed recipe registry. File
// presence alone never advertises an independent verification capability.
[[nodiscard]] VerificationCapability ResolveVerificationCapability(
    const core::ToolchainProfile& profile, std::string_view backend,
    std::string_view platform, bool platform_ready, bool drc_files_ready,
    bool lvs_files_ready);

// Runs one explicit DRC or LVS request against an immutable Layout Run.
class PhysicalVerificationService final {
 public:
  explicit PhysicalVerificationService(runtime::ExecutionProvider* provider);
  PhysicalVerificationService(const PhysicalVerificationService&) = delete;
  PhysicalVerificationService& operator=(const PhysicalVerificationService&) =
      delete;
  ~PhysicalVerificationService();

  [[nodiscard]] core::Status Start(PhysicalVerificationRequest request,
                                   PhysicalVerificationEventSink sink);
  void Cancel() noexcept;
  void Shutdown() noexcept;
  [[nodiscard]] bool IsActive() const;

 private:
  struct Implementation;
  std::shared_ptr<Implementation> implementation_;
};

}  // namespace designpp::application

#endif  // DESIGNPP_APPLICATION_PHYSICAL_VERIFICATION_SERVICE_H_

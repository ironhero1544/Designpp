// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_APPLICATION_PHYSICAL_VERIFICATION_SERVICE_H_
#define DESIGNPP_APPLICATION_PHYSICAL_VERIFICATION_SERVICE_H_

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

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
  kCdlGenerating,
  kCdlCombining,
  kRunning,
  kCollecting,
  kSucceeded,
  kViolated,
  kFailed,
  kCancelled,
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

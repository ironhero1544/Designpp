// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_APPLICATION_RUN_STORE_H_
#define DESIGNPP_APPLICATION_RUN_STORE_H_

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "designpp/core/diagnostic.h"
#include "designpp/core/project.h"
#include "designpp/core/status.h"

namespace designpp::application {

enum class RunStatus {
  kPending,
  kQueued,
  kRunning,
  kSucceeded,
  kFailed,
  kCancelled,
  kInterrupted,
};

struct RunArtifact {
  std::string kind;
  std::string format;
  std::string relative_path;
  std::uintmax_t size = 0;
  std::string input_hash;
  bool partial = false;
};

// Separates external-process completion from the domain result. For example,
// a cocotb process can exit normally while its xUnit report contains failures.
struct RunOutcome {
  bool process_succeeded = false;
  bool result_succeeded = false;
  std::string summary_relative_path;
};

struct RunRecord {
  std::string id;
  std::string project_id;
  std::string stage;
  std::string tool;
  std::string tool_version;
  RunStatus status = RunStatus::kPending;
  std::string started_utc;
  std::string finished_utc;
  std::uint32_t exit_code = 0;
  RunOutcome outcome;
  std::filesystem::path directory;
  std::vector<RunArtifact> artifacts;
};

class RunStore final {
 public:
  [[nodiscard]] core::Result<RunRecord> Begin(
      const std::filesystem::path& cell_directory, const core::Project& project,
      std::string stage, std::string tool, std::string tool_version) const;
  [[nodiscard]] core::Status AppendLog(const RunRecord& run,
                                       std::string_view bytes) const;
  [[nodiscard]] core::Status Complete(
      RunRecord* run, RunStatus status, std::uint32_t exit_code,
      const std::vector<core::Diagnostic>& diagnostics,
      std::vector<RunArtifact> artifacts = {}, RunOutcome outcome = {}) const;
  [[nodiscard]] core::Result<std::vector<RunRecord>> List(
      const std::filesystem::path& cell_directory) const;
  [[nodiscard]] core::Status RecoverInterrupted(
      const std::filesystem::path& cell_directory) const;
};

[[nodiscard]] std::string_view RunStatusName(RunStatus status) noexcept;

}  // namespace designpp::application

#endif  // DESIGNPP_APPLICATION_RUN_STORE_H_

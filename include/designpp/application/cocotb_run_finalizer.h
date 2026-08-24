// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_APPLICATION_COCOTB_RUN_FINALIZER_H_
#define DESIGNPP_APPLICATION_COCOTB_RUN_FINALIZER_H_

#include <optional>
#include <vector>

#include "designpp/adapters/cocotb_runner_adapter.h"
#include "designpp/adapters/simulation_result_parser.h"
#include "designpp/application/run_store.h"
#include "designpp/core/diagnostic.h"
#include "designpp/core/status.h"
#include "designpp/runtime/process_runner.h"

namespace designpp::application {

struct CocotbRunFinalization {
  core::Status status;
  RunStatus run_status = RunStatus::kFailed;
  std::vector<core::Diagnostic> diagnostics;
  std::optional<adapters::SimulationTestSummary> summary;
};

// Validates and preserves cocotb outputs after the external process completes.
// The original xUnit file remains authoritative even when normalized parsing
// fails. This function performs bounded file I/O and must not run on a GUI
// thread.
[[nodiscard]] CocotbRunFinalization FinalizeCocotbRun(
    RunRecord* run, const adapters::CocotbPlan& plan,
    const runtime::ProcessResult& process_result,
    const RunStore& run_store = RunStore());

// Reloads a normalized summary from the authoritative xUnit artifact of an
// existing run. File size and relative-path safety are validated before read.
[[nodiscard]] core::Result<adapters::SimulationTestSummary>
LoadCocotbRunSummary(const RunRecord& run);

}  // namespace designpp::application

#endif  // DESIGNPP_APPLICATION_COCOTB_RUN_FINALIZER_H_

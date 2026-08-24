// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_APPLICATION_TIMING_RUN_HISTORY_H_
#define DESIGNPP_APPLICATION_TIMING_RUN_HISTORY_H_

#include <filesystem>
#include <string>
#include <vector>

#include "designpp/adapters/opensta_adapter.h"
#include "designpp/application/run_store.h"
#include "designpp/core/diagnostic.h"
#include "designpp/core/status.h"

namespace designpp::application {

struct TimingRunSnapshot {
  RunRecord run;
  std::string corner;
  adapters::TimingMetrics metrics;
  std::vector<core::Diagnostic> diagnostics;
  std::string script_text;
};

// Restores all persisted timing runs newest-first. File I/O and OpenSTA report
// parsing happen in the application layer, so callers must invoke this on a
// bounded worker rather than a GUI thread.
[[nodiscard]] core::Result<std::vector<TimingRunSnapshot>> LoadTimingRunHistory(
    const std::filesystem::path& cell_directory,
    std::string_view fallback_corner);

}  // namespace designpp::application

#endif  // DESIGNPP_APPLICATION_TIMING_RUN_HISTORY_H_

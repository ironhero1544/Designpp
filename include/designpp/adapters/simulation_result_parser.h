// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_ADAPTERS_SIMULATION_RESULT_PARSER_H_
#define DESIGNPP_ADAPTERS_SIMULATION_RESULT_PARSER_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "designpp/core/status.h"

namespace designpp::adapters {

enum class SimulationTestStatus {
  kPassed,
  kFailed,
  kError,
  kSkipped,
};

struct SimulationTestCaseResult {
  std::string suite;
  std::string name;
  SimulationTestStatus status = SimulationTestStatus::kPassed;
  double duration_seconds = 0.0;
  std::string detail;
};

struct SimulationTestSummary {
  std::uint32_t total = 0;
  std::uint32_t passed = 0;
  std::uint32_t failed = 0;
  std::uint32_t errors = 0;
  std::uint32_t skipped = 0;
  double duration_seconds = 0.0;
  std::vector<SimulationTestCaseResult> cases;

  [[nodiscard]] bool Passed() const noexcept {
    return total > 0 && failed == 0 && errors == 0;
  }
};

// Parses the bounded xUnit subset emitted by cocotb. The original XML remains
// the authoritative artifact when normalized parsing fails.
[[nodiscard]] core::Result<SimulationTestSummary> ParseCocotbResults(
    std::string_view xml);

}  // namespace designpp::adapters

#endif  // DESIGNPP_ADAPTERS_SIMULATION_RESULT_PARSER_H_

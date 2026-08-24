// Copyright 2026 The Design++ Authors

#include "designpp/application/cocotb_run_finalizer.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>

#include "designpp/core/project.h"

namespace designpp::application {
namespace {

constexpr std::uintmax_t kMaximumResultsSize = 16U * 1024U * 1024U;

std::string PathToUtf8(const std::filesystem::path& path) {
  const std::u8string value = path.generic_u8string();
  return std::string(value.begin(), value.end());
}

std::string EscapeJson(std::string_view value) {
  std::ostringstream escaped;
  for (const unsigned char character : value) {
    switch (character) {
      case '\\':
        escaped << "\\\\";
        break;
      case '"':
        escaped << "\\\"";
        break;
      case '\n':
        escaped << "\\n";
        break;
      case '\r':
        escaped << "\\r";
        break;
      case '\t':
        escaped << "\\t";
        break;
      default:
        if (character < 0x20) {
          escaped << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                  << static_cast<unsigned int>(character) << std::dec;
        } else {
          escaped << static_cast<char>(character);
        }
    }
  }
  return escaped.str();
}

std::string TestStatusName(adapters::SimulationTestStatus status) {
  switch (status) {
    case adapters::SimulationTestStatus::kPassed:
      return "passed";
    case adapters::SimulationTestStatus::kFailed:
      return "failed";
    case adapters::SimulationTestStatus::kError:
      return "error";
    case adapters::SimulationTestStatus::kSkipped:
      return "skipped";
  }
  return "error";
}

core::Status AddArtifact(const RunRecord& run,
                         const std::filesystem::path& path, std::string kind,
                         std::string format, std::string_view input_hash,
                         bool partial, std::vector<RunArtifact>* artifacts) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error) || error) {
    return {core::ErrorCode::kNotFound, "cocotb artifact is missing", 0};
  }
  const std::uintmax_t size = std::filesystem::file_size(path, error);
  if (error || size == 0) {
    return {core::ErrorCode::kNotFound, "cocotb artifact is empty", 0};
  }
  const std::filesystem::path relative =
      std::filesystem::relative(path, run.directory, error);
  const std::string relative_utf8 =
      error ? std::string() : PathToUtf8(relative);
  if (error || !core::IsSafeRelativePath(relative_utf8)) {
    return {core::ErrorCode::kInvalidArgument,
            "cocotb artifact is outside the run directory", 0};
  }
  artifacts->push_back({std::move(kind), std::move(format), relative_utf8, size,
                        std::string(input_hash), partial});
  return core::Status::Success();
}

core::Status WriteSummary(const std::filesystem::path& path,
                          const adapters::SimulationTestSummary& summary) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    return {core::ErrorCode::kIoError,
            "Cannot create cocotb simulation summary", 0};
  }
  output << "{\"schema_version\":2,\"total\":" << summary.total
         << ",\"passed\":" << summary.passed << ",\"failed\":" << summary.failed
         << ",\"errors\":" << summary.errors
         << ",\"skipped\":" << summary.skipped
         << ",\"duration_seconds\":" << std::setprecision(17)
         << summary.duration_seconds << ",\"cases\":[";
  for (std::size_t index = 0; index < summary.cases.size(); ++index) {
    const adapters::SimulationTestCaseResult& test_case = summary.cases[index];
    if (index != 0) output << ',';
    output << "{\"suite\":\"" << EscapeJson(test_case.suite) << "\",\"name\":\""
           << EscapeJson(test_case.name) << "\",\"status\":\""
           << TestStatusName(test_case.status)
           << "\",\"duration_seconds\":" << std::setprecision(17)
           << test_case.duration_seconds << ",\"detail\":\""
           << EscapeJson(test_case.detail) << "\"}";
  }
  output << "]}\n";
  output.flush();
  return output ? core::Status::Success()
                : core::Status{core::ErrorCode::kIoError,
                               "Cannot flush cocotb simulation summary", 0};
}

void AddDiagnostic(std::string code, std::string message,
                   std::vector<core::Diagnostic>* diagnostics) {
  diagnostics->push_back({core::DiagnosticSeverity::kError, std::move(code), "",
                          0, 0, std::move(message)});
}

}  // namespace

CocotbRunFinalization FinalizeCocotbRun(
    RunRecord* run, const adapters::CocotbPlan& plan,
    const runtime::ProcessResult& process_result, const RunStore& run_store) {
  CocotbRunFinalization finalization;
  if (run == nullptr) {
    finalization.status = {core::ErrorCode::kInvalidArgument,
                           "Cocotb run record is missing", 0};
    return finalization;
  }

  const bool process_succeeded = process_result.started &&
                                 !process_result.cancelled &&
                                 process_result.exit_code == 0;
  const bool partial = !process_succeeded;
  std::vector<RunArtifact> artifacts;

  std::error_code error;
  const bool results_exist =
      std::filesystem::is_regular_file(plan.results_path, error) && !error;
  const std::uintmax_t results_size =
      results_exist ? std::filesystem::file_size(plan.results_path, error) : 0;
  core::Status results_status = core::Status::Success();
  if (!results_exist || error || results_size == 0) {
    results_status = {core::ErrorCode::kNotFound,
                      "cocotb did not produce results.xml", 0};
  } else {
    const core::Status artifact_status =
        AddArtifact(*run, plan.results_path, "test-results", "xunit",
                    plan.input_hash, partial, &artifacts);
    if (!artifact_status.Ok()) results_status = artifact_status;
  }

  if (results_status.Ok() && results_size > kMaximumResultsSize) {
    results_status = {core::ErrorCode::kFileTooLarge,
                      "cocotb results.xml is too large", 0};
  } else if (results_status.Ok()) {
    std::ifstream input(plan.results_path, std::ios::binary);
    const std::string xml((std::istreambuf_iterator<char>(input)),
                          std::istreambuf_iterator<char>());
    auto parsed = adapters::ParseCocotbResults(xml);
    if (!parsed.Ok()) {
      results_status = parsed.GetStatus();
    } else {
      finalization.summary = std::move(parsed).Value();
    }
  }
  if (!results_status.Ok() && !process_result.cancelled) {
    AddDiagnostic("COCOTB_RESULTS", results_status.message,
                  &finalization.diagnostics);
  }

  std::string summary_relative_path;
  if (finalization.summary) {
    const std::filesystem::path reports = run->directory / L"reports";
    std::filesystem::create_directories(reports, error);
    core::Status summary_status =
        error ? core::Status{core::ErrorCode::kIoError,
                             "Cannot create cocotb reports directory", 0}
              : WriteSummary(reports / L"simulation-summary.json",
                             *finalization.summary);
    if (summary_status.Ok()) {
      summary_status =
          AddArtifact(*run, reports / L"simulation-summary.json", "summary",
                      "json", plan.input_hash, partial, &artifacts);
    }
    if (!summary_status.Ok()) {
      AddDiagnostic("COCOTB_SUMMARY", summary_status.message,
                    &finalization.diagnostics);
      results_status = summary_status;
    } else {
      summary_relative_path = "reports/simulation-summary.json";
    }
  }

  bool waveform_found = plan.waveform_path.empty();
  if (!plan.waveform_path.empty()) {
    const std::filesystem::path parent = plan.waveform_path.parent_path();
    const std::filesystem::path extension = plan.waveform_path.extension();
    error.clear();
    for (std::filesystem::directory_iterator iterator(parent, error), end;
         !error && iterator != end; iterator.increment(error)) {
      if (iterator->path().extension() != extension) continue;
      std::string format = PathToUtf8(extension);
      if (!format.empty() && format.front() == '.') format.erase(0, 1);
      if (AddArtifact(*run, iterator->path(), "waveform", std::move(format),
                      plan.input_hash, partial, &artifacts)
              .Ok()) {
        waveform_found = true;
        break;
      }
    }
  }
  if (!waveform_found && !process_result.cancelled) {
    AddDiagnostic("COCOTB_WAVEFORM", "cocotb waveform is missing",
                  &finalization.diagnostics);
  }

  const bool result_succeeded =
      process_succeeded && results_status.Ok() && finalization.summary &&
      finalization.summary->Passed() && waveform_found;
  finalization.run_status = process_result.cancelled ? RunStatus::kCancelled
                            : result_succeeded       ? RunStatus::kSucceeded
                                                     : RunStatus::kFailed;
  RunOutcome outcome;
  outcome.process_succeeded = process_succeeded;
  outcome.result_succeeded = result_succeeded;
  outcome.summary_relative_path = std::move(summary_relative_path);
  const core::Status complete = run_store.Complete(
      run, finalization.run_status, process_result.exit_code,
      finalization.diagnostics, std::move(artifacts), std::move(outcome));
  if (!complete.Ok()) {
    finalization.status = complete;
  } else if (result_succeeded) {
    finalization.status = core::Status::Success();
  } else if (process_result.cancelled) {
    finalization.status = {core::ErrorCode::kCancelled, "cocotb run cancelled",
                           0};
  } else if (!results_status.Ok()) {
    finalization.status = results_status;
  } else if (!waveform_found) {
    finalization.status = {core::ErrorCode::kNotFound,
                           "cocotb waveform is missing", 0};
  } else if (!process_succeeded) {
    finalization.status = {core::ErrorCode::kIoError, "cocotb process failed",
                           0};
  } else {
    finalization.status = {core::ErrorCode::kIoError, "cocotb tests failed", 0};
  }
  return finalization;
}

core::Result<adapters::SimulationTestSummary> LoadCocotbRunSummary(
    const RunRecord& run) {
  const auto artifact = std::find_if(
      run.artifacts.begin(), run.artifacts.end(),
      [](const RunArtifact& candidate) {
        return candidate.kind == "test-results" && candidate.format == "xunit";
      });
  if (artifact == run.artifacts.end() ||
      !core::IsSafeRelativePath(artifact->relative_path)) {
    return core::Status{core::ErrorCode::kNotFound,
                        "Cocotb xUnit artifact is unavailable", 0};
  }
  const std::filesystem::path path = run.directory / artifact->relative_path;
  std::error_code error;
  const std::uintmax_t size = std::filesystem::file_size(path, error);
  if (error || size == 0) {
    return core::Status{core::ErrorCode::kNotFound,
                        "Cocotb xUnit artifact is missing or empty", 0};
  }
  if (size > kMaximumResultsSize) {
    return core::Status{core::ErrorCode::kFileTooLarge,
                        "Cocotb xUnit artifact is too large", 0};
  }
  std::ifstream input(path, std::ios::binary);
  const std::string xml((std::istreambuf_iterator<char>(input)),
                        std::istreambuf_iterator<char>());
  if (!input.eof() && input.fail()) {
    return core::Status{core::ErrorCode::kIoError,
                        "Cannot read cocotb xUnit artifact", 0};
  }
  return adapters::ParseCocotbResults(xml);
}

}  // namespace designpp::application

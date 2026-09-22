// Copyright 2026 The Design++ Authors

#include "designpp/application/physical_verification_service.h"

#include <objbase.h>
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>
#include <sstream>
#include <utility>
#include <vector>

#include "designpp/application/synthesis_fingerprint.h"
#include "designpp/runtime/path_mapper.h"
#include "designpp/runtime/resource_coordinator.h"
#include "designpp/runtime/task_scheduler.h"

namespace designpp::application {
namespace {

std::wstring Utf8ToWide(std::string_view text) {
  if (text.empty()) return {};
  const int size =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), nullptr, 0);
  if (size <= 0) return {};
  std::wstring result(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                      static_cast<int>(text.size()), result.data(), size);
  return result;
}

std::string WideToUtf8(std::wstring_view text) {
  if (text.empty()) return {};
  const int size = WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
      nullptr, 0, nullptr, nullptr);
  if (size <= 0) return {};
  std::string result(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                      static_cast<int>(text.size()), result.data(), size,
                      nullptr, nullptr);
  return result;
}

std::wstring NewUuid() {
  GUID guid{};
  if (FAILED(CoCreateGuid(&guid))) return L"verification";
  wchar_t value[39]{};
  if (StringFromGUID2(guid, value, static_cast<int>(std::size(value))) <= 0) {
    return L"verification";
  }
  std::wstring result(value);
  if (!result.empty() && result.front() == L'{') result.erase(result.begin());
  if (!result.empty() && result.back() == L'}') result.pop_back();
  return result;
}

core::Result<std::filesystem::path> CreateAsciiStagingDirectory(
    std::wstring_view id) {
  std::error_code error;
  const std::filesystem::path root =
      std::filesystem::temp_directory_path(error);
  if (error || root.empty()) {
    return core::Status{core::ErrorCode::kIoError,
                        "LVS-CDL-INPUT: cannot resolve the staging directory",
                        static_cast<unsigned long>(error.value())};
  }
  if (std::any_of(root.native().begin(), root.native().end(),
                  [](wchar_t character) { return character > 0x7f; })) {
    return core::Status{
        core::ErrorCode::kInvalidArgument,
        "LVS-CDL-INPUT: verification staging path must be ASCII-safe", 0};
  }
  const std::filesystem::path directory =
      root / L"DesignPlusPlus" / L"verification" / std::wstring(id);
  std::filesystem::create_directories(directory, error);
  if (error) {
    return core::Status{
        core::ErrorCode::kIoError,
        "LVS-CDL-INPUT: cannot create the verification staging directory",
        static_cast<unsigned long>(error.value())};
  }
  return directory;
}

core::Status CopyInput(const std::filesystem::path& source,
                       const std::filesystem::path& destination,
                       std::string_view name) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(source, error) || error ||
      std::filesystem::file_size(source, error) == 0) {
    return {core::ErrorCode::kNotFound,
            std::string("Verification ") + std::string(name) +
                " is missing or empty",
            0};
  }
  std::filesystem::create_directories(destination.parent_path(), error);
  if (!error) {
    std::filesystem::copy_file(
        source, destination, std::filesystem::copy_options::overwrite_existing,
        error);
  }
  return error ? core::Status{core::ErrorCode::kIoError,
                              "Cannot stage physical verification input",
                              static_cast<unsigned long>(error.value())}
               : core::Status::Success();
}

core::Status WriteTextFile(const std::filesystem::path& path,
                           std::string_view contents, std::string_view name) {
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  output.flush();
  if (error || !output) {
    return {core::ErrorCode::kIoError,
            "Cannot write verification " + std::string(name),
            static_cast<unsigned long>(error.value())};
  }
  return core::Status::Success();
}

bool IsSafeRelativeLinuxPath(std::string_view path) {
  if (path.empty() || path.front() == '/' ||
      path.find('\\') != std::string_view::npos) {
    return false;
  }
  std::size_t begin = 0;
  while (begin < path.size()) {
    const std::size_t end = path.find('/', begin);
    const std::string_view component =
        path.substr(begin, end == std::string_view::npos ? path.size() - begin
                                                         : end - begin);
    if (component.empty() || component == "." || component == "..") {
      return false;
    }
    if (end == std::string_view::npos) break;
    begin = end + 1;
  }
  return true;
}

std::vector<std::string> CdlSubcktNames(std::string_view contents) {
  std::vector<std::string> names;
  std::istringstream lines{std::string(contents)};
  std::string line;
  while (std::getline(lines, line)) {
    const std::size_t first = line.find_first_not_of(" \t\r\n");
    if (first == std::string::npos || line[first] == '*' ||
        line[first] == ';' || line[first] == '#') {
      continue;
    }
    std::istringstream fields(line.substr(first));
    std::string directive;
    std::string name;
    fields >> directive >> name;
    std::transform(directive.begin(), directive.end(), directive.begin(),
                   [](unsigned char character) {
                     return static_cast<char>(std::tolower(character));
                   });
    if (directive == ".subckt" && !name.empty()) {
      std::transform(name.begin(), name.end(), name.begin(),
                     [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                     });
      names.push_back(std::move(name));
    }
  }
  return names;
}

core::Status ValidateGeneratedCdl(const std::filesystem::path& design_path,
                                  const std::filesystem::path& model_path,
                                  std::string_view top_cell) {
  std::ifstream design_input(design_path, std::ios::binary);
  std::ifstream model_input(model_path, std::ios::binary);
  const std::string design((std::istreambuf_iterator<char>(design_input)),
                           std::istreambuf_iterator<char>());
  const std::string model((std::istreambuf_iterator<char>(model_input)),
                          std::istreambuf_iterator<char>());
  if (design.empty() || model.empty()) {
    return {core::ErrorCode::kCorruptData,
            "LVS-CDL-PARSE: generated CDL or cell model is empty", 0};
  }
  const std::vector<std::string> design_subckts = CdlSubcktNames(design);
  const std::vector<std::string> model_subckts = CdlSubcktNames(model);
  if (design_subckts.empty()) {
    return {core::ErrorCode::kCorruptData,
            "LVS-CDL-PARSE: OpenROAD output has no subcircuit", 0};
  }
  if (model_subckts.empty()) {
    return {core::ErrorCode::kCorruptData,
            "LVS-MODEL-MISSING: platform CDL has no cell definitions", 0};
  }
  if (!top_cell.empty()) {
    std::string lower_top(top_cell);
    std::transform(lower_top.begin(), lower_top.end(), lower_top.begin(),
                   [](unsigned char character) {
                     return static_cast<char>(std::tolower(character));
                   });
    if (std::find(design_subckts.begin(), design_subckts.end(), lower_top) ==
        design_subckts.end()) {
      return {core::ErrorCode::kCorruptData,
              "LVS-CDL-PARSE: generated CDL top cell is missing", 0};
    }
  }
  return core::Status::Success();
}

core::Status ValidateCombinedCdl(const std::filesystem::path& path,
                                 std::string_view top_cell) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return {core::ErrorCode::kNotFound,
            "LVS-CDL-PARSE: combined CDL was not generated", 0};
  }
  const std::string contents((std::istreambuf_iterator<char>(input)),
                             std::istreambuf_iterator<char>());
  if (contents.empty()) {
    return {core::ErrorCode::kCorruptData,
            "LVS-CDL-PARSE: combined CDL is empty", 0};
  }
  const std::vector<std::string> names = CdlSubcktNames(contents);
  std::set<std::string> definitions;
  std::string lower_top(top_cell);
  std::transform(lower_top.begin(), lower_top.end(), lower_top.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  bool has_top = false;
  for (const std::string& name : names) {
    if (!definitions.insert(name).second) {
      return {core::ErrorCode::kCorruptData,
              "LVS-CDL-PARSE: duplicate CDL subcircuit definition", 0};
    }
    if (!lower_top.empty() && name == lower_top) has_top = true;
  }
  if (names.size() < 2 || (!lower_top.empty() && !has_top)) {
    return {core::ErrorCode::kCorruptData,
            "LVS-CDL-PARSE: combined CDL is missing the top or cell model", 0};
  }
  return core::Status::Success();
}

std::string EscapeJson(std::string_view input) {
  std::string result;
  for (char character : input) {
    if (character == '"' || character == '\\') result.push_back('\\');
    if (character == '\n') {
      result += "\\n";
    } else if (character != '\r') {
      result.push_back(character);
    }
  }
  return result;
}

}  // namespace

struct PhysicalVerificationService::Implementation final
    : public std::enable_shared_from_this<Implementation> {
  explicit Implementation(runtime::ExecutionProvider* execution_provider)
      : provider(execution_provider) {}

  void QueueHandleCleanup(
      std::shared_ptr<runtime::ExecutionHandle> completed_handle) {
    if (!completed_handle) return;
    auto holder = std::make_shared<std::shared_ptr<runtime::ExecutionHandle>>(
        std::move(completed_handle));
    const bool queued = cleanup.Submit([holder](std::stop_token) mutable {
      // Moving the sole handle reference to this worker makes its destructor
      // join the process callback thread from a different thread.
      auto handle = std::move(*holder);
      holder.reset();
      static_cast<void>(handle->IsRunning());
    });
    if (!queued) {
      std::scoped_lock lock(mutex);
      deferred_handle_cleanup.push_back(std::move(*holder));
    }
  }

  void Emit(PhysicalVerificationEvent event) {
    PhysicalVerificationEventSink current;
    std::shared_ptr<runtime::ExecutionHandle> completed_handle;
    {
      std::scoped_lock lock(mutex);
      if (shutdown || event.generation != request.generation) return;
      if (event.completed) {
        if (terminal) return;
        terminal = true;
        active = false;
        active_process_id = 0;
        completed_handle = std::move(handle);
        cpu_lease.reset();
      }
      state = event.state;
      current = sink;
    }
    if (current) current(std::move(event));
    QueueHandleCleanup(std::move(completed_handle));
  }

  void EmitState(PhysicalVerificationState next) {
    PhysicalVerificationEvent event;
    event.generation = request.generation;
    event.state = next;
    Emit(std::move(event));
  }

  void EmitOutput(std::string output) {
    if (run) static_cast<void>(store.AppendLog(*run, output));
    {
      std::scoped_lock lock(mutex);
      constexpr std::size_t kMaximumResultOutputBytes = 1024 * 1024;
      if (process_output.size() < kMaximumResultOutputBytes) {
        const std::size_t available =
            kMaximumResultOutputBytes - process_output.size();
        process_output.append(output.substr(0, available));
      }
    }
    PhysicalVerificationEvent event;
    event.generation = request.generation;
    event.state = state;
    event.output = std::move(output);
    Emit(std::move(event));
  }

  core::Status WriteInputManifest(std::string_view model_hash = {},
                                  std::string_view design_cdl_hash = {},
                                  std::string_view combined_cdl_hash = {}) {
    std::error_code error;
    std::filesystem::create_directories(input_manifest_path.parent_path(),
                                        error);
    if (error) {
      return {core::ErrorCode::kIoError,
              "Cannot create verification input manifest directory",
              static_cast<unsigned long>(error.value())};
    }
    std::ofstream manifest(input_manifest_path,
                           std::ios::binary | std::ios::trunc);
    manifest << "{\n  \"schema_version\": 3,\n  \"contract_version\": "
             << context.contract_version << ",\n  \"source_run_id\": \""
             << EscapeJson(context.source_run_id) << "\",\n  \"platform\": \""
             << EscapeJson(context.platform) << "\",\n  \"gds_sha256\": \""
             << context.gds_sha256 << "\",\n  \"odb_sha256\": \""
             << context.odb_sha256 << "\",\n  \"source_netlist_sha256\": \""
             << context.source_netlist_sha256
             << "\",\n  \"schematic_sha256\": \"" << schematic_hash
             << "\",\n  \"model_sha256\": \"" << model_hash
             << "\",\n  \"design_cdl_sha256\": \"" << design_cdl_hash
             << "\",\n  \"combined_cdl_sha256\": \"" << combined_cdl_hash
             << "\",\n  \"recipe_id\": \"" << EscapeJson(request.recipe.id)
             << "\",\n  \"recipe_hash\": \""
             << EscapeJson(context.recipe_sha256)
             << "\",\n  \"environment_id\": \""
             << EscapeJson(context.environment_id)
             << "\",\n  \"environment_fingerprint\": \""
             << EscapeJson(context.environment_fingerprint)
             << "\",\n  \"model_source\": \""
             << EscapeJson(WideToUtf8(command_input.model_path))
             << "\",\n  \"preparation_contract\": \""
             << (request.recipe.engine == "klayout_lvs" ? "orfs-sky130hd-cdl-v1"
                                                        : "none")
             << "\"\n}\n";
    manifest.flush();
    if (!manifest) {
      return {core::ErrorCode::kIoError,
              "Cannot preserve verification input manifest", 0};
    }
    return core::Status::Success();
  }

  void Finish(core::Status status, adapters::VerificationResult result,
              std::uint32_t exit_code, bool process_succeeded) {
    if (run && request.recipe.engine == "klayout_lvs" &&
        !staging_directory.empty()) {
      const auto recipe_hash =
          CalculateFileSha256(staging_directory / L"rule-effective.lylvs");
      if (recipe_hash.Ok()) {
        context.recipe_sha256 = recipe_hash.Value();
        request.recipe.content_hash = recipe_hash.Value();
        const auto model = CalculateFileSha256(staged_model_path);
        const auto design = CalculateFileSha256(design_cdl_path);
        const auto combined = CalculateFileSha256(combined_cdl_path);
        const auto saved = WriteInputManifest(
            model.Ok() ? model.Value() : "", design.Ok() ? design.Value() : "",
            combined.Ok() ? combined.Value() : "");
        if (!saved.Ok()) status = saved;
      }
      if (!status.Ok()) {
        std::scoped_lock lock(mutex);
        for (const auto* code :
             {"LVS-RECIPE-INCOMPATIBLE:", "LVS-SUBSTRATE-UNSUPPORTED:",
              "LVS-REPORT-MISSING:", "LVS-COMPARISON-MISSING:"}) {
          const auto start = process_output.find(code);
          if (start == std::string::npos) continue;
          status.message = process_output.substr(
              start, process_output.find('\n', start) - start);
          break;
        }
      }
    }
    std::vector<RunArtifact> artifacts;
    RunOutcome outcome;
    if (run) {
      std::error_code error;
      const std::filesystem::path reports = run->directory / L"reports";
      std::filesystem::create_directories(reports, error);
      const std::filesystem::path report_destination =
          reports / report_path.filename();
      if (std::filesystem::is_regular_file(report_path, error) && !error) {
        std::filesystem::copy_file(
            report_path, report_destination,
            std::filesystem::copy_options::overwrite_existing, error);
        if (!error) {
          artifacts.push_back(
              {"verification.report", request.recipe.output_format,
               std::filesystem::relative(report_destination, run->directory)
                   .generic_string(),
               std::filesystem::file_size(report_destination, error),
               request.recipe.content_hash, false});
        }
      }
      if (std::filesystem::is_regular_file(input_manifest_path, error) &&
          !error) {
        artifacts.push_back(
            {"verification.inputs", "json",
             std::filesystem::relative(input_manifest_path, run->directory)
                 .generic_string(),
             std::filesystem::file_size(input_manifest_path, error),
             request.recipe.content_hash, false});
      }
      const auto preserve = [this, &reports, &artifacts](
                                const std::filesystem::path& source,
                                std::wstring_view name, std::string_view kind,
                                std::string_view format) {
        std::error_code preserve_error;
        if (!std::filesystem::is_regular_file(source, preserve_error) ||
            preserve_error ||
            std::filesystem::file_size(source, preserve_error) == 0) {
          return;
        }
        const std::filesystem::path destination =
            reports / L"cdl" / std::filesystem::path(name);
        std::filesystem::create_directories(destination.parent_path(),
                                            preserve_error);
        std::filesystem::copy_file(
            source, destination,
            std::filesystem::copy_options::overwrite_existing, preserve_error);
        if (preserve_error) return;
        artifacts.push_back(
            {std::string(kind), std::string(format),
             std::filesystem::relative(destination, run->directory)
                 .generic_string(),
             std::filesystem::file_size(destination, preserve_error),
             request.recipe.content_hash, false});
      };
      preserve(staged_odb_path, L"final.odb", "verification.source_odb", "odb");
      preserve(staging_directory / L"source.netlist.v", L"source.netlist.v",
               "verification.source_netlist", "verilog");
      preserve(staged_model_path, L"cell-model.cdl", "verification.cell_model",
               "cdl");
      preserve(design_cdl_path, L"design.cdl", "verification.design_cdl",
               "cdl");
      preserve(combined_cdl_path, L"combined.cdl", "verification.combined_cdl",
               "cdl");
      preserve(preparation_script_path, L"cdl-export.tcl",
               "verification.cdl_script", "tcl");
      preserve(staging_directory / L"lvs-driver.rb", L"lvs-driver.rb",
               "verification.driver", "ruby");
      for (const auto* name :
           {L"rule-original.lylvs", L"rule-effective.lylvs",
            L"rule-contract.json", L"lvs.report.summary",
            L"lvs.report.details.json", L"lvs.report.normalization.json",
            L"lvs.report.layout-before.spice", L"lvs.report.layout-after.spice",
            L"lvs.report.schematic-before.spice",
            L"lvs.report.schematic-after.spice"}) {
        preserve(staging_directory / name, name, "verification.evidence",
                 "text");
      }
      const std::filesystem::path extracted =
          staging_directory / L"extracted.spice";
      if (std::filesystem::is_regular_file(extracted, error) && !error &&
          std::filesystem::file_size(extracted, error) > 0) {
        const std::filesystem::path destination = reports / L"extracted.spice";
        std::filesystem::copy_file(
            extracted, destination,
            std::filesystem::copy_options::overwrite_existing, error);
        if (!error) {
          artifacts.push_back(
              {"verification.extracted_netlist", "spice",
               std::filesystem::relative(destination, run->directory)
                   .generic_string(),
               std::filesystem::file_size(destination, error),
               request.recipe.content_hash, false});
        }
      }
      const std::filesystem::path summary =
          reports / L"physical-verification-summary.json";
      std::ofstream output(summary, std::ios::binary | std::ios::trunc);
      const auto combined_hash =
          std::filesystem::is_regular_file(combined_cdl_path, error)
              ? CalculateFileSha256(combined_cdl_path)
              : core::Result<std::string>(std::string{});
      const std::string detail =
          result.detail.empty() ? status.message : result.detail;
      const std::size_t separator = status.message.find(':');
      const std::string failure_code =
          status.Ok() ? ""
          : separator == std::string::npos
              ? "PHYSICAL-VERIFICATION"
              : status.message.substr(0, separator);
      output << "{\n  \"schema_version\": 3,\n  \"contract_version\": "
             << context.contract_version << ",\n  \"source_run_id\": \""
             << EscapeJson(request.source_run.id) << "\",\n  \"recipe_id\": \""
             << EscapeJson(request.recipe.id) << "\",\n  \"recipe_hash\": \""
             << EscapeJson(request.recipe.content_hash)
             << "\",\n  \"check\": \""
             << (request.check == adapters::VerificationCheck::kDrc ? "drc"
                                                                    : "lvs")
             << "\",\n  \"parsed\": " << (result.parsed ? "true" : "false")
             << ",\n  \"comparison_completed\": "
             << (result.comparison_completed ? "true" : "false")
             << ",\n  \"passed\": " << (result.passed ? "true" : "false")
             << ",\n  \"violation_count\": " << result.violation_count
             << ",\n  \"process_succeeded\": "
             << (process_succeeded ? "true" : "false")
             << ",\n  \"failure_code\": \"" << EscapeJson(failure_code)
             << "\",\n  \"detail\": \"" << EscapeJson(detail)
             << "\",\n  \"cdl_sha256\": \""
             << (combined_hash.Ok() ? combined_hash.Value() : "") << "\"\n}\n";
      output.flush();
      if (output) {
        outcome.summary_relative_path =
            std::filesystem::relative(summary, run->directory).generic_string();
        artifacts.push_back({"verification.summary", "json",
                             outcome.summary_relative_path,
                             std::filesystem::file_size(summary, error),
                             request.recipe.content_hash, false});
      }
      outcome.process_succeeded = process_succeeded;
      outcome.result_succeeded =
          process_succeeded && result.parsed && result.passed && status.Ok();
      const RunStatus run_status =
          status.code == core::ErrorCode::kCancelled ? RunStatus::kCancelled
          : outcome.result_succeeded                 ? RunStatus::kSucceeded
          : process_succeeded && result.parsed && status.Ok()
              ? RunStatus::kViolated
              : RunStatus::kFailed;
      std::vector<core::Diagnostic> diagnostics;
      if (!status.Ok()) {
        core::Diagnostic diagnostic;
        diagnostic.severity = core::DiagnosticSeverity::kError;
        const std::size_t diagnostic_separator = status.message.find(':');
        diagnostic.code = diagnostic_separator == std::string::npos
                              ? "PHYSICAL-VERIFICATION"
                              : status.message.substr(0, diagnostic_separator);
        diagnostic.message = status.message;
        diagnostics.push_back(std::move(diagnostic));
      }
      static_cast<void>(store.Complete(run.get(), run_status, exit_code,
                                       diagnostics, std::move(artifacts),
                                       outcome));
    }
    PhysicalVerificationEvent event;
    event.generation = request.generation;
    event.status = std::move(status);
    event.result = std::move(result);
    event.run = run;
    event.completed = true;
    if (event.status.code == core::ErrorCode::kCancelled) {
      event.state = PhysicalVerificationState::kCancelled;
    } else if (!process_succeeded || !event.status.Ok() ||
               !event.result.parsed) {
      event.state = PhysicalVerificationState::kFailed;
    } else if (!event.result.passed) {
      event.state = PhysicalVerificationState::kViolated;
    } else {
      event.state = PhysicalVerificationState::kSucceeded;
    }
    Emit(std::move(event));
  }

  void Prepare(std::stop_token stop_token) {
    EmitState(PhysicalVerificationState::kPreparing);
    bool cancelled = false;
    {
      std::scoped_lock lock(mutex);
      cancelled = cancellation_requested;
    }
    if (cancelled) {
      Finish({core::ErrorCode::kCancelled, "Verification cancelled", 0}, {}, 0,
             false);
      return;
    }
    if (stop_token.stop_requested()) {
      Finish({core::ErrorCode::kCancelled, "Verification cancelled", 0}, {}, 0,
             false);
      return;
    }
    adapter =
        adapters::CreatePhysicalVerificationAdapter(request.recipe.engine);
    command_input = {};
    preparation_commands.clear();
    preparation_index = 0;
    if (!adapter || adapter->Check() != request.check ||
        !request.recipe.trusted) {
      Finish({core::ErrorCode::kInvalidArgument,
              "A trusted compatible verification recipe is required", 0},
             {}, 0, false);
      return;
    }
    if (request.source_run.id.empty() || request.gds_path.empty()) {
      Finish({core::ErrorCode::kInvalidArgument,
              "Verification requires an immutable source Layout Run", 0},
             {}, 0, false);
      return;
    }
    auto begun = store.Begin(request.cell_directory, request.project,
                             "physical_verification", request.recipe.engine,
                             request.recipe.content_hash);
    if (!begun.Ok()) {
      Finish(begun.GetStatus(), {}, 0, false);
      return;
    }
    run = std::make_shared<RunRecord>(std::move(begun).Value());
    run->source_run_id = request.source_run.id;
    run->environment_id = request.environment_id;
    run->environment_fingerprint = request.environment_fingerprint;
    const auto gds_hash_result = CalculateFileSha256(request.gds_path);
    const auto odb_hash_result = request.odb_path.empty()
                                     ? core::Result<std::string>(std::string{})
                                     : CalculateFileSha256(request.odb_path);
    const auto source_netlist_hash_result =
        request.source_netlist_path.empty()
            ? core::Result<std::string>(std::string{})
            : CalculateFileSha256(request.source_netlist_path);
    const auto schematic_hash_result =
        request.schematic_path.empty()
            ? core::Result<std::string>(std::string{})
            : CalculateFileSha256(request.schematic_path);
    if (!gds_hash_result.Ok() || !odb_hash_result.Ok() ||
        !source_netlist_hash_result.Ok() || !schematic_hash_result.Ok()) {
      const core::Status status =
          !gds_hash_result.Ok()   ? gds_hash_result.GetStatus()
          : !odb_hash_result.Ok() ? odb_hash_result.GetStatus()
          : !source_netlist_hash_result.Ok()
              ? source_netlist_hash_result.GetStatus()
              : schematic_hash_result.GetStatus();
      Finish(status, {}, 0, false);
      return;
    }
    const std::filesystem::path inputs = run->directory / L"inputs";
    std::error_code manifest_error;
    std::filesystem::create_directories(inputs, manifest_error);
    input_manifest_path = inputs / L"verification-inputs.json";
    if (manifest_error) {
      Finish({core::ErrorCode::kIoError,
              "Cannot preserve verification input manifest", 0},
             {}, 0, false);
      return;
    }
    gds_hash = gds_hash_result.Value();
    odb_hash = odb_hash_result.Value();
    source_netlist_hash = source_netlist_hash_result.Value();
    schematic_hash = schematic_hash_result.Value();
    context = request.resolved_context;
    context.contract_version = ResolvedVerificationContext::kContractVersion;
    context.source_run_id = request.source_run.id;
    context.platform = request.recipe.platform;
    context.gds_sha256 = gds_hash;
    context.odb_sha256 = odb_hash;
    context.source_netlist_sha256 = source_netlist_hash;
    context.recipe_sha256 = request.recipe.content_hash;
    context.environment_id = request.environment_id;
    context.environment_fingerprint = request.environment_fingerprint;
    const core::Status manifest_status = WriteInputManifest();
    if (!manifest_status.Ok()) {
      Finish(manifest_status, {}, 0, false);
      return;
    }
    auto staging = CreateAsciiStagingDirectory(NewUuid());
    if (!staging.Ok()) {
      Finish(staging.GetStatus(), {}, 0, false);
      return;
    }
    staging_directory = std::move(staging).Value();
    core::Status copied =
        CopyInput(request.gds_path, staging_directory / L"layout.gds", "GDS");
    if (!copied.Ok()) {
      Finish(std::move(copied), {}, 0, false);
      return;
    }
    if (request.check == adapters::VerificationCheck::kLvs) {
      if (!request.schematic_path.empty()) {
        copied = CopyInput(request.schematic_path,
                           staging_directory / L"schematic.netlist",
                           "schematic netlist");
        if (!copied.Ok()) {
          Finish(std::move(copied), {}, 0, false);
          return;
        }
      }
      if (request.schematic_path.empty() &&
          !request.source_netlist_path.empty()) {
        copied = CopyInput(request.source_netlist_path,
                           staging_directory / L"source.netlist.v",
                           "source netlist");
        if (!copied.Ok()) {
          Finish(std::move(copied), {}, 0, false);
          return;
        }
      }
      if (request.source_netlist_path.empty() &&
          request.schematic_path.empty()) {
        Finish({core::ErrorCode::kNotFound,
                "LVS-CDL-INPUT: same-run source netlist is missing", 0},
               {}, 0, false);
        return;
      }
      if (request.recipe.engine == "klayout_lvs" && request.odb_path.empty()) {
        Finish({core::ErrorCode::kNotFound,
                "LVS-CDL-INPUT: same-run final ODB is missing", 0},
               {}, 0, false);
        return;
      }
      if (!request.extracted_path.empty()) {
        copied = CopyInput(request.extracted_path,
                           staging_directory / L"extracted.spice",
                           "extracted netlist");
        if (!copied.Ok()) {
          Finish(std::move(copied), {}, 0, false);
          return;
        }
      }
      if (request.recipe.engine == "klayout_lvs") {
        copied = CopyInput(request.odb_path, staging_directory / L"final.odb",
                           "final ODB");
        if (!copied.Ok()) {
          Finish({core::ErrorCode::kNotFound,
                  "LVS-CDL-INPUT: same-run final ODB is missing", 0},
                 {}, 0, false);
          return;
        }
      }
    }
    auto lease = resources.AcquireCpu(
        std::max<std::uint32_t>(1, request.project.cpu_budget), stop_token);
    if (!lease.Ok()) {
      Finish(lease.GetStatus(), {}, 0, false);
      return;
    }
    cpu_lease.emplace(std::move(lease).Value());
    auto gds = mapper.WindowsToWsl(staging_directory / L"layout.gds");
    auto schematic =
        mapper.WindowsToWsl(staging_directory / L"schematic.netlist");
    auto extracted =
        mapper.WindowsToWsl(staging_directory / L"extracted.spice");
    staged_odb_path = staging_directory / L"final.odb";
    staged_model_path = staging_directory / L"cell-model.cdl";
    design_cdl_path = staging_directory / L"design.cdl";
    combined_cdl_path = staging_directory / L"combined.cdl";
    preparation_script_path = staging_directory / L"cdl-export.tcl";
    auto odb = mapper.WindowsToWsl(staged_odb_path);
    auto staged_model = mapper.WindowsToWsl(staged_model_path);
    auto design_cdl = mapper.WindowsToWsl(design_cdl_path);
    auto combined_cdl = mapper.WindowsToWsl(combined_cdl_path);
    auto preparation_script = mapper.WindowsToWsl(preparation_script_path);
    report_path =
        staging_directory / (request.check == adapters::VerificationCheck::kDrc
                                 ? L"drc.lyrdb"
                                 : L"lvs.report");
    auto report = mapper.WindowsToWsl(report_path);
    if (!gds.Ok() || !report.Ok() ||
        (request.check == adapters::VerificationCheck::kLvs &&
         ((!request.schematic_path.empty() && !schematic.Ok()) ||
          (request.recipe.engine == "klayout_lvs" &&
           (!odb.Ok() || !staged_model.Ok() || !design_cdl.Ok() ||
            !combined_cdl.Ok() || !preparation_script.Ok())) ||
          (request.recipe.engine == "netgen_lvs" && !extracted.Ok())))) {
      Finish({core::ErrorCode::kIoError,
              "LVS-CDL-INPUT: cannot map verification staging paths to WSL", 0},
             {}, 0, false);
      return;
    }
    command_input.recipe = request.recipe;
    command_input.check = request.check;
    command_input.gds_path = gds.Value();
    command_input.schematic_path = request.recipe.engine == "klayout_lvs"
                                       ? combined_cdl.Value()
                                   : schematic.Ok() ? schematic.Value()
                                                    : L"";
    command_input.extracted_path = extracted.Ok() ? extracted.Value() : L"";
    command_input.report_path = report.Value();
    command_input.odb_path = odb.Ok() ? odb.Value() : L"";
    command_input.staged_model_path =
        staged_model.Ok() ? staged_model.Value() : L"";
    command_input.design_cdl_path = design_cdl.Ok() ? design_cdl.Value() : L"";
    command_input.combined_cdl_path =
        combined_cdl.Ok() ? combined_cdl.Value() : L"";
    command_input.preparation_script_path =
        preparation_script.Ok() ? preparation_script.Value() : L"";
    command_input.toolchain_root = Utf8ToWide(request.profile.orfs_root);
    if (request.recipe.engine == "klayout_lvs") {
      const auto driver_path = staging_directory / L"lvs-driver.rb";
      auto driver = mapper.WindowsToWsl(driver_path);
      const auto driver_status = WriteTextFile(
          driver_path, adapters::BuildKLayoutLvsDriver(), "LVS driver");
      if (!driver.Ok() || !driver_status.Ok()) {
        Finish(driver.Ok() ? driver_status : driver.GetStatus(), {}, 0, false);
        return;
      }
      command_input.verification_script_path = driver.Value();
      if (request.recipe.root.empty() ||
          !IsSafeRelativeLinuxPath(request.recipe.reference_netlist)) {
        Finish({core::ErrorCode::kNotFound,
                "LVS-MODEL-MISSING: sky130hd CDL model path is invalid", 0},
               {}, 0, false);
        return;
      }
      std::string model_path = request.recipe.root;
      if (!model_path.empty() && model_path.back() != '/')
        model_path.push_back('/');
      model_path += request.recipe.reference_netlist;
      command_input.model_path = Utf8ToWide(model_path);
      auto script = adapter->BuildPreparationScript(command_input);
      if (!script.Ok()) {
        Finish(
            {core::ErrorCode::kInvalidArgument,
             "LVS-CDL-EXPORT: adapter cannot build the CDL export script", 0},
            {}, 0, false);
        return;
      }
      const core::Status script_status = WriteTextFile(
          preparation_script_path, script.Value(), "CDL export script");
      if (!script_status.Ok()) {
        Finish({core::ErrorCode::kIoError,
                "LVS-CDL-EXPORT: cannot stage the CDL export script", 0},
               {}, 0, false);
        return;
      }
      auto commands = adapter->BuildPreparationCommands(command_input);
      if (!commands.Ok()) {
        Finish({core::ErrorCode::kInvalidArgument,
                "LVS-CDL-INPUT: adapter cannot prepare CDL inputs", 0},
               {}, 0, false);
        return;
      }
      preparation_commands = std::move(commands).Value();
    }
    command_input.top_cell =
        Utf8ToWide(request.project.physical_verification.top_cell.empty()
                       ? request.project.top_module
                       : request.project.physical_verification.top_cell);
    command_input.distribution = Utf8ToWide(request.profile.wsl_distribution);
    Probe();
  }

  void StartPreparationCommand() {
    if (preparation_index >= preparation_commands.size()) {
      EmitState(PhysicalVerificationState::kCdlCombining);
      Execute();
      return;
    }
    const PhysicalVerificationState next_state =
        preparation_index == 0 ? PhysicalVerificationState::kCdlModelValidating
        : preparation_index == 1 ? PhysicalVerificationState::kCdlGenerating
                                 : PhysicalVerificationState::kCdlCombining;
    EmitState(next_state);
    StartProcess(preparation_commands[preparation_index], false, true);
  }

  void Probe() {
    EmitState(PhysicalVerificationState::kProbing);
    StartProcess(adapter->BuildProbeCommand(command_input), true);
  }

  void Execute() {
    auto command = adapter->BuildCommand(command_input);
    if (!command.Ok()) {
      Finish(command.GetStatus(), {}, 0, false);
      return;
    }
    EmitState(PhysicalVerificationState::kRunning);
    StartProcess(std::move(command).Value(), false);
  }

  void StartProcess(runtime::WslCommand command, bool probing,
                    bool preparation = false) {
    const auto self = shared_from_this();
    std::uint64_t process_id = 0;
    {
      std::scoped_lock lock(mutex);
      process_id = ++process_sequence;
      active_process_id = process_id;
    }
    auto started = provider->Start(
        command,
        [self](std::string output) { self->EmitOutput(std::move(output)); },
        [self, process_id, probing,
         preparation](runtime::ProcessResult result) {
          std::shared_ptr<runtime::ExecutionHandle> finished;
          {
            std::scoped_lock lock(self->mutex);
            if (self->terminal || self->shutdown ||
                self->active_process_id != process_id) {
              return;
            }
            self->active_process_id = 0;
            finished = std::move(self->handle);
          }
          self->QueueHandleCleanup(std::move(finished));
          if (preparation) {
            if (!result.started || result.cancelled || result.exit_code != 0) {
              self->Finish(
                  result.cancelled
                      ? core::Status{core::ErrorCode::kCancelled,
                                     "Verification cancelled", 0}
                      : core::Status{core::ErrorCode::kIoError,
                                     self->preparation_index == 0
                                         ? "LVS-MODEL-MISSING: cannot stage "
                                           "the sky130hd CDL model"
                                     : self->preparation_index == 1
                                         ? "LVS-CDL-EXPORT: OpenROAD could not "
                                           "export CDL from the final ODB"
                                         : "LVS-CDL-PARSE: generated CDL model "
                                           "combination failed",
                                     result.exit_code},
                  {}, result.exit_code, false);
              return;
            }
            if (self->preparation_index == 1) {
              const std::string top_cell =
                  self->request.project.physical_verification.top_cell.empty()
                      ? self->request.project.top_module
                      : self->request.project.physical_verification.top_cell;
              const core::Status cdl_status = ValidateGeneratedCdl(
                  self->design_cdl_path, self->staged_model_path, top_cell);
              if (!cdl_status.Ok()) {
                self->Finish(cdl_status, {}, result.exit_code, false);
                return;
              }
            }
            if (self->preparation_index == 2) {
              std::ifstream input(self->combined_cdl_path, std::ios::binary);
              const std::string contents(
                  (std::istreambuf_iterator<char>(input)),
                  std::istreambuf_iterator<char>());
              input.close();
              auto normalized = adapters::NormalizeCdlForSpice(contents);
              if (!normalized.Ok()) {
                self->Finish(normalized.GetStatus(), {}, result.exit_code,
                             false);
                return;
              }
              const auto write_status =
                  WriteTextFile(self->combined_cdl_path, normalized.Value(),
                                "SPICE-compatible CDL");
              if (!write_status.Ok()) {
                self->Finish(write_status, {}, result.exit_code, false);
                return;
              }
              const std::string top_cell =
                  self->request.project.physical_verification.top_cell.empty()
                      ? self->request.project.top_module
                      : self->request.project.physical_verification.top_cell;
              const core::Status combined_status =
                  ValidateCombinedCdl(self->combined_cdl_path, top_cell);
              if (!combined_status.Ok()) {
                self->Finish(combined_status, {}, result.exit_code, false);
                return;
              }
            }
            const auto model_hash =
                CalculateFileSha256(self->staged_model_path);
            const auto design_hash = CalculateFileSha256(self->design_cdl_path);
            const auto combined_hash =
                CalculateFileSha256(self->combined_cdl_path);
            const std::string model_value =
                model_hash.Ok() ? model_hash.Value() : std::string{};
            const std::string design_value =
                design_hash.Ok() ? design_hash.Value() : std::string{};
            const std::string combined_value =
                combined_hash.Ok() ? combined_hash.Value() : std::string{};
            if (self->preparation_index == 0 && !model_hash.Ok()) {
              self->Finish(
                  {core::ErrorCode::kNotFound,
                   "LVS-MODEL-MISSING: staged sky130hd CDL model is missing",
                   0},
                  {}, result.exit_code, false);
              return;
            }
            const core::Status manifest_status =
                self->preparation_index == 0
                    ? self->WriteInputManifest(model_value)
                : self->preparation_index == 1
                    ? self->WriteInputManifest(model_value, design_value)
                : self->preparation_index == 2
                    ? self->WriteInputManifest(model_value, design_value,
                                               combined_value)
                    : core::Status::Success();
            if ((self->preparation_index == 0 && !model_hash.Ok()) ||
                (self->preparation_index == 1 && !design_hash.Ok()) ||
                (self->preparation_index == 2 && !combined_hash.Ok()) ||
                !manifest_status.Ok()) {
              self->Finish(
                  {core::ErrorCode::kCorruptData,
                   self->preparation_index == 0
                       ? "LVS-MODEL-MISSING: staged sky130hd CDL model "
                         "cannot be recorded"
                   : self->preparation_index == 1
                       ? "LVS-CDL-PARSE: generated design CDL cannot be "
                         "recorded"
                       : "LVS-CDL-PARSE: combined CDL cannot be recorded",
                   0},
                  {}, result.exit_code, false);
              return;
            }
            ++self->preparation_index;
            self->StartPreparationCommand();
            return;
          }
          if (!result.started || result.cancelled || result.exit_code != 0) {
            self->Finish(
                result.cancelled
                    ? core::Status{core::ErrorCode::kCancelled,
                                   "Verification cancelled", 0}
                    : core::
                          Status{core::ErrorCode::kIoError,
                                 probing
                                     ? self->request.check ==
                                               adapters::VerificationCheck::kLvs
                                           ? "LVS-TOOL-PROBE: compatible "
                                             "KLayout/OpenROAD environment "
                                             "is unavailable"
                                           : "DRC-TOOL-PROBE: compatible "
                                             "verification environment is "
                                             "unavailable"
                                     : "Verification tool execution failed",
                                 0},
                {}, result.exit_code, false);
            return;
          }
          if (probing) {
            if (!self->preparation_commands.empty()) {
              self->preparation_index = 0;
              self->StartPreparationCommand();
              return;
            }
            self->Execute();
            return;
          }
          self->EmitState(PhysicalVerificationState::kCollecting);
          std::ifstream input(self->report_path, std::ios::binary);
          const bool report_exists = static_cast<bool>(input);
          const std::string report((std::istreambuf_iterator<char>(input)),
                                   std::istreambuf_iterator<char>());
          std::string output;
          {
            std::scoped_lock lock(self->mutex);
            output = self->process_output;
          }
          adapters::VerificationResultArtifacts artifacts;
          artifacts.report = report;
          artifacts.process_output = std::move(output);
          artifacts.report_format = self->request.recipe.output_format;
          artifacts.report_exists = report_exists;
          std::ifstream comparison(self->report_path.wstring() + L".summary",
                                   std::ios::binary);
          artifacts.comparison_summary.assign(
              std::istreambuf_iterator<char>(comparison),
              std::istreambuf_iterator<char>());
          auto parsed = self->adapter->ParseResult(artifacts);
          if (!parsed.Ok()) {
            self->Finish(parsed.GetStatus(), {}, result.exit_code, true);
          } else {
            self->Finish(core::Status::Success(), std::move(parsed).Value(),
                         result.exit_code, true);
          }
        });
    if (!started.Ok()) {
      if (preparation) {
        Finish({core::ErrorCode::kIoError,
                preparation_index == 0
                    ? "LVS-MODEL-MISSING: cannot start CDL model staging"
                : preparation_index == 1
                    ? "LVS-CDL-EXPORT: cannot start OpenROAD CDL export"
                    : "LVS-CDL-PARSE: cannot start CDL model combination",
                started.status.native_error},
               {}, 0, false);
      } else {
        Finish(started.status, {}, 0, false);
      }
      return;
    }
    std::shared_ptr<runtime::ExecutionHandle> new_handle(
        std::move(started.handle));
    std::shared_ptr<runtime::ExecutionHandle> current_handle;
    bool cancel = false;
    bool retire = false;
    {
      std::scoped_lock lock(mutex);
      if (terminal || shutdown || active_process_id != process_id) {
        cancel = terminal || shutdown;
        retire = true;
      } else {
        handle = std::move(new_handle);
        cancel = cancellation_requested;
        current_handle = handle;
      }
    }
    if (cancel && new_handle) new_handle->Cancel();
    if (cancel && current_handle) current_handle->Cancel();
    if (retire) QueueHandleCleanup(std::move(new_handle));
  }

  runtime::ExecutionProvider* provider = nullptr;
  mutable std::mutex mutex;
  runtime::TaskScheduler scheduler{1};
  runtime::TaskScheduler cleanup{1};
  runtime::ResourceCoordinator resources;
  runtime::PathMapper mapper;
  RunStore store;
  PhysicalVerificationRequest request;
  PhysicalVerificationEventSink sink;
  PhysicalVerificationState state = PhysicalVerificationState::kIdle;
  std::unique_ptr<adapters::PhysicalVerificationAdapter> adapter;
  adapters::VerificationCommandInput command_input;
  std::shared_ptr<RunRecord> run;
  std::shared_ptr<runtime::ExecutionHandle> handle;
  std::uint64_t process_sequence = 0;
  std::uint64_t active_process_id = 0;
  std::vector<std::shared_ptr<runtime::ExecutionHandle>>
      deferred_handle_cleanup;
  std::optional<runtime::CpuTokenLease> cpu_lease;
  std::filesystem::path staging_directory;
  std::filesystem::path report_path;
  std::filesystem::path input_manifest_path;
  std::filesystem::path staged_odb_path;
  std::filesystem::path staged_model_path;
  std::filesystem::path design_cdl_path;
  std::filesystem::path combined_cdl_path;
  std::filesystem::path preparation_script_path;
  std::vector<runtime::WslCommand> preparation_commands;
  ResolvedVerificationContext context;
  std::string process_output;
  std::string gds_hash;
  std::string odb_hash;
  std::string source_netlist_hash;
  std::string schematic_hash;
  std::size_t preparation_index = 0;
  bool active = false;
  bool terminal = false;
  bool shutdown = false;
  bool cancellation_requested = false;
};

std::vector<core::VerificationRecipe> ResolveManagedVerificationRecipes(
    const core::ToolchainProfile& profile, std::string_view backend,
    std::string_view platform) {
  std::vector<core::VerificationRecipe> recipes;
  if (backend != "orfs" || profile.orfs_root.empty() || platform.empty()) {
    return recipes;
  }
  const std::string platform_name(platform);
  if (platform_name == "asap7" || platform_name == "sky130hd") {
    core::VerificationRecipe drc;
    drc.id = "builtin.orfs." + platform_name + ".klayout-drc";
    drc.name = "ORFS " + platform_name + " KLayout DRC";
    drc.engine = "klayout_drc";
    drc.provider_id = "orfs";
    drc.platform = platform_name;
    drc.root = profile.orfs_root + "/flow/platforms/" + platform_name;
    drc.entrypoint = "drc/" + platform_name + ".lydrc";
    drc.output_format = "lyrdb";
    drc.content_hash = "managed-orfs-recipe-v1:" + platform_name + ":drc";
    drc.trusted = true;
    drc.managed = true;
    recipes.push_back(std::move(drc));
  }
  if (platform_name == "sky130hd") {
    core::VerificationRecipe lvs;
    lvs.id = "builtin.orfs.sky130hd.klayout-lvs";
    lvs.name = "ORFS sky130hd KLayout LVS";
    lvs.engine = "klayout_lvs";
    lvs.provider_id = "orfs";
    lvs.platform = platform_name;
    lvs.root = profile.orfs_root + "/flow/platforms/sky130hd";
    lvs.entrypoint = "lvs/sky130hd.lylvs";
    lvs.reference_netlist = "cdl/sky130hd.cdl";
    lvs.output_format = "lvsdb";
    lvs.content_hash = "managed-orfs-recipe-v1:sky130hd:lvs";
    lvs.trusted = true;
    lvs.managed = true;
    recipes.push_back(std::move(lvs));
  }
  return recipes;
}

VerificationCapability ResolveVerificationCapability(
    const core::ToolchainProfile& profile, std::string_view backend,
    std::string_view platform, bool platform_ready, bool drc_files_ready,
    bool lvs_files_ready) {
  if (backend != "orfs") {
    return {false, false, "Flow results only", "Flow results only"};
  }
  if (!platform_ready) {
    return {false, false, "Platform unavailable", "Platform unavailable"};
  }
  const auto recipes =
      ResolveManagedVerificationRecipes(profile, backend, platform);
  const bool has_drc_recipe = std::any_of(
      recipes.begin(), recipes.end(),
      [](const auto& recipe) { return recipe.engine == "klayout_drc"; });
  const bool has_lvs_recipe = std::any_of(
      recipes.begin(), recipes.end(),
      [](const auto& recipe) { return recipe.engine == "klayout_lvs"; });
  VerificationCapability result;
  result.drc_ready = drc_files_ready && has_drc_recipe;
  result.lvs_ready = lvs_files_ready && has_lvs_recipe;
  result.drc_status = result.drc_ready  ? "Ready"
                      : !has_drc_recipe ? "No managed recipe"
                                        : "Required rule files missing";
  result.lvs_status = result.lvs_ready  ? "Ready"
                      : !has_lvs_recipe ? "No managed recipe"
                                        : "Required model/rule files missing";
  return result;
}

PhysicalVerificationService::PhysicalVerificationService(
    runtime::ExecutionProvider* provider)
    : implementation_(std::make_shared<Implementation>(provider)) {}

PhysicalVerificationService::~PhysicalVerificationService() { Shutdown(); }

core::Status PhysicalVerificationService::Start(
    PhysicalVerificationRequest request, PhysicalVerificationEventSink sink) {
  const auto implementation = implementation_;
  if (!implementation || implementation->provider == nullptr || !sink ||
      request.generation == 0) {
    return {core::ErrorCode::kInvalidArgument,
            "Physical verification request identity is incomplete", 0};
  }
  {
    std::scoped_lock lock(implementation->mutex);
    if (implementation->shutdown || implementation->active) {
      return {core::ErrorCode::kConflict,
              "Physical verification is already active", 0};
    }
    implementation->request = std::move(request);
    implementation->sink = std::move(sink);
    implementation->state = PhysicalVerificationState::kIdle;
    implementation->active = true;
    implementation->terminal = false;
    implementation->cancellation_requested = false;
    implementation->active_process_id = 0;
  }
  if (!implementation->scheduler.Submit(
          [implementation](std::stop_token token) {
            implementation->Prepare(token);
          })) {
    std::scoped_lock lock(implementation->mutex);
    implementation->active = false;
    return {core::ErrorCode::kConflict,
            "Physical verification queue is unavailable", 0};
  }
  return core::Status::Success();
}

void PhysicalVerificationService::Cancel() noexcept {
  const auto implementation = implementation_;
  if (!implementation) return;
  std::shared_ptr<runtime::ExecutionHandle> handle;
  {
    std::scoped_lock lock(implementation->mutex);
    if (!implementation->active || implementation->terminal) return;
    implementation->cancellation_requested = true;
    handle = implementation->handle;
  }
  if (handle) handle->Cancel();
}

void PhysicalVerificationService::Shutdown() noexcept {
  const auto implementation = implementation_;
  if (!implementation) return;
  std::shared_ptr<runtime::ExecutionHandle> handle;
  std::vector<std::shared_ptr<runtime::ExecutionHandle>> deferred_handles;
  {
    std::scoped_lock lock(implementation->mutex);
    if (implementation->shutdown) return;
    implementation->shutdown = true;
    implementation->active = false;
    implementation->active_process_id = 0;
    implementation->sink = {};
    handle = std::move(implementation->handle);
    deferred_handles.swap(implementation->deferred_handle_cleanup);
  }
  if (handle) handle->Cancel();
  implementation->scheduler.RequestStop();
  implementation->cleanup.RequestStop();
  deferred_handles.clear();
}

bool PhysicalVerificationService::IsActive() const {
  const auto implementation = implementation_;
  if (!implementation) return false;
  std::scoped_lock lock(implementation->mutex);
  return implementation->active;
}

}  // namespace designpp::application

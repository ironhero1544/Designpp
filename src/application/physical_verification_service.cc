// Copyright 2026 The Design++ Authors

#include "designpp/application/physical_verification_service.h"

#include <objbase.h>
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <utility>

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
                           std::string_view contents,
                           std::string_view name) {
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
  if (path.empty() || path.front() == '/' || path.find('\\') !=
                                       std::string_view::npos) {
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
  if (design.find(".subckt") == std::string::npos &&
      design.find(".SUBCKT") == std::string::npos) {
    return {core::ErrorCode::kCorruptData,
            "LVS-CDL-PARSE: OpenROAD output has no subcircuit", 0};
  }
  if (model.find(".subckt") == std::string::npos &&
      model.find(".SUBCKT") == std::string::npos) {
    return {core::ErrorCode::kCorruptData,
            "LVS-MODEL-MISSING: platform CDL has no cell definitions", 0};
  }
  if (!top_cell.empty()) {
    const std::string lower_design = [&design] {
      std::string result = design;
      std::transform(result.begin(), result.end(), result.begin(),
                     [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                     });
      return result;
    }();
    std::string lower_top(top_cell);
    std::transform(lower_top.begin(), lower_top.end(), lower_top.begin(),
                   [](unsigned char character) {
                     return static_cast<char>(std::tolower(character));
                   });
    if (lower_design.find(".subckt " + lower_top) == std::string::npos) {
      return {core::ErrorCode::kCorruptData,
              "LVS-CDL-PARSE: generated CDL top cell is missing", 0};
    }
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
    PhysicalVerificationEvent event;
    event.generation = request.generation;
    event.state = state;
    event.output = std::move(output);
    Emit(std::move(event));
  }

  void Finish(core::Status status, adapters::VerificationResult result,
              std::uint32_t exit_code, bool process_succeeded) {
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
            preserve_error || std::filesystem::file_size(source, preserve_error) ==
                                  0) {
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
      preserve(staged_odb_path, L"final.odb", "verification.source_odb",
               "odb");
      preserve(staging_directory / L"source.netlist.v", L"source.netlist.v",
               "verification.source_netlist", "verilog");
      preserve(staged_model_path, L"cell-model.cdl",
               "verification.cell_model", "cdl");
      preserve(design_cdl_path, L"design.cdl", "verification.design_cdl",
               "cdl");
      preserve(combined_cdl_path, L"combined.cdl",
               "verification.combined_cdl", "cdl");
      preserve(preparation_script_path, L"cdl-export.tcl",
               "verification.cdl_script", "tcl");
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
      output << "{\n  \"schema_version\": 2,\n  \"source_run_id\": \""
             << EscapeJson(request.source_run.id) << "\",\n  \"recipe_id\": \""
             << EscapeJson(request.recipe.id) << "\",\n  \"recipe_hash\": \""
             << EscapeJson(request.recipe.content_hash)
             << "\",\n  \"check\": \""
             << (request.check == adapters::VerificationCheck::kDrc ? "drc"
                                                                    : "lvs")
             << "\",\n  \"parsed\": " << (result.parsed ? "true" : "false")
             << ",\n  \"passed\": " << (result.passed ? "true" : "false")
             << ",\n  \"violation_count\": " << result.violation_count
             << ",\n  \"detail\": \"" << EscapeJson(result.detail)
             << "\",\n  \"cdl_sha256\": \""
             << (combined_hash.Ok() ? combined_hash.Value() : "")
             << "\"\n}\n";
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
                                                     : RunStatus::kFailed;
      std::vector<core::Diagnostic> diagnostics;
      if (!status.Ok()) {
        core::Diagnostic diagnostic;
        diagnostic.severity = core::DiagnosticSeverity::kError;
        const std::size_t separator = status.message.find(':');
        diagnostic.code = separator == std::string::npos
                              ? "PHYSICAL-VERIFICATION"
                              : status.message.substr(0, separator);
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
    const auto gds_hash = CalculateFileSha256(request.gds_path);
    const auto odb_hash =
        request.odb_path.empty()
            ? core::Result<std::string>(std::string{})
            : CalculateFileSha256(request.odb_path);
    const auto source_netlist_hash =
        request.source_netlist_path.empty()
            ? core::Result<std::string>(std::string{})
            : CalculateFileSha256(request.source_netlist_path);
    const auto schematic_hash =
        request.schematic_path.empty()
            ? core::Result<std::string>(std::string{})
            : CalculateFileSha256(request.schematic_path);
    if (!gds_hash.Ok() || !odb_hash.Ok() || !source_netlist_hash.Ok() ||
        !schematic_hash.Ok()) {
      const core::Status status =
          !gds_hash.Ok()           ? gds_hash.GetStatus()
          : !odb_hash.Ok()         ? odb_hash.GetStatus()
          : !source_netlist_hash.Ok() ? source_netlist_hash.GetStatus()
                                      : schematic_hash.GetStatus();
      Finish(status, {}, 0, false);
      return;
    }
    const std::filesystem::path inputs = run->directory / L"inputs";
    std::error_code manifest_error;
    std::filesystem::create_directories(inputs, manifest_error);
    input_manifest_path = inputs / L"verification-inputs.json";
    std::ofstream manifest(input_manifest_path,
                           std::ios::binary | std::ios::trunc);
    manifest << "{\n  \"schema_version\": 2,\n  \"source_run_id\": \""
             << EscapeJson(request.source_run.id) << "\",\n  \"gds_sha256\": \""
             << gds_hash.Value() << "\",\n  \"schematic_sha256\": \""
             << schematic_hash.Value() << "\",\n  \"recipe_id\": \""
             << EscapeJson(request.recipe.id) << "\",\n  \"recipe_hash\": \""
             << EscapeJson(request.recipe.content_hash)
             << "\",\n  \"environment_id\": \""
             << EscapeJson(request.environment_id)
             << "\",\n  \"environment_fingerprint\": \""
             << EscapeJson(request.environment_fingerprint)
             << "\",\n  \"odb_sha256\": \"" << odb_hash.Value()
             << "\",\n  \"source_netlist_sha256\": \""
             << source_netlist_hash.Value()
             << "\",\n  \"preparation_contract\": \"orfs-sky130hd-cdl-v1\"\n}\n";
    manifest.flush();
    if (manifest_error || !manifest) {
      Finish({core::ErrorCode::kIoError,
              "Cannot preserve verification input manifest", 0},
             {}, 0, false);
      return;
    }
    staging_directory = std::filesystem::temp_directory_path() /
                        L"DesignPlusPlus" / L"verification" / NewUuid();
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
      if (request.schematic_path.empty() && !request.source_netlist_path.empty()) {
        copied = CopyInput(request.source_netlist_path,
                           staging_directory / L"source.netlist.v",
                           "source netlist");
        if (!copied.Ok()) {
          Finish(std::move(copied), {}, 0, false);
          return;
        }
      }
      if (request.source_netlist_path.empty() && request.schematic_path.empty()) {
        Finish({core::ErrorCode::kNotFound,
                "LVS-CDL-INPUT: same-run source netlist is missing", 0}, {},
               0, false);
        return;
      }
      if (request.recipe.engine == "klayout_lvs" && request.odb_path.empty()) {
        Finish({core::ErrorCode::kNotFound,
                "LVS-CDL-INPUT: same-run final ODB is missing", 0}, {}, 0,
               false);
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
                  "LVS-CDL-INPUT: same-run final ODB is missing", 0}, {}, 0,
                 false);
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
              "LVS-CDL-INPUT: cannot map verification staging paths to WSL",
              0},
             {}, 0, false);
      return;
    }
    command_input.recipe = request.recipe;
    command_input.check = request.check;
    command_input.gds_path = gds.Value();
    command_input.schematic_path =
        request.recipe.engine == "klayout_lvs"
            ? combined_cdl.Value()
            : schematic.Ok() ? schematic.Value() : L"";
    command_input.extracted_path = extracted.Ok() ? extracted.Value() : L"";
    command_input.report_path = report.Value();
    command_input.odb_path = odb.Ok() ? odb.Value() : L"";
    command_input.staged_model_path =
        staged_model.Ok() ? staged_model.Value() : L"";
    command_input.design_cdl_path =
        design_cdl.Ok() ? design_cdl.Value() : L"";
    command_input.combined_cdl_path =
        combined_cdl.Ok() ? combined_cdl.Value() : L"";
    command_input.preparation_script_path =
        preparation_script.Ok() ? preparation_script.Value() : L"";
    if (request.recipe.engine == "klayout_lvs") {
      if (request.recipe.root.empty() ||
          !IsSafeRelativeLinuxPath(request.recipe.reference_netlist)) {
        Finish({core::ErrorCode::kNotFound,
                "LVS-MODEL-MISSING: sky130hd CDL model path is invalid", 0},
               {}, 0, false);
        return;
      }
      std::string model_path = request.recipe.root;
      if (!model_path.empty() && model_path.back() != '/') model_path.push_back('/');
      model_path += request.recipe.reference_netlist;
      command_input.model_path = Utf8ToWide(model_path);
      auto script = adapter->BuildPreparationScript(command_input);
      if (!script.Ok()) {
        Finish({core::ErrorCode::kInvalidArgument,
                "LVS-CDL-EXPORT: adapter cannot build the CDL export script",
                0},
               {}, 0, false);
        return;
      }
      const core::Status script_status =
          WriteTextFile(preparation_script_path, script.Value(),
                        "CDL export script");
      if (!script_status.Ok()) {
        Finish({core::ErrorCode::kIoError,
                "LVS-CDL-EXPORT: cannot stage the CDL export script", 0},
               {}, 0, false);
        return;
      }
      auto commands = adapter->BuildPreparationCommands(command_input);
      if (!commands.Ok()) {
        Finish({core::ErrorCode::kInvalidArgument,
                "LVS-CDL-INPUT: adapter cannot prepare CDL inputs", 0}, {},
               0, false);
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
    EmitState(preparation_index == 1
                  ? PhysicalVerificationState::kCdlGenerating
                  : PhysicalVerificationState::kCdlCombining);
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
    auto started = provider->Start(
        command,
        [self](std::string output) { self->EmitOutput(std::move(output)); },
        [self, probing, preparation](runtime::ProcessResult result) {
          std::shared_ptr<runtime::ExecutionHandle> finished;
          {
            std::scoped_lock lock(self->mutex);
            if (self->terminal) return;
            finished = std::move(self->handle);
          }
          self->QueueHandleCleanup(std::move(finished));
          if (preparation) {
            if (!result.started || result.cancelled || result.exit_code != 0) {
              self->Finish(
                  result.cancelled
                      ? core::Status{core::ErrorCode::kCancelled,
                                     "Verification cancelled", 0}
                      : core::Status{
                            core::ErrorCode::kIoError,
                            self->preparation_index == 0
                                ? "LVS-MODEL-MISSING: cannot stage the sky130hd CDL model"
                                : self->preparation_index == 1
                                    ? "LVS-CDL-EXPORT: OpenROAD could not export CDL from the final ODB"
                                    : "LVS-CDL-PARSE: generated CDL model combination failed",
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
            ++self->preparation_index;
            self->StartPreparationCommand();
            return;
          }
          if (!result.started || result.cancelled || result.exit_code != 0) {
            self->Finish(
                result.cancelled
                    ? core::Status{core::ErrorCode::kCancelled,
                                   "Verification cancelled", 0}
                    : core::Status{core::ErrorCode::kIoError,
                                   probing
                                       ? "Verification tool probe failed"
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
          const std::string report((std::istreambuf_iterator<char>(input)),
                                   std::istreambuf_iterator<char>());
          auto parsed = self->adapter->ParseReport(report);
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
    std::scoped_lock lock(mutex);
    if (terminal || shutdown) {
      new_handle->Cancel();
    } else {
      handle = std::move(new_handle);
    }
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
  std::size_t preparation_index = 0;
  bool active = false;
  bool terminal = false;
  bool shutdown = false;
  bool cancellation_requested = false;
};

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
  {
    std::scoped_lock lock(implementation->mutex);
    if (implementation->shutdown) return;
    implementation->shutdown = true;
    implementation->active = false;
    implementation->sink = {};
    handle = std::move(implementation->handle);
  }
  if (handle) handle->Cancel();
  implementation->scheduler.RequestStop();
  implementation->cleanup.RequestStop();
}

bool PhysicalVerificationService::IsActive() const {
  const auto implementation = implementation_;
  if (!implementation) return false;
  std::scoped_lock lock(implementation->mutex);
  return implementation->active;
}

}  // namespace designpp::application

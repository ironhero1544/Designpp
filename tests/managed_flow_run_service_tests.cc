// Copyright 2026 The Design++ Authors

#include <CppUnitTest.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <vector>

#include "designpp/application/managed_flow_run_service.h"
#include "designpp/core/toolchain_compatibility.h"
#include "synthesis_test_support.h"

using Microsoft::VisualStudio::CppUnitTestFramework::Assert;

namespace designpp::tests {
namespace {

class ManagedFlowCollector final {
 public:
  void Add(application::ManagedFlowRunEvent event) {
    {
      std::scoped_lock lock(mutex_);
      events_.push_back(std::move(event));
    }
    changed_.notify_all();
  }

  [[nodiscard]] bool WaitForTerminal() {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, std::chrono::seconds(8),
                             [&] { return TerminalCountLocked() != 0; });
  }

  [[nodiscard]] std::size_t TerminalCount() const {
    std::scoped_lock lock(mutex_);
    return TerminalCountLocked();
  }

  [[nodiscard]] application::ManagedFlowRunEvent Terminal() const {
    std::scoped_lock lock(mutex_);
    const auto found =
        std::find_if(events_.begin(), events_.end(), [](const auto& event) {
          return event.kind == application::ManagedFlowRunEventKind::kCompleted;
        });
    return found == events_.end() ? application::ManagedFlowRunEvent{} : *found;
  }

 private:
  [[nodiscard]] std::size_t TerminalCountLocked() const {
    return static_cast<std::size_t>(
        std::count_if(events_.begin(), events_.end(), [](const auto& event) {
          return event.kind == application::ManagedFlowRunEventKind::kCompleted;
        }));
  }

  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::vector<application::ManagedFlowRunEvent> events_;
};

application::ManagedFlowRunRequest MakeRequest(
    const TemporarySynthesisWorkspace& workspace) {
  const application::SynthesisRunRequest synthesis = workspace.Request();
  application::ManagedFlowRunRequest request;
  request.project = synthesis.project;
  request.project.id = "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa";
  request.project.library_id = "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb";
  request.project.cell_id = "cccccccc-cccc-cccc-cccc-cccccccccccc";
  request.project.created_utc = "2026-08-24T00:00:00Z";
  request.project.modified_utc = request.project.created_utc;
  request.project.physical_implementation.clock_ports = {"clk"};
  request.profile.id = "dddddddd-dddd-dddd-dddd-dddddddddddd";
  request.profile.name = "OpenLane";
  request.profile.openlane_root = "/home/test/openlane2";
  request.profile.pdk_root = "/home/test/.volare";
  request.profile.cpu_budget = 1;
  request.sources = synthesis.sources;
  request.library_directory = synthesis.library_directory;
  request.cell_directory = synthesis.cell_directory;
  request.generation = synthesis.generation;
  return request;
}

std::filesystem::path WslToWindows(std::wstring_view path) {
  if (path.size() < 7 || !path.starts_with(L"/mnt/") || path[6] != L'/') {
    return {};
  }
  std::wstring windows{static_cast<wchar_t>(std::towupper(path[5])), L':'};
  windows.append(path.substr(6));
  std::replace(windows.begin(), windows.end(), L'/', L'\\');
  return windows;
}

void WriteCollectedArtifacts(const runtime::WslCommand& copy_command,
                             bool violations = false) {
  const std::filesystem::path root =
      WslToWindows(copy_command.arguments.back());
  std::filesystem::create_directories(root / L"final");
  const auto write = [&root](const wchar_t* path, std::string_view contents) {
    const std::filesystem::path destination = root / path;
    std::filesystem::create_directories(destination.parent_path());
    std::ofstream(destination, std::ios::binary) << contents;
  };
  write(L"resolved.json", "{}\n");
  write(L"state_out.json", "{}\n");
  write(L"metrics.json", violations ? "{\"timing__setup__wns\":-0.1,"
                                      "\"magic__drc_error__count\":0}"
                                    : "{\"timing__setup__wns\":0.1,"
                                      "\"timing__setup__tns\":0.0,"
                                      "\"timing__hold__wns\":0.0,"
                                      "\"timing__hold__tns\":0.0,"
                                      "\"route__antenna_violation__count\":0,"
                                      "\"magic__drc_error__count\":0,"
                                      "\"klayout__xor_error__count\":0,"
                                      "\"design__lvs_error__count\":0}");
  write(L"metrics.csv", "metric,value\n");
  write(L"final/top.gds", "gds");
  write(L"final/top.def", "def");
  write(L"final/top.lef", "lef");
  write(L"final/top.odb", "odb");
  write(L"final/top.v", "module top; endmodule\n");
  write(L"final/top.sdf", "sdf");
  write(L"final/top.spef", "spef");
  write(L"54-magic-drc/reports/drc.rpt", "Magic DRC clean\n");
  write(L"56-netgen-lvs/reports/lvs.rpt", "Netgen LVS clean\n");
}

runtime::ProcessResult Success(std::string output = {}) {
  // Successful fake probes supply the same complete evidence as real adapters.
  const std::string provider = output == "OpenLane v2.3.10\n" ? "openlane2"
                               : output.starts_with("DESIGNPP_ORFS_TOOL_MODE=")
                                   ? "orfs"
                                   : "";
  for (const auto& entry : core::ToolchainCompatibilityCatalog::Entries()) {
    if (entry.provider_id != provider) continue;
    output +=
        "DESIGNPP_COMPAT_SCHEMA=1\nDESIGNPP_COMPAT_PROVIDER=" + provider +
        "\nDESIGNPP_COMPAT_BUNDLE=" + std::string(entry.bundle_id) +
        "\nDESIGNPP_COMPAT_REVISION=" + std::string(entry.revision) +
        "\nDESIGNPP_COMPAT_CONTRACT=" + std::string(entry.command_contract_id) +
        "\nDESIGNPP_COMPAT_LOCK=" + std::string(64, 'a') +
        "\nDESIGNPP_COMPAT_FINGERPRINT=" + std::string(64, 'b') + "\n";
    for (const auto& dependency : entry.dependencies) {
      output += "DESIGNPP_COMPAT_DEPENDENCY_" + std::string(dependency.name) +
                "=" + std::string(dependency.revision) + "\n";
    }
    for (const auto feature : entry.required_features) {
      output += "DESIGNPP_COMPAT_FEATURE_" + std::string(feature) + "=1\n";
    }
  }
  runtime::ProcessResult result;
  result.started = true;
  result.exit_code = 0;
  result.output = std::move(output);
  return result;
}

}  // namespace

TEST_CLASS(ManagedFlowRunServiceTests){
  public : TEST_METHOD(CleanFlowPreservesArtifactsAndCompletesExactlyOnce){
      TemporarySynthesisWorkspace workspace;
ControlledExecutionProvider provider;
application::ManagedFlowRunService service(&provider);
ManagedFlowCollector collector;
application::ManagedFlowRunRequest request = MakeRequest(workspace);
request.project.physical_implementation.io_placement.north.entries = {
    "data\\[\\d+\\]", "$2"};
request.project.physical_implementation.io_placement.north.bit_major = true;
Assert::IsTrue(service
                   .Start(std::move(request),
                          [&collector](auto event) {
                            collector.Add(std::move(event));
                          })
                   .Ok());
Assert::IsTrue(provider.WaitForStarts(1));
provider.Complete(0, Success("OpenLane v2.3.10\n"));
Assert::IsTrue(provider.WaitForStarts(2));
const runtime::WslCommand validation_command = provider.Command(1);
Assert::IsTrue(validation_command.arguments.size() > 7);
const std::filesystem::path staging =
    WslToWindows(validation_command.arguments[7]);
std::ifstream pin_order_stream(staging / L"constraints" / L"pin_order.cfg",
                               std::ios::binary);
const std::string pin_order((std::istreambuf_iterator<char>(pin_order_stream)),
                            std::istreambuf_iterator<char>());
Assert::AreEqual(std::string("#N\n@bit_major\ndata\\[\\d+\\]\n$2\n\n"),
                 pin_order);
std::ifstream config_stream(staging / L"config.json", std::ios::binary);
const std::string config((std::istreambuf_iterator<char>(config_stream)),
                         std::istreambuf_iterator<char>());
Assert::IsTrue(config.find("\"FP_PIN_ORDER_CFG\": "
                           "\"dir::constraints/pin_order.cfg\"") !=
               std::string::npos);
provider.Complete(1, Success("Starting step Verilator.Lint\n"));
Assert::IsFalse(provider.DestroyedDuringCompletionCallback(1));
Assert::IsTrue(provider.WaitForStarts(3));
provider.Complete(2, Success("Starting step KLayout.StreamOut\n"), 2);
Assert::IsTrue(provider.WaitForStarts(4));
WriteCollectedArtifacts(provider.Command(3));
provider.Complete(3, Success(), 2);
Assert::IsTrue(collector.WaitForTerminal());
Assert::AreEqual(static_cast<std::size_t>(1), collector.TerminalCount());
const auto terminal = collector.Terminal();
Assert::IsTrue(terminal.status.Ok());
Assert::IsNotNull(terminal.run.get());
Assert::IsTrue(terminal.run->outcome.process_succeeded);
Assert::IsTrue(terminal.run->outcome.result_succeeded);
Assert::IsTrue(std::filesystem::is_regular_file(terminal.run->directory /
                                                L"artifacts" / L"final.gds"));
Assert::IsTrue(std::filesystem::is_regular_file(terminal.run->directory /
                                                L"reports" / L"openlane" /
                                                L"54-magic-drc" / L"reports" /
                                                L"drc.rpt"));
Assert::IsTrue(std::filesystem::is_regular_file(terminal.run->directory /
                                                L"reports" / L"openlane" /
                                                L"56-netgen-lvs" / L"reports" /
                                                L"lvs.rpt"));
Assert::AreEqual(static_cast<std::size_t>(2),
                 static_cast<std::size_t>(std::count_if(
                     terminal.run->artifacts.begin(),
                     terminal.run->artifacts.end(),
                     [](const application::RunArtifact& artifact) {
                       return artifact.kind == "report" &&
                              artifact.format == "rpt";
                     })));
}  // namespace designpp::tests

TEST_METHOD(PhysicalViolationSeparatesProcessAndResultFailure) {
  TemporarySynthesisWorkspace workspace;
  ControlledExecutionProvider provider;
  application::ManagedFlowRunService service(&provider);
  ManagedFlowCollector collector;
  Assert::IsTrue(
      service
          .Start(MakeRequest(workspace),
                 [&collector](auto event) { collector.Add(std::move(event)); })
          .Ok());
  Assert::IsTrue(provider.WaitForStarts(1));
  provider.Complete(0, Success("OpenLane v2.3.10\n"));
  Assert::IsTrue(provider.WaitForStarts(2));
  provider.Complete(1, Success());
  Assert::IsTrue(provider.WaitForStarts(3));
  provider.Complete(2, Success());
  Assert::IsTrue(provider.WaitForStarts(4));
  WriteCollectedArtifacts(provider.Command(3), true);
  provider.Complete(3, Success());
  Assert::IsTrue(collector.WaitForTerminal());
  const auto terminal = collector.Terminal();
  Assert::IsFalse(terminal.status.Ok());
  Assert::IsTrue(terminal.run->outcome.process_succeeded);
  Assert::IsFalse(terminal.run->outcome.result_succeeded);
}

TEST_METHOD(NonzeroFlowPreservesPhysicalVerificationReportsAsPartial) {
  TemporarySynthesisWorkspace workspace;
  ControlledExecutionProvider provider;
  application::ManagedFlowRunService service(&provider);
  ManagedFlowCollector collector;
  Assert::IsTrue(
      service
          .Start(MakeRequest(workspace),
                 [&collector](auto event) { collector.Add(std::move(event)); })
          .Ok());
  Assert::IsTrue(provider.WaitForStarts(1));
  provider.Complete(0, Success("OpenLane v2.3.10\n"));
  Assert::IsTrue(provider.WaitForStarts(2));
  provider.Complete(1, Success());
  Assert::IsTrue(provider.WaitForStarts(3));
  runtime::ProcessResult failed = Success("Starting step Magic.DRC\n");
  failed.exit_code = 7;
  provider.Complete(2, failed);
  Assert::IsTrue(provider.WaitForStarts(4));
  WriteCollectedArtifacts(provider.Command(3));
  provider.Complete(3, Success());
  Assert::IsTrue(collector.WaitForTerminal());
  const auto terminal = collector.Terminal();
  Assert::IsFalse(terminal.run->outcome.process_succeeded);
  const auto report = std::find_if(
      terminal.run->artifacts.begin(), terminal.run->artifacts.end(),
      [](const application::RunArtifact& artifact) {
        return artifact.kind == "report" && artifact.partial &&
               artifact.relative_path.ends_with("drc.rpt");
      });
  Assert::IsTrue(report != terminal.run->artifacts.end());
}

TEST_METHOD(CancelAndDuplicateCallbackDeliverTerminalExactlyOnce) {
  TemporarySynthesisWorkspace workspace;
  ControlledExecutionProvider provider;
  application::ManagedFlowRunService service(&provider);
  ManagedFlowCollector collector;
  Assert::IsTrue(
      service
          .Start(MakeRequest(workspace),
                 [&collector](auto event) { collector.Add(std::move(event)); })
          .Ok());
  Assert::IsTrue(provider.WaitForStarts(1));
  service.Cancel();
  runtime::ProcessResult cancelled;
  cancelled.started = true;
  cancelled.cancelled = true;
  provider.Complete(0, cancelled, 2);
  Assert::IsTrue(collector.WaitForTerminal());
  Assert::AreEqual(static_cast<std::size_t>(1), collector.TerminalCount());
  Assert::IsTrue(collector.Terminal().state ==
                 application::ManagedFlowRunState::kCancelled);
}

TEST_METHOD(ProbeStartFailureCompletesExactlyOnce) {
  TemporarySynthesisWorkspace workspace;
  ControlledExecutionProvider provider;
  provider.FailNextStart();
  application::ManagedFlowRunService service(&provider);
  ManagedFlowCollector collector;
  Assert::IsTrue(
      service
          .Start(MakeRequest(workspace),
                 [&collector](auto event) { collector.Add(std::move(event)); })
          .Ok());
  Assert::IsTrue(collector.WaitForTerminal());
  Assert::AreEqual<std::size_t>(1U, collector.TerminalCount());
  Assert::IsFalse(collector.Terminal().status.Ok());
}

TEST_METHOD(ProbeNonzeroAndDuplicateCompletionAreTerminalOnce) {
  TemporarySynthesisWorkspace workspace;
  ControlledExecutionProvider provider;
  application::ManagedFlowRunService service(&provider);
  ManagedFlowCollector collector;
  Assert::IsTrue(
      service
          .Start(MakeRequest(workspace),
                 [&collector](auto event) { collector.Add(std::move(event)); })
          .Ok());
  Assert::IsTrue(provider.WaitForStarts(1));
  runtime::ProcessResult failure = Success("OpenLane v2.2.0\n");
  failure.exit_code = 3;
  provider.Complete(0, failure, 2);
  Assert::IsTrue(collector.WaitForTerminal());
  Assert::AreEqual<std::size_t>(1U, collector.TerminalCount());
}

TEST_METHOD(MissingFinalArtifactsFailWithoutLosingProcessSuccess) {
  TemporarySynthesisWorkspace workspace;
  ControlledExecutionProvider provider;
  application::ManagedFlowRunService service(&provider);
  ManagedFlowCollector collector;
  Assert::IsTrue(
      service
          .Start(MakeRequest(workspace),
                 [&collector](auto event) { collector.Add(std::move(event)); })
          .Ok());
  Assert::IsTrue(provider.WaitForStarts(1));
  provider.Complete(0, Success("OpenLane v2.3.10\n"));
  Assert::IsTrue(provider.WaitForStarts(2));
  provider.Complete(1, Success());
  Assert::IsTrue(provider.WaitForStarts(3));
  provider.Complete(2, Success());
  Assert::IsTrue(provider.WaitForStarts(4));
  provider.Complete(3, Success());
  Assert::IsTrue(collector.WaitForTerminal());
  const auto terminal = collector.Terminal();
  Assert::IsFalse(terminal.status.Ok());
  Assert::IsTrue(terminal.run->outcome.process_succeeded);
  Assert::IsFalse(terminal.run->outcome.result_succeeded);
}

TEST_METHOD(ChangedFingerprintRejectsResumeBeforeExecution) {
  TemporarySynthesisWorkspace workspace;
  ControlledExecutionProvider provider;
  application::ManagedFlowRunService service(&provider);
  ManagedFlowCollector collector;
  application::ManagedFlowRunRequest request = MakeRequest(workspace);
  request.resume = application::ManagedFlowResumeRequest{
      "parent", "lineage", "OpenROAD.GeneratePDN", "stale-fingerprint",
      std::string(64, 'a')};
  Assert::IsTrue(
      service
          .Start(std::move(request),
                 [&collector](auto event) { collector.Add(std::move(event)); })
          .Ok());
  Assert::IsTrue(provider.WaitForStarts(1));
  provider.Complete(0, Success("OpenLane v2.3.10\n"));
  Assert::IsTrue(collector.WaitForTerminal());
  Assert::IsFalse(collector.Terminal().status.Ok());
  Assert::AreEqual<std::size_t>(1U, collector.TerminalCount());
}

TEST_METHOD(ChangedOrfsFingerprintStartsFreshLineage) {
  TemporarySynthesisWorkspace workspace;
  ControlledExecutionProvider provider;
  application::ManagedFlowRunService service(&provider);
  ManagedFlowCollector collector;
  application::ManagedFlowRunRequest request = MakeRequest(workspace);
  request.project.physical_implementation.backend_id = "orfs";
  request.project.physical_implementation.orfs.platform = "asap7";
  request.profile.orfs_root = "/home/test/orfs";
  request.full_flow = true;
  request.resume = application::ManagedFlowResumeRequest{
      "parent", "lineage", "route", "stale-fingerprint", std::string(64, 'a')};
  Assert::IsTrue(
      service
          .Start(std::move(request),
                 [&collector](auto event) { collector.Add(std::move(event)); })
          .Ok());
  Assert::IsTrue(provider.WaitForStarts(1));
  provider.Complete(0, Success("DESIGNPP_ORFS_TOOL_MODE=orfs-flake\n"
                               "DESIGNPP_ORFS_READY\n"
                               "036d106273e66855cd5214d49518fd0f0df7de61\n"));
  Assert::IsTrue(provider.WaitForStarts(2));
  const runtime::WslCommand validation = provider.Command(1);
  Assert::AreEqual(std::wstring(L"/bin/bash"), validation.program);
  Assert::AreEqual(std::wstring(L"all"), validation.arguments[7]);
  Assert::IsTrue(validation.arguments[9].empty());
  Assert::IsTrue(validation.arguments[10].empty());
  provider.Complete(1, Success());
  Assert::IsTrue(provider.WaitForStarts(3));
  runtime::ProcessResult failed = Success();
  failed.exit_code = 7;
  provider.Complete(2, failed);
  Assert::IsTrue(provider.WaitForStarts(4));
  provider.Complete(3, Success(), 2);
  Assert::IsTrue(collector.WaitForTerminal());
  Assert::AreEqual(static_cast<std::size_t>(1), collector.TerminalCount());
  Assert::AreNotEqual(std::string("lineage"), collector.Terminal().lineage_id);
}

TEST_METHOD(OrfsSuccessfulProcessWithoutTimeUnitMarkerFailsContract) {
  TemporarySynthesisWorkspace workspace;
  ControlledExecutionProvider provider;
  application::ManagedFlowRunService service(&provider);
  ManagedFlowCollector collector;
  application::ManagedFlowRunRequest request = MakeRequest(workspace);
  request.project.physical_implementation.backend_id = "orfs";
  request.project.physical_implementation.orfs.platform = "asap7";
  request.profile.orfs_root = "/home/test/orfs";
  request.full_flow = true;

  Assert::IsTrue(
      service
          .Start(std::move(request),
                 [&collector](auto event) { collector.Add(std::move(event)); })
          .Ok());
  Assert::IsTrue(provider.WaitForStarts(1));
  provider.Complete(0, Success("DESIGNPP_ORFS_TOOL_MODE=orfs-flake\n"
                               "DESIGNPP_ORFS_READY\n"
                               "036d106273e66855cd5214d49518fd0f0df7de61\n"));
  Assert::IsTrue(provider.WaitForStarts(2));
  provider.Complete(1, Success());
  Assert::IsTrue(provider.WaitForStarts(3));
  provider.Complete(2, Success());
  Assert::IsTrue(provider.WaitForStarts(4));
  provider.Complete(3, Success());
  Assert::IsTrue(collector.WaitForTerminal());

  const application::ManagedFlowRunEvent terminal = collector.Terminal();
  Assert::IsFalse(terminal.status.Ok());
  Assert::AreEqual(std::string("timing_unit_validation"),
                   terminal.failure_stage);
  Assert::AreEqual(std::string("ORFS-SDC-UNIT"), terminal.failure_code);
  Assert::IsTrue(std::any_of(terminal.diagnostics.begin(),
                             terminal.diagnostics.end(),
                             [](const core::Diagnostic& diagnostic) {
                               return diagnostic.code == "ORFS-SDC-UNIT";
                             }));
}

TEST_METHOD(ChangedOrfsFingerprintRejectsExplicitRebuildAndPersistsReason) {
  TemporarySynthesisWorkspace workspace;
  ControlledExecutionProvider provider;
  application::ManagedFlowRunService service(&provider);
  ManagedFlowCollector collector;
  application::ManagedFlowRunRequest request = MakeRequest(workspace);
  request.project.physical_implementation.backend_id = "orfs";
  request.project.physical_implementation.orfs.platform = "asap7";
  request.profile.orfs_root = "/home/test/orfs";
  request.rebuild_from_stage = core::StageId::kRouting;
  request.resume = application::ManagedFlowResumeRequest{
      "parent", "lineage", "route", "stale-fingerprint", std::string(64, 'a')};
  Assert::IsTrue(
      service
          .Start(std::move(request),
                 [&collector](auto event) { collector.Add(std::move(event)); })
          .Ok());
  Assert::IsTrue(provider.WaitForStarts(1));
  provider.Complete(0, Success("DESIGNPP_ORFS_TOOL_MODE=orfs-flake\n"
                               "DESIGNPP_ORFS_READY\n"
                               "036d106273e66855cd5214d49518fd0f0df7de61\n"));
  Assert::IsTrue(collector.WaitForTerminal());
  const application::ManagedFlowRunEvent terminal = collector.Terminal();
  Assert::AreEqual(static_cast<std::size_t>(1), collector.TerminalCount());
  Assert::IsFalse(terminal.status.Ok());
  Assert::IsNotNull(terminal.run.get());
  Assert::AreEqual(std::string("input_preparation"), terminal.failure_stage);
  Assert::AreEqual(std::string("ORFS-PREPARE"), terminal.failure_code);
  Assert::AreEqual(static_cast<std::size_t>(1), terminal.diagnostics.size());
  Assert::AreEqual(std::string("ORFS-PREPARE"),
                   terminal.diagnostics.front().code);
  Assert::AreEqual(std::string("reports/orfs-summary.json"),
                   terminal.run->outcome.summary_relative_path);

  const auto read_file = [](const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream),
                       std::istreambuf_iterator<char>());
  };
  const std::string summary = read_file(
      terminal.run->directory / terminal.run->outcome.summary_relative_path);
  Assert::IsTrue(summary.find("\"failure_stage\":\"input_preparation\"") !=
                 std::string::npos);
  Assert::IsTrue(summary.find("ORFS rebuild fingerprint no longer matches") !=
                 std::string::npos);
  const std::string diagnostics =
      read_file(terminal.run->directory / "diagnostics.jsonl");
  Assert::IsTrue(diagnostics.find("ORFS-PREPARE") != std::string::npos);
  Assert::IsTrue(diagnostics.find("fingerprint no longer matches") !=
                 std::string::npos);
}
}
;

}  // namespace designpp::tests

// Copyright 2026 The Design++ Authors

#include <CppUnitTest.h>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "designpp/application/layout_viewer_service.h"
#include "designpp/application/physical_implementation_service.h"
#include "designpp/application/prepared_physical_inputs.h"
#include "synthesis_test_support.h"

using Microsoft::VisualStudio::CppUnitTestFramework::Assert;

namespace designpp::tests {
namespace {

runtime::ProcessResult Success(std::string output = {}) {
  runtime::ProcessResult result;
  result.started = true;
  result.exit_code = 0;
  result.output = std::move(output);
  return result;
}

class LayoutCollector final {
 public:
  void Add(application::PhysicalImplementationEvent event) {
    {
      std::scoped_lock lock(mutex_);
      events_.push_back(std::move(event));
    }
    changed_.notify_all();
  }

  bool Wait() {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, std::chrono::seconds(5), [&] {
      return !events_.empty() &&
             events_.back().kind ==
                 application::PhysicalImplementationEventKind::kCompleted;
    });
  }

  application::PhysicalImplementationEvent Last() const {
    std::scoped_lock lock(mutex_);
    return events_.empty() ? application::PhysicalImplementationEvent{}
                           : events_.back();
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::vector<application::PhysicalImplementationEvent> events_;
};

class ViewerCollector final {
 public:
  void Add(application::LayoutViewerEvent event) {
    {
      std::scoped_lock lock(mutex_);
      if (event.completed) {
        completed_ = event;
        ++completion_count_;
      }
    }
    changed_.notify_all();
  }

  bool Wait() {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, std::chrono::seconds(5),
                             [&] { return completion_count_ != 0; });
  }

  application::LayoutViewerEvent Completed() const {
    std::scoped_lock lock(mutex_);
    return completed_;
  }

  std::size_t Count() const {
    std::scoped_lock lock(mutex_);
    return completion_count_;
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  application::LayoutViewerEvent completed_;
  std::size_t completion_count_ = 0;
};

application::ManagedFlowRunRequest MakeRequest(
    const TemporarySynthesisWorkspace& workspace) {
  const application::SynthesisRunRequest synthesis = workspace.Request();
  application::ManagedFlowRunRequest request;
  request.project = synthesis.project;
  request.project.physical_implementation.clock_ports = {"clk"};
  request.profile.id = "profile";
  request.profile.name = "Profile";
  request.profile.openlane_root = "/home/test/openlane2";
  request.profile.pdk_root = "/home/test/pdk";
  request.profile.cpu_budget = 1;
  request.sources = synthesis.sources;
  request.library_directory = synthesis.library_directory;
  request.cell_directory = synthesis.cell_directory;
  request.generation = synthesis.generation;
  return request;
}

}  // namespace

// clang-format off
TEST_CLASS(PhysicalImplementationServiceTests) {
 public:
  TEST_METHOD(CompatibleGdsIsReusedWithoutStartingBackend) {
    TemporarySynthesisWorkspace workspace;
    application::ManagedFlowRunRequest request = MakeRequest(workspace);
    application::RunStore store;
    auto begun = store.Begin(request.cell_directory, request.project,
                             "physical_implementation", "openlane2", "2.3.10");
    Assert::IsTrue(begun.Ok());
    application::RunRecord run = std::move(begun).Value();
    std::filesystem::create_directories(run.directory / L"artifacts");
    std::filesystem::create_directories(run.directory / L"reports");
    std::ofstream(run.directory / L"artifacts" / L"final.gds",
                  std::ios::binary) << "gds";
    std::ofstream(run.directory / L"reports" / L"openlane-summary.json",
                  std::ios::binary)
        << "{\"project_revision\":" << request.project.revision
        << ",\"backend_id\":\"openlane2\","
           "\"configuration_fingerprint\":\"fingerprint\"}";
    application::RunArtifact gds{"final.gds", "gds", "artifacts/final.gds",
                                 3, "fingerprint", false};
    application::RunArtifact summary{
        "summary", "json", "reports/openlane-summary.json", 1,
        "fingerprint", false};
    application::RunOutcome outcome{true, true,
                                    "reports/openlane-summary.json"};
    Assert::IsTrue(store.Complete(&run, application::RunStatus::kSucceeded, 0,
                                  {}, {gds, summary}, outcome).Ok());

    ControlledExecutionProvider provider;
    application::PhysicalImplementationService service(&provider);
    LayoutCollector collector;
    Assert::IsTrue(service.EnsureLayout(
        request, [&collector](auto event) { collector.Add(std::move(event)); }).Ok());
    Assert::IsTrue(collector.Wait());
    Assert::IsTrue(collector.Last().reused);
    Assert::IsFalse(collector.Last().gds_path.empty());
    Assert::AreEqual(static_cast<std::size_t>(0), provider.StartCount());
  }

  TEST_METHOD(InterruptedCheckpointRemainsEligibleForResume) {
    TemporarySynthesisWorkspace workspace;
    application::ManagedFlowRunRequest request = MakeRequest(workspace);
    application::RunRecord run;
    run.id = "interrupted-parent";
    run.project_id = request.project.id;
    run.stage = "physical_implementation";
    run.status = application::RunStatus::kInterrupted;
    run.directory = request.cell_directory / L"interrupted-run";
    run.outcome.summary_relative_path =
        "reports/openlane-summary.json";
    std::filesystem::create_directories(run.directory / L"reports");
    std::filesystem::create_directories(run.directory / L"artifacts");
    std::ofstream(run.directory / L"reports" / L"openlane-summary.json",
                  std::ios::binary)
        << "{\"project_revision\":" << request.project.revision
        << ",\"backend_id\":\"openlane2\"," 
           "\"configuration_fingerprint\":\"fingerprint\"," 
           "\"lineage_id\":\"lineage\"," 
           "\"last_step\":\"OpenROAD.GeneratePDN\"}";
    std::ofstream(run.directory / L"artifacts" / L"checkpoint-state.json",
                  std::ios::binary)
        << "checkpoint";

    const auto resume =
        application::BuildPhysicalImplementationResume(run, request);
    Assert::IsTrue(resume.has_value());
    Assert::AreEqual(std::string("interrupted-parent"),
                     resume->parent_run_id);
    Assert::AreEqual(std::string("OpenROAD.GeneratePDN"),
                     resume->resume_step);
    Assert::AreEqual(static_cast<std::size_t>(64),
                     resume->checkpoint_hash.size());
  }

  TEST_METHOD(PreparedOrfsInputsUseManagedSdcClockInsteadOfSetupDefault) {
    TemporarySynthesisWorkspace workspace;
    application::ManagedFlowRunRequest request = MakeRequest(workspace);
    request.project.physical_implementation.backend_id = "orfs";
    request.project.physical_implementation.orfs.platform = "asap7";
    request.project.physical_implementation.clock_period_ns = "10.0";
    request.project.physical_implementation.pnr_sdc_path = "clock.sdc";
    std::ofstream(request.library_directory / L"clock.sdc", std::ios::binary)
        << "create_clock -name slow -period 40.0 [get_ports slow_clk]\n"
           "create_clock -name fast -period 20.0 [get_ports clk]\n";

    auto prepared = application::PrepareOrfsPhysicalInputs(request, "ORFS 26Q2");
    Assert::IsTrue(prepared.Ok());
    Assert::IsTrue(prepared.Value()->effective_clock_period_ns.has_value());
    Assert::AreEqual(20.0, *prepared.Value()->effective_clock_period_ns);
    Assert::AreEqual(std::string("20"),
                     prepared.Value()->effective_clock_period_text);
    Assert::AreEqual(static_cast<std::size_t>(2),
                     prepared.Value()->clocks.size());
    Assert::AreEqual(std::string("slow"), prepared.Value()->clocks[0].name);
    Assert::AreEqual(std::string("slow_clk"),
                     prepared.Value()->clocks[0].target_port);
    Assert::AreEqual(40.0, prepared.Value()->clocks[0].period_ns);
    Assert::AreEqual(std::string("fast"), prepared.Value()->clocks[1].name);
    Assert::AreEqual(std::string("clk"),
                     prepared.Value()->clocks[1].target_port);
  }

  TEST_METHOD(PreparedOrfsInputsHonorExplicitPicosecondSdcUnits) {
    TemporarySynthesisWorkspace workspace;
    application::ManagedFlowRunRequest request = MakeRequest(workspace);
    request.project.physical_implementation.backend_id = "orfs";
    request.project.physical_implementation.orfs.platform = "asap7";
    request.project.physical_implementation.pnr_sdc_path = "clock.sdc";
    std::ofstream(request.library_directory / L"clock.sdc", std::ios::binary)
        << "set_cmd_units -time ps\n"
           "create_clock -period 20000.0 [get_ports clk]\n";

    auto prepared = application::PrepareOrfsPhysicalInputs(request, "ORFS 26Q2");
    Assert::IsTrue(prepared.Ok());
    Assert::AreEqual(std::string("ps"), prepared.Value()->sdc_time_unit);
    Assert::AreEqual(20.0, *prepared.Value()->effective_clock_period_ns);
  }

  TEST_METHOD(OrfsFingerprintUsesContentsAndIgnoresOpenLaneOnlySettings) {
    TemporarySynthesisWorkspace workspace;
    application::ManagedFlowRunRequest request = MakeRequest(workspace);
    request.project.physical_implementation.backend_id = "orfs";
    request.project.physical_implementation.orfs.platform = "asap7";
    request.profile.orfs_mode = "managed";
    request.profile.orfs_bundle_id = "orfs-26q2";
    auto first = application::PrepareOrfsPhysicalInputs(request, "ORFS 26Q2");
    Assert::IsTrue(first.Ok());

    request.project.physical_implementation.pdk = "unused_pdk";
    request.project.physical_implementation.standard_cell_library =
        "unused_scl";
    request.project.physical_implementation.power_distribution.core_ring =
        true;
    auto inactive_change =
        application::PrepareOrfsPhysicalInputs(request, "ORFS 26Q2");
    Assert::IsTrue(inactive_change.Ok());
    Assert::AreEqual(first.Value()->fingerprint,
                     inactive_change.Value()->fingerprint);

    ++request.project.revision;
    request.project.modified_utc = "2099-01-01T00:00:00Z";
    auto metadata_change =
        application::PrepareOrfsPhysicalInputs(request, "ORFS 26Q2");
    Assert::IsTrue(metadata_change.Ok());
    Assert::AreEqual(first.Value()->fingerprint,
                     metadata_change.Value()->fingerprint);

    request.profile.orfs_bundle_id = "orfs-other";
    auto environment_change =
        application::PrepareOrfsPhysicalInputs(request, "ORFS 26Q2");
    Assert::IsTrue(environment_change.Ok());
    Assert::AreNotEqual(first.Value()->fingerprint,
                        environment_change.Value()->fingerprint);
    request.profile.orfs_bundle_id = "orfs-26q2";

    request.project.physical_implementation.die_area = {"0", "0", "20",
                                                        "20"};
    request.project.physical_implementation.core_area = {"2", "2", "18",
                                                         "18"};
    auto fixed_area =
        application::PrepareOrfsPhysicalInputs(request, "ORFS 26Q2");
    Assert::IsTrue(fixed_area.Ok());
    request.project.physical_implementation.core_utilization_percent = 75;
    auto unused_utilization =
        application::PrepareOrfsPhysicalInputs(request, "ORFS 26Q2");
    Assert::IsTrue(unused_utilization.Ok());
    Assert::AreEqual(fixed_area.Value()->fingerprint,
                     unused_utilization.Value()->fingerprint);

    const std::filesystem::path rtl = request.sources.front().windows_path;
    const auto timestamp = std::filesystem::last_write_time(rtl);
    std::ofstream(rtl, std::ios::binary | std::ios::app) << "\n// changed";
    std::filesystem::last_write_time(rtl, timestamp);
    auto content_change =
        application::PrepareOrfsPhysicalInputs(request, "ORFS 26Q2");
    Assert::IsTrue(content_change.Ok());
    Assert::AreNotEqual(first.Value()->fingerprint,
                        content_change.Value()->fingerprint);
  }

  TEST_METHOD(KLayoutUsesProbeThenAsciiSafeStagedGds) {
    TemporarySynthesisWorkspace workspace;
    const std::filesystem::path gds =
        workspace.Request().library_directory / L"한글 layout.gds";
    std::ofstream(gds, std::ios::binary) << "gds";
    const std::filesystem::path markers =
        workspace.Request().library_directory / L"검증 markers.lyrdb";
    std::ofstream(markers, std::ios::binary) << "<report-database/>";
    ControlledExecutionProvider provider;
    application::LayoutViewerService service(&provider);
    ViewerCollector collector;
    application::LayoutViewerRequest request;
    request.profile.wsl_distribution = "Ubuntu";
    request.gds_path = gds;
    request.marker_database_path = markers;
    request.generation = 7;
    Assert::IsTrue(service.Open(
        request, [&collector](auto event) { collector.Add(std::move(event)); }).Ok());
    Assert::IsTrue(provider.WaitForStarts(1));
    Assert::AreEqual(std::wstring(L"klayout"), provider.Command(0).program);
    provider.Complete(0, Success());
    Assert::IsTrue(provider.WaitForStarts(2));
    Assert::AreEqual(std::wstring(L"/usr/bin/stat"),
                      provider.Command(1).program);
    provider.Complete(1, Success("tmpfs\n"));
    Assert::IsTrue(provider.WaitForStarts(3));
    Assert::AreEqual(std::wstring(L"/usr/bin/touch"),
                     provider.Command(2).program);
    provider.Complete(2, Success());
    Assert::IsTrue(provider.WaitForStarts(4));
    Assert::AreEqual(std::wstring(L"/usr/bin/rm"), provider.Command(3).program);
    provider.Complete(3, Success());
    Assert::IsTrue(provider.WaitForStarts(5));
    const runtime::WslCommand launch = provider.Command(4);
    Assert::AreEqual(std::wstring(L"klayout"), launch.program);
    Assert::IsTrue(launch.arguments[0].ends_with(L"/layout.gds"));
    Assert::AreEqual(std::wstring(L"-m"), launch.arguments[1]);
    Assert::IsTrue(launch.arguments[2].ends_with(L"markers.lyrdb"));
    provider.Complete(4, Success(), 2);
    Assert::IsTrue(collector.Wait());
    Assert::AreEqual(static_cast<std::size_t>(1), collector.Count());
    Assert::IsTrue(collector.Completed().status.Ok());
  }
};
// clang-format on

}  // namespace designpp::tests

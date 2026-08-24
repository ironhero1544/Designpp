// Copyright 2026 The Design++ Authors

#include <CppUnitTest.h>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

#include "designpp/application/layout_viewer_service.h"
#include "designpp/application/physical_implementation_service.h"
#include "synthesis_test_support.h"

using Microsoft::VisualStudio::CppUnitTestFramework::Assert;

namespace designpp::tests {
namespace {

runtime::ProcessResult Success() {
  runtime::ProcessResult result;
  result.started = true;
  result.exit_code = 0;
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

  TEST_METHOD(KLayoutUsesProbeThenAsciiSafeStagedGds) {
    TemporarySynthesisWorkspace workspace;
    const std::filesystem::path gds =
        workspace.Request().library_directory / L"한글 layout.gds";
    std::ofstream(gds, std::ios::binary) << "gds";
    ControlledExecutionProvider provider;
    application::LayoutViewerService service(&provider);
    ViewerCollector collector;
    application::LayoutViewerRequest request;
    request.profile.wsl_distribution = "Ubuntu";
    request.gds_path = gds;
    request.generation = 7;
    Assert::IsTrue(service.Open(
        request, [&collector](auto event) { collector.Add(std::move(event)); }).Ok());
    Assert::IsTrue(provider.WaitForStarts(1));
    Assert::AreEqual(std::wstring(L"klayout"), provider.Command(0).program);
    provider.Complete(0, Success());
    Assert::IsTrue(provider.WaitForStarts(2));
    Assert::AreEqual(std::wstring(L"/usr/bin/touch"),
                     provider.Command(1).program);
    provider.Complete(1, Success());
    Assert::IsTrue(provider.WaitForStarts(3));
    Assert::AreEqual(std::wstring(L"/usr/bin/rm"),
                     provider.Command(2).program);
    provider.Complete(2, Success());
    Assert::IsTrue(provider.WaitForStarts(4));
    const runtime::WslCommand launch = provider.Command(3);
    Assert::AreEqual(std::wstring(L"klayout"), launch.program);
    Assert::IsTrue(launch.arguments[0].ends_with(L"/layout.gds"));
    provider.Complete(3, Success(), 2);
    Assert::IsTrue(collector.Wait());
    Assert::AreEqual(static_cast<std::size_t>(1), collector.Count());
    Assert::IsTrue(collector.Completed().status.Ok());
  }
};
// clang-format on

}  // namespace designpp::tests

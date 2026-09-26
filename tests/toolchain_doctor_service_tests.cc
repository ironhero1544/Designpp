// Copyright 2026 The Design++ Authors

#include <CppUnitTest.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "designpp/application/toolchain_doctor_service.h"
#include "designpp/application/toolchain_profile_store.h"
#include "synthesis_test_support.h"

using Microsoft::VisualStudio::CppUnitTestFramework::Assert;

namespace designpp::tests {
namespace {

class DoctorEventCollector final {
 public:
  void Add(application::DoctorEvent event) {
    {
      std::scoped_lock lock(mutex_);
      events_.push_back(std::move(event));
    }
    changed_.notify_all();
  }

  [[nodiscard]] bool WaitForTerminal() {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, std::chrono::seconds(5), [this] {
      return std::any_of(events_.begin(), events_.end(), [](const auto& event) {
        return event.kind == application::DoctorEventKind::kCompleted;
      });
    });
  }

  [[nodiscard]] std::size_t TerminalCount() const {
    std::scoped_lock lock(mutex_);
    return static_cast<std::size_t>(
        std::count_if(events_.begin(), events_.end(), [](const auto& event) {
          return event.kind == application::DoctorEventKind::kCompleted;
        }));
  }

  [[nodiscard]] application::DoctorEvent Terminal() const {
    std::scoped_lock lock(mutex_);
    const auto found =
        std::find_if(events_.begin(), events_.end(), [](const auto& event) {
          return event.kind == application::DoctorEventKind::kCompleted;
        });
    return found == events_.end() ? application::DoctorEvent{} : *found;
  }

  [[nodiscard]] application::DoctorCheckResult Check(
      application::DoctorCheckId id) const {
    std::scoped_lock lock(mutex_);
    const auto found =
        std::find_if(events_.begin(), events_.end(), [id](const auto& event) {
          return event.kind == application::DoctorEventKind::kCheckCompleted &&
                 event.check.id == id;
        });
    return found == events_.end() ? application::DoctorCheckResult{}
                                  : found->check;
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::vector<application::DoctorEvent> events_;
};

core::ToolchainProfile DoctorProfile() {
  core::ToolchainProfile profile =
      application::ToolchainProfileStore::CreateDefaults().profiles.front();
  profile.wsl_distribution = "Ubuntu";
  profile.pdk_root = "/opt/pdk/sky130";
  return profile;
}

runtime::ProcessResult SuccessfulDoctorResult(std::string output = {}) {
  runtime::ProcessResult result;
  result.started = true;
  result.exit_code = 0;
  result.output = std::move(output);
  return result;
}

runtime::ProcessResult SuccessfulPdkResult(std::string_view source = "ORFS") {
  return SuccessfulDoctorResult("DESIGNPP_PDK_SOURCE=" + std::string(source) +
                                "\n");
}

}  // namespace

TEST_CLASS(ToolchainDoctorServiceTests){
  public : TEST_METHOD(AllChecksPassAndDuplicateCompletionIsIgnored){
      ControlledExecutionProvider provider;
application::ToolchainDoctorService service(&provider);
DoctorEventCollector collector;
Assert::IsTrue(service
                   .Start(DoctorProfile(), 7,
                          [&collector](application::DoctorEvent event) {
                            collector.Add(std::move(event));
                          })
                   .Ok());
Assert::IsTrue(provider.WaitForStarts(1));
Assert::IsTrue(provider.Command(0).distribution.has_value());
provider.Complete(0, SuccessfulDoctorResult("5.15-microsoft-standard-WSL2\n"));
Assert::IsTrue(provider.WaitForStarts(2));
provider.Complete(1, SuccessfulDoctorResult());
Assert::IsTrue(provider.WaitForStarts(3));
provider.Complete(2, SuccessfulDoctorResult());
Assert::IsTrue(provider.WaitForStarts(4));
provider.Complete(3, SuccessfulPdkResult(), 2);
Assert::IsTrue(collector.WaitForTerminal());
Assert::IsTrue(collector.Terminal().status.Ok());
Assert::AreEqual<std::size_t>(1U, collector.TerminalCount());
Assert::IsTrue(collector.Check(application::DoctorCheckId::kPdk).passed);
}  // namespace designpp::tests

TEST_METHOD(WslOneKernelFailsDiagnosisButOtherChecksStillRun) {
  ControlledExecutionProvider provider;
  application::ToolchainDoctorService service(&provider);
  DoctorEventCollector collector;
  Assert::IsTrue(service
                     .Start(DoctorProfile(), 8,
                            [&collector](application::DoctorEvent event) {
                              collector.Add(std::move(event));
                            })
                     .Ok());
  Assert::IsTrue(provider.WaitForStarts(1));
  provider.Complete(0, SuccessfulDoctorResult("4.4.0-microsoft\n"));
  for (std::size_t index = 1; index < 4; ++index) {
    Assert::IsTrue(provider.WaitForStarts(index + 1));
    provider.Complete(
        index, index == 3 ? SuccessfulPdkResult() : SuccessfulDoctorResult());
  }
  Assert::IsTrue(collector.WaitForTerminal());
  Assert::IsFalse(collector.Terminal().status.Ok());
  Assert::IsFalse(collector.Check(application::DoctorCheckId::kWsl2).passed);
  Assert::AreEqual<std::size_t>(1U, collector.TerminalCount());
}

TEST_METHOD(InstalledFrameworksAtDifferentPathsExplainDoctorFailure) {
  ControlledExecutionProvider provider;
  application::ToolchainDoctorService service(&provider);
  DoctorEventCollector collector;
  core::ToolchainProfile profile = DoctorProfile();
  profile.openlane_root = "~/.designpp/toolchains/openlane2";
  profile.orfs_root = "~/.designpp/toolchains/orfs";
  Assert::IsTrue(service
                     .Start(std::move(profile), 14,
                            [&collector](application::DoctorEvent event) {
                              collector.Add(std::move(event));
                            })
                     .Ok());
  Assert::IsTrue(provider.WaitForStarts(1));
  provider.Complete(0, SuccessfulDoctorResult("5.15-WSL2\n"));
  runtime::ProcessResult missing_root = SuccessfulDoctorResult();
  missing_root.exit_code = 1;
  Assert::IsTrue(provider.WaitForStarts(2));
  provider.Complete(1, missing_root);
  Assert::IsTrue(provider.WaitForStarts(3));
  provider.Complete(2, missing_root);
  Assert::IsTrue(provider.WaitForStarts(4));
  provider.Complete(3, SuccessfulPdkResult());
  Assert::IsTrue(collector.WaitForTerminal());
  const auto openlane = collector.Check(application::DoctorCheckId::kOpenLane);
  const auto orfs = collector.Check(application::DoctorCheckId::kOrfs);
  Assert::IsFalse(openlane.passed);
  Assert::IsFalse(orfs.passed);
  Assert::IsTrue(openlane.message.find("~/.designpp/toolchains/openlane2") !=
                 std::string::npos);
  Assert::IsTrue(orfs.message.find("~/.designpp/toolchains/orfs") !=
                 std::string::npos);
  Assert::IsTrue(orfs.message.find("activate") != std::string::npos);
}

TEST_METHOD(UnconfiguredRootsDoNotProbeTheHomeDirectory) {
  ControlledExecutionProvider provider;
  application::ToolchainDoctorService service(&provider);
  DoctorEventCollector collector;
  core::ToolchainProfile profile = DoctorProfile();
  profile.openlane_root.clear();
  profile.orfs_root.clear();
  profile.pdk_root.clear();
  Assert::IsTrue(service
                     .Start(std::move(profile), 15,
                            [&collector](application::DoctorEvent event) {
                              collector.Add(std::move(event));
                            })
                     .Ok());
  Assert::IsTrue(provider.WaitForStarts(1));
  provider.Complete(0, SuccessfulDoctorResult("5.15-WSL2\n"));
  Assert::IsTrue(collector.WaitForTerminal());
  Assert::IsFalse(
      collector.Check(application::DoctorCheckId::kOpenLane).passed);
  Assert::IsFalse(collector.Check(application::DoctorCheckId::kOrfs).passed);
  Assert::IsFalse(collector.Check(application::DoctorCheckId::kPdk).passed);
  Assert::IsTrue(collector.Check(application::DoctorCheckId::kOpenLane)
                     .message.find("not configured") != std::string::npos);
}

TEST_METHOD(OrfsPlatformsSatisfyPdkCheckWithoutOpenLanePdkRoot) {
  ControlledExecutionProvider provider;
  application::ToolchainDoctorService service(&provider);
  DoctorEventCollector collector;
  core::ToolchainProfile profile = DoctorProfile();
  profile.pdk_root.clear();
  const std::string orfs_root = profile.orfs_root;
  Assert::IsTrue(service
                     .Start(std::move(profile), 16,
                            [&collector](application::DoctorEvent event) {
                              collector.Add(std::move(event));
                            })
                     .Ok());
  Assert::IsTrue(provider.WaitForStarts(1));
  provider.Complete(0, SuccessfulDoctorResult("5.15-WSL2\n"));
  for (std::size_t index = 1; index < 3; ++index) {
    Assert::IsTrue(provider.WaitForStarts(index + 1));
    provider.Complete(index, SuccessfulDoctorResult());
  }
  Assert::IsTrue(provider.WaitForStarts(4));
  const runtime::WslCommand pdk_command = provider.Command(3);
  Assert::AreEqual(std::wstring(L"/bin/bash"), pdk_command.program);
  Assert::AreEqual(std::wstring(), pdk_command.arguments[3]);
  Assert::AreEqual(std::wstring(orfs_root.begin(), orfs_root.end()),
                   pdk_command.arguments[4]);
  provider.Complete(3, SuccessfulPdkResult());
  Assert::IsTrue(collector.WaitForTerminal());
  Assert::IsTrue(collector.Check(application::DoctorCheckId::kPdk).passed);
  Assert::IsTrue(collector.Check(application::DoctorCheckId::kPdk)
                     .message.find("ORFS platforms") != std::string::npos);
}

TEST_METHOD(PdkCheckRequiresEvidenceEvenWhenProcessExitsZero) {
  ControlledExecutionProvider provider;
  application::ToolchainDoctorService service(&provider);
  DoctorEventCollector collector;
  Assert::IsTrue(service
                     .Start(DoctorProfile(), 17,
                            [&collector](application::DoctorEvent event) {
                              collector.Add(std::move(event));
                            })
                     .Ok());
  Assert::IsTrue(provider.WaitForStarts(1));
  provider.Complete(0, SuccessfulDoctorResult("5.15-WSL2\n"));
  for (std::size_t index = 1; index < 4; ++index) {
    Assert::IsTrue(provider.WaitForStarts(index + 1));
    provider.Complete(index, SuccessfulDoctorResult());
  }
  Assert::IsTrue(collector.WaitForTerminal());
  Assert::IsFalse(collector.Check(application::DoctorCheckId::kPdk).passed);
}

TEST_METHOD(WslUnexpectedOutputCannotPassWithZeroExitCode) {
  ControlledExecutionProvider provider;
  application::ToolchainDoctorService service(&provider);
  DoctorEventCollector collector;
  Assert::IsTrue(service
                     .Start(DoctorProfile(), 12,
                            [&collector](application::DoctorEvent event) {
                              collector.Add(std::move(event));
                            })
                     .Ok());
  Assert::IsTrue(provider.WaitForStarts(1));
  provider.Complete(0, SuccessfulDoctorResult("Wsl/Service/E_UNEXPECTED WSL2"));
  for (std::size_t index = 1; index < 4; ++index) {
    Assert::IsTrue(provider.WaitForStarts(index + 1));
    provider.Complete(
        index, index == 3 ? SuccessfulPdkResult() : SuccessfulDoctorResult());
  }
  Assert::IsTrue(collector.WaitForTerminal());
  Assert::IsFalse(collector.Terminal().status.Ok());
  const auto check = collector.Check(application::DoctorCheckId::kWsl2);
  Assert::IsFalse(check.passed);
  Assert::IsTrue(check.message.find("update WSL") != std::string::npos);
}

TEST_METHOD(WslUtf16UnexpectedOutputCannotPassWithZeroExitCode) {
  ControlledExecutionProvider provider;
  application::ToolchainDoctorService service(&provider);
  DoctorEventCollector collector;
  Assert::IsTrue(service
                     .Start(DoctorProfile(), 13,
                            [&collector](application::DoctorEvent event) {
                              collector.Add(std::move(event));
                            })
                     .Ok());
  Assert::IsTrue(provider.WaitForStarts(1));
  const std::string error = "Wsl/Service/E_UNEXPECTED WSL2";
  std::string utf16;
  for (char character : error) {
    utf16.push_back(character);
    utf16.push_back('\0');
  }
  provider.Complete(0, SuccessfulDoctorResult(std::move(utf16)));
  for (std::size_t index = 1; index < 4; ++index) {
    Assert::IsTrue(provider.WaitForStarts(index + 1));
    provider.Complete(
        index, index == 3 ? SuccessfulPdkResult() : SuccessfulDoctorResult());
  }
  Assert::IsTrue(collector.WaitForTerminal());
  Assert::IsFalse(collector.Check(application::DoctorCheckId::kWsl2).passed);
}

TEST_METHOD(LateCompletionFromPreviousCheckIsIgnored) {
  ControlledExecutionProvider provider;
  application::ToolchainDoctorService service(&provider);
  DoctorEventCollector collector;
  Assert::IsTrue(service
                     .Start(DoctorProfile(), 10,
                            [&collector](application::DoctorEvent event) {
                              collector.Add(std::move(event));
                            })
                     .Ok());
  Assert::IsTrue(provider.WaitForStarts(1));
  provider.Complete(0, SuccessfulDoctorResult("5.15-WSL2\n"));
  Assert::IsTrue(provider.WaitForStarts(2));

  runtime::ProcessResult late_failure = SuccessfulDoctorResult();
  late_failure.exit_code = 9;
  provider.Complete(0, late_failure);
  provider.Complete(1, SuccessfulDoctorResult());
  Assert::IsTrue(provider.WaitForStarts(3));
  provider.Complete(2, SuccessfulDoctorResult());
  Assert::IsTrue(provider.WaitForStarts(4));
  provider.Complete(3, SuccessfulPdkResult());

  Assert::IsTrue(collector.WaitForTerminal());
  Assert::IsTrue(collector.Terminal().status.Ok());
  Assert::AreEqual<std::size_t>(1U, collector.TerminalCount());
}

TEST_METHOD(StartFailureIsReportedAndDiagnosisContinues) {
  ControlledExecutionProvider provider;
  provider.FailNextStart();
  application::ToolchainDoctorService service(&provider);
  DoctorEventCollector collector;
  Assert::IsTrue(service
                     .Start(DoctorProfile(), 11,
                            [&collector](application::DoctorEvent event) {
                              collector.Add(std::move(event));
                            })
                     .Ok());
  Assert::IsTrue(provider.WaitForStarts(1));
  provider.Complete(0, SuccessfulDoctorResult());
  Assert::IsTrue(provider.WaitForStarts(2));
  provider.Complete(1, SuccessfulDoctorResult());
  Assert::IsTrue(provider.WaitForStarts(3));
  provider.Complete(2, SuccessfulDoctorResult());

  Assert::IsTrue(collector.WaitForTerminal());
  Assert::IsFalse(collector.Terminal().status.Ok());
  Assert::IsFalse(collector.Check(application::DoctorCheckId::kWsl2).passed);
}

TEST_METHOD(CancelStopsActiveCheckAndCompletesOnce) {
  ControlledExecutionProvider provider;
  application::ToolchainDoctorService service(&provider);
  DoctorEventCollector collector;
  Assert::IsTrue(service
                     .Start(DoctorProfile(), 9,
                            [&collector](application::DoctorEvent event) {
                              collector.Add(std::move(event));
                            })
                     .Ok());
  Assert::IsTrue(provider.WaitForStarts(1));
  service.Cancel();
  Assert::IsTrue(provider.Cancelled(0));
  runtime::ProcessResult cancelled = SuccessfulDoctorResult();
  cancelled.cancelled = true;
  provider.Complete(0, cancelled, 2);
  Assert::IsTrue(collector.WaitForTerminal());
  Assert::IsTrue(collector.Terminal().status.code ==
                 core::ErrorCode::kCancelled);
  Assert::AreEqual<std::size_t>(1U, collector.TerminalCount());
}
}
;

}  // namespace designpp::tests

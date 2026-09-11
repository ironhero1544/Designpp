// Copyright 2026 The Design++ Authors

#include <CppUnitTest.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "designpp/application/wsl_gui_execution_service.h"
#include "synthesis_test_support.h"

using Microsoft::VisualStudio::CppUnitTestFramework::Assert;

namespace designpp::tests {
namespace {

runtime::ProcessResult ProcessResult(std::uint32_t exit_code = 0,
                                     std::string output = "tmpfs\n") {
  runtime::ProcessResult result;
  result.started = true;
  result.exit_code = exit_code;
  result.output = std::move(output);
  return result;
}

class ResultCollector final {
 public:
  void Complete(application::WslGuiExecutionResult result) {
    {
      std::scoped_lock lock(mutex_);
      result_ = std::move(result);
      ++count_;
    }
    changed_.notify_all();
  }

  bool Wait() {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, std::chrono::seconds(5),
                             [&] { return count_ != 0; });
  }

  std::size_t Count() const {
    std::scoped_lock lock(mutex_);
    return count_;
  }

  application::WslGuiExecutionResult Result() const {
    std::scoped_lock lock(mutex_);
    return result_;
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  application::WslGuiExecutionResult result_;
  std::size_t count_ = 0;
};

runtime::WslCommand ViewerCommand() {
  runtime::WslCommand command;
  command.distribution = L"Ubuntu";
  command.program = L"klayout";
  command.arguments = {L"/tmp/layout.gds"};
  return command;
}

}  // namespace

TEST_CLASS(WslGuiExecutionServiceTests){
  public : TEST_METHOD(HealthyTransportIsCheckedAndViewerStarts){
      ControlledExecutionProvider provider;
application::WslGuiExecutionService service(&provider);
ResultCollector collector;
Assert::IsTrue(service
                   .Start(
                       ViewerCommand(), [](std::string) {},
                       [&collector](auto result) {
                         collector.Complete(std::move(result));
                       })
                   .Ok());

Assert::IsTrue(provider.WaitForStarts(1));
Assert::AreEqual(std::wstring(L"/usr/bin/stat"), provider.Command(0).program);
Assert::IsTrue(provider.Command(0).arguments ==
               std::vector<std::wstring>{L"-f", L"-c", L"%T",
                                         L"/mnt/shared_memory"});
provider.Complete(0, ProcessResult());
Assert::IsTrue(provider.WaitForStarts(2));
Assert::AreEqual(std::wstring(L"/usr/bin/touch"), provider.Command(1).program);
provider.Complete(1, ProcessResult());
Assert::IsTrue(provider.WaitForStarts(3));
Assert::AreEqual(std::wstring(L"/usr/bin/rm"), provider.Command(2).program);
provider.Complete(2, ProcessResult());
Assert::IsTrue(provider.WaitForStarts(4));
Assert::AreEqual(std::wstring(L"klayout"), provider.Command(3).program);
provider.Complete(3, ProcessResult(), 2);

Assert::IsTrue(collector.Wait());
Assert::AreEqual(static_cast<std::size_t>(1), collector.Count());
Assert::IsTrue(collector.Result().status.Ok());
Assert::IsFalse(collector.Result().repair_attempted);
}  // namespace designpp::tests

TEST_METHOD(WritableNonTmpfsDirectoryIsRepairedBeforeViewerStarts) {
  ControlledExecutionProvider provider;
  application::WslGuiExecutionService service(&provider);
  ResultCollector collector;
  Assert::IsTrue(service
                     .Start(
                         ViewerCommand(), [](std::string) {},
                         [&collector](auto result) {
                           collector.Complete(std::move(result));
                         })
                     .Ok());

  Assert::IsTrue(provider.WaitForStarts(1));
  provider.Complete(0, ProcessResult(0, "ext2\n"));
  Assert::IsTrue(provider.WaitForStarts(2));
  Assert::AreEqual(std::wstring(L"/usr/bin/mkdir"),
                   provider.Command(1).program);
  provider.Complete(1, ProcessResult());
  Assert::IsTrue(provider.WaitForStarts(3));
  provider.Complete(2, ProcessResult());
  Assert::IsTrue(provider.WaitForStarts(4));
  Assert::AreEqual(std::wstring(L"/usr/bin/mount"),
                   provider.Command(3).program);
  provider.Complete(3, ProcessResult());
  Assert::IsTrue(provider.WaitForStarts(5));
  Assert::AreEqual(std::wstring(L"/usr/bin/touch"),
                   provider.Command(4).program);
  provider.Complete(4, ProcessResult());
  Assert::IsTrue(provider.WaitForStarts(6));
  Assert::AreEqual(std::wstring(L"/usr/bin/rm"), provider.Command(5).program);
  provider.Complete(5, ProcessResult());
  Assert::IsTrue(provider.WaitForStarts(7));
  Assert::AreEqual(std::wstring(L"klayout"), provider.Command(6).program);
  provider.Complete(6, ProcessResult());

  Assert::IsTrue(collector.Wait());
  Assert::IsTrue(collector.Result().status.Ok());
  Assert::IsTrue(collector.Result().repair_attempted);
}

TEST_METHOD(BrokenTransportIsRepairedBeforeViewerStarts) {
  ControlledExecutionProvider provider;
  application::WslGuiExecutionService service(&provider);
  ResultCollector collector;
  Assert::IsTrue(service
                     .Start(
                         ViewerCommand(), [](std::string) {},
                         [&collector](auto result) {
                           collector.Complete(std::move(result));
                         })
                     .Ok());

  Assert::IsTrue(provider.WaitForStarts(1));
  provider.Complete(0, ProcessResult(1));
  Assert::IsTrue(provider.WaitForStarts(2));
  const runtime::WslCommand create = provider.Command(1);
  Assert::AreEqual(std::wstring(L"/usr/bin/mkdir"), create.program);
  Assert::IsTrue(create.user.has_value());
  Assert::AreEqual(std::wstring(L"root"), *create.user);
  provider.Complete(1, ProcessResult());

  Assert::IsTrue(provider.WaitForStarts(3));
  const runtime::WslCommand chmod = provider.Command(2);
  Assert::AreEqual(std::wstring(L"/usr/bin/chmod"), chmod.program);
  Assert::AreEqual(std::wstring(L"root"), *chmod.user);
  Assert::IsTrue(chmod.arguments ==
                 std::vector<std::wstring>{L"1777", L"/mnt/shared_memory"});
  provider.Complete(2, ProcessResult());

  Assert::IsTrue(provider.WaitForStarts(4));
  Assert::AreEqual(std::wstring(L"/usr/bin/mount"),
                   provider.Command(3).program);
  provider.Complete(3, ProcessResult());
  Assert::IsTrue(provider.WaitForStarts(5));
  provider.Complete(4, ProcessResult());
  Assert::IsTrue(provider.WaitForStarts(6));
  provider.Complete(5, ProcessResult());
  Assert::IsTrue(provider.WaitForStarts(7));
  Assert::AreEqual(std::wstring(L"klayout"), provider.Command(6).program);
  provider.Complete(6, ProcessResult());

  Assert::IsTrue(collector.Wait());
  Assert::IsTrue(collector.Result().status.Ok());
  Assert::IsTrue(collector.Result().repair_attempted);
}

TEST_METHOD(PermissionRepairFallsBackToTmpfs) {
  ControlledExecutionProvider provider;
  application::WslGuiExecutionService service(&provider);
  ResultCollector collector;
  Assert::IsTrue(service
                     .Start(
                         ViewerCommand(), [](std::string) {},
                         [&collector](auto result) {
                           collector.Complete(std::move(result));
                         })
                     .Ok());
  Assert::IsTrue(provider.WaitForStarts(1));
  provider.Complete(0, ProcessResult(1));
  Assert::IsTrue(provider.WaitForStarts(2));
  provider.Complete(1, ProcessResult());
  Assert::IsTrue(provider.WaitForStarts(3));
  provider.Complete(2, ProcessResult(1));
  Assert::IsTrue(provider.WaitForStarts(4));
  const runtime::WslCommand mount = provider.Command(3);
  Assert::AreEqual(std::wstring(L"/usr/bin/mount"), mount.program);
  Assert::AreEqual(std::wstring(L"root"), *mount.user);
  Assert::IsTrue(mount.arguments ==
                 std::vector<std::wstring>{L"-t", L"tmpfs", L"-o", L"mode=1777",
                                           L"tmpfs", L"/mnt/shared_memory"});
  provider.Complete(3, ProcessResult());
  Assert::IsTrue(provider.WaitForStarts(5));
  provider.Complete(4, ProcessResult());
  Assert::IsTrue(provider.WaitForStarts(6));
  provider.Complete(5, ProcessResult());
  Assert::IsTrue(provider.WaitForStarts(7));
  Assert::AreEqual(std::wstring(L"klayout"), provider.Command(6).program);
  provider.Complete(6, ProcessResult());

  Assert::IsTrue(collector.Wait());
  Assert::IsTrue(collector.Result().status.Ok());
  Assert::IsTrue(collector.Result().repair_attempted);
}

TEST_METHOD(RepairFailureDoesNotLaunchViewer) {
  ControlledExecutionProvider provider;
  application::WslGuiExecutionService service(&provider);
  ResultCollector collector;
  Assert::IsTrue(service
                     .Start(
                         ViewerCommand(), [](std::string) {},
                         [&collector](auto result) {
                           collector.Complete(std::move(result));
                         })
                     .Ok());
  Assert::IsTrue(provider.WaitForStarts(1));
  provider.Complete(0, ProcessResult(1));
  Assert::IsTrue(provider.WaitForStarts(2));
  provider.Complete(1, ProcessResult());
  Assert::IsTrue(provider.WaitForStarts(3));
  provider.Complete(2, ProcessResult(32), 2);
  Assert::IsTrue(provider.WaitForStarts(4));
  provider.Complete(3, ProcessResult(32), 3);

  Assert::IsTrue(collector.Wait());
  Assert::AreEqual(static_cast<std::size_t>(1), collector.Count());
  Assert::IsFalse(collector.Result().status.Ok());
  Assert::IsTrue(collector.Result().repair_attempted);
  Assert::AreEqual(static_cast<std::size_t>(4), provider.StartCount());
}
}
;

}  // namespace designpp::tests

// Copyright 2026 The Design++ Authors

#include <CppUnitTest.h>
#include <windows.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <semaphore>
#include <set>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "designpp/runtime/resource_coordinator.h"
#include "designpp/runtime/task_scheduler.h"

using Microsoft::VisualStudio::CppUnitTestFramework::Assert;

namespace designpp::tests {
namespace {

struct HandleCloser final {
  void operator()(void* handle) const {
    if (handle != nullptr) CloseHandle(handle);
  }
};

using ScopedHandle = std::unique_ptr<void, HandleCloser>;

std::wstring UniqueQuotaName(std::wstring_view suffix) {
  static std::atomic_uint64_t sequence = 0;
  return L"Local\\DesignPlusPlus.TestQuota." +
         std::to_wstring(GetCurrentProcessId()) + L"." +
         std::to_wstring(++sequence) + L"." + std::wstring(suffix);
}

ScopedHandle LaunchQuotaHolder(std::wstring_view quota_name,
                               std::wstring_view ready_name,
                               std::wstring_view release_name) {
  std::wstring script = L"$s=[Threading.Semaphore]::OpenExisting('" +
                        std::wstring(quota_name) +
                        L"');$r=[Threading.EventWaitHandle]::OpenExisting('" +
                        std::wstring(ready_name) +
                        L"');$q=[Threading.EventWaitHandle]::OpenExisting('" +
                        std::wstring(release_name) +
                        L"');$null=$s.WaitOne();$null=$r.Set();$null=$q."
                        L"WaitOne();$null=$s.Release()";
  std::wstring command =
      L"powershell.exe -NoProfile -NonInteractive -Command \"" + script + L"\"";
  STARTUPINFOW startup{sizeof(startup)};
  PROCESS_INFORMATION process{};
  const BOOL started =
      CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                     CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
  if (!started) return {};
  CloseHandle(process.hThread);
  return ScopedHandle(process.hProcess);
}

}  // namespace

TEST_CLASS(RuntimeResourceTests){
  public : TEST_METHOD(WaitingAcquisitionContinuesAfterLeaseRelease){
      runtime::ResourceCoordinator coordinator(UniqueQuotaName(L"wait"), 1);
std::binary_semaphore started(0);
std::binary_semaphore completed(0);
std::atomic_bool acquired = false;
std::jthread waiter;
{
  auto held = coordinator.TryAcquireCpu(1);
  Assert::IsTrue(held.Ok());
  waiter = std::jthread([&](std::stop_token token) {
    started.release();
    auto result = coordinator.AcquireCpu(1, token);
    acquired = result.Ok();
    completed.release();
  });
  started.acquire();
}
completed.acquire();
Assert::IsTrue(acquired.load());
}  // namespace designpp::tests

TEST_METHOD(WaitingAcquisitionIsCancellationSafe) {
  runtime::ResourceCoordinator coordinator(UniqueQuotaName(L"cancel"), 1);
  auto held = coordinator.TryAcquireCpu(1);
  Assert::IsTrue(held.Ok());
  std::binary_semaphore started(0);
  std::binary_semaphore completed(0);
  std::atomic<core::ErrorCode> error = core::ErrorCode::kOk;
  std::stop_source cancellation;
  std::jthread waiter([&] {
    started.release();
    auto result = coordinator.AcquireCpu(1, cancellation.get_token());
    if (!result.Ok()) error = result.GetStatus().code;
    completed.release();
  });
  started.acquire();
  cancellation.request_stop();
  completed.acquire();
  Assert::IsTrue(error.load() == core::ErrorCode::kCancelled);
}

TEST_METHOD(CpuQuotaIsEnforcedAcrossProcesses) {
  const std::wstring quota_name = UniqueQuotaName(L"process");
  const std::wstring ready_name = quota_name + L".Ready";
  const std::wstring release_name = quota_name + L".Release";
  ScopedHandle semaphore(CreateSemaphoreW(nullptr, 1, 1, quota_name.c_str()));
  ScopedHandle ready(CreateEventW(nullptr, TRUE, FALSE, ready_name.c_str()));
  ScopedHandle release(
      CreateEventW(nullptr, TRUE, FALSE, release_name.c_str()));
  Assert::IsNotNull(semaphore.get());
  Assert::IsNotNull(ready.get());
  Assert::IsNotNull(release.get());

  ScopedHandle child = LaunchQuotaHolder(quota_name, ready_name, release_name);
  Assert::IsNotNull(child.get());
  Assert::AreEqual(static_cast<DWORD>(WAIT_OBJECT_0),
                   WaitForSingleObject(ready.get(), 10000));

  runtime::ResourceCoordinator coordinator(quota_name, 1);
  auto blocked = coordinator.TryAcquireCpu(1);
  Assert::IsFalse(blocked.Ok());
  Assert::IsTrue(blocked.GetStatus().code == core::ErrorCode::kConflict);

  Assert::IsTrue(SetEvent(release.get()) != FALSE);
  Assert::AreEqual(static_cast<DWORD>(WAIT_OBJECT_0),
                   WaitForSingleObject(child.get(), 10000));
  auto acquired = coordinator.TryAcquireCpu(1);
  Assert::IsTrue(acquired.Ok());
}

TEST_METHOD(SingleWorkerPreservesSubmissionOrder) {
  runtime::TaskScheduler scheduler(1);
  std::binary_semaphore first_started(0);
  std::binary_semaphore release_first(0);
  std::counting_semaphore<3> completed(0);
  std::mutex mutex;
  std::vector<int> order;
  Assert::IsTrue(scheduler.Submit([&](std::stop_token) {
    first_started.release();
    release_first.acquire();
  }));
  Assert::IsTrue(scheduler.Submit([&](std::stop_token) {
    std::scoped_lock lock(mutex);
    order.push_back(1);
    completed.release();
  }));
  Assert::IsTrue(scheduler.Submit([&](std::stop_token) {
    std::scoped_lock lock(mutex);
    order.push_back(2);
    completed.release();
  }));
  first_started.acquire();
  release_first.release();
  completed.acquire();
  completed.acquire();
  Assert::AreEqual<std::size_t>(2, order.size());
  Assert::AreEqual(1, order[0]);
  Assert::AreEqual(2, order[1]);
}

TEST_METHOD(ShutdownStopsRunningTaskAndDropsPendingTasks) {
  runtime::TaskScheduler scheduler(1);
  std::binary_semaphore started(0);
  std::atomic_bool observed_stop = false;
  std::atomic_bool pending_ran = false;
  Assert::IsTrue(scheduler.Submit([&](std::stop_token token) {
    started.release();
    while (!token.stop_requested()) std::this_thread::yield();
    observed_stop = true;
  }));
  Assert::IsTrue(
      scheduler.Submit([&](std::stop_token) { pending_ran = true; }));
  started.acquire();
  scheduler.RequestStop();
  Assert::IsTrue(observed_stop.load());
  Assert::IsFalse(pending_ran.load());
}

TEST_METHOD(HighTaskCountUsesOnlyBoundedWorkerPool) {
  constexpr int kTaskCount = 500;
  constexpr std::size_t kWorkerCount = 4;
  runtime::TaskScheduler scheduler(kWorkerCount);
  std::counting_semaphore<kTaskCount> completed(0);
  std::mutex mutex;
  std::set<DWORD> worker_threads;
  std::atomic_int executed = 0;
  constexpr int kBatchSize = 100;
  for (int batch = 0; batch < kTaskCount / kBatchSize; ++batch) {
    for (int index = 0; index < kBatchSize; ++index) {
      Assert::IsTrue(scheduler.Submit([&](std::stop_token token) {
        if (!token.stop_requested()) {
          {
            std::scoped_lock lock(mutex);
            worker_threads.insert(GetCurrentThreadId());
          }
          ++executed;
        }
        completed.release();
      }));
    }
    for (int index = 0; index < kBatchSize; ++index) completed.acquire();
  }
  Assert::AreEqual(kTaskCount, executed.load());
  Assert::IsTrue(worker_threads.size() <= kWorkerCount);
}
}
;

}  // namespace designpp::tests

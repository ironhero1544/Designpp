// Copyright 2026 The Design++ Authors

#include "designpp/runtime/resource_coordinator.h"

#include <windows.h>

#include <algorithm>
#include <thread>
#include <utility>

namespace designpp::runtime {

struct CpuTokenLease::Implementation final {
  HANDLE semaphore = nullptr;
  std::uint32_t token_count = 0;
};

CpuTokenLease::CpuTokenLease() = default;

CpuTokenLease::CpuTokenLease(std::unique_ptr<Implementation> implementation)
    : implementation_(std::move(implementation)) {}

CpuTokenLease::CpuTokenLease(CpuTokenLease&&) noexcept = default;
CpuTokenLease& CpuTokenLease::operator=(CpuTokenLease&&) noexcept = default;

CpuTokenLease::~CpuTokenLease() {
  if (!implementation_) return;
  if (implementation_->semaphore != nullptr) {
    ReleaseSemaphore(implementation_->semaphore,
                     static_cast<LONG>(implementation_->token_count), nullptr);
    CloseHandle(implementation_->semaphore);
  }
}

std::uint32_t CpuTokenLease::token_count() const noexcept {
  return implementation_ ? implementation_->token_count : 0;
}

ResourceCoordinator::ResourceCoordinator()
    : semaphore_name_(L"Local\\DesignPlusPlus.CpuQuota.v1") {
  const unsigned int logical_cpus = std::thread::hardware_concurrency();
  total_cpu_tokens_ = logical_cpus > 1 ? logical_cpus - 1 : 1;
}

ResourceCoordinator::ResourceCoordinator(std::wstring semaphore_name,
                                         std::uint32_t total_cpu_tokens)
    : semaphore_name_(std::move(semaphore_name)),
      total_cpu_tokens_(std::max<std::uint32_t>(1, total_cpu_tokens)) {}

core::Result<CpuTokenLease> ResourceCoordinator::TryAcquireCpu(
    std::uint32_t requested_tokens) const {
  if (requested_tokens == 0 || requested_tokens > total_cpu_tokens_) {
    return core::Status{core::ErrorCode::kInvalidArgument,
                        "Requested CPU token count is invalid", 0};
  }
  HANDLE semaphore = CreateSemaphoreW(
      nullptr, static_cast<LONG>(total_cpu_tokens_),
      static_cast<LONG>(total_cpu_tokens_), semaphore_name_.c_str());
  if (semaphore == nullptr) {
    return core::Status{core::ErrorCode::kIoError,
                        "Cannot open cross-process CPU quota", GetLastError()};
  }
  std::uint32_t acquired = 0;
  while (acquired < requested_tokens) {
    const DWORD result = WaitForSingleObject(semaphore, 0);
    if (result != WAIT_OBJECT_0) break;
    ++acquired;
  }
  if (acquired != requested_tokens) {
    if (acquired > 0) {
      ReleaseSemaphore(semaphore, static_cast<LONG>(acquired), nullptr);
    }
    CloseHandle(semaphore);
    return core::Status{core::ErrorCode::kConflict,
                        "CPU quota is busy; retry after another run finishes",
                        0};
  }
  auto implementation = std::make_unique<CpuTokenLease::Implementation>();
  implementation->semaphore = semaphore;
  implementation->token_count = acquired;
  return CpuTokenLease(std::move(implementation));
}

std::uint32_t ResourceCoordinator::total_cpu_tokens() const noexcept {
  return total_cpu_tokens_;
}

}  // namespace designpp::runtime

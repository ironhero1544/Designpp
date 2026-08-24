// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_RUNTIME_RESOURCE_COORDINATOR_H_
#define DESIGNPP_RUNTIME_RESOURCE_COORDINATOR_H_

#include <cstdint>
#include <memory>
#include <stop_token>
#include <string>

#include "designpp/core/status.h"

namespace designpp::runtime {

class CpuTokenLease final {
 public:
  CpuTokenLease();
  CpuTokenLease(const CpuTokenLease&) = delete;
  CpuTokenLease& operator=(const CpuTokenLease&) = delete;
  CpuTokenLease(CpuTokenLease&&) noexcept;
  CpuTokenLease& operator=(CpuTokenLease&&) noexcept;
  ~CpuTokenLease();

  [[nodiscard]] std::uint32_t token_count() const noexcept;

 private:
  struct Implementation;
  explicit CpuTokenLease(std::unique_ptr<Implementation> implementation);
  std::unique_ptr<Implementation> implementation_;

  friend class ResourceCoordinator;
};

class ResourceCoordinator final {
 public:
  ResourceCoordinator();
  ResourceCoordinator(std::wstring semaphore_name,
                      std::uint32_t total_cpu_tokens);

  [[nodiscard]] core::Result<CpuTokenLease> TryAcquireCpu(
      std::uint32_t requested_tokens) const;
  // Waits for the complete token request without partially interleaving
  // multi-token acquisitions from other processes. Cancellation releases any
  // partial acquisition before returning.
  [[nodiscard]] core::Result<CpuTokenLease> AcquireCpu(
      std::uint32_t requested_tokens, std::stop_token stop_token) const;
  [[nodiscard]] std::uint32_t total_cpu_tokens() const noexcept;

 private:
  std::wstring semaphore_name_;
  std::uint32_t total_cpu_tokens_ = 1;
};

}  // namespace designpp::runtime

#endif  // DESIGNPP_RUNTIME_RESOURCE_COORDINATOR_H_

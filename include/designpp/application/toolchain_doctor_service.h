// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_APPLICATION_TOOLCHAIN_DOCTOR_SERVICE_H_
#define DESIGNPP_APPLICATION_TOOLCHAIN_DOCTOR_SERVICE_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "designpp/core/status.h"
#include "designpp/core/toolchain_profile.h"
#include "designpp/runtime/execution_provider.h"

namespace designpp::application {

enum class DoctorCheckId { kProfile, kWsl2, kOpenLane, kOrfs, kPdk };

struct DoctorCheckResult {
  DoctorCheckId id = DoctorCheckId::kProfile;
  bool passed = false;
  bool required = true;
  std::string message;
  std::string detail;
};

enum class DoctorEventKind { kStarted, kCheckCompleted, kCompleted };

struct DoctorEvent {
  DoctorEventKind kind = DoctorEventKind::kStarted;
  std::uint64_t generation = 0;
  DoctorCheckResult check;
  core::Status status;
};

using DoctorEventSink = std::function<void(DoctorEvent)>;

class ToolchainDoctorService final {
 public:
  explicit ToolchainDoctorService(runtime::ExecutionProvider* provider);
  ToolchainDoctorService(const ToolchainDoctorService&) = delete;
  ToolchainDoctorService& operator=(const ToolchainDoctorService&) = delete;
  ~ToolchainDoctorService();

  [[nodiscard]] core::Status Start(core::ToolchainProfile profile,
                                   std::uint64_t generation,
                                   DoctorEventSink sink);
  void Cancel() noexcept;
  void Shutdown() noexcept;
  [[nodiscard]] bool IsActive() const;

 private:
  struct Implementation;
  std::shared_ptr<Implementation> implementation_;
};

[[nodiscard]] std::string_view DoctorCheckName(DoctorCheckId id) noexcept;

}  // namespace designpp::application

#endif  // DESIGNPP_APPLICATION_TOOLCHAIN_DOCTOR_SERVICE_H_

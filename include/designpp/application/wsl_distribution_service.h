// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_APPLICATION_WSL_DISTRIBUTION_SERVICE_H_
#define DESIGNPP_APPLICATION_WSL_DISTRIBUTION_SERVICE_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "designpp/core/status.h"

namespace designpp::application {

struct WslDistribution {
  std::string name;
  std::string state;
  std::uint32_t version = 0;
  bool is_default = false;
};

// Parses the raw output of `wsl.exe --list --verbose`. Windows may emit this
// output as UTF-16LE even when the process output pipe otherwise carries bytes.
[[nodiscard]] core::Result<std::vector<WslDistribution>>
ParseWslDistributionList(std::string_view raw_output);

struct WslDistributionEvent {
  std::uint64_t generation = 0;
  core::Status status;
  std::vector<WslDistribution> distributions;
};

using WslDistributionEventSink = std::function<void(WslDistributionEvent)>;

// Discovers installed WSL distributions asynchronously. Completion is
// delivered at most once and never on the caller's GUI thread by contract.
class WslDistributionService final {
 public:
  WslDistributionService();
  WslDistributionService(const WslDistributionService&) = delete;
  WslDistributionService& operator=(const WslDistributionService&) = delete;
  ~WslDistributionService();

  [[nodiscard]] core::Status Start(std::uint64_t generation,
                                   WslDistributionEventSink sink);
  void Cancel() noexcept;
  void Shutdown() noexcept;
  [[nodiscard]] bool IsActive() const;

 private:
  struct Implementation;
  std::shared_ptr<Implementation> implementation_;
};

}  // namespace designpp::application

#endif  // DESIGNPP_APPLICATION_WSL_DISTRIBUTION_SERVICE_H_

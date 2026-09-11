// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_APPLICATION_LAYOUT_VIEWER_SERVICE_H_
#define DESIGNPP_APPLICATION_LAYOUT_VIEWER_SERVICE_H_

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

#include "designpp/core/status.h"
#include "designpp/core/toolchain_profile.h"
#include "designpp/runtime/execution_provider.h"

namespace designpp::application {

struct LayoutViewerRequest {
  core::ToolchainProfile profile;
  std::filesystem::path gds_path;
  std::uint64_t generation = 0;
  std::filesystem::path marker_database_path;
};

struct LayoutViewerEvent {
  std::uint64_t generation = 0;
  core::Status status;
  runtime::ProcessResult process_result;
  std::string output;
  bool completed = false;
};

using LayoutViewerEventSink = std::function<void(LayoutViewerEvent)>;

// Probes and launches external KLayout through structured WSL commands. Viewer
// failure never changes the validity of the source GDS artifact.
class LayoutViewerService final {
 public:
  explicit LayoutViewerService(runtime::ExecutionProvider* provider);
  LayoutViewerService(const LayoutViewerService&) = delete;
  LayoutViewerService& operator=(const LayoutViewerService&) = delete;
  ~LayoutViewerService();

  [[nodiscard]] core::Status Open(LayoutViewerRequest request,
                                  LayoutViewerEventSink sink);
  void Cancel() noexcept;
  void Shutdown() noexcept;
  [[nodiscard]] bool IsActive() const;

 private:
  struct Implementation;
  std::shared_ptr<Implementation> implementation_;
};

}  // namespace designpp::application

#endif  // DESIGNPP_APPLICATION_LAYOUT_VIEWER_SERVICE_H_

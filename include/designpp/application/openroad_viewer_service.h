// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_APPLICATION_OPENROAD_VIEWER_SERVICE_H_
#define DESIGNPP_APPLICATION_OPENROAD_VIEWER_SERVICE_H_

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

#include "designpp/core/status.h"
#include "designpp/core/toolchain_profile.h"
#include "designpp/runtime/execution_provider.h"

namespace designpp::application {

struct OpenRoadViewerRequest {
  core::ToolchainProfile profile;
  std::filesystem::path odb_path;
  std::string gui_target;
  std::uint64_t generation = 0;
  // Optional ORFS context.  When all values are present the viewer invokes
  // the backend's stage-specific gui_* target in the preserved lineage.  An
  // empty context intentionally falls back to opening the staged ODB with
  // OpenROAD directly.
  std::string orfs_root;
  std::string backend_workspace;
  std::filesystem::path config_path;
  std::string flow_variant;
};

struct OpenRoadViewerEvent {
  std::uint64_t generation = 0;
  core::Status status;
  runtime::ProcessResult process_result;
  std::string output;
  bool completed = false;
};

using OpenRoadViewerEventSink = std::function<void(OpenRoadViewerEvent)>;

// Opens an ORFS checkpoint in an independent OpenROAD/WSLg viewer session.
// Viewer failures never alter the checkpoint or physical-flow Run result.
class OpenRoadViewerService final {
 public:
  explicit OpenRoadViewerService(runtime::ExecutionProvider* provider);
  OpenRoadViewerService(const OpenRoadViewerService&) = delete;
  OpenRoadViewerService& operator=(const OpenRoadViewerService&) = delete;
  ~OpenRoadViewerService();

  [[nodiscard]] core::Status Open(OpenRoadViewerRequest request,
                                  OpenRoadViewerEventSink sink);
  void Cancel() noexcept;
  void Shutdown() noexcept;
  [[nodiscard]] bool IsActive() const;

 private:
  struct Implementation;
  std::shared_ptr<Implementation> implementation_;
};

}  // namespace designpp::application

#endif  // DESIGNPP_APPLICATION_OPENROAD_VIEWER_SERVICE_H_

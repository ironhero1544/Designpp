// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_APPLICATION_ORFS_MANAGED_FLOW_RUN_SERVICE_H_
#define DESIGNPP_APPLICATION_ORFS_MANAGED_FLOW_RUN_SERVICE_H_

#include "designpp/application/managed_flow_run_service.h"

namespace designpp::application {

// ORFS implementation used by ManagedFlowRunService for the "orfs" backend.
// The public service deliberately uses the same immutable event contract as
// OpenLane so Layout and future stage views remain backend-neutral.
class OrfsManagedFlowRunService final {
 public:
  explicit OrfsManagedFlowRunService(runtime::ExecutionProvider* provider);
  OrfsManagedFlowRunService(const OrfsManagedFlowRunService&) = delete;
  OrfsManagedFlowRunService& operator=(const OrfsManagedFlowRunService&) =
      delete;
  ~OrfsManagedFlowRunService();

  [[nodiscard]] core::Status Start(ManagedFlowRunRequest request,
                                   ManagedFlowRunEventSink sink);
  void Cancel() noexcept;
  void Shutdown() noexcept;
  [[nodiscard]] ManagedFlowRunState State() const;
  [[nodiscard]] bool IsActive() const;

 private:
  struct Implementation;
  std::shared_ptr<Implementation> implementation_;
};

}  // namespace designpp::application

#endif  // DESIGNPP_APPLICATION_ORFS_MANAGED_FLOW_RUN_SERVICE_H_

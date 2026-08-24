// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_APPLICATION_RECENT_WORKSPACE_STORE_H_
#define DESIGNPP_APPLICATION_RECENT_WORKSPACE_STORE_H_

#include <windows.h>

#include <string>
#include <vector>

#include "designpp/application/workspace.h"
#include "designpp/core/status.h"

namespace designpp::application {

struct RecentWorkspace {
  WorkspaceOpenRequest request;
  std::string display_name;
};

// Stores a bounded recent-workspace list as one atomic HKCU registry value.
// A named writer mutex prevents read/merge/write loss across app processes.
class RecentWorkspaceStore final {
 public:
  RecentWorkspaceStore();
  explicit RecentWorkspaceStore(std::wstring registry_subkey);

  [[nodiscard]] core::Result<std::vector<RecentWorkspace>> Load() const;
  [[nodiscard]] core::Status Touch(const RecentWorkspace& workspace) const;
  [[nodiscard]] core::Status Replace(
      const std::vector<RecentWorkspace>& workspaces) const;
  [[nodiscard]] core::Status Clear() const;

  static constexpr std::size_t kMaximumEntries = 20;

 private:
  [[nodiscard]] core::Status Save(
      const std::vector<RecentWorkspace>& workspaces) const;

  std::wstring registry_subkey_;
};

}  // namespace designpp::application

#endif  // DESIGNPP_APPLICATION_RECENT_WORKSPACE_STORE_H_

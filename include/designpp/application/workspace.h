// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_APPLICATION_WORKSPACE_H_
#define DESIGNPP_APPLICATION_WORKSPACE_H_

#include <string>

namespace designpp::application {

struct WorkspaceOpenRequest {
  std::string library_id;
  std::string cell_id;
  std::string view_id;
};

}  // namespace designpp::application

#endif  // DESIGNPP_APPLICATION_WORKSPACE_H_

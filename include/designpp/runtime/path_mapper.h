// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_RUNTIME_PATH_MAPPER_H_
#define DESIGNPP_RUNTIME_PATH_MAPPER_H_

#include <filesystem>
#include <string>

#include "designpp/core/status.h"

namespace designpp::runtime {

class PathMapper final {
 public:
  [[nodiscard]] core::Result<std::wstring> WindowsToWsl(
      const std::filesystem::path& path) const;
};

}  // namespace designpp::runtime

#endif  // DESIGNPP_RUNTIME_PATH_MAPPER_H_

// Copyright 2026 The Design++ Authors

#include "designpp/runtime/path_mapper.h"

#include <algorithm>
#include <cwctype>

namespace designpp::runtime {

core::Result<std::wstring> PathMapper::WindowsToWsl(
    const std::filesystem::path& path) const {
  const std::filesystem::path normalized = path.lexically_normal();
  const std::wstring native = normalized.native();
  if (!normalized.is_absolute() || native.size() < 3 || native[1] != L':' ||
      (native[2] != L'\\' && native[2] != L'/')) {
    return core::Status{core::ErrorCode::kInvalidArgument,
                        "Only absolute Windows drive paths can map to WSL", 0};
  }
  std::wstring result = L"/mnt/";
  result.push_back(static_cast<wchar_t>(std::towlower(native[0])));
  for (std::size_t index = 2; index < native.size(); ++index) {
    result.push_back(native[index] == L'\\' ? L'/' : native[index]);
  }
  return result;
}

}  // namespace designpp::runtime

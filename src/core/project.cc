#include "designpp/core/project.h"

#include <unordered_set>

namespace designpp::core {

std::vector<std::wstring> Project::Validate() const {
  std::vector<std::wstring> errors;

  if (name.empty()) {
    errors.emplace_back(L"프로젝트 이름이 필요합니다.");
  }
  if (top_module.empty()) {
    errors.emplace_back(L"최상위 RTL 모듈이 필요합니다.");
  }
  if (rtl_sources.empty()) {
    errors.emplace_back(L"RTL 소스가 하나 이상 필요합니다.");
  }

  std::unordered_set<std::filesystem::path> unique_sources;
  for (const auto& source : rtl_sources) {
    if (source.empty()) {
      errors.emplace_back(L"비어 있는 RTL 소스 경로가 있습니다.");
      continue;
    }
    if (!unique_sources.insert(source.lexically_normal()).second) {
      errors.emplace_back(L"중복 RTL 소스가 있습니다: " + source.wstring());
    }
  }

  return errors;
}

}  // namespace designpp::core

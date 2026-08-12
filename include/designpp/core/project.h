#ifndef DESIGNPP_CORE_PROJECT_H_
#define DESIGNPP_CORE_PROJECT_H_

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace designpp::core {

/** Selects the backend that owns an RTL-to-GDS run. */
enum class FlowBackend {
  kOpenLane2,
  kOpenRoadFlowScripts,
};

/** Describes a Design++ project independently of the GUI and execution host. */
struct Project {
  std::wstring name;
  std::string top_module;
  std::vector<std::filesystem::path> rtl_sources;
  std::vector<std::filesystem::path> include_directories;
  std::vector<std::string> defines;
  std::optional<std::filesystem::path> constraints;
  std::optional<std::filesystem::path> testbench_directory;
  std::string platform;
  FlowBackend flow_backend{FlowBackend::kOpenLane2};

  // Returns human-readable validation failures.
  [[nodiscard]] std::vector<std::wstring> Validate() const;
};

}  // namespace designpp::core

#endif  // DESIGNPP_CORE_PROJECT_H_

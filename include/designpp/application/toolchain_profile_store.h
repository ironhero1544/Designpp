// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_APPLICATION_TOOLCHAIN_PROFILE_STORE_H_
#define DESIGNPP_APPLICATION_TOOLCHAIN_PROFILE_STORE_H_

#include <filesystem>

#include "designpp/core/status.h"
#include "designpp/core/toolchain_profile.h"

namespace designpp::application {

class ToolchainProfileStore final {
 public:
  ToolchainProfileStore();
  explicit ToolchainProfileStore(std::filesystem::path settings_path);

  [[nodiscard]] core::Result<core::ToolchainSettings> Load() const;
  [[nodiscard]] core::Result<core::ToolchainSettings> LoadOrCreateDefaults()
      const;
  [[nodiscard]] core::Status Save(
      const core::ToolchainSettings& settings) const;
  // Atomically rejects a stale settings update. The caller supplies the
  // revision loaded before editing and persists expected_revision + 1.
  [[nodiscard]] core::Status Save(const core::ToolchainSettings& settings,
                                  std::uint64_t expected_revision) const;
  [[nodiscard]] const std::filesystem::path& settings_path() const noexcept;

  [[nodiscard]] static core::ToolchainSettings CreateDefaults();
  [[nodiscard]] static core::Result<std::filesystem::path>
  DefaultSettingsPath();

 private:
  std::filesystem::path settings_path_;
  core::Status initialization_status_;
};

}  // namespace designpp::application

#endif  // DESIGNPP_APPLICATION_TOOLCHAIN_PROFILE_STORE_H_

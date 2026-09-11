// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_APPLICATION_SYNTHESIS_FINGERPRINT_H_
#define DESIGNPP_APPLICATION_SYNTHESIS_FINGERPRINT_H_

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "designpp/application/project_service.h"
#include "designpp/core/project.h"
#include "designpp/core/status.h"

namespace designpp::application {

struct LibertyFingerprint {
  std::string relative_path;
  std::string sha256;
};

struct SynthesisFingerprint {
  std::string configuration_sha256;
  std::vector<LibertyFingerprint> liberty_files;
};

[[nodiscard]] core::Result<std::string> CalculateFileSha256(
    const std::filesystem::path& path);

[[nodiscard]] core::Result<std::string> CalculateSha256(std::string_view bytes);

[[nodiscard]] core::Result<SynthesisFingerprint> CalculateSynthesisFingerprint(
    const core::Project& project, const std::vector<ResolvedSource>& sources,
    const std::filesystem::path& library_directory);

}  // namespace designpp::application

#endif  // DESIGNPP_APPLICATION_SYNTHESIS_FINGERPRINT_H_

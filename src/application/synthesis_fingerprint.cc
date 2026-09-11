// Copyright 2026 The Design++ Authors

#include "designpp/application/synthesis_fingerprint.h"

#include <windows.h>

#include <bcrypt.h>

#include <array>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <vector>

namespace designpp::application {
namespace {

class Sha256 final {
 public:
  Sha256() {
    if (BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM,
                                    nullptr, 0) < 0) {
      return;
    }
    DWORD size = 0;
    DWORD copied = 0;
    if (BCryptGetProperty(algorithm_, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&size), sizeof(size),
                          &copied, 0) < 0) {
      return;
    }
    object_.resize(size);
    valid_ = BCryptCreateHash(algorithm_, &hash_, object_.data(),
                              static_cast<ULONG>(object_.size()), nullptr, 0,
                              0) >= 0;
  }

  Sha256(const Sha256&) = delete;
  Sha256& operator=(const Sha256&) = delete;

  ~Sha256() {
    if (hash_ != nullptr) BCryptDestroyHash(hash_);
    if (algorithm_ != nullptr) BCryptCloseAlgorithmProvider(algorithm_, 0);
  }

  [[nodiscard]] bool valid() const { return valid_; }

  [[nodiscard]] bool Append(std::string_view bytes) {
    return valid_ &&
           BCryptHashData(
               hash_, reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),
               static_cast<ULONG>(bytes.size()), 0) >= 0;
  }

  [[nodiscard]] core::Result<std::string> Finish() {
    std::array<unsigned char, 32> digest{};
    if (!valid_ || BCryptFinishHash(hash_, digest.data(),
                                    static_cast<ULONG>(digest.size()), 0) < 0) {
      return core::Status{core::ErrorCode::kIoError, "Cannot calculate SHA-256",
                          0};
    }
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const unsigned char byte : digest) output << std::setw(2) << +byte;
    return output.str();
  }

 private:
  BCRYPT_ALG_HANDLE algorithm_ = nullptr;
  BCRYPT_HASH_HANDLE hash_ = nullptr;
  std::vector<unsigned char> object_;
  bool valid_ = false;
};

core::Status AppendFile(const std::filesystem::path& path, Sha256* hash) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return {core::ErrorCode::kNotFound, "Fingerprint input is missing", 0};
  }
  std::array<char, 64 * 1024> buffer{};
  while (input.read(buffer.data(), buffer.size()) || input.gcount() > 0) {
    if (!hash->Append(std::string_view(
            buffer.data(), static_cast<std::size_t>(input.gcount())))) {
      return {core::ErrorCode::kIoError, "Cannot hash fingerprint input", 0};
    }
  }
  return input.eof()
             ? core::Status::Success()
             : core::Status{core::ErrorCode::kIoError,
                            "Cannot finish reading fingerprint input", 0};
}

}  // namespace

core::Result<std::string> CalculateFileSha256(
    const std::filesystem::path& path) {
  Sha256 hash;
  if (!hash.valid()) {
    return core::Status{core::ErrorCode::kIoError,
                        "SHA-256 provider is unavailable", 0};
  }
  const core::Status status = AppendFile(path, &hash);
  return status.Ok() ? hash.Finish() : core::Result<std::string>(status);
}

core::Result<std::string> CalculateSha256(std::string_view bytes) {
  Sha256 hash;
  if (!hash.valid() || !hash.Append(bytes)) {
    return core::Status{core::ErrorCode::kIoError,
                        "Cannot hash in-memory input", 0};
  }
  return hash.Finish();
}

core::Result<SynthesisFingerprint> CalculateSynthesisFingerprint(
    const core::Project& project, const std::vector<ResolvedSource>& sources,
    const std::filesystem::path& library_directory) {
  Sha256 aggregate;
  if (!aggregate.valid()) {
    return core::Status{core::ErrorCode::kIoError,
                        "SHA-256 provider is unavailable", 0};
  }
  const auto append_field = [&aggregate](std::string_view value) {
    const std::string size = std::to_string(value.size());
    return aggregate.Append(size) && aggregate.Append(":") &&
           aggregate.Append(value) && aggregate.Append("\n");
  };
  if (!append_field(project.top_module) ||
      !append_field(project.synthesis.flatten ? "flatten" : "hierarchy")) {
    return core::Status{core::ErrorCode::kIoError,
                        "Cannot hash synthesis configuration", 0};
  }
  for (const std::string& define : project.defines) {
    if (!append_field(define)) {
      return core::Status{core::ErrorCode::kIoError,
                          "Cannot hash synthesis define", 0};
    }
  }
  for (const std::string& include : project.include_directories) {
    if (!append_field(include)) {
      return core::Status{core::ErrorCode::kIoError,
                          "Cannot hash synthesis include", 0};
    }
  }
  for (const ResolvedSource& source : sources) {
    if (!source.enabled || !source.exists ||
        source.view_kind != core::ViewKind::kVerilog) {
      continue;
    }
    if (!append_field(source.relative_path)) {
      return core::Status{core::ErrorCode::kIoError,
                          "Cannot hash synthesis source", 0};
    }
    auto digest = CalculateFileSha256(source.windows_path);
    if (!digest.Ok()) return digest.GetStatus();
    if (!append_field(digest.Value())) {
      return core::Status{core::ErrorCode::kIoError,
                          "Cannot hash synthesis source digest", 0};
    }
  }

  SynthesisFingerprint result;
  for (const std::string& relative_path : project.synthesis.liberty_paths) {
    const std::filesystem::path path = library_directory / relative_path;
    auto digest = CalculateFileSha256(path);
    if (!digest.Ok()) return digest.GetStatus();
    result.liberty_files.push_back({relative_path, digest.Value()});
    if (!append_field(relative_path) || !append_field(digest.Value())) {
      return core::Status{core::ErrorCode::kIoError,
                          "Cannot hash synthesis Liberty", 0};
    }
  }
  auto finished = aggregate.Finish();
  if (!finished.Ok()) return finished.GetStatus();
  result.configuration_sha256 = std::move(finished).Value();
  return result;
}

}  // namespace designpp::application

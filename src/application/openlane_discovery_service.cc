// Copyright 2026 The Design++ Authors

#include "designpp/application/openlane_discovery_service.h"

#include <windows.h>

#include <algorithm>
#include <sstream>
#include <tuple>
#include <utility>

namespace designpp::application {
namespace {

std::wstring Utf8ToWide(std::string_view text) {
  if (text.empty()) return {};
  const int length =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), nullptr, 0);
  if (length <= 0) return {};
  std::wstring result(static_cast<std::size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                      static_cast<int>(text.size()), result.data(), length);
  return result;
}

std::vector<OpenLanePdkCandidate> ParseCandidates(std::string_view output) {
  std::vector<OpenLanePdkCandidate> candidates;
  std::istringstream lines{std::string(output)};
  std::string line;
  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const std::size_t separator = line.find('|');
    if (separator == std::string::npos || separator == 0 ||
        separator + 1 >= line.size()) {
      continue;
    }
    OpenLanePdkCandidate candidate{line.substr(0, separator),
                                   line.substr(separator + 1)};
    const auto duplicate = std::find_if(
        candidates.begin(), candidates.end(), [&](const auto& existing) {
          return existing.pdk == candidate.pdk &&
                 existing.standard_cell_library ==
                     candidate.standard_cell_library;
        });
    if (duplicate == candidates.end())
      candidates.push_back(std::move(candidate));
  }
  std::sort(candidates.begin(), candidates.end(),
            [](const auto& left, const auto& right) {
              return std::tie(left.pdk, left.standard_cell_library) <
                     std::tie(right.pdk, right.standard_cell_library);
            });
  return candidates;
}

}  // namespace

OpenLaneDiscoveryService::OpenLaneDiscoveryService(
    runtime::ExecutionProvider* provider)
    : provider_(provider), live_(std::make_shared<std::atomic_bool>(true)) {}

OpenLaneDiscoveryService::~OpenLaneDiscoveryService() { Cancel(); }

core::Status OpenLaneDiscoveryService::Start(
    const core::ToolchainProfile& profile, std::uint64_t generation,
    OpenLaneDiscoverySink sink) {
  if (provider_ == nullptr || generation == 0 || profile.pdk_root.empty()) {
    return {core::ErrorCode::kInvalidArgument,
            "OpenLane PDK discovery input is incomplete", 0};
  }
  Cancel();
  live_ = std::make_shared<std::atomic_bool>(true);
  const auto live = live_;
  runtime::WslCommand command;
  command.program = L"/bin/bash";
  command.arguments = {
      L"-lc",
      L"root=\"$1\"; case \"$root\" in '~/'*) "
      L"root=\"$HOME/${root#\\~/}\";; esac; "
      L"find \"$root\" -type d -name libs.ref 2>/dev/null | "
      L"while IFS= read -r reference; do "
      L"pdk=$(basename \"$(dirname \"$reference\")\"); "
      L"find \"$reference\" -mindepth 1 -maxdepth 1 -type d "
      L"-printf '%f\\n' 2>/dev/null | while IFS= read -r scl; do "
      L"case \"$scl\" in *_sc_*) "
      L"printf '%s|%s\\n' \"$pdk\" \"$scl\";; esac; done; done",
      L"designpp-openlane-pdk-discovery", Utf8ToWide(profile.pdk_root)};
  if (!profile.wsl_distribution.empty()) {
    command.distribution = Utf8ToWide(profile.wsl_distribution);
  }
  runtime::ExecutionStartResult started = provider_->Start(
      command, [](std::string) {},
      [live, generation,
       sink = std::move(sink)](runtime::ProcessResult result) mutable {
        if (!live->load() || !sink) return;
        OpenLaneDiscoveryResult discovery;
        discovery.generation = generation;
        if (!result.started || result.cancelled || result.exit_code != 0) {
          discovery.status =
              result.cancelled
                  ? core::Status{core::ErrorCode::kCancelled,
                                 "OpenLane PDK discovery cancelled", 0}
                  : core::Status{core::ErrorCode::kNotFound,
                                 "OpenLane PDK root cannot be explored", 0};
        } else {
          discovery.candidates = ParseCandidates(result.output);
          discovery.status =
              discovery.candidates.empty()
                  ? core::Status{core::ErrorCode::kNotFound,
                                 "No installed OpenLane PDK/SCL found", 0}
                  : core::Status::Success();
        }
        sink(std::move(discovery));
      });
  if (!started.Ok()) return started.status;
  handle_ = std::move(started.handle);
  return core::Status::Success();
}

void OpenLaneDiscoveryService::Cancel() noexcept {
  if (live_) live_->store(false);
  if (handle_) handle_->Cancel();
  handle_.reset();
}

}  // namespace designpp::application

// Copyright 2026 The Design++ Authors

#include "designpp/application/toolchain_environment_service.h"

#include <algorithm>
#include <utility>

namespace designpp::application {
namespace {

const core::InstalledToolchainEnvironment* FindEnvironment(
    const core::ToolchainSettings& settings, std::string_view id) {
  const auto found = std::find_if(
      settings.environments.begin(), settings.environments.end(),
      [id](const core::InstalledToolchainEnvironment& environment) {
        return environment.id == id;
      });
  return found == settings.environments.end() ? nullptr : &*found;
}

std::string ActiveId(const core::ToolchainProfile& profile,
                     std::string_view provider_id) {
  if (provider_id == "openlane2") {
    return profile.active_openlane_environment_id;
  }
  if (provider_id == "orfs") return profile.active_orfs_environment_id;
  return {};
}

ResolvedToolchainEnvironment ResolveValue(
    const core::InstalledToolchainEnvironment& environment,
    const core::ToolchainProfile& profile) {
  ResolvedToolchainEnvironment result;
  result.installation_id = environment.id;
  result.provider_id = environment.provider_id;
  result.bundle_id = environment.bundle_id;
  result.version = environment.version;
  result.root = environment.root;
  result.executable = environment.executable;
  result.fingerprint = environment.fingerprint;
  result.wsl_distribution = profile.wsl_distribution;
  result.verified = environment.verified;
  return result;
}

}  // namespace

core::Result<ResolvedToolchainEnvironment> ToolchainInventoryService::Resolve(
    const core::ToolchainSettings& settings,
    const core::ToolchainProfile& profile, std::string_view provider_id) const {
  const std::string id = ActiveId(profile, provider_id);
  if (id.empty()) {
    return core::Status{core::ErrorCode::kNotFound,
                        "Toolchain environment preparation is required", 0};
  }
  const core::InstalledToolchainEnvironment* environment =
      FindEnvironment(settings, id);
  if (environment == nullptr || environment->provider_id != provider_id) {
    return core::Status{core::ErrorCode::kCorruptData,
                        "Active toolchain environment is incompatible", 0};
  }
  return ResolveValue(*environment, profile);
}

ToolInventoryResult ToolchainInventoryService::Inspect(
    const core::ToolchainSettings& settings,
    const core::ToolchainProfile& profile, std::string_view provider_id) const {
  ToolInventoryResult result;
  auto resolved = Resolve(settings, profile, provider_id);
  if (!resolved.Ok()) {
    result.state = resolved.GetStatus().code == core::ErrorCode::kNotFound
                       ? ToolInventoryState::kPreparationRequired
                       : ToolInventoryState::kIncompatible;
    result.summary = resolved.GetStatus().message;
    return result;
  }
  result.environment = std::move(resolved).Value();
  result.state = ToolInventoryState::kReady;
  result.summary = result.environment.verified
                       ? "Prepared Design++ verified environment"
                       : "Prepared user environment; compatibility unverified";
  return result;
}

runtime::WslCommand ToolchainInventoryService::BuildVersionProbe(
    const ResolvedToolchainEnvironment& environment) const {
  runtime::WslCommand command;
  command.program = std::wstring(environment.executable.begin(),
                                 environment.executable.end());
  command.arguments = {L"--version"};
  command.distribution = std::wstring(environment.wsl_distribution.begin(),
                                      environment.wsl_distribution.end());
  return command;
}

ToolchainPreparationService::ToolchainPreparationService(
    ToolchainProfileStore* store)
    : store_(store) {}

core::Result<ResolvedToolchainEnvironment> ToolchainPreparationService::Adopt(
    ToolchainAdoptionRequest request) {
  if (store_ == nullptr) {
    return core::Status{core::ErrorCode::kInvalidArgument,
                        "Toolchain settings store is unavailable", 0};
  }
  auto loaded = store_->LoadOrCreateDefaults();
  if (!loaded.Ok()) return loaded.GetStatus();
  core::ToolchainSettings settings = std::move(loaded).Value();
  const std::uint64_t expected_revision = settings.revision;
  auto profile =
      std::find_if(settings.profiles.begin(), settings.profiles.end(),
                   [&request](const core::ToolchainProfile& value) {
                     return value.id == request.profile_id;
                   });
  if (profile == settings.profiles.end()) {
    return core::Status{core::ErrorCode::kNotFound,
                        "Toolchain profile does not exist", 0};
  }
  auto existing = std::find_if(
      settings.environments.begin(), settings.environments.end(),
      [&request](const core::InstalledToolchainEnvironment& value) {
        return value.id == request.environment.id;
      });
  if (existing == settings.environments.end()) {
    settings.environments.push_back(request.environment);
  } else if (existing->fingerprint != request.environment.fingerprint) {
    return core::Status{core::ErrorCode::kConflict,
                        "Environment ID belongs to another immutable revision",
                        0};
  }
  if (request.environment.provider_id == "openlane2") {
    profile->rollback_openlane_environment_id =
        profile->active_openlane_environment_id;
    profile->active_openlane_environment_id = request.environment.id;
  } else if (request.environment.provider_id == "orfs") {
    profile->rollback_orfs_environment_id = profile->active_orfs_environment_id;
    profile->active_orfs_environment_id = request.environment.id;
  } else {
    return core::Status{core::ErrorCode::kInvalidArgument,
                        "Unsupported managed toolchain provider", 0};
  }
  settings.revision = expected_revision + 1;
  const core::Status saved = store_->Save(settings, expected_revision);
  if (!saved.Ok()) return saved;
  return ResolveValue(request.environment, *profile);
}

core::Status ToolchainPreparationService::Rollback(
    std::string_view profile_id, std::string_view provider_id) {
  if (store_ == nullptr) {
    return {core::ErrorCode::kInvalidArgument,
            "Toolchain settings store is unavailable", 0};
  }
  auto loaded = store_->Load();
  if (!loaded.Ok()) return loaded.GetStatus();
  core::ToolchainSettings settings = std::move(loaded).Value();
  const std::uint64_t expected_revision = settings.revision;
  auto profile =
      std::find_if(settings.profiles.begin(), settings.profiles.end(),
                   [profile_id](const core::ToolchainProfile& value) {
                     return value.id == profile_id;
                   });
  if (profile == settings.profiles.end()) {
    return {core::ErrorCode::kNotFound, "Toolchain profile does not exist", 0};
  }
  std::string* active = nullptr;
  std::string* rollback = nullptr;
  if (provider_id == "openlane2") {
    active = &profile->active_openlane_environment_id;
    rollback = &profile->rollback_openlane_environment_id;
  } else if (provider_id == "orfs") {
    active = &profile->active_orfs_environment_id;
    rollback = &profile->rollback_orfs_environment_id;
  }
  if (active == nullptr || rollback->empty() ||
      FindEnvironment(settings, *rollback) == nullptr) {
    return {core::ErrorCode::kNotFound,
            "No compatible rollback environment is available", 0};
  }
  std::swap(*active, *rollback);
  settings.revision = expected_revision + 1;
  return store_->Save(settings, expected_revision);
}

}  // namespace designpp::application

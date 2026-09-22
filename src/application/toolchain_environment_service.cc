// Copyright 2026 The Design++ Authors

#include "designpp/application/toolchain_environment_service.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <utility>

#include "designpp/adapters/openlane2_adapter.h"
#include "designpp/adapters/orfs_adapter.h"

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
  result.command_contract_id = environment.command_contract_id;
  result.framework_revision = environment.framework_revision;
  result.lock_hash = environment.lock_hash;
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
  result.state = ToolInventoryState::kUnchecked;
  result.summary =
      "Registered environment; a fresh compatibility probe is required";
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
  const auto compatible =
      core::ValidateToolchainCompatibility(request.evidence);
  if (!compatible.Ok()) return compatible;
  if (request.environment.provider_id != request.evidence.provider_id ||
      request.environment.bundle_id != request.evidence.bundle_id ||
      request.environment.fingerprint !=
          request.evidence.environment_fingerprint) {
    return core::Status{core::ErrorCode::kConflict,
                        "Probe evidence belongs to another environment", 0};
  }
  request.environment.command_contract_id =
      request.evidence.command_contract_id;
  request.environment.framework_revision = request.evidence.framework_revision;
  request.environment.lock_hash = request.evidence.lock_hash;
  request.environment.verified = true;
  auto loaded = store_->LoadOrCreateDefaults();
  if (!loaded.Ok()) return loaded.GetStatus();
  core::ToolchainSettings settings = std::move(loaded).Value();
  const std::uint64_t expected_revision = settings.revision;
  if (request.expected_revision != 0 &&
      request.expected_revision != expected_revision) {
    return core::Status{
        core::ErrorCode::kConflict,
        "Toolchain settings changed while the probe was running", 0};
  }
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
  const bool already_registered = existing != settings.environments.end();
  if (!already_registered) {
    settings.environments.push_back(request.environment);
  } else if (existing->fingerprint != request.environment.fingerprint ||
             existing->provider_id != request.environment.provider_id ||
             existing->bundle_id != request.environment.bundle_id ||
             existing->root != request.environment.root ||
             existing->executable != request.environment.executable ||
             existing->command_contract_id !=
                 request.environment.command_contract_id) {
    return core::Status{core::ErrorCode::kConflict,
                        "Environment ID belongs to another immutable revision",
                        0};
  }
  if (already_registered &&
      (!request.activate ||
       ActiveId(*profile, request.environment.provider_id) ==
           request.environment.id)) {
    return ResolveValue(request.environment, *profile);
  }
  if (!request.activate) {
    // Registration publishes an installation without selecting it for Runs.
  } else if (request.environment.provider_id == "openlane2") {
    profile->rollback_openlane_environment_id =
        profile->active_openlane_environment_id;
    profile->active_openlane_environment_id = request.environment.id;
    profile->openlane_root = request.environment.root;
    profile->openlane_mode = "managed";
    profile->openlane_bundle_id = request.environment.bundle_id;
  } else if (request.environment.provider_id == "orfs") {
    profile->rollback_orfs_environment_id = profile->active_orfs_environment_id;
    profile->active_orfs_environment_id = request.environment.id;
    profile->orfs_root = request.environment.root;
    profile->orfs_mode = "managed";
    profile->orfs_bundle_id = request.environment.bundle_id;
  } else {
    return core::Status{core::ErrorCode::kInvalidArgument,
                        "Unsupported managed toolchain provider", 0};
  }
  settings.revision = expected_revision + 1;
  const core::Status saved = store_->Save(settings, expected_revision);
  if (!saved.Ok()) return saved;
  return ResolveValue(request.environment, *profile);
}

core::Result<ResolvedToolchainEnvironment>
ToolchainPreparationService::Register(ToolchainAdoptionRequest request) {
  request.activate = false;
  return Adopt(std::move(request));
}

core::Result<core::ToolchainCompatibilityEvidence> ProbeToolchainCompatibility(
    std::string_view provider_id, const runtime::ProcessResult& result) {
  if (result.cancelled) {
    return core::Status{core::ErrorCode::kCancelled,
                        "Toolchain probe cancelled", 0};
  }
  if (!result.started || result.exit_code != 0) {
    return core::Status{core::ErrorCode::kIoError,
                        "Toolchain probe failed (exit " +
                            std::to_string(result.exit_code) + ")",
                        result.exit_code};
  }
  if (result.output.size() > 1024 * 1024) {
    return core::Status{core::ErrorCode::kFileTooLarge,
                        "Toolchain probe output exceeds limit", 0};
  }
  std::map<std::string, std::string> fields;
  std::istringstream input(result.output);
  std::string line;
  constexpr std::string_view kPrefix = "DESIGNPP_COMPAT_";
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!line.starts_with(kPrefix)) continue;
    const auto equal = line.find('=');
    if (equal == std::string::npos ||
        !fields
             .emplace(line.substr(kPrefix.size(), equal - kPrefix.size()),
                      line.substr(equal + 1))
             .second) {
      return core::Status{core::ErrorCode::kCorruptData,
                          "Ambiguous toolchain probe evidence", 0};
    }
  }
  if (fields["SCHEMA"] != "1" || fields["PROVIDER"] != provider_id) {
    return core::Status{core::ErrorCode::kCorruptData,
                        "Missing toolchain probe protocol", 0};
  }
  core::ToolchainCompatibilityEvidence evidence;
  evidence.provider_id = fields["PROVIDER"];
  evidence.bundle_id = fields["BUNDLE"];
  evidence.command_contract_id = fields["CONTRACT"];
  evidence.framework_revision = fields["REVISION"];
  evidence.lock_hash = fields["LOCK"];
  evidence.environment_fingerprint = fields["FINGERPRINT"];
  for (const auto& [key, value] : fields) {
    if (key.starts_with("DEPENDENCY_")) {
      evidence.dependency_revisions.emplace_back(key.substr(11), value);
    } else if (key.starts_with("FEATURE_") && value == "1") {
      evidence.features.push_back(key.substr(8));
    }
  }
  const auto status = core::ValidateToolchainCompatibility(evidence);
  if (!status.Ok()) return status;
  return evidence;
}

core::Status ToolchainPreparationService::Rollback(
    std::string_view profile_id, std::string_view provider_id,
    const core::ToolchainCompatibilityEvidence& evidence) {
  if (store_ == nullptr) {
    return {core::ErrorCode::kInvalidArgument,
            "Toolchain settings store is unavailable", 0};
  }
  const auto compatible = core::ValidateToolchainCompatibility(evidence);
  if (!compatible.Ok()) return compatible;
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
  const auto* target = FindEnvironment(settings, *rollback);
  if (target->provider_id != provider_id ||
      evidence.provider_id != provider_id ||
      target->fingerprint != evidence.environment_fingerprint) {
    return {core::ErrorCode::kConflict,
            "Rollback environment evidence does not match", 0};
  }
  ToolchainAdoptionRequest request;
  request.profile_id = profile_id;
  request.environment = *target;
  request.evidence = evidence;
  request.expected_revision = expected_revision;
  auto adopted = Adopt(std::move(request));
  return adopted.Ok() ? core::Status::Success() : adopted.GetStatus();
}

core::Result<ResolvedToolchainEnvironment> ManageToolchain(
    ToolchainProfileStore& store, runtime::ExecutionProvider& provider,
    std::string_view provider_id, std::string_view bundle_id,
    ToolchainManagementAction action, std::stop_token stop) {
  const auto* entry =
      core::ToolchainCompatibilityCatalog::Find(provider_id, bundle_id);
  if (!entry)
    return core::Status{core::ErrorCode::kNotFound,
                        "Unsupported toolchain bundle", 0};
  auto loaded = store.LoadOrCreateDefaults();
  if (!loaded.Ok()) return loaded.GetStatus();
  const auto& settings = loaded.Value();
  const auto selected =
      std::find_if(settings.profiles.begin(), settings.profiles.end(),
                   [&](const auto& value) {
                     return value.id == settings.selected_profile_id;
                   });
  if (selected == settings.profiles.end()) {
    return core::Status{core::ErrorCode::kNotFound,
                        "Selected profile is missing", 0};
  }
  auto profile = *selected;
  core::InstalledToolchainEnvironment environment;
  environment.provider_id = provider_id;
  environment.bundle_id = bundle_id;
  environment.version = entry->version;
  environment.root =
      "~/.designpp/toolchains/environments/" + std::string(bundle_id);
  if (action == ToolchainManagementAction::kInspectConfigured) {
    environment.root =
        provider_id == "orfs" ? profile.orfs_root : profile.openlane_root;
    const auto* active =
        FindEnvironment(settings, ActiveId(profile, provider_id));
    if (active) environment = *active;
  } else if (action == ToolchainManagementAction::kRollback) {
    const auto* previous = FindEnvironment(
        settings, provider_id == "orfs"
                      ? profile.rollback_orfs_environment_id
                      : profile.rollback_openlane_environment_id);
    if (!previous || previous->provider_id != provider_id) {
      return core::Status{core::ErrorCode::kNotFound,
                          "No rollback environment is available", 0};
    }
    environment = *previous;
  }
  if (provider_id == "orfs")
    profile.orfs_root = environment.root;
  else
    profile.openlane_root = environment.root;
  const auto command =
      provider_id == "orfs"
          ? adapters::OrfsAdapter().BuildProbeCommand(profile)
          : adapters::OpenLane2Adapter().BuildProbeCommand(profile);
  struct Completion {
    std::mutex mutex;
    std::condition_variable_any changed;
    bool done = false;
    runtime::ProcessResult result;
  };
  auto completion = std::make_shared<Completion>();
  if (stop.stop_requested())
    return core::Status{core::ErrorCode::kCancelled,
                        "Toolchain operation cancelled", 0};
  auto started = provider.Start(
      command, [](std::string) {},
      [completion](runtime::ProcessResult result) {
        {
          std::scoped_lock lock(completion->mutex);
          if (completion->done) return;
          completion->result = std::move(result);
          completion->done = true;
        }
        completion->changed.notify_all();
      });
  if (!started.Ok()) return started.status;
  runtime::ProcessResult process;
  {
    std::unique_lock lock(completion->mutex);
    const bool finished = completion->changed.wait_for(
        lock, stop, std::chrono::minutes(2), [&] { return completion->done; });
    if (!finished || stop.stop_requested()) {
      lock.unlock();
      started.handle->Cancel();
      started.handle.reset();
      return core::Status{stop.stop_requested() ? core::ErrorCode::kCancelled
                                                : core::ErrorCode::kIoError,
                          stop.stop_requested()
                              ? "Toolchain operation cancelled"
                              : "Toolchain probe timed out",
                          0};
    }
    process = std::move(completion->result);
  }
  started.handle.reset();
  const auto evidence = ProbeToolchainCompatibility(provider_id, process);
  if (!evidence.Ok()) return evidence.GetStatus();
  if (!environment.fingerprint.empty() &&
      environment.fingerprint != evidence.Value().environment_fingerprint) {
    return core::Status{core::ErrorCode::kExternalModification,
                        "Registered environment has changed", 0};
  }
  environment.fingerprint = evidence.Value().environment_fingerprint;
  environment.command_contract_id = evidence.Value().command_contract_id;
  environment.framework_revision = evidence.Value().framework_revision;
  environment.lock_hash = evidence.Value().lock_hash;
  environment.verified = true;
  if (environment.id.empty()) {
    // A selected distribution is part of the identity even for equal checkouts.
    environment.id = std::string(bundle_id) + ":" + environment.fingerprint;
    environment.executable =
        environment.root + "/" +
        (provider_id == "orfs" ? "flow/Makefile" : "shell.nix");
  }
  if (action == ToolchainManagementAction::kInspectConfigured) {
    return ResolveValue(environment, profile);
  }
  ToolchainAdoptionRequest adoption;
  adoption.profile_id = profile.id;
  adoption.environment = environment;
  adoption.evidence = evidence.Value();
  adoption.expected_revision = settings.revision;
  adoption.activate = action != ToolchainManagementAction::kRegisterPrepared;
  ToolchainPreparationService service(&store);
  return service.Adopt(std::move(adoption));
}

}  // namespace designpp::application

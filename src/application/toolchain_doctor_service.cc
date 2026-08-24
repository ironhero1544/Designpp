// Copyright 2026 The Design++ Authors

#include "designpp/application/toolchain_doctor_service.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <mutex>
#include <utility>

#include "designpp/runtime/task_scheduler.h"

namespace designpp::application {
namespace {

std::wstring Utf8ToWide(std::string_view text) {
  if (text.empty()) return {};
  const int size =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), nullptr, 0);
  if (size <= 0) return {};
  std::wstring result(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                      static_cast<int>(text.size()), result.data(), size);
  return result;
}

std::string WideToUtf8(std::wstring_view text) {
  if (text.empty()) return {};
  const int size = WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
      nullptr, 0, nullptr, nullptr);
  if (size <= 0) return {};
  std::string result(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                      static_cast<int>(text.size()), result.data(), size,
                      nullptr, nullptr);
  return result;
}

std::wstring ToolPath(std::string_view root, std::wstring_view suffix) {
  std::string_view normalized = root;
  if (normalized.starts_with("~/")) normalized.remove_prefix(2);
  std::wstring result = Utf8ToWide(normalized);
  if (!result.empty() && result.back() != L'/') result.push_back(L'/');
  result.append(suffix);
  return result;
}

bool ContainsCaseInsensitive(std::string_view text, std::string_view needle) {
  return std::search(text.begin(), text.end(), needle.begin(), needle.end(),
                     [](char left, char right) {
                       return std::tolower(static_cast<unsigned char>(left)) ==
                              std::tolower(static_cast<unsigned char>(right));
                     }) != text.end();
}

}  // namespace

struct ToolchainDoctorService::Implementation final
    : std::enable_shared_from_this<Implementation> {
  explicit Implementation(runtime::ExecutionProvider* execution_provider)
      : provider(execution_provider) {}

  void Emit(DoctorEvent event) {
    DoctorEventSink callback;
    {
      std::scoped_lock lock(mutex);
      if (shutdown || !sink || event.generation != generation) return;
      callback = sink;
    }
    callback(std::move(event));
  }

  void EmitCheck(DoctorCheckResult result) {
    DoctorEvent event;
    event.kind = DoctorEventKind::kCheckCompleted;
    event.generation = generation;
    event.check = std::move(result);
    Emit(std::move(event));
  }

  void Finish(core::Status status) {
    DoctorEventSink callback;
    std::uint64_t event_generation = 0;
    {
      std::scoped_lock lock(mutex);
      if (shutdown || terminal_delivered || !active || !sink) return;
      terminal_delivered = true;
      active = false;
      event_generation = generation;
      callback = sink;
    }
    DoctorEvent event;
    event.kind = DoctorEventKind::kCompleted;
    event.generation = event_generation;
    event.status = std::move(status);
    callback(std::move(event));
  }

  runtime::WslCommand BuildCommand(DoctorCheckId id) const {
    runtime::WslCommand command;
    if (!profile.wsl_distribution.empty()) {
      command.distribution = Utf8ToWide(profile.wsl_distribution);
    }
    if (id == DoctorCheckId::kWsl2) {
      command.program = L"/usr/bin/uname";
      command.arguments = {L"-r"};
      return command;
    }
    command.program = L"/usr/bin/test";
    if (id == DoctorCheckId::kOpenLane) {
      command.arguments = {L"-f",
                           ToolPath(profile.openlane_root, L"shell.nix")};
    } else if (id == DoctorCheckId::kOrfs) {
      command.arguments = {L"-f",
                           ToolPath(profile.orfs_root, L"flow/Makefile")};
    } else if (id == DoctorCheckId::kPdk) {
      command.arguments = {L"-d", ToolPath(profile.pdk_root, L"")};
    }
    return command;
  }

  void StartNext() {
    DoctorCheckId id = DoctorCheckId::kProfile;
    bool cancelled = false;
    bool finished = false;
    bool diagnosis_passed = false;
    std::uint64_t operation_id = 0;
    {
      std::scoped_lock lock(mutex);
      if (shutdown || !active) return;
      if (cancellation_requested) {
        cancelled = true;
      } else if (next_check < checks.size()) {
        id = checks[next_check++];
        completion_claimed = false;
        operation_id = ++operation_sequence;
        current_operation = operation_id;
      } else {
        finished = true;
        diagnosis_passed = all_required_passed;
      }
    }
    if (cancelled) {
      Finish({core::ErrorCode::kCancelled, "Toolchain diagnosis cancelled", 0});
      return;
    }
    if (finished) {
      Finish(diagnosis_passed
                 ? core::Status::Success()
                 : core::Status{core::ErrorCode::kNotFound,
                                "Toolchain diagnosis found required failures",
                                0});
      return;
    }
    if (id == DoctorCheckId::kPdk && profile.pdk_root.empty()) {
      {
        std::scoped_lock lock(mutex);
        all_required_passed = false;
      }
      EmitCheck({id, false, true, "PDK root is not configured", {}});
      StartNext();
      return;
    }
    const auto self = shared_from_this();
    {
      std::scoped_lock lock(mutex);
      output.clear();
    }
    auto started = provider->Start(
        BuildCommand(id),
        [self, operation_id](std::string output) {
          std::scoped_lock lock(self->mutex);
          if (!self->active || self->shutdown ||
              self->current_operation != operation_id) {
            return;
          }
          self->output.append(output);
        },
        [self, id, operation_id](runtime::ProcessResult result) mutable {
          {
            std::scoped_lock lock(self->mutex);
            if (!self->active || self->shutdown || self->completion_claimed ||
                self->current_operation != operation_id) {
              return;
            }
            self->completion_claimed = true;
          }
          self->CompleteCheck(id, std::move(result));
        });
    if (!started.Ok()) {
      {
        std::scoped_lock lock(mutex);
        if (current_operation != operation_id) return;
        completion_claimed = true;
        all_required_passed = false;
      }
      EmitCheck({id, false, true, started.status.message, {}});
      StartNext();
      return;
    }
    {
      std::scoped_lock lock(mutex);
      if (active && current_operation == operation_id && !completion_claimed) {
        handle = std::move(started.handle);
        if (cancellation_requested && handle) handle->Cancel();
      }
    }
  }

  void CompleteCheck(DoctorCheckId id, runtime::ProcessResult result) {
    std::string captured;
    {
      std::scoped_lock lock(mutex);
      captured.swap(output);
    }
    if (captured.empty()) captured = result.output;
    bool passed = result.started && !result.cancelled &&
                  result.error_message.empty() && result.exit_code == 0;
    std::string message;
    if (id == DoctorCheckId::kWsl2 && passed &&
        !ContainsCaseInsensitive(captured, "wsl2")) {
      passed = false;
      message = "Selected distribution is not running as WSL2";
    } else if (passed) {
      message = std::string(DoctorCheckName(id)) + " is ready";
    } else if (!result.error_message.empty()) {
      message = WideToUtf8(result.error_message);
    } else {
      message = std::string(DoctorCheckName(id)) + " is unavailable";
    }
    {
      std::scoped_lock lock(mutex);
      all_required_passed = all_required_passed && passed;
    }
    EmitCheck({id, passed, true, std::move(message), std::move(captured)});
    if (result.cancelled) {
      Finish({core::ErrorCode::kCancelled, "Toolchain diagnosis cancelled", 0});
      return;
    }
    const auto self = shared_from_this();
    if (!scheduler.Submit([self](std::stop_token token) {
          if (!token.stop_requested()) self->StartNext();
        })) {
      Finish({core::ErrorCode::kConflict,
              "Toolchain diagnosis queue is unavailable", 0});
    }
  }

  runtime::ExecutionProvider* provider = nullptr;
  runtime::TaskScheduler scheduler{1};
  mutable std::mutex mutex;
  core::ToolchainProfile profile;
  DoctorEventSink sink;
  std::unique_ptr<runtime::ExecutionHandle> handle;
  std::string output;
  std::uint64_t generation = 0;
  const std::array<DoctorCheckId, 4> checks = {
      DoctorCheckId::kWsl2, DoctorCheckId::kOpenLane, DoctorCheckId::kOrfs,
      DoctorCheckId::kPdk};
  std::size_t next_check = 0;
  std::uint64_t operation_sequence = 0;
  std::uint64_t current_operation = 0;
  bool all_required_passed = true;
  bool active = false;
  bool cancellation_requested = false;
  bool completion_claimed = false;
  bool terminal_delivered = false;
  bool shutdown = false;
};

ToolchainDoctorService::ToolchainDoctorService(
    runtime::ExecutionProvider* provider)
    : implementation_(std::make_shared<Implementation>(provider)) {}

ToolchainDoctorService::~ToolchainDoctorService() { Shutdown(); }

core::Status ToolchainDoctorService::Start(core::ToolchainProfile profile,
                                           std::uint64_t generation,
                                           DoctorEventSink sink) {
  if (!implementation_ || !implementation_->provider || !sink ||
      generation == 0) {
    return {core::ErrorCode::kInvalidArgument,
            "Toolchain doctor request is incomplete", 0};
  }
  core::ToolchainSettings settings;
  settings.selected_profile_id = profile.id;
  settings.profiles.push_back(profile);
  const core::Status validation = core::ValidateToolchainSettings(settings);
  if (!validation.Ok()) return validation;
  {
    std::scoped_lock lock(implementation_->mutex);
    if (implementation_->shutdown) {
      return {core::ErrorCode::kCancelled, "Toolchain doctor is shutting down",
              0};
    }
    if (implementation_->active) {
      return {core::ErrorCode::kConflict,
              "Toolchain diagnosis is already active", 0};
    }
    implementation_->profile = std::move(profile);
    implementation_->generation = generation;
    implementation_->sink = std::move(sink);
    implementation_->next_check = 0;
    implementation_->all_required_passed = true;
    implementation_->active = true;
    implementation_->cancellation_requested = false;
    implementation_->completion_claimed = false;
    implementation_->terminal_delivered = false;
  }
  DoctorEvent started;
  started.kind = DoctorEventKind::kStarted;
  started.generation = generation;
  implementation_->Emit(std::move(started));
  implementation_->EmitCheck(
      {DoctorCheckId::kProfile, true, true, "Profile is valid", {}});
  implementation_->StartNext();
  return core::Status::Success();
}

void ToolchainDoctorService::Cancel() noexcept {
  if (!implementation_) return;
  std::scoped_lock lock(implementation_->mutex);
  if (!implementation_->active) return;
  implementation_->cancellation_requested = true;
  if (implementation_->handle) implementation_->handle->Cancel();
}

void ToolchainDoctorService::Shutdown() noexcept {
  if (!implementation_) return;
  {
    std::scoped_lock lock(implementation_->mutex);
    if (implementation_->shutdown) return;
    implementation_->shutdown = true;
    implementation_->active = false;
    implementation_->sink = {};
    if (implementation_->handle) implementation_->handle->Cancel();
  }
  implementation_->scheduler.RequestStop();
}

bool ToolchainDoctorService::IsActive() const {
  if (!implementation_) return false;
  std::scoped_lock lock(implementation_->mutex);
  return implementation_->active;
}

std::string_view DoctorCheckName(DoctorCheckId id) noexcept {
  switch (id) {
    case DoctorCheckId::kProfile:
      return "Profile";
    case DoctorCheckId::kWsl2:
      return "WSL2 distribution";
    case DoctorCheckId::kOpenLane:
      return "OpenLane 2";
    case DoctorCheckId::kOrfs:
      return "ORFS";
    case DoctorCheckId::kPdk:
      return "PDK root";
  }
  return "Unknown";
}

}  // namespace designpp::application

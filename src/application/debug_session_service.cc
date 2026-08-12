// Copyright 2026 The Design++ Authors

#include "designpp/application/debug_session_service.h"

#include "designpp/adapters/icarus_debug_adapter.h"

namespace designpp::application {

void DebugSessionService::BeginProbe() {
  snapshot_ = {};
  snapshot_.state = DebugState::kProbing;
}

void DebugSessionService::MarkCompiling() {
  snapshot_.state = DebugState::kCompiling;
  snapshot_.prompt_ready = false;
}

void DebugSessionService::MarkStarting() {
  snapshot_.state = DebugState::kStarting;
  snapshot_.prompt_ready = false;
}

void DebugSessionService::Complete(bool cancelled, bool succeeded) {
  snapshot_.state = cancelled   ? DebugState::kCancelled
                    : succeeded ? DebugState::kCompleted
                                : DebugState::kFailed;
  snapshot_.prompt_ready = false;
}

void DebugSessionService::Apply(
    const std::vector<adapters::DebugProtocolEvent>& events) {
  for (const adapters::DebugProtocolEvent& event : events) {
    switch (event.kind) {
      case adapters::DebugProtocolEventKind::kPromptReady:
        snapshot_.prompt_ready = true;
        if (snapshot_.state != DebugState::kFinishing) {
          snapshot_.state = DebugState::kPaused;
        }
        break;
      case adapters::DebugProtocolEventKind::kContinued:
        snapshot_.state = DebugState::kRunning;
        snapshot_.prompt_ready = false;
        break;
      case adapters::DebugProtocolEventKind::kStopped:
        snapshot_.state = DebugState::kPaused;
        snapshot_.simulation_time = event.simulation_time;
        break;
      case adapters::DebugProtocolEventKind::kTimeChanged:
        snapshot_.simulation_time = event.simulation_time;
        break;
      case adapters::DebugProtocolEventKind::kScopeChanged:
        snapshot_.scope = event.scope;
        break;
      case adapters::DebugProtocolEventKind::kScopeItems:
        snapshot_.scope_items = event.scope_items;
        break;
      case adapters::DebugProtocolEventKind::kEvaluatedValue:
        snapshot_.evaluated_value = event.text;
        break;
      case adapters::DebugProtocolEventKind::kSourceStopLocation:
        snapshot_.stop_file = event.file;
        snapshot_.stop_line = event.line;
        break;
      case adapters::DebugProtocolEventKind::kProtocolError:
        snapshot_.state = DebugState::kFailed;
        snapshot_.prompt_ready = false;
        break;
    }
  }
}

core::Result<std::string> DebugSessionService::PrepareCommand(
    DebugCommandKind kind, std::string_view console_command) {
  if (!snapshot_.prompt_ready || snapshot_.state != DebugState::kPaused) {
    return core::Status{core::ErrorCode::kConflict,
                        "The debugger is not waiting at a VVP prompt", 0};
  }
  std::string command;
  switch (kind) {
    case DebugCommandKind::kContinue:
      command = "cont";
      break;
    case DebugCommandKind::kStep:
      command = "step";
      break;
    case DebugCommandKind::kFinish:
      command = "finish";
      break;
    case DebugCommandKind::kConsole:
      command = std::string(console_command);
      break;
  }
  adapters::IcarusDebugAdapter adapter;
  const core::Status validation = adapter.ValidateConsoleCommand(command);
  if (!validation.Ok()) return validation;
  if (kind == DebugCommandKind::kContinue || kind == DebugCommandKind::kStep) {
    snapshot_.state = DebugState::kRunning;
  } else if (kind == DebugCommandKind::kFinish) {
    snapshot_.state = DebugState::kFinishing;
  }
  snapshot_.prompt_ready = false;
  return command;
}

const DebugSnapshot& DebugSessionService::Snapshot() const noexcept {
  return snapshot_;
}

std::string_view DebugStateName(DebugState state) noexcept {
  switch (state) {
    case DebugState::kIdle:
      return "Idle";
    case DebugState::kProbing:
      return "Probing";
    case DebugState::kCompiling:
      return "Compiling";
    case DebugState::kStarting:
      return "Starting";
    case DebugState::kPaused:
      return "Paused";
    case DebugState::kRunning:
      return "Running";
    case DebugState::kFinishing:
      return "Finishing";
    case DebugState::kCompleted:
      return "Completed";
    case DebugState::kFailed:
      return "Failed";
    case DebugState::kCancelled:
      return "Cancelled";
  }
  return "Unknown";
}

}  // namespace designpp::application

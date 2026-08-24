// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_APPLICATION_DEBUG_SESSION_SERVICE_H_
#define DESIGNPP_APPLICATION_DEBUG_SESSION_SERVICE_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "designpp/adapters/interactive_debug_adapter.h"
#include "designpp/core/status.h"

namespace designpp::application {

enum class DebugState {
  kIdle,
  kProbing,
  kCompiling,
  kStarting,
  kPaused,
  kRunning,
  kFinishing,
  kCompleted,
  kFailed,
  kCancelled,
};

enum class DebugCommandKind {
  kContinue,
  kStep,
  kFinish,
  kConsole,
};

struct DebugSnapshot {
  DebugState state = DebugState::kIdle;
  std::string simulation_time;
  std::string scope;
  std::vector<adapters::DebugScopeItem> scope_items;
  std::string evaluated_value;
  std::string stop_file;
  std::uint32_t stop_line = 0;
  bool prompt_ready = false;
};

class DebugSessionService final {
 public:
  void BeginProbe();
  void MarkCompiling();
  void MarkStarting();
  void Complete(bool cancelled, bool succeeded);
  void Apply(const std::vector<adapters::DebugProtocolEvent>& events);
  [[nodiscard]] core::Result<std::string> PrepareCommand(
      DebugCommandKind kind, std::string_view console_command = {});
  [[nodiscard]] const DebugSnapshot& Snapshot() const noexcept;

 private:
  DebugSnapshot snapshot_;
};

[[nodiscard]] std::string_view DebugStateName(DebugState state) noexcept;

}  // namespace designpp::application

#endif  // DESIGNPP_APPLICATION_DEBUG_SESSION_SERVICE_H_

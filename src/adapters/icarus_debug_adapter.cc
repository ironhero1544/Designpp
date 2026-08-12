// Copyright 2026 The Design++ Authors

#include "designpp/adapters/icarus_debug_adapter.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <sstream>

#include "designpp/adapters/icarus_simulation_adapter.h"

namespace designpp::adapters {
namespace {

std::string Trim(std::string value) {
  const auto visible = [](unsigned char character) {
    return std::isspace(character) == 0;
  };
  value.erase(value.begin(), std::find_if(value.begin(), value.end(), visible));
  value.erase(std::find_if(value.rbegin(), value.rend(), visible).base(),
              value.end());
  return value;
}

bool IsIdentifier(std::string_view value) {
  if (value.empty() ||
      !(std::isalpha(static_cast<unsigned char>(value.front())) ||
        value.front() == '_' || value.front() == '$')) {
    return false;
  }
  return std::all_of(value.begin() + 1, value.end(), [](char character) {
    return std::isalnum(static_cast<unsigned char>(character)) ||
           character == '_' || character == '$';
  });
}

bool IsSignalExpression(std::string_view value) {
  if (value.empty() ||
      !(std::isalpha(static_cast<unsigned char>(value.front())) ||
        value.front() == '_' || value.front() == '$')) {
    return false;
  }
  int bracket_depth = 0;
  for (const char character : value.substr(1)) {
    if (character == '[') {
      if (bracket_depth != 0) return false;
      bracket_depth = 1;
    } else if (character == ']') {
      if (bracket_depth != 1) return false;
      bracket_depth = 0;
    } else if (bracket_depth == 0) {
      if (!(std::isalnum(static_cast<unsigned char>(character)) ||
            character == '_' || character == '$')) {
        return false;
      }
    } else if (!(std::isdigit(static_cast<unsigned char>(character)) ||
                 character == ':')) {
      return false;
    }
  }
  return bracket_depth == 0;
}

std::uint32_t ParseLine(std::string_view value) {
  std::uint32_t result = 0;
  const auto parsed =
      std::from_chars(value.data(), value.data() + value.size(), result);
  return parsed.ec == std::errc() ? result : 0;
}

}  // namespace

void IcarusDebugProtocolParser::SetPendingCommand(std::string command) {
  pending_command_ = Trim(std::move(command));
}

std::vector<DebugProtocolEvent> IcarusDebugProtocolParser::Feed(
    std::string_view bytes) {
  buffer_.append(bytes);
  if (buffer_.size() < 2 || !buffer_.ends_with("> ")) return {};
  const std::size_t prompt = buffer_.size() - 2;
  if (prompt != 0 && buffer_[prompt - 1] != '\n') return {};
  std::string response = buffer_.substr(0, prompt);
  buffer_.clear();
  return ParseResponse(std::move(response));
}

std::vector<DebugProtocolEvent> IcarusDebugProtocolParser::Finish() {
  if (buffer_.empty()) return {};
  std::string response = std::move(buffer_);
  buffer_.clear();
  return ParseResponse(std::move(response));
}

std::vector<DebugProtocolEvent> IcarusDebugProtocolParser::ParseResponse(
    std::string response) {
  std::vector<DebugProtocolEvent> events;
  if (response.find("** Continue **") != std::string::npos) {
    events.push_back({DebugProtocolEventKind::kContinued});
  }
  if (response.find("** VVP Stop(") != std::string::npos) {
    DebugProtocolEvent stopped;
    stopped.kind = DebugProtocolEventKind::kStopped;
    stopped.text = response;
    const std::string marker = "** Current simulation time is ";
    const std::size_t begin = response.rfind(marker);
    if (begin != std::string::npos) {
      const std::size_t value_begin = begin + marker.size();
      const std::size_t value_end = response.find(" ticks", value_begin);
      if (value_end != std::string::npos) {
        stopped.simulation_time =
            response.substr(value_begin, value_end - value_begin);
      }
    }
    events.push_back(std::move(stopped));
  }
  const std::string stop_marker = ": $stop called";
  const std::size_t stop = response.find(stop_marker);
  if (stop != std::string::npos) {
    const std::size_t line_start = response.rfind('\n', stop);
    const std::size_t location_start =
        line_start == std::string::npos ? 0 : line_start + 1;
    const std::size_t separator = response.rfind(':', stop - 1);
    if (separator != std::string::npos && separator >= location_start) {
      DebugProtocolEvent location;
      location.kind = DebugProtocolEventKind::kSourceStopLocation;
      location.file =
          response.substr(location_start, separator - location_start);
      location.line = ParseLine(std::string_view(response).substr(
          separator + 1, stop - separator - 1));
      events.push_back(std::move(location));
    }
  }

  std::istringstream input(response);
  std::string line;
  std::vector<std::string> lines;
  while (std::getline(input, line)) {
    line = Trim(std::move(line));
    if (!line.empty()) lines.push_back(std::move(line));
  }
  if (pending_command_ == "time") {
    for (const std::string& value : lines) {
      if (value.ends_with(" ticks") &&
          value.find("Current simulation") == std::string::npos) {
        events.push_back({DebugProtocolEventKind::kTimeChanged,
                          {},
                          value.substr(0, value.size() - 6)});
      }
    }
  } else if (pending_command_ == "where") {
    for (const std::string& value : lines) {
      if (value.starts_with("module ") || value.starts_with("package ")) {
        const std::size_t space = value.find(' ');
        events.push_back({DebugProtocolEventKind::kScopeChanged,
                          {},
                          {},
                          value.substr(space + 1)});
        break;
      }
    }
  } else if (pending_command_ == "list" || pending_command_ == "ls") {
    DebugProtocolEvent items;
    items.kind = DebugProtocolEventKind::kScopeItems;
    for (const std::string& value : lines) {
      const std::size_t separator = value.find(':');
      if (separator == std::string::npos) continue;
      const std::string kind = Trim(value.substr(0, separator));
      std::string name = Trim(value.substr(separator + 1));
      const std::size_t annotation = name.find(" -- ");
      if (annotation != std::string::npos) name.resize(annotation);
      if (!kind.empty() && !name.empty()) {
        items.scope_items.push_back({kind, name, {}});
      }
    }
    events.push_back(std::move(items));
  } else if (pending_command_.starts_with("$display")) {
    for (auto iterator = lines.rbegin(); iterator != lines.rend(); ++iterator) {
      if (*iterator != pending_command_ && !iterator->starts_with("**")) {
        events.push_back({DebugProtocolEventKind::kEvaluatedValue, *iterator});
        break;
      }
    }
  }
  pending_command_.clear();
  events.push_back({DebugProtocolEventKind::kPromptReady});
  return events;
}

core::Result<DebugPlan> IcarusDebugAdapter::BuildDebugPlan(
    const SimulationRequest& request,
    const runtime::PathMapper& path_mapper) const {
  IcarusSimulationAdapter simulation_adapter;
  auto simulation = simulation_adapter.BuildPlan(request, path_mapper);
  if (!simulation.Ok()) return simulation.GetStatus();
  DebugPlan plan;
  plan.simulation = std::move(simulation).Value();
  if (plan.simulation.execute.arguments.empty()) {
    return core::Status{core::ErrorCode::kCorruptData,
                        "Icarus execution plan is incomplete", 0};
  }
  const std::wstring binary = plan.simulation.execute.arguments.back();
  plan.simulation.execute.arguments = {L"-i", L"-s", binary};
  plan.simulation.execute.interactive_input = true;
  return plan;
}

core::Status IcarusDebugAdapter::ValidateConsoleCommand(
    std::string_view command) const {
  if (command.empty() || command.size() > 512 ||
      command.find_first_of("\r\n\0") != std::string_view::npos) {
    return {core::ErrorCode::kInvalidArgument,
            "Debug command is empty, oversized, or contains a control line", 0};
  }
  const std::string value = Trim(std::string(command));
  for (const std::string_view fixed :
       {"time", "where", "list", "ls", "pop", "step", "cont", "finish",
        "trace on", "trace off"}) {
    if (value == fixed) return core::Status::Success();
  }
  for (const std::string_view prefix : {"push ", "cd "}) {
    if (value.starts_with(prefix) &&
        IsIdentifier(value.substr(prefix.size()))) {
      return core::Status::Success();
    }
  }
  if (value.starts_with("$display ")) {
    const std::string expression = Trim(value.substr(9));
    if (IsSignalExpression(expression)) return core::Status::Success();
  }
  return {core::ErrorCode::kPermissionDenied,
          "Debug command is not in the allowed command set", 0};
}

}  // namespace designpp::adapters

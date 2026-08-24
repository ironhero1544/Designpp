// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_APPLICATION_EDITOR_PROTOCOL_H_
#define DESIGNPP_APPLICATION_EDITOR_PROTOCOL_H_

#include <cstdint>
#include <string>
#include <string_view>

#include "designpp/core/status.h"

namespace designpp::application {

inline constexpr std::uint32_t kEditorProtocolVersion = 1;
inline constexpr std::size_t kMaximumEditorMessageBytes = 40 * 1024 * 1024;

struct EditorWebMessage {
  std::string type;
  std::string session_id;
  std::string document_id;
  std::string command;
  std::string text;
  std::uint64_t version = 0;
  bool dirty = false;
};

[[nodiscard]] core::Result<EditorWebMessage> DecodeEditorWebMessage(
    std::string_view json, std::string_view expected_session_id);
[[nodiscard]] std::string EscapeEditorJson(std::string_view value);

}  // namespace designpp::application

#endif  // DESIGNPP_APPLICATION_EDITOR_PROTOCOL_H_

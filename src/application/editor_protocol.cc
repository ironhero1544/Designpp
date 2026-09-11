// Copyright 2026 The Design++ Authors

#include "designpp/application/editor_protocol.h"

#include <cctype>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace designpp::application {
namespace {

using core::ErrorCode;
using core::Status;

void AppendUtf8(std::uint32_t value, std::string* output) {
  if (value <= 0x7f) {
    output->push_back(static_cast<char>(value));
  } else if (value <= 0x7ff) {
    output->push_back(static_cast<char>(0xc0 | (value >> 6)));
    output->push_back(static_cast<char>(0x80 | (value & 0x3f)));
  } else {
    output->push_back(static_cast<char>(0xe0 | (value >> 12)));
    output->push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
    output->push_back(static_cast<char>(0x80 | (value & 0x3f)));
  }
}

std::optional<std::size_t> FindValue(std::string_view json,
                                     std::string_view name) {
  int object_depth = 0;
  int array_depth = 0;
  for (std::size_t position = 0; position < json.size(); ++position) {
    if (json[position] == '{') {
      ++object_depth;
      continue;
    }
    if (json[position] == '}') {
      --object_depth;
      continue;
    }
    if (json[position] == '[') {
      ++array_depth;
      continue;
    }
    if (json[position] == ']') {
      --array_depth;
      continue;
    }
    if (json[position] != '"') continue;
    std::size_t end = position + 1;
    bool escaped = false;
    for (; end < json.size(); ++end) {
      if (!escaped && json[end] == '"') break;
      if (!escaped && json[end] == '\\') {
        escaped = true;
      } else {
        escaped = false;
      }
    }
    if (end >= json.size()) return std::nullopt;
    std::size_t cursor = end + 1;
    while (cursor < json.size() &&
           std::isspace(static_cast<unsigned char>(json[cursor]))) {
      ++cursor;
    }
    if (object_depth == 1 && array_depth == 0 &&
        json.substr(position + 1, end - position - 1) == name &&
        cursor < json.size() && json[cursor] == ':') {
      ++cursor;
      while (cursor < json.size() &&
             std::isspace(static_cast<unsigned char>(json[cursor]))) {
        ++cursor;
      }
      return cursor;
    }
    position = end;
  }
  return std::nullopt;
}

bool HasValueDelimiter(std::string_view json, const char* position) {
  while (position < json.data() + json.size() &&
         std::isspace(static_cast<unsigned char>(*position))) {
    ++position;
  }
  return position < json.data() + json.size() &&
         (*position == ',' || *position == '}');
}

std::optional<std::string> ReadString(std::string_view json,
                                      std::string_view name,
                                      bool required = false) {
  const auto position = FindValue(json, name);
  if (!position)
    return required ? std::nullopt : std::optional<std::string>("");
  std::size_t cursor = *position;
  if (cursor >= json.size() || json[cursor++] != '"') return std::nullopt;
  std::string value;
  while (cursor < json.size()) {
    const char character = json[cursor++];
    if (character == '"') return value;
    if (static_cast<unsigned char>(character) < 0x20) return std::nullopt;
    if (character != '\\') {
      value.push_back(character);
      continue;
    }
    if (cursor >= json.size()) return std::nullopt;
    const char escaped = json[cursor++];
    switch (escaped) {
      case '"':
      case '\\':
      case '/':
        value.push_back(escaped);
        break;
      case 'b':
        value.push_back('\b');
        break;
      case 'f':
        value.push_back('\f');
        break;
      case 'n':
        value.push_back('\n');
        break;
      case 'r':
        value.push_back('\r');
        break;
      case 't':
        value.push_back('\t');
        break;
      case 'u': {
        if (cursor + 4 > json.size()) return std::nullopt;
        std::uint32_t code_point = 0;
        const auto result = std::from_chars(
            json.data() + cursor, json.data() + cursor + 4, code_point, 16);
        if (result.ec != std::errc{} ||
            result.ptr != json.data() + cursor + 4 ||
            (code_point >= 0xd800 && code_point <= 0xdfff)) {
          return std::nullopt;
        }
        cursor += 4;
        AppendUtf8(code_point, &value);
        break;
      }
      default:
        return std::nullopt;
    }
  }
  return std::nullopt;
}

std::optional<std::uint64_t> ReadUnsigned(std::string_view json,
                                          std::string_view name) {
  const auto position = FindValue(json, name);
  if (!position) return 0;
  std::uint64_t value = 0;
  const auto result = std::from_chars(json.data() + *position,
                                      json.data() + json.size(), value);
  return result.ec == std::errc{} && HasValueDelimiter(json, result.ptr)
             ? std::optional(value)
             : std::nullopt;
}

std::optional<bool> ReadBool(std::string_view json, std::string_view name) {
  const auto position = FindValue(json, name);
  if (!position) return false;
  if (json.substr(*position, 4) == "true" &&
      HasValueDelimiter(json, json.data() + *position + 4)) {
    return true;
  }
  if (json.substr(*position, 5) == "false" &&
      HasValueDelimiter(json, json.data() + *position + 5)) {
    return false;
  }
  return std::nullopt;
}

bool IsAllowedType(std::string_view type) {
  return type == "ready_for_initialize" || type == "ready" ||
         type == "active_document_changed" || type == "document_changed" ||
         type == "document_text" || type == "save_document" ||
         type == "close_document_requested" || type == "editor_command" ||
         type == "fatal_error";
}

}  // namespace

core::Result<EditorWebMessage> DecodeEditorWebMessage(
    std::string_view json, std::string_view expected_session_id) {
  if (json.empty() || json.size() > kMaximumEditorMessageBytes ||
      json.front() != '{' || json.back() != '}') {
    return Status{ErrorCode::kInvalidArgument,
                  "Editor message has an invalid envelope", 0};
  }
  auto protocol = ReadUnsigned(json, "protocol");
  auto type = ReadString(json, "type", true);
  auto session = ReadString(json, "session_id", true);
  auto document = ReadString(json, "document_id");
  auto command = ReadString(json, "command");
  auto text = ReadString(json, "text");
  auto version = ReadUnsigned(json, "version");
  auto dirty = ReadBool(json, "dirty");
  if (!protocol || *protocol != kEditorProtocolVersion || !type ||
      !IsAllowedType(*type) || !session || !document || !command || !text ||
      !version || !dirty) {
    return Status{ErrorCode::kInvalidArgument,
                  "Editor message fields are invalid", 0};
  }
  if (*type != "ready_for_initialize" && *session != expected_session_id) {
    return Status{ErrorCode::kPermissionDenied, "Editor session does not match",
                  0};
  }
  return EditorWebMessage{std::move(*type),
                          std::move(*session),
                          std::move(*document),
                          std::move(*command),
                          std::move(*text),
                          *version,
                          *dirty};
}

std::string EscapeEditorJson(std::string_view value) {
  std::string output;
  output.reserve(value.size() + 2);
  output.push_back('"');
  constexpr char kHex[] = "0123456789abcdef";
  for (const unsigned char character : value) {
    switch (character) {
      case '"':
        output += "\\\"";
        break;
      case '\\':
        output += "\\\\";
        break;
      case '\b':
        output += "\\b";
        break;
      case '\f':
        output += "\\f";
        break;
      case '\n':
        output += "\\n";
        break;
      case '\r':
        output += "\\r";
        break;
      case '\t':
        output += "\\t";
        break;
      default:
        if (character < 0x20) {
          output += "\\u00";
          output.push_back(kHex[character >> 4]);
          output.push_back(kHex[character & 0x0f]);
        } else {
          output.push_back(static_cast<char>(character));
        }
    }
  }
  output.push_back('"');
  return output;
}

}  // namespace designpp::application

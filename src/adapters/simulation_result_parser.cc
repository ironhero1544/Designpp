// Copyright 2026 The Design++ Authors

#include "designpp/adapters/simulation_result_parser.h"

#include <charconv>
#include <limits>

namespace designpp::adapters {
namespace {

constexpr std::size_t kMaximumResultsSize = 16 * 1024 * 1024;
constexpr std::size_t kMaximumTestCases = 100000;

std::string DecodeEntities(std::string_view value) {
  std::string result;
  result.reserve(value.size());
  for (std::size_t index = 0; index < value.size();) {
    if (value[index] != '&') {
      result.push_back(value[index++]);
      continue;
    }
    const std::size_t end = value.find(';', index + 1);
    if (end == std::string_view::npos) {
      result.push_back(value[index++]);
      continue;
    }
    const std::string_view entity = value.substr(index, end - index + 1);
    if (entity == "&amp;") {
      result.push_back('&');
    } else if (entity == "&lt;") {
      result.push_back('<');
    } else if (entity == "&gt;") {
      result.push_back('>');
    } else if (entity == "&quot;") {
      result.push_back('"');
    } else if (entity == "&apos;") {
      result.push_back('\'');
    } else {
      result.append(entity);
    }
    index = end + 1;
  }
  return result;
}

core::Result<std::string> Attribute(std::string_view tag, std::string_view name,
                                    bool required = true) {
  const std::string key = std::string(name) + "=";
  std::size_t position = tag.find(key);
  while (position != std::string_view::npos && position > 0 &&
         tag[position - 1] != ' ' && tag[position - 1] != '\t' &&
         tag[position - 1] != '\r' && tag[position - 1] != '\n') {
    position = tag.find(key, position + 1);
  }
  if (position == std::string_view::npos) {
    return required ? core::Result<std::string>(core::Status{
                          core::ErrorCode::kCorruptData,
                          "Required xUnit attribute is missing", 0})
                    : core::Result<std::string>(std::string());
  }
  position += key.size();
  if (position >= tag.size() ||
      (tag[position] != '"' && tag[position] != '\'')) {
    return core::Status{core::ErrorCode::kCorruptData,
                        "xUnit attribute is malformed", 0};
  }
  const char quote = tag[position++];
  const std::size_t end = tag.find(quote, position);
  if (end == std::string_view::npos) {
    return core::Status{core::ErrorCode::kCorruptData,
                        "xUnit attribute is unterminated", 0};
  }
  return DecodeEntities(tag.substr(position, end - position));
}

core::Result<double> Duration(std::string_view value) {
  if (value.empty()) return 0.0;
  double duration = 0.0;
  const auto parsed =
      std::from_chars(value.data(), value.data() + value.size(), duration);
  if (parsed.ec != std::errc() || parsed.ptr != value.data() + value.size() ||
      duration < 0.0 || duration == std::numeric_limits<double>::infinity()) {
    return core::Status{core::ErrorCode::kCorruptData,
                        "xUnit duration is invalid", 0};
  }
  return duration;
}

std::string ElementDetail(std::string_view body, std::string_view element) {
  const std::string opening = "<" + std::string(element);
  const std::size_t begin = body.find(opening);
  if (begin == std::string_view::npos) return {};
  const std::size_t tag_end = body.find('>', begin + opening.size());
  if (tag_end == std::string_view::npos) return {};
  const std::string closing = "</" + std::string(element) + ">";
  const std::size_t end = body.find(closing, tag_end + 1);
  if (end == std::string_view::npos) return {};
  return DecodeEntities(body.substr(tag_end + 1, end - tag_end - 1));
}

}  // namespace

core::Result<SimulationTestSummary> ParseCocotbResults(std::string_view xml) {
  if (xml.empty() || xml.size() > kMaximumResultsSize) {
    return core::Status{xml.empty() ? core::ErrorCode::kCorruptData
                                    : core::ErrorCode::kFileTooLarge,
                        "cocotb results XML size is invalid", 0};
  }

  SimulationTestSummary summary;
  std::size_t position = 0;
  while ((position = xml.find("<testcase", position)) !=
         std::string_view::npos) {
    if (summary.cases.size() >= kMaximumTestCases) {
      return core::Status{core::ErrorCode::kFileTooLarge,
                          "cocotb results contain too many test cases", 0};
    }
    const std::size_t tag_end = xml.find('>', position + 9);
    if (tag_end == std::string_view::npos) {
      return core::Status{core::ErrorCode::kCorruptData,
                          "xUnit testcase tag is unterminated", 0};
    }
    const std::string_view tag = xml.substr(position, tag_end - position + 1);
    auto name = Attribute(tag, "name");
    auto suite = Attribute(tag, "classname", false);
    auto duration_text = Attribute(tag, "time", false);
    if (!name.Ok()) return name.GetStatus();
    if (!suite.Ok()) return suite.GetStatus();
    if (!duration_text.Ok()) return duration_text.GetStatus();
    auto duration = Duration(duration_text.Value());
    if (!duration.Ok()) return duration.GetStatus();

    const bool self_closing = tag.size() >= 2 && tag[tag.size() - 2] == '/';
    std::string_view body;
    std::size_t next = tag_end + 1;
    if (!self_closing) {
      const std::size_t close = xml.find("</testcase>", next);
      if (close == std::string_view::npos) {
        return core::Status{core::ErrorCode::kCorruptData,
                            "xUnit testcase is unterminated", 0};
      }
      body = xml.substr(next, close - next);
      next = close + 11;
    }

    SimulationTestCaseResult test_case;
    test_case.name = std::move(name).Value();
    test_case.suite = std::move(suite).Value();
    test_case.duration_seconds = duration.Value();
    if (body.find("<failure") != std::string_view::npos) {
      test_case.status = SimulationTestStatus::kFailed;
      test_case.detail = ElementDetail(body, "failure");
      ++summary.failed;
    } else if (body.find("<error") != std::string_view::npos) {
      test_case.status = SimulationTestStatus::kError;
      test_case.detail = ElementDetail(body, "error");
      ++summary.errors;
    } else if (body.find("<skipped") != std::string_view::npos) {
      test_case.status = SimulationTestStatus::kSkipped;
      ++summary.skipped;
    } else {
      ++summary.passed;
    }
    summary.duration_seconds += test_case.duration_seconds;
    summary.cases.push_back(std::move(test_case));
    position = next;
  }
  summary.total = static_cast<std::uint32_t>(summary.cases.size());
  if (summary.total == 0) {
    return core::Status{core::ErrorCode::kCorruptData,
                        "cocotb results contain no test cases", 0};
  }
  return summary;
}

}  // namespace designpp::adapters

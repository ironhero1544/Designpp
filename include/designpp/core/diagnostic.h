// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_CORE_DIAGNOSTIC_H_
#define DESIGNPP_CORE_DIAGNOSTIC_H_

#include <cstdint>
#include <string>

namespace designpp::core {

enum class DiagnosticSeverity { kInfo, kWarning, kError };

struct Diagnostic {
  DiagnosticSeverity severity = DiagnosticSeverity::kInfo;
  std::string code;
  std::string file;
  std::uint32_t line = 0;
  std::uint32_t column = 0;
  std::string message;
};

}  // namespace designpp::core

#endif  // DESIGNPP_CORE_DIAGNOSTIC_H_

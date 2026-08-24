// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_CORE_LIBRARY_H_
#define DESIGNPP_CORE_LIBRARY_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "designpp/core/status.h"

namespace designpp::core {

enum class ViewKind {
  kVerilog,
  kTestbench,
  kConstraints,
  kSynthesis,
  kTiming,
  kPhysicalDesign,
  kLayout,
  kReport,
};

enum class LibraryStatus {
  kReady,
  kEmpty,
  kMissing,
  kInvalid,
  kReadOnly,
};

struct ManagedFile {
  std::string relative_path;
  std::string role;
  std::uint64_t size = 0;
  std::string modified_utc;
};

struct View {
  std::string id;
  std::string name;
  std::string description;
  ViewKind kind = ViewKind::kVerilog;
  std::vector<ManagedFile> files;
};

struct Cell {
  std::string id;
  std::string name;
  std::string description;
  std::vector<View> views;
};

struct Library {
  static constexpr std::uint32_t kSchemaVersion = 5;

  std::uint32_t schema_version = kSchemaVersion;
  std::string id;
  std::uint64_t revision = 1;
  std::string name;
  std::string description;
  std::string created_utc;
  std::string modified_utc;
  std::vector<ManagedFile> files;
  std::vector<Cell> cells;
};

[[nodiscard]] Status ValidateLibrary(const Library& library);
[[nodiscard]] Status ValidateDisplayName(std::string_view name);
[[nodiscard]] std::string_view ViewKindName(ViewKind kind) noexcept;
[[nodiscard]] std::string_view LibraryStatusName(LibraryStatus status) noexcept;

}  // namespace designpp::core

#endif  // DESIGNPP_CORE_LIBRARY_H_

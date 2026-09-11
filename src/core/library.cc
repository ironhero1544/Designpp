// Copyright 2026 The Design++ Authors

#include "designpp/core/library.h"

#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace designpp::core {
namespace {

std::string FoldAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](char character) {
    return static_cast<char>(
        std::tolower(static_cast<unsigned char>(character)));
  });
  return value;
}

bool IsSafeRelativePath(std::string_view path) {
  if (path.empty() || path.front() == '/' || path.front() == '\\' ||
      path.find(':') != std::string_view::npos ||
      path.find('\\') != std::string_view::npos) {
    return false;
  }
  std::size_t start = 0;
  while (start <= path.size()) {
    const std::size_t end = path.find('/', start);
    const std::string_view component = path.substr(start, end - start);
    if (component.empty() || component == "." || component == "..") {
      return false;
    }
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }
  return true;
}

}  // namespace

Status ValidateDisplayName(std::string_view name) {
  if (name.empty() || name.size() > 128) {
    return {ErrorCode::kInvalidArgument,
            "Name must contain between 1 and 128 UTF-8 bytes", 0};
  }
  for (unsigned char character : name) {
    if (character < 0x20 || character == 0x7f) {
      return {ErrorCode::kInvalidArgument, "Name contains a control character",
              0};
    }
  }
  return Status::Success();
}

Status ValidateLibrary(const Library& library) {
  if (library.schema_version != Library::kSchemaVersion) {
    return {ErrorCode::kUnsupportedSchema, "Unsupported library schema", 0};
  }
  if (library.id.empty()) {
    return {ErrorCode::kCorruptData, "Library id is missing", 0};
  }
  Status status = ValidateDisplayName(library.name);
  if (!status.Ok()) {
    return status;
  }
  for (const ManagedFile& file : library.files) {
    if (!IsSafeRelativePath(file.relative_path)) {
      return {ErrorCode::kCorruptData,
              "Managed library file path escapes the library", 0};
    }
  }
  // Cell and view IDs are directory names on Windows.  Treat them as
  // case-insensitive identities so two manifest entries can never resolve to
  // the same project.dpproj or view files directory and accidentally share
  // physical-implementation settings.
  std::unordered_set<std::string> cell_ids;
  std::unordered_set<std::string> cell_names;
  for (const Cell& cell : library.cells) {
    if (cell.id.empty() || !(status = ValidateDisplayName(cell.name)).Ok()) {
      return status.Ok()
                 ? Status{ErrorCode::kCorruptData, "Cell id is missing", 0}
                 : status;
    }
    if (!cell_ids.insert(FoldAscii(cell.id)).second) {
      return {ErrorCode::kAlreadyExists, "Duplicate cell id", 0};
    }
    if (!cell_names.insert(FoldAscii(cell.name)).second) {
      return {ErrorCode::kAlreadyExists, "Duplicate cell name", 0};
    }
    std::unordered_set<std::string> view_ids;
    std::unordered_set<std::string> view_names;
    for (const View& view : cell.views) {
      if (view.kind == ViewKind::kPhysicalDesign) {
        return {ErrorCode::kUnsupportedSchema,
                "Physical Design views must migrate to Layout", 0};
      }
      if (view.id.empty() || !(status = ValidateDisplayName(view.name)).Ok()) {
        return status.Ok()
                   ? Status{ErrorCode::kCorruptData, "View id is missing", 0}
                   : status;
      }
      if (!view_ids.insert(FoldAscii(view.id)).second) {
        return {ErrorCode::kAlreadyExists, "Duplicate view id", 0};
      }
      if (!view_names.insert(FoldAscii(view.name)).second) {
        return {ErrorCode::kAlreadyExists, "Duplicate view name", 0};
      }
      for (const ManagedFile& file : view.files) {
        if (!IsSafeRelativePath(file.relative_path)) {
          return {ErrorCode::kCorruptData,
                  "Managed file path escapes the library", 0};
        }
      }
    }
  }
  return Status::Success();
}

std::string_view ViewKindName(ViewKind kind) noexcept {
  switch (kind) {
    case ViewKind::kVerilog:
      return "Verilog / SystemVerilog";
    case ViewKind::kTestbench:
      return "Testbench";
    case ViewKind::kConstraints:
      return "Constraints";
    case ViewKind::kSynthesis:
      return "Synthesis";
    case ViewKind::kTiming:
      return "Timing";
    case ViewKind::kPhysicalDesign:
      return "Physical Design";
    case ViewKind::kLayout:
      return "Layout";
    case ViewKind::kReport:
      return "Report";
  }
  return "Unknown";
}

std::string_view LibraryStatusName(LibraryStatus status) noexcept {
  switch (status) {
    case LibraryStatus::kReady:
      return "Ready";
    case LibraryStatus::kEmpty:
      return "Empty";
    case LibraryStatus::kMissing:
      return "Missing";
    case LibraryStatus::kInvalid:
      return "Invalid";
    case LibraryStatus::kReadOnly:
      return "Read-only";
  }
  return "Invalid";
}

}  // namespace designpp::core

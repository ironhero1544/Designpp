// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_APPLICATION_MANAGED_SOURCE_SERVICE_H_
#define DESIGNPP_APPLICATION_MANAGED_SOURCE_SERVICE_H_

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "designpp/application/library_service.h"
#include "designpp/core/status.h"

namespace designpp::application {

enum class TextLineEnding { kLf, kCrLf };

struct EditorDocumentSnapshot {
  std::string id;
  std::string library_id;
  std::string cell_id;
  std::string view_id;
  std::string relative_path;
  std::string model_uri;
  std::string text;
  std::string diagnostic;
  std::string content_hash;
  std::uint64_t size = 0;
  std::string modified_utc;
  TextLineEnding line_ending = TextLineEnding::kLf;
  bool has_utf8_bom = false;
  bool read_only = false;
  bool missing = false;
  bool externally_modified = false;
};

struct SaveDocumentResult {
  LibraryRecord library;
  EditorDocumentSnapshot document;
};

class ManagedSourceService final {
 public:
  static constexpr std::uint64_t kMaximumEditableBytes = 16 * 1024 * 1024;

  [[nodiscard]] core::Result<EditorDocumentSnapshot> LoadDocument(
      const LibraryRecord& library, std::string_view cell_id,
      std::string_view view_id, std::string_view relative_path,
      bool workspace_read_only) const;
  [[nodiscard]] core::Result<SaveDocumentResult> SaveDocument(
      const LibraryRecord& library, const EditorDocumentSnapshot& snapshot,
      std::string_view utf8_text, bool overwrite_external) const;
  [[nodiscard]] core::Result<EditorDocumentSnapshot> RefreshDocument(
      const LibraryRecord& library,
      const EditorDocumentSnapshot& snapshot) const;
  [[nodiscard]] core::Status CloseDocument(std::string_view document_id) const;
};

}  // namespace designpp::application

#endif  // DESIGNPP_APPLICATION_MANAGED_SOURCE_SERVICE_H_

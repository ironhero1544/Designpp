// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_APPLICATION_LIBRARY_SERVICE_H_
#define DESIGNPP_APPLICATION_LIBRARY_SERVICE_H_

#include <filesystem>
#include <string>
#include <vector>

#include "designpp/core/library.h"
#include "designpp/core/status.h"

namespace designpp::application {

struct LibraryRecord {
  core::Library library;
  std::filesystem::path directory;
  core::LibraryStatus status = core::LibraryStatus::kInvalid;
  std::string diagnostic;
};

struct CreateViewRequest {
  std::string name;
  std::string description;
  core::ViewKind kind = core::ViewKind::kVerilog;
  std::string source_extension = ".sv";
};

class LibraryStore final {
 public:
  [[nodiscard]] core::Result<std::vector<LibraryRecord>> Scan(
      const std::filesystem::path& root) const;
  [[nodiscard]] core::Result<LibraryRecord> Load(
      const std::filesystem::path& directory) const;
  [[nodiscard]] core::Status Save(const LibraryRecord& record,
                                  std::uint64_t expected_revision) const;
};

class LibraryService final {
 public:
  [[nodiscard]] core::Result<std::filesystem::path> GetLibraryRoot() const;
  [[nodiscard]] core::Status SetLibraryRoot(
      const std::filesystem::path& root) const;
  [[nodiscard]] core::Result<std::vector<LibraryRecord>> Refresh() const;
  [[nodiscard]] core::Result<LibraryRecord> CreateLibrary(
      std::string name, std::string description) const;
  [[nodiscard]] core::Result<LibraryRecord> CreateCell(
      const LibraryRecord& record, std::string name,
      std::string description) const;
  [[nodiscard]] core::Result<LibraryRecord> CreateView(
      const LibraryRecord& record, std::string_view cell_id,
      const CreateViewRequest& request) const;
  [[nodiscard]] core::Result<LibraryRecord> RenameItem(
      const LibraryRecord& record, std::string_view cell_id,
      std::string_view view_id, std::string name,
      std::string description) const;
  // Imports into the Library when both ids are empty, or into the selected
  // View when both ids are present. The manifest update is atomic.
  [[nodiscard]] core::Result<LibraryRecord> ImportFiles(
      const LibraryRecord& record, std::string_view cell_id,
      std::string_view view_id,
      const std::vector<std::filesystem::path>& sources) const;
  // Replaces a Library-scoped managed Liberty file and adopts the replacement
  // file name. The file and manifest changes are rolled back together on
  // failure.
  [[nodiscard]] core::Result<LibraryRecord> ReplaceLibraryFile(
      const LibraryRecord& record, std::string_view relative_path,
      const std::filesystem::path& source) const;
  // Removes a Library-scoped managed Liberty file. The original file remains
  // recoverable until the manifest mutation has committed successfully.
  [[nodiscard]] core::Result<LibraryRecord> RemoveLibraryFile(
      const LibraryRecord& record, std::string_view relative_path) const;
  [[nodiscard]] core::Status DeleteItem(const LibraryRecord& record,
                                        std::string_view cell_id,
                                        std::string_view view_id,
                                        bool allow_unc_permanent) const;

 private:
  LibraryStore store_{};
};

}  // namespace designpp::application

#endif  // DESIGNPP_APPLICATION_LIBRARY_SERVICE_H_

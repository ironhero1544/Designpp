// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_APPLICATION_PROJECT_SERVICE_H_
#define DESIGNPP_APPLICATION_PROJECT_SERVICE_H_

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "designpp/application/library_service.h"
#include "designpp/core/project.h"

namespace designpp::application {

class ProjectWriterLease final {
 public:
  ProjectWriterLease();
  ProjectWriterLease(const ProjectWriterLease&) = delete;
  ProjectWriterLease& operator=(const ProjectWriterLease&) = delete;
  ProjectWriterLease(ProjectWriterLease&&) noexcept;
  ProjectWriterLease& operator=(ProjectWriterLease&&) noexcept;
  ~ProjectWriterLease();

  [[nodiscard]] static std::unique_ptr<ProjectWriterLease> TryAcquire(
      const std::filesystem::path& path);
  [[nodiscard]] bool Acquired() const noexcept;

 private:
  struct Implementation;
  explicit ProjectWriterLease(std::unique_ptr<Implementation> implementation);
  std::unique_ptr<Implementation> implementation_;
};

struct ProjectDocument {
  core::Project project;
  std::filesystem::path library_directory;
  std::filesystem::path project_path;
  bool read_only = false;
  bool valid_configuration = true;
  std::string diagnostic;
  std::unique_ptr<ProjectWriterLease> writer_lease;
};

struct ResolvedSource {
  std::string view_id;
  std::string view_name;
  core::ViewKind view_kind = core::ViewKind::kVerilog;
  std::string relative_path;
  std::string role;
  std::filesystem::path library_directory;
  std::filesystem::path windows_path;
  bool enabled = true;
  bool exists = false;
};

class ProjectStore final {
 public:
  [[nodiscard]] core::Result<core::Project> Load(
      const std::filesystem::path& path) const;
  [[nodiscard]] core::Status Save(const core::Project& project,
                                  const std::filesystem::path& path,
                                  std::uint64_t expected_revision) const;
};

class ProjectService final {
 public:
  [[nodiscard]] core::Result<ProjectDocument> OpenOrCreate(
      const LibraryRecord& library, std::string_view cell_id) const;
  [[nodiscard]] core::Status Save(ProjectDocument* document) const;
  [[nodiscard]] core::Status Refresh(ProjectDocument* document) const;
  [[nodiscard]] core::Result<std::vector<ResolvedSource>> ResolveSources(
      const LibraryRecord& library, std::string_view cell_id,
      const core::Project& project) const;

 private:
  ProjectStore store_{};
};

}  // namespace designpp::application

#endif  // DESIGNPP_APPLICATION_PROJECT_SERVICE_H_

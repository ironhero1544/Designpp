// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_RUNTIME_FILE_WATCH_SERVICE_H_
#define DESIGNPP_RUNTIME_FILE_WATCH_SERVICE_H_

#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

#include "designpp/core/status.h"

namespace designpp::runtime {

class FileWatchService final {
 public:
  using ChangeCallback =
      std::function<void(std::vector<std::filesystem::path>)>;

  FileWatchService();
  FileWatchService(const FileWatchService&) = delete;
  FileWatchService& operator=(const FileWatchService&) = delete;
  ~FileWatchService();

  [[nodiscard]] core::Status Start(const std::filesystem::path& directory,
                                   ChangeCallback callback);
  void Stop();

 private:
  struct Implementation;
  std::unique_ptr<Implementation> implementation_;
};

}  // namespace designpp::runtime

#endif  // DESIGNPP_RUNTIME_FILE_WATCH_SERVICE_H_

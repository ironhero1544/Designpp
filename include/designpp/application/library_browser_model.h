// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_APPLICATION_LIBRARY_BROWSER_MODEL_H_
#define DESIGNPP_APPLICATION_LIBRARY_BROWSER_MODEL_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "designpp/application/library_service.h"

namespace designpp::application {

enum class BrowserScope { kLibrary, kCell, kView };

struct BrowserQuery {
  std::string text;
  std::optional<core::LibraryStatus> status;
  std::optional<core::ViewKind> view_kind;
};

struct BrowserItemRef {
  std::string library_id;
  std::string cell_id;
  std::string view_id;
  std::string name;
  std::string type;
  core::LibraryStatus status = core::LibraryStatus::kInvalid;
  bool create_action = false;
};

struct BrowserResult {
  std::vector<BrowserItemRef> items;
  std::optional<BrowserItemRef> exact_match;
  bool exact_match_visible = false;
  bool can_create = false;
};

class LibraryBrowserModel final {
 public:
  explicit LibraryBrowserModel(const std::vector<LibraryRecord>& libraries);

  [[nodiscard]] BrowserResult FilterLibraries(const BrowserQuery& query) const;
  [[nodiscard]] BrowserResult FilterCells(std::string_view library_id,
                                          const BrowserQuery& query) const;
  [[nodiscard]] BrowserResult FilterViews(std::string_view library_id,
                                          std::string_view cell_id,
                                          const BrowserQuery& query) const;

 private:
  const std::vector<LibraryRecord>& libraries_;
};

[[nodiscard]] core::LibraryStatus ComputeCellStatus(
    const LibraryRecord& library, const core::Cell& cell);
[[nodiscard]] core::LibraryStatus ComputeViewStatus(
    const LibraryRecord& library, const core::View& view);

}  // namespace designpp::application

#endif  // DESIGNPP_APPLICATION_LIBRARY_BROWSER_MODEL_H_

// Copyright 2026 The Design++ Authors

#include "designpp/application/library_browser_model.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace designpp::application {
namespace {

std::string FoldForSearch(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](char character) {
    const unsigned char byte = static_cast<unsigned char>(character);
    return byte < 0x80 ? static_cast<char>(std::tolower(byte)) : character;
  });
  return value;
}

std::string Trim(std::string value) {
  const auto is_space = [](unsigned char character) {
    return std::isspace(character) != 0;
  };
  value.erase(value.begin(),
              std::find_if_not(value.begin(), value.end(), is_space));
  value.erase(std::find_if_not(value.rbegin(), value.rend(), is_space).base(),
              value.end());
  return value;
}

bool Matches(const BrowserItemRef& item, const BrowserQuery& query) {
  const std::string folded_query = FoldForSearch(Trim(query.text));
  if (!folded_query.empty() &&
      FoldForSearch(item.name).find(folded_query) == std::string::npos) {
    return false;
  }
  if (query.status && item.status != *query.status) return false;
  return !query.view_kind || item.type == core::ViewKindName(*query.view_kind);
}

BrowserResult BuildResult(std::vector<BrowserItemRef> all,
                          const BrowserQuery& query, bool parent_writable,
                          BrowserScope scope) {
  BrowserResult result;
  const std::string folded_query = FoldForSearch(Trim(query.text));
  for (const BrowserItemRef& item : all) {
    if (!folded_query.empty() && FoldForSearch(item.name) == folded_query) {
      result.exact_match = item;
      result.exact_match_visible = Matches(item, query);
    }
    if (Matches(item, query)) result.items.push_back(item);
  }
  std::sort(result.items.begin(), result.items.end(),
            [](const BrowserItemRef& left, const BrowserItemRef& right) {
              return FoldForSearch(left.name) < FoldForSearch(right.name);
            });
  result.can_create = !folded_query.empty() && !result.exact_match &&
                      parent_writable &&
                      core::ValidateDisplayName(Trim(query.text)).Ok();
  if (result.can_create) {
    BrowserItemRef create;
    create.name = Trim(query.text);
    create.type = scope == BrowserScope::kLibrary ? "New Library"
                  : scope == BrowserScope::kCell  ? "New Cell"
                                                  : "New View";
    create.status = core::LibraryStatus::kEmpty;
    create.create_action = true;
    result.items.push_back(std::move(create));
  }
  return result;
}

const LibraryRecord* FindLibrary(const std::vector<LibraryRecord>& libraries,
                                 std::string_view id) {
  const auto iterator = std::find_if(
      libraries.begin(), libraries.end(),
      [id](const LibraryRecord& record) { return record.library.id == id; });
  return iterator == libraries.end() ? nullptr : &*iterator;
}

bool IsWritable(const LibraryRecord& record) {
  return record.status == core::LibraryStatus::kReady ||
         record.status == core::LibraryStatus::kEmpty;
}

std::filesystem::path Utf8Path(std::string_view value) {
  std::u8string utf8;
  utf8.reserve(value.size());
  for (const char character : value) {
    utf8.push_back(static_cast<char8_t>(character));
  }
  return std::filesystem::path(utf8);
}

}  // namespace

core::LibraryStatus ComputeViewStatus(const LibraryRecord& library,
                                      const core::View& view) {
  if (!IsWritable(library)) return library.status;
  if (view.files.empty()) return core::LibraryStatus::kEmpty;
  std::error_code error;
  for (const core::ManagedFile& file : view.files) {
    const std::filesystem::path path =
        library.directory / Utf8Path(file.relative_path);
    if (!std::filesystem::is_regular_file(path, error) || error) {
      return core::LibraryStatus::kMissing;
    }
  }
  return core::LibraryStatus::kReady;
}

core::LibraryStatus ComputeCellStatus(const LibraryRecord& library,
                                      const core::Cell& cell) {
  if (!IsWritable(library)) return library.status;
  if (cell.views.empty()) return core::LibraryStatus::kEmpty;
  bool has_ready = false;
  for (const core::View& view : cell.views) {
    const core::LibraryStatus status = ComputeViewStatus(library, view);
    if (status == core::LibraryStatus::kMissing ||
        status == core::LibraryStatus::kInvalid) {
      return status;
    }
    has_ready |= status == core::LibraryStatus::kReady;
  }
  return has_ready ? core::LibraryStatus::kReady : core::LibraryStatus::kEmpty;
}

LibraryBrowserModel::LibraryBrowserModel(
    const std::vector<LibraryRecord>& libraries)
    : libraries_(libraries) {}

BrowserResult LibraryBrowserModel::FilterLibraries(
    const BrowserQuery& query) const {
  std::vector<BrowserItemRef> items;
  items.reserve(libraries_.size());
  for (const LibraryRecord& record : libraries_) {
    items.push_back({record.library.id,
                     {},
                     {},
                     record.library.name,
                     "Library",
                     record.status,
                     false});
  }
  return BuildResult(std::move(items), query, true, BrowserScope::kLibrary);
}

BrowserResult LibraryBrowserModel::FilterCells(
    std::string_view library_id, const BrowserQuery& query) const {
  const LibraryRecord* library = FindLibrary(libraries_, library_id);
  if (library == nullptr) return {};
  std::vector<BrowserItemRef> items;
  items.reserve(library->library.cells.size());
  for (const core::Cell& cell : library->library.cells) {
    items.push_back({library->library.id,
                     cell.id,
                     {},
                     cell.name,
                     "Cell",
                     ComputeCellStatus(*library, cell),
                     false});
  }
  BrowserResult result = BuildResult(std::move(items), query,
                                     IsWritable(*library), BrowserScope::kCell);
  if (result.can_create) result.items.back().library_id = library->library.id;
  return result;
}

BrowserResult LibraryBrowserModel::FilterViews(
    std::string_view library_id, std::string_view cell_id,
    const BrowserQuery& query) const {
  const LibraryRecord* library = FindLibrary(libraries_, library_id);
  if (library == nullptr) return {};
  const auto cell = std::find_if(
      library->library.cells.begin(), library->library.cells.end(),
      [cell_id](const core::Cell& item) { return item.id == cell_id; });
  if (cell == library->library.cells.end()) return {};
  std::vector<BrowserItemRef> items;
  items.reserve(cell->views.size());
  for (const core::View& view : cell->views) {
    items.push_back({library->library.id, cell->id, view.id, view.name,
                     std::string(core::ViewKindName(view.kind)),
                     ComputeViewStatus(*library, view), false});
  }
  BrowserResult result = BuildResult(std::move(items), query,
                                     IsWritable(*library), BrowserScope::kView);
  if (result.can_create) {
    result.items.back().library_id = library->library.id;
    result.items.back().cell_id = cell->id;
  }
  return result;
}

}  // namespace designpp::application

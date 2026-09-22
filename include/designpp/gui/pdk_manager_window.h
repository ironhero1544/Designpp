// Copyright 2026 The Design++ Authors
#ifndef DESIGNPP_GUI_PDK_MANAGER_WINDOW_H_
#define DESIGNPP_GUI_PDK_MANAGER_WINDOW_H_

#include <windows.h>

#include <functional>
#include <memory>
#include <string>

#include "designpp/application/library_service.h"
#include "designpp/runtime/execution_provider.h"

namespace designpp::gui {
class PdkManagerWindow final {
 public:
  explicit PdkManagerWindow(
      runtime::ExecutionProvider* execution_provider = nullptr);
  ~PdkManagerWindow();
  PdkManagerWindow(const PdkManagerWindow&) = delete;
  PdkManagerWindow& operator=(const PdkManagerWindow&) = delete;
  [[nodiscard]] bool CreateOrShow(HINSTANCE instance, HWND owner,
                                  application::LibraryRecord library,
                                  std::string cell_id, std::string cell_name,
                                  std::function<void()> manage_paths,
                                  std::function<void()> saved);

 private:
  struct Implementation;
  std::unique_ptr<Implementation> implementation_;
};
}  // namespace designpp::gui
#endif  // DESIGNPP_GUI_PDK_MANAGER_WINDOW_H_

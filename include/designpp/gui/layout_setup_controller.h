// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_GUI_LAYOUT_SETUP_CONTROLLER_H_
#define DESIGNPP_GUI_LAYOUT_SETUP_CONTROLLER_H_

#include <windows.h>

#include <functional>
#include <memory>
#include <vector>

#include "designpp/adapters/orfs_adapter.h"
#include "designpp/application/project_service.h"

namespace designpp::gui {

using LayoutConfigurationSaveCallback =
    std::function<void(const core::PhysicalImplementationConfiguration&)>;
class LayoutJsonSession {
 public:
  virtual ~LayoutJsonSession() = default;
  virtual bool IsOpen() const = 0;
  virtual void Close() = 0;
  virtual void Saving() = 0;
  virtual void Saved(const core::Status& status) = 0;
};
using LayoutJsonApplyCallback =
    std::function<void(const core::PhysicalImplementationConfiguration&, bool)>;
using LayoutJsonEditorCallback =
    std::function<std::unique_ptr<LayoutJsonSession>(
        HWND, const core::PhysicalImplementationConfiguration&,
        LayoutJsonApplyCallback)>;

struct LayoutSetupDialogState;

// UI-thread-owned modeless Setup session. Completion must be delivered on the
// owning thread; destruction invalidates all controls before releasing state.
class LayoutSetupController final {
 public:
  LayoutSetupController();
  ~LayoutSetupController();
  bool Open(HWND owner, HINSTANCE instance,
            const core::PhysicalImplementationConfiguration& configuration,
            const std::vector<const application::ResolvedSource*>& sources,
            const std::vector<adapters::OrfsPlatformCandidate>& platforms,
            LayoutJsonEditorCallback json_editor,
            LayoutConfigurationSaveCallback save);
  void Saved(const core::Status& status,
             const core::PhysicalImplementationConfiguration& configuration);
  bool RequestClose();
  void Close();
  bool TranslateAccelerator(const MSG& message);

 private:
  std::unique_ptr<LayoutSetupDialogState> state_;
};

}  // namespace designpp::gui
#endif  // DESIGNPP_GUI_LAYOUT_SETUP_CONTROLLER_H_

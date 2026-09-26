// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_GUI_TOOLCHAIN_DOCTOR_WINDOW_H_
#define DESIGNPP_GUI_TOOLCHAIN_DOCTOR_WINDOW_H_

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "designpp/application/toolchain_doctor_service.h"
#include "designpp/application/toolchain_profile_store.h"
#include "designpp/application/wsl_distribution_service.h"
#include "designpp/core/toolchain_profile.h"
#include "designpp/gui/dpi.h"
#include "designpp/runtime/execution_provider.h"
#include "designpp/runtime/task_scheduler.h"

namespace designpp::gui {

// Edits persisted WSL toolchain profiles and diagnoses the selected profile in
// an independent top-level window.
class ToolchainDoctorWindow final {
 public:
  ToolchainDoctorWindow();
  ToolchainDoctorWindow(const ToolchainDoctorWindow&) = delete;
  ToolchainDoctorWindow& operator=(const ToolchainDoctorWindow&) = delete;
  ~ToolchainDoctorWindow();

  [[nodiscard]] bool CreateOrShow(HINSTANCE instance, HWND owner);

 private:
  struct EventChannel;
  struct UiEvent;

  static constexpr UINT kEventMessage = WM_APP + 71;

  static LRESULT CALLBACK WindowProcedure(HWND window, UINT message,
                                          WPARAM wparam, LPARAM lparam);
  LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);

  [[nodiscard]] bool CreateControls();
  void LayoutControls(int width, int height) const;
  void ApplyDpi(UINT dpi);
  void BeginLoad();
  void BeginDistributionDiscovery(std::uint64_t generation);
  void BeginSave();
  void AddProfile();
  void DuplicateProfile();
  void DeleteProfile();
  void BeginDiagnosis();
  void CancelDiagnosis();
  void HandleEvents();
  void PopulateProfiles();
  void PopulateSelectedProfile();
  void PopulateDistributions();
  [[nodiscard]] bool ReadSelectedProfile(core::ToolchainProfile* profile);
  void PopulateChecks();
  void ApplyDoctorEvent(application::DoctorEvent event);
  void SetBusy(bool busy) const;
  void SetStatus(std::wstring_view text) const;
  void ShutdownChannel();

  HINSTANCE instance_ = nullptr;
  HWND owner_ = nullptr;
  HWND window_ = nullptr;
  HWND profile_selector_ = nullptr;
  HWND name_edit_ = nullptr;
  HWND distribution_edit_ = nullptr;
  HWND openlane_edit_ = nullptr;
  HWND orfs_edit_ = nullptr;
  HWND pdk_edit_ = nullptr;
  HWND cpu_edit_ = nullptr;
  HWND add_button_ = nullptr;
  HWND duplicate_button_ = nullptr;
  HWND delete_button_ = nullptr;
  HWND save_button_ = nullptr;
  HWND diagnose_button_ = nullptr;
  HWND repair_wsl_button_ = nullptr;
  HWND cancel_button_ = nullptr;
  HWND checks_ = nullptr;
  HWND status_ = nullptr;
  UINT dpi_ = kDefaultDpi;
  UniqueFont font_;
  core::ToolchainSettings settings_;
  std::vector<application::WslDistribution> distributions_;
  std::vector<std::string> distribution_values_;
  std::uint64_t generation_ = 0;
  std::shared_ptr<EventChannel> event_channel_;
  application::ToolchainProfileStore store_;
  application::WslDistributionService distribution_service_;
  runtime::TaskScheduler scheduler_{1};
  runtime::WslExecutionProvider execution_provider_;
  application::ToolchainDoctorService doctor_;
};

}  // namespace designpp::gui

#endif  // DESIGNPP_GUI_TOOLCHAIN_DOCTOR_WINDOW_H_

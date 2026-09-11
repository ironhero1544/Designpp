// Copyright 2026 The Design++ Authors

#include "designpp/gui/layout_setup_controller.h"

#include <commctrl.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cwchar>
#include <sstream>
#include <utility>

#include "designpp/adapters/openlane2_adapter.h"
#include "designpp/application/layout_setup_draft.h"

namespace designpp::gui {
struct LayoutSetupDialogState {
  // JSON callbacks can outlive their window. Access remains UI-thread-only;
  // this token prevents dereferencing the old state after a session is
  // replaced.
  std::shared_ptr<const int> lifetime = std::make_shared<const int>(0);
  core::PhysicalImplementationConfiguration configuration;
  HWND window = nullptr;
  bool saving = false;
  bool dirty = false;
  bool populating = false;
  bool close_after_save = false;
  std::vector<application::ResolvedSource> owned_sources;
  std::vector<const application::ResolvedSource*> source_pointers;
  std::vector<adapters::OrfsPlatformCandidate> owned_platforms;
  std::vector<std::string> platform_names;
  const std::vector<const application::ResolvedSource*>* sdc_candidates =
      nullptr;
  const std::vector<adapters::OrfsPlatformCandidate>* orfs_candidates = nullptr;
  LayoutJsonEditorCallback json_editor;
  LayoutConfigurationSaveCallback save_configuration;
  UINT dpi = 96;
  HFONT font = nullptr;
  HWND tabs = nullptr;
  HWND io_side_tabs = nullptr;
  std::array<HWND, 4> pages{};
  HWND json_button = nullptr;
  HWND save_button = nullptr;
  HWND cancel_button = nullptr;
  HWND status = nullptr;
  std::unique_ptr<LayoutJsonSession> json_session;
};

namespace {
constexpr int kSetupOk = 7701;
constexpr int kSetupCancel = 7702;
constexpr int kSetupPdk = 7703;
constexpr int kSetupScl = 7704;
constexpr int kSetupClock = 7705;
constexpr int kSetupPeriod = 7706;
constexpr int kSetupUtilization = 7707;
constexpr int kSetupDensity = 7708;
constexpr int kSetupDieArea = 7709;
constexpr int kSetupPnrSdc = 7710;
constexpr int kSetupSignoffSdc = 7711;
constexpr int kPdnMultilayer = 7723;
constexpr int kPdnCoreRing = 7724;
constexpr int kPdnRails = 7725;
constexpr int kPdnVerticalWidth = 7726;
constexpr int kPdnHorizontalWidth = 7727;
constexpr int kPdnVerticalSpacing = 7728;
constexpr int kPdnHorizontalSpacing = 7729;
constexpr int kPdnVerticalPitch = 7730;
constexpr int kPdnHorizontalPitch = 7731;
constexpr int kPdnVerticalOffset = 7732;
constexpr int kPdnHorizontalOffset = 7733;
constexpr int kSetupCoreArea = 7734;
constexpr int kSetupTapCellDistance = 7735;
constexpr int kSetupTabs = 7740;
constexpr int kSetupEditJson = 7741;
constexpr int kSetupStatus = 7742;
constexpr int kSetupIoAlgorithm = 7750;
constexpr int kSetupIoUnmatched = 7751;
constexpr int kSetupIoMinimumDistance = 7752;
constexpr int kSetupIoVerticalLength = 7753;
constexpr int kSetupIoHorizontalLength = 7754;
constexpr int kSetupIoVerticalThickness = 7755;
constexpr int kSetupIoHorizontalThickness = 7756;
constexpr int kSetupIoVerticalExtension = 7757;
constexpr int kSetupIoHorizontalExtension = 7758;
constexpr int kSetupIoVerticalLayer = 7759;
constexpr int kSetupIoHorizontalLayer = 7760;
constexpr int kSetupIoNorthDistance = 7761;
constexpr int kSetupIoSouthDistance = 7762;
constexpr int kSetupIoEastDistance = 7763;
constexpr int kSetupIoWestDistance = 7764;
constexpr int kSetupIoNorthSort = 7765;
constexpr int kSetupIoSouthSort = 7766;
constexpr int kSetupIoEastSort = 7767;
constexpr int kSetupIoWestSort = 7768;
constexpr int kSetupIoNorthEntries = 7769;
constexpr int kSetupIoSouthEntries = 7770;
constexpr int kSetupIoEastEntries = 7771;
constexpr int kSetupIoWestEntries = 7772;
constexpr int kSetupIoSideTabs = 7773;
constexpr int kSetupGeneralHint = 7790;
constexpr int kSetupFloorplanHint = 7791;
constexpr int kSetupIoHint = 7792;
constexpr int kSetupPdnHint = 7793;
constexpr int kSetupBackend = 7794;
constexpr int kSetupOrfsPlatform = 7795;
constexpr int kSetupOrfsVariant = 7796;

std::wstring Utf8ToWide(std::string_view text) {
  if (text.empty()) return {};
  const int length =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), nullptr, 0);
  if (length <= 0) return L"[invalid UTF-8]";
  std::wstring result(static_cast<std::size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                      static_cast<int>(text.size()), result.data(), length);
  return result;
}

std::string WideToUtf8(std::wstring_view text) {
  if (text.empty()) return {};
  const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr,
                                         0, nullptr, nullptr);
  if (length <= 0) return {};
  std::string result(static_cast<std::size_t>(length), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                      result.data(), length, nullptr, nullptr);
  return result;
}

std::wstring ControlText(HWND control) {
  const int length = GetWindowTextLengthW(control);
  std::wstring value(static_cast<std::size_t>(std::max(length, 0)) + 1, L'\0');
  GetWindowTextW(control, value.data(), static_cast<int>(value.size()));
  value.resize(std::wcslen(value.c_str()));
  return value;
}

void SetControlText(HWND control, std::string_view text) {
  const std::wstring wide = Utf8ToWide(text);
  SetWindowTextW(control, wide.c_str());
}

std::vector<std::string> Split(std::string_view value, char delimiter) {
  std::vector<std::string> result;
  std::size_t begin = 0;
  while (begin <= value.size()) {
    const std::size_t end = value.find(delimiter, begin);
    std::string item(value.substr(begin, end - begin));
    const std::size_t first = item.find_first_not_of(" \t\r\n");
    const std::size_t last = item.find_last_not_of(" \t\r\n");
    if (first != std::string::npos) {
      result.push_back(item.substr(first, last - first + 1));
    }
    if (end == std::string_view::npos) break;
    begin = end + 1;
  }
  return result;
}

std::string Join(const std::vector<std::string>& values,
                 std::string_view delimiter) {
  std::ostringstream output;
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (index != 0) output << delimiter;
    output << values[index];
  }
  return output.str();
}

BOOL CALLBACK ApplyFontToChild(HWND window, LPARAM font_value) {
  SendMessageW(window, WM_SETFONT, static_cast<WPARAM>(font_value), TRUE);
  return TRUE;
}

void UpdateDialogFont(HWND window, UINT dpi, HFONT* owned_font) {
  NONCLIENTMETRICSW metrics{sizeof(metrics)};
  LOGFONTW logical_font{};
  if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics),
                                 &metrics, 0, dpi)) {
    logical_font = metrics.lfMessageFont;
  } else {
    logical_font.lfHeight = -MulDiv(9, static_cast<int>(dpi), 72);
    wcscpy_s(logical_font.lfFaceName, L"Segoe UI");
  }
  HFONT replacement = CreateFontIndirectW(&logical_font);
  if (!replacement) return;
  EnumChildWindows(window, ApplyFontToChild,
                   reinterpret_cast<LPARAM>(replacement));
  if (*owned_font) DeleteObject(*owned_font);
  *owned_font = replacement;
}

HWND SetupControl(const LayoutSetupDialogState& state, int id) {
  for (HWND page : state.pages) {
    if (HWND control = GetDlgItem(page, id)) return control;
  }
  return nullptr;
}

std::optional<std::string> OptionalControlText(HWND control) {
  const std::string value = WideToUtf8(ControlText(control));
  return value.empty() ? std::optional<std::string>{}
                       : std::optional<std::string>{value};
}

void SetOptionalControlText(HWND control,
                            const std::optional<std::string>& value) {
  SetControlText(control, value.value_or(""));
}

void PopulateSetupControls(LayoutSetupDialogState* state) {
  state->populating = true;
  const auto draft = application::MakeLayoutSetupDraft(state->configuration);
  const auto& configuration = draft.values;
  const auto select_text = [](HWND combo, std::string_view value) {
    const std::wstring wide = Utf8ToWide(value);
    const LRESULT index =
        SendMessageW(combo, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1),
                     reinterpret_cast<LPARAM>(wide.c_str()));
    SendMessageW(combo, CB_SETCURSEL, index == CB_ERR ? 0 : index, 0);
  };
  select_text(SetupControl(*state, kSetupBackend),
              configuration.backend_id == "orfs" ? "orfs" : "openlane2");
  SetControlText(SetupControl(*state, kSetupPdk), configuration.pdk);
  SetControlText(SetupControl(*state, kSetupScl),
                 configuration.standard_cell_library);
  const std::string orfs_platform = configuration.orfs.platform.empty()
                                        ? "sky130hd"
                                        : configuration.orfs.platform;
  const auto platform = std::find(state->platform_names.begin(),
                                  state->platform_names.end(), orfs_platform);
  if (platform == state->platform_names.end()) {
    state->platform_names.emplace_back(orfs_platform);
    SendMessageW(SetupControl(*state, kSetupOrfsPlatform), CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(Utf8ToWide(orfs_platform).c_str()));
  }
  const auto selected_platform =
      std::find(state->platform_names.begin(), state->platform_names.end(),
                orfs_platform);
  SendMessageW(
      SetupControl(*state, kSetupOrfsPlatform), CB_SETCURSEL,
      static_cast<WPARAM>(selected_platform - state->platform_names.begin()),
      0);
  SetControlText(SetupControl(*state, kSetupOrfsVariant),
                 configuration.orfs.flow_variant);
  SetControlText(SetupControl(*state, kSetupClock),
                 Join(configuration.clock_ports, ";"));
  SetControlText(SetupControl(*state, kSetupPeriod),
                 configuration.clock_period_ns);
  SetControlText(SetupControl(*state, kSetupUtilization), draft.utilization);
  SetOptionalControlText(SetupControl(*state, kSetupDensity),
                         configuration.placement_density_percent);
  SetControlText(SetupControl(*state, kSetupDieArea),
                 Join(configuration.die_area, ","));
  SetControlText(SetupControl(*state, kSetupCoreArea),
                 Join(configuration.core_area, ","));
  SetOptionalControlText(SetupControl(*state, kSetupTapCellDistance),
                         configuration.tap_cell_distance_um);
  const auto& io = configuration.io_placement;
  select_text(SetupControl(*state, kSetupIoAlgorithm), io.algorithm);
  select_text(SetupControl(*state, kSetupIoUnmatched), io.unmatched_policy);
  SetOptionalControlText(SetupControl(*state, kSetupIoMinimumDistance),
                         io.minimum_distance_um);
  SetOptionalControlText(SetupControl(*state, kSetupIoVerticalLength),
                         io.vertical_length_um);
  SetOptionalControlText(SetupControl(*state, kSetupIoHorizontalLength),
                         io.horizontal_length_um);
  SetOptionalControlText(SetupControl(*state, kSetupIoVerticalThickness),
                         io.vertical_thickness_multiplier);
  SetOptionalControlText(SetupControl(*state, kSetupIoHorizontalThickness),
                         io.horizontal_thickness_multiplier);
  SetOptionalControlText(SetupControl(*state, kSetupIoVerticalExtension),
                         io.vertical_extension_um);
  SetOptionalControlText(SetupControl(*state, kSetupIoHorizontalExtension),
                         io.horizontal_extension_um);
  SetOptionalControlText(SetupControl(*state, kSetupIoVerticalLayer),
                         io.vertical_layer);
  SetOptionalControlText(SetupControl(*state, kSetupIoHorizontalLayer),
                         io.horizontal_layer);
  const std::array<const core::IoPinSideConfiguration*, 4> sides = {
      &io.north, &io.south, &io.east, &io.west};
  const int distance_ids[] = {kSetupIoNorthDistance, kSetupIoSouthDistance,
                              kSetupIoEastDistance, kSetupIoWestDistance};
  const int sort_ids[] = {kSetupIoNorthSort, kSetupIoSouthSort,
                          kSetupIoEastSort, kSetupIoWestSort};
  const int entry_ids[] = {kSetupIoNorthEntries, kSetupIoSouthEntries,
                           kSetupIoEastEntries, kSetupIoWestEntries};
  for (std::size_t index = 0; index < sides.size(); ++index) {
    SetOptionalControlText(SetupControl(*state, distance_ids[index]),
                           sides[index]->minimum_distance_um);
    SendMessageW(SetupControl(*state, sort_ids[index]), CB_SETCURSEL,
                 sides[index]->bit_major ? 1 : 0, 0);
    SetControlText(SetupControl(*state, entry_ids[index]),
                   Join(sides[index]->entries, "\r\n"));
  }
  const auto& pdn = configuration.power_distribution;
  SendMessageW(SetupControl(*state, kPdnMultilayer), BM_SETCHECK,
               core::UsesAutomaticValue(configuration, "pdn.multilayer")
                   ? BST_INDETERMINATE
               : pdn.multilayer ? BST_CHECKED
                                : BST_UNCHECKED,
               0);
  SendMessageW(SetupControl(*state, kPdnCoreRing), BM_SETCHECK,
               core::UsesAutomaticValue(configuration, "pdn.core_ring")
                   ? BST_INDETERMINATE
               : pdn.core_ring ? BST_CHECKED
                               : BST_UNCHECKED,
               0);
  SendMessageW(SetupControl(*state, kPdnRails), BM_SETCHECK,
               core::UsesAutomaticValue(configuration, "pdn.enable_rails")
                   ? BST_INDETERMINATE
               : pdn.enable_rails ? BST_CHECKED
                                  : BST_UNCHECKED,
               0);
  const int pdn_ids[] = {kPdnVerticalWidth,   kPdnHorizontalWidth,
                         kPdnVerticalSpacing, kPdnHorizontalSpacing,
                         kPdnVerticalPitch,   kPdnHorizontalPitch,
                         kPdnVerticalOffset,  kPdnHorizontalOffset};
  const std::optional<std::string>* pdn_values[] = {
      &pdn.vertical_width_um,   &pdn.horizontal_width_um,
      &pdn.vertical_spacing_um, &pdn.horizontal_spacing_um,
      &pdn.vertical_pitch_um,   &pdn.horizontal_pitch_um,
      &pdn.vertical_offset_um,  &pdn.horizontal_offset_um};
  for (std::size_t index = 0; index < std::size(pdn_ids); ++index) {
    SetOptionalControlText(SetupControl(*state, pdn_ids[index]),
                           *pdn_values[index]);
  }
  state->populating = false;
}

void UpdateBackendFields(LayoutSetupDialogState* state) {
  const HWND backend = SetupControl(*state, kSetupBackend);
  const LRESULT selected = SendMessageW(backend, CB_GETCURSEL, 0, 0);
  wchar_t value[64]{};
  if (selected != CB_ERR) {
    SendMessageW(backend, CB_GETLBTEXT, selected,
                 reinterpret_cast<LPARAM>(value));
  }
  const bool orfs = std::wstring_view(value) == L"orfs";
  for (int id : {kSetupOrfsPlatform, kSetupOrfsVariant}) {
    EnableWindow(SetupControl(*state, id), orfs);
  }
  for (int id : {kSetupPdk,
                 kSetupScl,
                 kSetupIoAlgorithm,
                 kSetupIoUnmatched,
                 kSetupIoMinimumDistance,
                 kSetupIoVerticalLength,
                 kSetupIoHorizontalLength,
                 kSetupIoVerticalThickness,
                 kSetupIoHorizontalThickness,
                 kSetupIoVerticalExtension,
                 kSetupIoHorizontalExtension,
                 kSetupIoVerticalLayer,
                 kSetupIoHorizontalLayer,
                 kPdnMultilayer,
                 kPdnCoreRing,
                 kPdnRails,
                 kPdnVerticalWidth,
                 kPdnHorizontalWidth,
                 kPdnVerticalSpacing,
                 kPdnHorizontalSpacing,
                 kPdnVerticalPitch,
                 kPdnHorizontalPitch,
                 kPdnVerticalOffset,
                 kPdnHorizontalOffset}) {
    EnableWindow(SetupControl(*state, id), !orfs);
  }
  for (int id :
       {kSetupIoNorthDistance, kSetupIoSouthDistance, kSetupIoEastDistance,
        kSetupIoWestDistance, kSetupIoNorthSort, kSetupIoSouthSort,
        kSetupIoEastSort, kSetupIoWestSort, kSetupIoNorthEntries,
        kSetupIoSouthEntries, kSetupIoEastEntries, kSetupIoWestEntries}) {
    EnableWindow(SetupControl(*state, id), !orfs);
  }
}

core::Status ReadSetupControls(LayoutSetupDialogState* state) {
  auto configuration = state->configuration;
  const auto combo_text = [state](int id) {
    HWND combo = SetupControl(*state, id);
    const LRESULT selected = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (selected == CB_ERR) return std::string{};
    const LRESULT length = SendMessageW(combo, CB_GETLBTEXTLEN, selected, 0);
    if (length == CB_ERR || length > 65536) return std::string{};
    std::wstring buffer(static_cast<std::size_t>(length) + 1, L'\0');
    SendMessageW(combo, CB_GETLBTEXT, selected,
                 reinterpret_cast<LPARAM>(buffer.data()));
    buffer.resize(static_cast<std::size_t>(length));
    return WideToUtf8(buffer);
  };
  configuration.backend_id = combo_text(kSetupBackend);
  if (configuration.backend_id.empty()) configuration.backend_id = "openlane2";
  configuration.pdk = WideToUtf8(ControlText(SetupControl(*state, kSetupPdk)));
  configuration.standard_cell_library =
      WideToUtf8(ControlText(SetupControl(*state, kSetupScl)));
  const LRESULT selected_platform = SendMessageW(
      SetupControl(*state, kSetupOrfsPlatform), CB_GETCURSEL, 0, 0);
  if (selected_platform != CB_ERR && selected_platform >= 0 &&
      static_cast<std::size_t>(selected_platform) <
          state->platform_names.size()) {
    configuration.orfs.platform =
        state->platform_names[static_cast<std::size_t>(selected_platform)];
  } else {
    configuration.orfs.platform = combo_text(kSetupOrfsPlatform);
  }
  // A selection in the Setup combo is an explicit user choice.  It must not
  // remain marked automatic from an older migrated project.
  std::erase(configuration.automatic_fields, "orfs.platform");
  configuration.orfs.flow_variant =
      WideToUtf8(ControlText(SetupControl(*state, kSetupOrfsVariant)));
  configuration.clock_ports =
      Split(WideToUtf8(ControlText(SetupControl(*state, kSetupClock))), ';');
  configuration.clock_period_ns =
      WideToUtf8(ControlText(SetupControl(*state, kSetupPeriod)));
  const std::string utilization =
      WideToUtf8(ControlText(SetupControl(*state, kSetupUtilization)));
  configuration.placement_density_percent =
      OptionalControlText(SetupControl(*state, kSetupDensity));
  configuration.die_area =
      Split(WideToUtf8(ControlText(SetupControl(*state, kSetupDieArea))), ',');
  configuration.core_area =
      Split(WideToUtf8(ControlText(SetupControl(*state, kSetupCoreArea))), ',');
  configuration.tap_cell_distance_um =
      OptionalControlText(SetupControl(*state, kSetupTapCellDistance));
  const auto selected_path = [state](int id) {
    const LRESULT selected =
        SendMessageW(SetupControl(*state, id), CB_GETCURSEL, 0, 0);
    return selected > 0 && static_cast<std::size_t>(selected) <=
                               state->sdc_candidates->size()
               ? (*state
                       ->sdc_candidates)[static_cast<std::size_t>(selected - 1)]
                     ->relative_path
               : std::string{};
  };
  configuration.pnr_sdc_path = selected_path(kSetupPnrSdc);
  configuration.signoff_sdc_path = selected_path(kSetupSignoffSdc);
  auto& io = configuration.io_placement;
  io.algorithm = combo_text(kSetupIoAlgorithm);
  io.unmatched_policy = combo_text(kSetupIoUnmatched);
  io.minimum_distance_um =
      OptionalControlText(SetupControl(*state, kSetupIoMinimumDistance));
  io.vertical_length_um =
      OptionalControlText(SetupControl(*state, kSetupIoVerticalLength));
  io.horizontal_length_um =
      OptionalControlText(SetupControl(*state, kSetupIoHorizontalLength));
  io.vertical_thickness_multiplier =
      OptionalControlText(SetupControl(*state, kSetupIoVerticalThickness));
  io.horizontal_thickness_multiplier =
      OptionalControlText(SetupControl(*state, kSetupIoHorizontalThickness));
  io.vertical_extension_um =
      OptionalControlText(SetupControl(*state, kSetupIoVerticalExtension));
  io.horizontal_extension_um =
      OptionalControlText(SetupControl(*state, kSetupIoHorizontalExtension));
  io.vertical_layer =
      OptionalControlText(SetupControl(*state, kSetupIoVerticalLayer));
  io.horizontal_layer =
      OptionalControlText(SetupControl(*state, kSetupIoHorizontalLayer));
  const int distance_ids[] = {kSetupIoNorthDistance, kSetupIoSouthDistance,
                              kSetupIoEastDistance, kSetupIoWestDistance};
  const int sort_ids[] = {kSetupIoNorthSort, kSetupIoSouthSort,
                          kSetupIoEastSort, kSetupIoWestSort};
  const int entry_ids[] = {kSetupIoNorthEntries, kSetupIoSouthEntries,
                           kSetupIoEastEntries, kSetupIoWestEntries};
  core::IoPinSideConfiguration* sides[] = {&io.north, &io.south, &io.east,
                                           &io.west};
  for (std::size_t index = 0; index < std::size(sides); ++index) {
    sides[index]->minimum_distance_um =
        OptionalControlText(SetupControl(*state, distance_ids[index]));
    sides[index]->bit_major =
        SendMessageW(SetupControl(*state, sort_ids[index]), CB_GETCURSEL, 0,
                     0) == 1;
    sides[index]->entries = Split(
        WideToUtf8(ControlText(SetupControl(*state, entry_ids[index]))), '\n');
  }
  auto& pdn = configuration.power_distribution;
  for (const auto& [id, key] : {std::pair{kPdnMultilayer, "pdn.multilayer"},
                                std::pair{kPdnCoreRing, "pdn.core_ring"},
                                std::pair{kPdnRails, "pdn.enable_rails"}}) {
    std::erase(configuration.automatic_fields, std::string(key));
    if (SendMessageW(SetupControl(*state, id), BM_GETCHECK, 0, 0) ==
        BST_INDETERMINATE)
      configuration.automatic_fields.push_back(key);
  }
  pdn.multilayer = SendMessageW(SetupControl(*state, kPdnMultilayer),
                                BM_GETCHECK, 0, 0) == BST_CHECKED;
  pdn.core_ring = SendMessageW(SetupControl(*state, kPdnCoreRing), BM_GETCHECK,
                               0, 0) == BST_CHECKED;
  pdn.enable_rails = SendMessageW(SetupControl(*state, kPdnRails), BM_GETCHECK,
                                  0, 0) == BST_CHECKED;
  const int pdn_ids[] = {kPdnVerticalWidth,   kPdnHorizontalWidth,
                         kPdnVerticalSpacing, kPdnHorizontalSpacing,
                         kPdnVerticalPitch,   kPdnHorizontalPitch,
                         kPdnVerticalOffset,  kPdnHorizontalOffset};
  std::optional<std::string>* pdn_values[] = {
      &pdn.vertical_width_um,   &pdn.horizontal_width_um,
      &pdn.vertical_spacing_um, &pdn.horizontal_spacing_um,
      &pdn.vertical_pitch_um,   &pdn.horizontal_pitch_um,
      &pdn.vertical_offset_um,  &pdn.horizontal_offset_um};
  for (std::size_t index = 0; index < std::size(pdn_ids); ++index) {
    *pdn_values[index] =
        OptionalControlText(SetupControl(*state, pdn_ids[index]));
  }
  auto parsed = application::ValidateLayoutSetupDraft(
      {std::move(configuration), utilization});
  if (!parsed.Ok()) return parsed.GetStatus();
  state->configuration = std::move(parsed).Value();
  return core::Status::Success();
}

void SubmitSetupSave(LayoutSetupDialogState* state) {
  if (state->saving) return;
  core::Status status = ReadSetupControls(state);
  if (status.Ok()) {
    status = state->configuration.backend_id == "orfs"
                 ? adapters::OrfsAdapter().ValidateAdvancedVariables(
                       state->configuration.orfs.advanced_variables_json)
                 : adapters::OpenLane2Adapter().ValidateAdvancedOverrides(
                       state->configuration.advanced_overrides_json);
  }
  if (!status.Ok()) {
    state->close_after_save = false;
    SetControlText(state->status, status.message);
    return;
  }
  state->saving = true;
  if (state->json_session) state->json_session->Saving();
  for (HWND page : state->pages) EnableWindow(page, FALSE);
  EnableWindow(state->save_button, FALSE);
  EnableWindow(state->json_button, FALSE);
  SetWindowTextW(state->status, L"Saving selected Cell...");
  if (state->save_configuration)
    state->save_configuration(state->configuration);
}

LRESULT CALLBACK SetupPageProcedure(HWND window, UINT message, WPARAM wparam,
                                    LPARAM lparam) {
  const auto content_height = static_cast<int>(GetWindowLongPtrW(window, 0));
  int position = static_cast<int>(GetWindowLongPtrW(window, sizeof(LONG_PTR)));
  if (message == WM_SIZE || message == WM_VSCROLL || message == WM_MOUSEWHEEL) {
    RECT client{};
    GetClientRect(window, &client);
    const int maximum =
        std::max(0, content_height - static_cast<int>(client.bottom));
    int next = position;
    if (message == WM_VSCROLL) {
      switch (LOWORD(wparam)) {
        case SB_LINEUP:
          next -= 24;
          break;
        case SB_LINEDOWN:
          next += 24;
          break;
        case SB_PAGEUP:
          next -= client.bottom;
          break;
        case SB_PAGEDOWN:
          next += client.bottom;
          break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION:
          next = HIWORD(wparam);
          break;
      }
    } else if (message == WM_MOUSEWHEEL) {
      next -= GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA * 48;
    }
    next = std::clamp(next, 0, maximum);
    if (next != position) {
      ScrollWindowEx(window, 0, position - next, nullptr, nullptr, nullptr,
                     nullptr, SW_SCROLLCHILDREN | SW_INVALIDATE);
      SetWindowLongPtrW(window, sizeof(LONG_PTR), next);
      position = next;
    }
    SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS};
    info.nMax = std::max(content_height - 1, 0);
    info.nPage =
        static_cast<UINT>(std::max(static_cast<int>(client.bottom), 0));
    info.nPos = position;
    SetScrollInfo(window, SB_VERT, &info, TRUE);
    return 0;
  }
  if (message == WM_COMMAND) {
    HWND setup = GetParent(window);
    auto* session = reinterpret_cast<LayoutSetupDialogState*>(
        GetWindowLongPtrW(setup, GWLP_USERDATA));
    if (session && !session->populating) session->dirty = true;
  }
  if (message == WM_COMMAND && LOWORD(wparam) == kSetupBackend &&
      HIWORD(wparam) == CBN_SELCHANGE) {
    HWND setup_window = GetParent(window);
    auto* state = reinterpret_cast<LayoutSetupDialogState*>(
        GetWindowLongPtrW(setup_window, GWLP_USERDATA));
    if (state) UpdateBackendFields(state);
    return 0;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

HWND CreateSetupChild(HWND parent, const wchar_t* cls, const wchar_t* text,
                      DWORD style, int id) {
  if (std::wcscmp(cls, WC_COMBOBOXW) == 0) style |= CBS_HASSTRINGS;
  HWND result =
      CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0,
                      parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                      GetModuleHandleW(nullptr), nullptr);
  SendMessageW(result, WM_SETFONT,
               reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),
               TRUE);
  return result;
}

void CreateLabeledField(HWND page, int id, const wchar_t* label,
                        DWORD style = ES_AUTOHSCROLL) {
  CreateSetupChild(page, L"STATIC", label,
                   SS_LEFT | SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS, id + 1000);
  CreateSetupChild(page, L"EDIT", L"", WS_BORDER | style, id);
}

void MoveLabeledField(HWND page, int id, int x, int y, int label_width,
                      int field_width, int height = 27) {
  const UINT dpi = GetDpiForWindow(page);
  MoveWindow(GetDlgItem(page, id + 1000), x,
             y + MulDiv(3, static_cast<int>(dpi), 96), label_width,
             MulDiv(22, static_cast<int>(dpi), 96), TRUE);
  MoveWindow(GetDlgItem(page, id), x + label_width + 8, y, field_width, height,
             TRUE);
}

void ShowIoSide(LayoutSetupDialogState* state) {
  if (!state->io_side_tabs) return;
  const int selected = std::max(
      0,
      static_cast<int>(SendMessageW(state->io_side_tabs, TCM_GETCURSEL, 0, 0)));
  const int distance_ids[] = {kSetupIoNorthDistance, kSetupIoSouthDistance,
                              kSetupIoEastDistance, kSetupIoWestDistance};
  const int sort_ids[] = {kSetupIoNorthSort, kSetupIoSouthSort,
                          kSetupIoEastSort, kSetupIoWestSort};
  const int entry_ids[] = {kSetupIoNorthEntries, kSetupIoSouthEntries,
                           kSetupIoEastEntries, kSetupIoWestEntries};
  for (int index = 0; index < 4; ++index) {
    const int command = index == selected ? SW_SHOW : SW_HIDE;
    for (HWND control :
         {GetDlgItem(state->pages[2], entry_ids[index] + 1000),
          GetDlgItem(state->pages[2], distance_ids[index] + 1000),
          GetDlgItem(state->pages[2], sort_ids[index] + 1000),
          GetDlgItem(state->pages[2], distance_ids[index]),
          GetDlgItem(state->pages[2], sort_ids[index]),
          GetDlgItem(state->pages[2], entry_ids[index])}) {
      ShowWindow(control, command);
    }
  }
}

void LayoutSetupPages(LayoutSetupDialogState* state) {
  if (!state->tabs) return;
  const auto px = [state](int value) {
    return MulDiv(value, static_cast<int>(state->dpi), 96);
  };
  RECT tab_rect{};
  GetClientRect(state->tabs, &tab_rect);
  TabCtrl_AdjustRect(state->tabs, FALSE, &tab_rect);
  RECT tab_window{};
  GetWindowRect(state->tabs, &tab_window);
  MapWindowPoints(HWND_DESKTOP, GetParent(state->tabs),
                  reinterpret_cast<POINT*>(&tab_window), 2);
  OffsetRect(&tab_rect, tab_window.left, tab_window.top);
  for (HWND page : state->pages) {
    MoveWindow(page, tab_rect.left, tab_rect.top,
               tab_rect.right - tab_rect.left, tab_rect.bottom - tab_rect.top,
               TRUE);
  }
  const int width =
      std::max(static_cast<int>(tab_rect.right - tab_rect.left), px(500));
  HWND general = state->pages[0];
  MoveWindow(GetDlgItem(general, kSetupGeneralHint), px(16), px(12),
             width - px(32), px(30), TRUE);
  const int general_ids[] = {
      kSetupBackend,     kSetupPdk,    kSetupScl,
      kSetupClock,       kSetupPeriod, kSetupOrfsPlatform,
      kSetupOrfsVariant, kSetupPnrSdc, kSetupSignoffSdc};
  for (std::size_t row = 0; row < std::size(general_ids); ++row) {
    const int id = general_ids[row];
    MoveLabeledField(
        general, id, px(16), px(50 + static_cast<int>(row) * 42), px(180),
        width - px(230),
        id == kSetupBackend || id == kSetupPnrSdc || id == kSetupSignoffSdc
            ? px(160)
            : px(27));
  }
  HWND floorplan = state->pages[1];
  MoveWindow(GetDlgItem(floorplan, kSetupFloorplanHint), px(16), px(12),
             width - px(32), px(30), TRUE);
  const int floorplan_ids[] = {kSetupUtilization, kSetupDensity, kSetupDieArea,
                               kSetupCoreArea, kSetupTapCellDistance};
  for (std::size_t row = 0; row < std::size(floorplan_ids); ++row) {
    MoveLabeledField(floorplan, floorplan_ids[row], px(16),
                     px(50 + static_cast<int>(row) * 42), px(210),
                     width - px(260), px(27));
  }
  HWND io_page = state->pages[2];
  MoveWindow(GetDlgItem(io_page, kSetupIoHint), px(12), px(8), width - px(24),
             px(32), TRUE);
  const int left_ids[] = {kSetupIoAlgorithm,         kSetupIoMinimumDistance,
                          kSetupIoVerticalLength,    kSetupIoVerticalThickness,
                          kSetupIoVerticalExtension, kSetupIoVerticalLayer};
  const int right_ids[] = {
      kSetupIoUnmatched, kSetupIoHorizontalLength, kSetupIoHorizontalThickness,
      kSetupIoHorizontalExtension, kSetupIoHorizontalLayer};
  const int half = std::max(px(310), (width - px(48)) / 2);
  for (std::size_t row = 0; row < std::size(left_ids); ++row) {
    const int id = left_ids[row];
    MoveLabeledField(io_page, id, px(12), px(46 + static_cast<int>(row) * 36),
                     px(168), half - px(192),
                     id == kSetupIoAlgorithm ? px(160) : px(27));
  }
  for (std::size_t row = 0; row < std::size(right_ids); ++row) {
    const int id = right_ids[row];
    MoveLabeledField(
        io_page, id, px(24) + half, px(46 + static_cast<int>(row) * 36),
        px(168), half - px(192), id == kSetupIoUnmatched ? px(160) : px(27));
  }
  const int distance_ids[] = {kSetupIoNorthDistance, kSetupIoSouthDistance,
                              kSetupIoEastDistance, kSetupIoWestDistance};
  const int sort_ids[] = {kSetupIoNorthSort, kSetupIoSouthSort,
                          kSetupIoEastSort, kSetupIoWestSort};
  const int entry_ids[] = {kSetupIoNorthEntries, kSetupIoSouthEntries,
                           kSetupIoEastEntries, kSetupIoWestEntries};
  MoveWindow(state->io_side_tabs, px(12), px(270), width - px(24), px(300),
             TRUE);
  for (int index = 0; index < 4; ++index) {
    const int x = px(30);
    const int y = px(310);
    MoveWindow(GetDlgItem(io_page, distance_ids[index] + 1000), x, y, px(142),
               px(22), TRUE);
    MoveWindow(GetDlgItem(io_page, distance_ids[index]), x + px(146), y - px(4),
               px(130), px(27), TRUE);
    MoveWindow(GetDlgItem(io_page, sort_ids[index] + 1000), x + px(300), y,
               px(110), px(22), TRUE);
    MoveWindow(GetDlgItem(io_page, sort_ids[index]), x + px(414), y - px(4),
               std::max(px(130), width - px(456)), px(160), TRUE);
    MoveWindow(GetDlgItem(io_page, entry_ids[index] + 1000), x, y + px(40),
               width - px(60), px(22), TRUE);
    MoveWindow(GetDlgItem(io_page, entry_ids[index]), x, y + px(66),
               width - px(60), px(170), TRUE);
  }
  HWND pdn = state->pages[3];
  MoveWindow(GetDlgItem(pdn, kSetupPdnHint), px(16), px(10), width - px(32),
             px(30), TRUE);
  MoveWindow(GetDlgItem(pdn, kPdnMultilayer), px(16), px(48), px(220), px(26),
             TRUE);
  MoveWindow(GetDlgItem(pdn, kPdnCoreRing), px(250), px(48), px(180), px(26),
             TRUE);
  MoveWindow(GetDlgItem(pdn, kPdnRails), px(450), px(48), px(220), px(26),
             TRUE);
  const int pdn_ids[] = {kPdnVerticalWidth,   kPdnHorizontalWidth,
                         kPdnVerticalSpacing, kPdnHorizontalSpacing,
                         kPdnVerticalPitch,   kPdnHorizontalPitch,
                         kPdnVerticalOffset,  kPdnHorizontalOffset};
  for (std::size_t row = 0; row < std::size(pdn_ids); ++row) {
    MoveLabeledField(pdn, pdn_ids[row], px(16),
                     px(88 + static_cast<int>(row) * 40), px(210),
                     width - px(260), px(27));
  }
  for (HWND page : state->pages) {
    const int position =
        static_cast<int>(GetWindowLongPtrW(page, sizeof(LONG_PTR)));
    if (position > 0) {
      ScrollWindowEx(page, 0, -position, nullptr, nullptr, nullptr, nullptr,
                     SW_SCROLLCHILDREN | SW_INVALIDATE);
    }
  }
  ShowIoSide(state);
}

void ShowSetupPage(LayoutSetupDialogState* state) {
  const int selected =
      static_cast<int>(SendMessageW(state->tabs, TCM_GETCURSEL, 0, 0));
  for (std::size_t index = 0; index < state->pages.size(); ++index) {
    ShowWindow(state->pages[index],
               static_cast<int>(index) == selected ? SW_SHOW : SW_HIDE);
  }
}

LRESULT CALLBACK LayoutSetupProcedure(HWND window, UINT message, WPARAM wparam,
                                      LPARAM lparam) {
  auto* state = reinterpret_cast<LayoutSetupDialogState*>(
      GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    state = static_cast<LayoutSetupDialogState*>(create->lpCreateParams);
    state->window = window;
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
  }
  if (!state) return DefWindowProcW(window, message, wparam, lparam);
  if (message == WM_CREATE) {
    state->dpi = GetDpiForWindow(window);
    state->tabs =
        CreateSetupChild(window, WC_TABCONTROLW, L"", WS_TABSTOP, kSetupTabs);
    const wchar_t* tab_names[] = {L"General", L"Floorplan", L"I/O Placement",
                                  L"PDN"};
    for (int index = 0; index < 4; ++index) {
      TCITEMW item{};
      item.mask = TCIF_TEXT;
      item.pszText = const_cast<wchar_t*>(tab_names[index]);
      TabCtrl_InsertItem(state->tabs, index, &item);
      state->pages[index] =
          CreateWindowExW(0, L"DesignPlusPlus.LayoutSetupPage", L"",
                          WS_CHILD | WS_VISIBLE | WS_VSCROLL, 0, 0, 0, 0,
                          window, nullptr, GetModuleHandleW(nullptr), nullptr);
    }
    CreateSetupChild(
        state->pages[0], L"STATIC",
        L"Select the technology, clocks, and managed timing constraints.",
        SS_LEFT, kSetupGeneralHint);
    CreateSetupChild(
        state->pages[1], L"STATIC",
        L"Blank optional values inherit the selected PDK and SCL defaults.",
        SS_LEFT, kSetupFloorplanHint);
    CreateSetupChild(
        state->pages[2], L"STATIC",
        L"Use automatic placement or specify side-specific pin order below.",
        SS_LEFT, kSetupIoHint);
    CreateSetupChild(state->pages[3], L"STATIC",
                     L"Blank dimensions inherit the PDK grid; smaller cores "
                     L"may need lower offsets.",
                     SS_LEFT, kSetupPdnHint);
    CreateLabeledField(state->pages[0], kSetupBackend, L"Backend");
    DestroyWindow(GetDlgItem(state->pages[0], kSetupBackend));
    HWND backend = CreateSetupChild(
        state->pages[0], WC_COMBOBOXW, L"",
        CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL, kSetupBackend);
    for (const wchar_t* value : {L"openlane2", L"orfs"}) {
      SendMessageW(backend, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(value));
    }
    CreateLabeledField(state->pages[0], kSetupPdk, L"PDK");
    CreateLabeledField(state->pages[0], kSetupScl, L"Standard cell library");
    CreateLabeledField(state->pages[0], kSetupClock,
                       L"Clock ports (semicolon separated)");
    CreateLabeledField(state->pages[0], kSetupPeriod, L"Period (ns)");
    CreateLabeledField(state->pages[0], kSetupOrfsPlatform, L"ORFS platform");
    if (state->orfs_candidates != nullptr) {
      DestroyWindow(GetDlgItem(state->pages[0], kSetupOrfsPlatform));
      HWND platform = CreateSetupChild(
          state->pages[0], WC_COMBOBOXW, L"",
          CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL, kSetupOrfsPlatform);
      const std::string configured_platform =
          state->configuration.orfs.platform.empty()
              ? "sky130hd"
              : state->configuration.orfs.platform;
      for (const auto& candidate : *state->orfs_candidates) {
        if (!candidate.runnable) continue;
        const std::wstring value = Utf8ToWide(candidate.name);
        SendMessageW(platform, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(value.c_str()));
        state->platform_names.push_back(candidate.name);
      }
      // Discovery can still be running when Setup opens.  Keep the persisted
      // value selectable so switching backends never turns it into an empty
      // configuration; the backend probe will report availability later.
      if (std::find(state->platform_names.begin(), state->platform_names.end(),
                    configured_platform) == state->platform_names.end()) {
        const std::wstring value = Utf8ToWide(configured_platform);
        SendMessageW(platform, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(value.c_str()));
        state->platform_names.emplace_back(configured_platform);
      }
    }
    CreateLabeledField(state->pages[0], kSetupOrfsVariant,
                       L"ORFS flow variant");
    CreateLabeledField(state->pages[0], kSetupPnrSdc, L"PNR SDC",
                       CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL);
    DestroyWindow(GetDlgItem(state->pages[0], kSetupPnrSdc));
    CreateSetupChild(state->pages[0], WC_COMBOBOXW, L"",
                     CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL,
                     kSetupPnrSdc);
    CreateLabeledField(state->pages[0], kSetupSignoffSdc, L"Signoff SDC",
                       CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL);
    DestroyWindow(GetDlgItem(state->pages[0], kSetupSignoffSdc));
    CreateSetupChild(state->pages[0], WC_COMBOBOXW, L"",
                     CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL,
                     kSetupSignoffSdc);
    const wchar_t* floor_labels[] = {
        L"Core utilization (%)", L"Placement density (%)",
        L"Die area (x0,y0,x1,y1)", L"Core area (x0,y0,x1,y1)",
        L"Tap cell distance (um)"};
    const int floor_ids[] = {kSetupUtilization, kSetupDensity, kSetupDieArea,
                             kSetupCoreArea, kSetupTapCellDistance};
    for (std::size_t index = 0; index < std::size(floor_ids); ++index) {
      CreateLabeledField(state->pages[1], floor_ids[index],
                         floor_labels[index]);
    }
    const wchar_t* io_labels[] = {L"Algorithm",
                                  L"Unmatched policy",
                                  L"Min distance (um)",
                                  L"Vertical length (um)",
                                  L"Horizontal length (um)",
                                  L"Vertical thickness scale",
                                  L"Horizontal thickness scale",
                                  L"Vertical extension (um)",
                                  L"Horizontal extension (um)",
                                  L"Vertical routing layer",
                                  L"Horizontal routing layer"};
    const int io_ids[] = {
        kSetupIoAlgorithm,           kSetupIoUnmatched,
        kSetupIoMinimumDistance,     kSetupIoVerticalLength,
        kSetupIoHorizontalLength,    kSetupIoVerticalThickness,
        kSetupIoHorizontalThickness, kSetupIoVerticalExtension,
        kSetupIoHorizontalExtension, kSetupIoVerticalLayer,
        kSetupIoHorizontalLayer};
    for (std::size_t index = 0; index < std::size(io_ids); ++index) {
      CreateLabeledField(state->pages[2], io_ids[index], io_labels[index]);
    }
    for (int id : {kSetupIoAlgorithm, kSetupIoUnmatched}) {
      DestroyWindow(GetDlgItem(state->pages[2], id));
      CreateSetupChild(state->pages[2], WC_COMBOBOXW, L"",
                       CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL, id);
    }
    for (const wchar_t* value :
         {L"matching", L"random_equidistant", L"annealing"}) {
      SendMessageW(SetupControl(*state, kSetupIoAlgorithm), CB_ADDSTRING, 0,
                   reinterpret_cast<LPARAM>(value));
    }
    for (const wchar_t* value :
         {L"both", L"unmatched_design", L"unmatched_cfg", L"none"}) {
      SendMessageW(SetupControl(*state, kSetupIoUnmatched), CB_ADDSTRING, 0,
                   reinterpret_cast<LPARAM>(value));
    }
    state->io_side_tabs = CreateSetupChild(state->pages[2], WC_TABCONTROLW, L"",
                                           WS_TABSTOP, kSetupIoSideTabs);
    const wchar_t* side_names[] = {L"North", L"South", L"East", L"West"};
    for (int index = 0; index < 4; ++index) {
      TCITEMW item{};
      item.mask = TCIF_TEXT;
      item.pszText = const_cast<wchar_t*>(side_names[index]);
      TabCtrl_InsertItem(state->io_side_tabs, index, &item);
    }
    const int distance_ids[] = {kSetupIoNorthDistance, kSetupIoSouthDistance,
                                kSetupIoEastDistance, kSetupIoWestDistance};
    const int sort_ids[] = {kSetupIoNorthSort, kSetupIoSouthSort,
                            kSetupIoEastSort, kSetupIoWestSort};
    const int entry_ids[] = {kSetupIoNorthEntries, kSetupIoSouthEntries,
                             kSetupIoEastEntries, kSetupIoWestEntries};
    for (int index = 0; index < 4; ++index) {
      CreateSetupChild(
          state->pages[2], L"STATIC",
          L"Pin order (one pin name, regular expression, or $N per line)",
          SS_LEFT, entry_ids[index] + 1000);
      CreateSetupChild(state->pages[2], L"STATIC", L"Minimum distance (um)",
                       SS_LEFT, distance_ids[index] + 1000);
      CreateSetupChild(state->pages[2], L"STATIC", L"Bus ordering", SS_LEFT,
                       sort_ids[index] + 1000);
      CreateSetupChild(state->pages[2], L"EDIT", L"",
                       WS_BORDER | ES_AUTOHSCROLL, distance_ids[index]);
      HWND sort = CreateSetupChild(
          state->pages[2], WC_COMBOBOXW, L"",
          CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL, sort_ids[index]);
      for (const wchar_t* value : {L"bus_major", L"bit_major"}) {
        SendMessageW(sort, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(value));
      }
      CreateSetupChild(state->pages[2], L"EDIT", L"",
                       WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL,
                       entry_ids[index]);
    }
    for (int id : {kPdnMultilayer, kPdnCoreRing, kPdnRails}) {
      const wchar_t* label = id == kPdnMultilayer ? L"Multilayer grid"
                             : id == kPdnCoreRing ? L"Core ring"
                                                  : L"Standard-cell rails";
      CreateSetupChild(state->pages[3], L"BUTTON", label, BS_AUTO3STATE, id);
    }
    const wchar_t* pdn_labels[] = {
        L"Vertical width (um)",   L"Horizontal width (um)",
        L"Vertical spacing (um)", L"Horizontal spacing (um)",
        L"Vertical pitch (um)",   L"Horizontal pitch (um)",
        L"Vertical offset (um)",  L"Horizontal offset (um)"};
    const int pdn_ids[] = {kPdnVerticalWidth,   kPdnHorizontalWidth,
                           kPdnVerticalSpacing, kPdnHorizontalSpacing,
                           kPdnVerticalPitch,   kPdnHorizontalPitch,
                           kPdnVerticalOffset,  kPdnHorizontalOffset};
    for (std::size_t index = 0; index < std::size(pdn_ids); ++index) {
      CreateLabeledField(state->pages[3], pdn_ids[index], pdn_labels[index]);
    }
    for (HWND page : state->pages) {
      SetWindowLongPtrW(page, 0,
                        MulDiv(page == state->pages[2] ? 640 : 430,
                               static_cast<int>(state->dpi), 96));
    }
    for (int id : {kSetupPnrSdc, kSetupSignoffSdc}) {
      HWND combo = SetupControl(*state, id);
      SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"(None)"));
      for (const auto* source : *state->sdc_candidates) {
        const std::wstring path = Utf8ToWide(source->relative_path);
        SendMessageW(combo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(path.c_str()));
      }
    }
    const auto select_sdc = [state](int id, std::string_view path) {
      int selected = 0;
      for (std::size_t index = 0; index < state->sdc_candidates->size();
           ++index) {
        if ((*state->sdc_candidates)[index]->relative_path == path) {
          selected = static_cast<int>(index + 1);
          break;
        }
      }
      SendMessageW(SetupControl(*state, id), CB_SETCURSEL, selected, 0);
    };
    select_sdc(kSetupPnrSdc, state->configuration.pnr_sdc_path);
    select_sdc(kSetupSignoffSdc, state->configuration.signoff_sdc_path);
    state->json_button =
        CreateSetupChild(window, L"BUTTON", L"Edit backend JSON...",
                         BS_PUSHBUTTON, kSetupEditJson);
    state->save_button = CreateSetupChild(window, L"BUTTON", L"Save",
                                          BS_DEFPUSHBUTTON, kSetupOk);
    state->cancel_button = CreateSetupChild(window, L"BUTTON", L"Cancel",
                                            BS_PUSHBUTTON, kSetupCancel);
    state->status =
        CreateSetupChild(window, L"STATIC", L"", SS_LEFT, kSetupStatus);
    PopulateSetupControls(state);
    UpdateBackendFields(state);
    ShowSetupPage(state);
    UpdateDialogFont(window, state->dpi, &state->font);
    return 0;
  }
  if (message == WM_SIZE) {
    const auto px = [state](int value) {
      return MulDiv(value, static_cast<int>(state->dpi), 96);
    };
    const int width = LOWORD(lparam);
    const int height = HIWORD(lparam);
    MoveWindow(state->tabs, px(10), px(10), width - px(20),
               std::max(px(260), height - px(72)), TRUE);
    MoveWindow(state->status, px(12), height - px(52), width - px(440), px(28),
               TRUE);
    MoveWindow(state->json_button, width - px(424), height - px(58), px(190),
               px(32), TRUE);
    MoveWindow(state->save_button, width - px(226), height - px(58), px(100),
               px(32), TRUE);
    MoveWindow(state->cancel_button, width - px(118), height - px(58), px(100),
               px(32), TRUE);
    LayoutSetupPages(state);
    return 0;
  }
  if (message == WM_GETMINMAXINFO) {
    auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
    limits->ptMinTrackSize = {MulDiv(640, static_cast<int>(state->dpi), 96),
                              MulDiv(500, static_cast<int>(state->dpi), 96)};
    return 0;
  }
  if (message == WM_DPICHANGED) {
    state->dpi = HIWORD(wparam);
    for (HWND page : state->pages) {
      SetWindowLongPtrW(page, 0,
                        MulDiv(page == state->pages[2] ? 640 : 430,
                               static_cast<int>(state->dpi), 96));
    }
    const auto* suggested = reinterpret_cast<RECT*>(lparam);
    SetWindowPos(window, nullptr, suggested->left, suggested->top,
                 suggested->right - suggested->left,
                 suggested->bottom - suggested->top,
                 SWP_NOACTIVATE | SWP_NOZORDER);
    UpdateDialogFont(window, state->dpi, &state->font);
    return 0;
  }
  if (message == WM_NOTIFY &&
      reinterpret_cast<NMHDR*>(lparam)->hwndFrom == state->tabs &&
      reinterpret_cast<NMHDR*>(lparam)->code == TCN_SELCHANGE) {
    ShowSetupPage(state);
    return 0;
  }
  if (message == WM_COMMAND && LOWORD(wparam) == kSetupBackend &&
      HIWORD(wparam) == CBN_SELCHANGE) {
    UpdateBackendFields(state);
    return 0;
  }
  if (message == WM_NOTIFY &&
      reinterpret_cast<NMHDR*>(lparam)->hwndFrom == state->io_side_tabs &&
      reinterpret_cast<NMHDR*>(lparam)->code == TCN_SELCHANGE) {
    ShowIoSide(state);
    return 0;
  }
  if (message == WM_COMMAND && LOWORD(wparam) == kSetupEditJson) {
    if (state->saving || (state->json_session && state->json_session->IsOpen()))
      return 0;
    core::Status status = ReadSetupControls(state);
    if (!status.Ok()) {
      SetControlText(state->status, status.message);
      return 0;
    }
    if (!state->json_editor) return 0;
    const std::weak_ptr<const int> lifetime = state->lifetime;
    state->json_session = state->json_editor(
        window, state->configuration,
        [state, lifetime](
            const core::PhysicalImplementationConfiguration& configuration,
            bool save) {
          if (lifetime.expired() || !state->window) return;
          state->configuration = configuration;
          state->dirty = true;
          PopulateSetupControls(state);
          UpdateBackendFields(state);
          if (save) SubmitSetupSave(state);
        });
    return 0;
  }
  if (message == WM_COMMAND && LOWORD(wparam) == kSetupOk) {
    SubmitSetupSave(state);
    return 0;
  }
  if ((message == WM_COMMAND && LOWORD(wparam) == kSetupCancel) ||
      message == WM_CLOSE) {
    if (state->saving) {
      state->close_after_save = true;
      return 0;
    }
    if (state->dirty) {
      const int choice = MessageBoxW(
          window, L"Save changes before closing? No discards unsaved changes.",
          L"Layout Setup", MB_YESNOCANCEL | MB_ICONQUESTION);
      if (choice == IDCANCEL) return 0;
      if (choice == IDYES) {
        state->close_after_save = true;
        SubmitSetupSave(state);
        return 0;
      }
    }
    DestroyWindow(window);
    return 0;
  }
  if (message == WM_NCDESTROY) {
    state->window = nullptr;
    SetWindowLongPtrW(window, GWLP_USERDATA, 0);
  }
  if (message == WM_DESTROY && state->font) {
    DeleteObject(state->font);
    state->font = nullptr;
    return 0;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace

LayoutSetupController::LayoutSetupController() = default;
LayoutSetupController::~LayoutSetupController() { Close(); }

bool LayoutSetupController::Open(
    HWND owner, HINSTANCE instance,
    const core::PhysicalImplementationConfiguration& configuration,
    const std::vector<const application::ResolvedSource*>& sdc_candidates,
    const std::vector<adapters::OrfsPlatformCandidate>& orfs_candidates,
    LayoutJsonEditorCallback json_editor,
    LayoutConfigurationSaveCallback save_configuration) {
  if (state_ && IsWindow(state_->window)) {
    SetForegroundWindow(state_->window);
    return true;
  }
  constexpr wchar_t kSetupClass[] = L"DesignPlusPlus.LayoutSetupDialog";
  WNDCLASSEXW page_class{sizeof(page_class)};
  page_class.lpfnWndProc = SetupPageProcedure;
  page_class.hInstance = instance;
  page_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  page_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  page_class.cbWndExtra = 2 * sizeof(LONG_PTR);
  page_class.lpszClassName = L"DesignPlusPlus.LayoutSetupPage";
  if (!RegisterClassExW(&page_class) &&
      GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return false;
  }
  WNDCLASSEXW window_class{sizeof(window_class)};
  window_class.lpfnWndProc = LayoutSetupProcedure;
  window_class.hInstance = instance;
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  window_class.lpszClassName = kSetupClass;
  if (!RegisterClassExW(&window_class) &&
      GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return false;
  }
  state_ = std::make_unique<LayoutSetupDialogState>();
  auto& state = *state_;
  state.configuration = configuration;
  for (const auto* source : sdc_candidates)
    state.owned_sources.push_back(*source);
  for (const auto& source : state.owned_sources)
    state.source_pointers.push_back(&source);
  state.sdc_candidates = &state.source_pointers;
  state.owned_platforms = orfs_candidates;
  state.orfs_candidates = &state.owned_platforms;
  state.json_editor = std::move(json_editor);
  state.save_configuration = std::move(save_configuration);
  RECT owner_rect{};
  GetWindowRect(owner, &owner_rect);
  HMONITOR monitor = MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST);
  MONITORINFO monitor_info{sizeof(monitor_info)};
  GetMonitorInfoW(monitor, &monitor_info);
  const UINT dpi = GetDpiForWindow(owner);
  const int desired_width = MulDiv(860, dpi, 96);
  const int desired_height = MulDiv(700, dpi, 96);
  const int available_width =
      monitor_info.rcWork.right - monitor_info.rcWork.left;
  const int available_height =
      monitor_info.rcWork.bottom - monitor_info.rcWork.top;
  const int width = std::min(desired_width, available_width - 24);
  const int height = std::min(desired_height, available_height - 24);
  const int x = std::clamp(owner_rect.left + 40, monitor_info.rcWork.left,
                           monitor_info.rcWork.right - width);
  const int y = std::clamp(owner_rect.top + 40, monitor_info.rcWork.top,
                           monitor_info.rcWork.bottom - height);
  HWND dialog =
      CreateWindowExW(WS_EX_DLGMODALFRAME, kSetupClass, L"Layout Setup",
                      WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MAXIMIZEBOX,
                      x, y, width, height, owner, nullptr, instance, &state);
  if (!dialog) return false;
  ShowWindow(dialog, SW_SHOW);
  return true;
}

void LayoutSetupController::Saved(
    const core::Status& status,
    const core::PhysicalImplementationConfiguration& configuration) {
  if (!state_ || !state_->window || !state_->saving) return;
  state_->saving = false;
  if (state_->json_session) state_->json_session->Saved(status);
  for (HWND page : state_->pages) EnableWindow(page, TRUE);
  EnableWindow(state_->save_button, TRUE);
  EnableWindow(state_->json_button, TRUE);
  if (!status.Ok()) {
    state_->close_after_save = false;
    SetControlText(state_->status, status.message);
    return;
  }
  state_->configuration = configuration;
  PopulateSetupControls(state_.get());
  UpdateBackendFields(state_.get());
  state_->dirty = false;
  SetWindowTextW(state_->status, L"Saved to the selected Cell");
  if (state_->close_after_save) PostMessageW(state_->window, WM_CLOSE, 0, 0);
}

bool LayoutSetupController::RequestClose() {
  if (!state_ || !state_->window) return true;
  SendMessageW(state_->window, WM_CLOSE, 0, 0);
  return !state_->window;
}
void LayoutSetupController::Close() {
  if (state_ && state_->json_session) state_->json_session->Close();
  if (state_ && state_->window) DestroyWindow(state_->window);
}
bool LayoutSetupController::TranslateAccelerator(const MSG& message) {
  if (!state_ || !state_->window) return false;
  MSG copy = message;
  return IsDialogMessageW(state_->window, &copy) != FALSE;
}

}  // namespace designpp::gui

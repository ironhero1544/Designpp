// Copyright 2026 The Design++ Authors

#include "designpp/application/layout_setup_draft.h"

#include <algorithm>
#include <charconv>
#include <utility>

namespace designpp::application {
namespace {
std::string Trim(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}
void Automatic(core::PhysicalImplementationConfiguration* value,
               const std::string& key, bool automatic) {
  std::erase(value->automatic_fields, key);
  if (automatic) value->automatic_fields.push_back(key);
}
void Optional(std::optional<std::string>* value) {
  if (!*value) return;
  **value = Trim(**value);
  if ((*value)->empty()) value->reset();
}
}  // namespace

LayoutSetupDraft MakeLayoutSetupDraft(
    const core::PhysicalImplementationConfiguration& configuration) {
  LayoutSetupDraft draft{
      configuration,
      core::UsesAutomaticValue(configuration, "core_utilization_percent")
          ? ""
          : std::to_string(configuration.core_utilization_percent)};
  const auto clear = [&](const char* name, std::string* field) {
    if (core::UsesAutomaticValue(configuration, name)) field->clear();
  };
  clear("pdk", &draft.values.pdk);
  clear("standard_cell_library", &draft.values.standard_cell_library);
  clear("clock_period_ns", &draft.values.clock_period_ns);
  clear("orfs.flow_variant", &draft.values.orfs.flow_variant);
  return draft;
}

core::Result<core::PhysicalImplementationConfiguration>
ValidateLayoutSetupDraft(LayoutSetupDraft draft) {
  auto& value = draft.values;
  const auto scalar = [&](const char* name, std::string* text,
                          const char* fallback) {
    *text = Trim(*text);
    Automatic(&value, name, text->empty());
    if (text->empty()) *text = fallback;
  };
  scalar("pdk", &value.pdk, "sky130A");
  scalar("standard_cell_library", &value.standard_cell_library,
         "sky130_fd_sc_hd");
  scalar("clock_period_ns", &value.clock_period_ns,
         value.clock_ports.empty() ? "" : "10.0");
  scalar("orfs.flow_variant", &value.orfs.flow_variant, "base");
  value.orfs.platform = Trim(value.orfs.platform);
  Automatic(&value, "orfs.platform", value.orfs.platform.empty());
  if (value.orfs.platform.empty()) {
    value.orfs.platform = "sky130hd";
  }
  scalar("io.algorithm", &value.io_placement.algorithm, "matching");
  scalar("io.unmatched_policy", &value.io_placement.unmatched_policy, "both");
  draft.utilization = Trim(draft.utilization);
  Automatic(&value, "core_utilization_percent", draft.utilization.empty());
  if (draft.utilization.empty()) {
    value.core_utilization_percent = 40;
  } else {
    std::uint32_t parsed = 0;
    const auto result = std::from_chars(
        draft.utilization.data(),
        draft.utilization.data() + draft.utilization.size(), parsed);
    if (result.ec != std::errc{} ||
        result.ptr != draft.utilization.data() + draft.utilization.size() ||
        parsed == 0 || parsed >= 100) {
      return core::Status{core::ErrorCode::kInvalidArgument,
                          "Core utilization: enter an integer from 1 to 99, or "
                          "leave blank for Auto (40%)",
                          0};
    }
    value.core_utilization_percent = parsed;
  }
  Optional(&value.placement_density_percent);
  Optional(&value.tap_cell_distance_um);
  auto& pdn = value.power_distribution;
  for (auto* field : {&pdn.vertical_width_um, &pdn.horizontal_width_um,
                      &pdn.vertical_spacing_um, &pdn.horizontal_spacing_um,
                      &pdn.vertical_pitch_um, &pdn.horizontal_pitch_um,
                      &pdn.vertical_offset_um, &pdn.horizontal_offset_um})
    Optional(field);
  auto& io = value.io_placement;
  for (auto* field :
       {&io.minimum_distance_um, &io.vertical_length_um,
        &io.horizontal_length_um, &io.vertical_thickness_multiplier,
        &io.horizontal_thickness_multiplier, &io.vertical_extension_um,
        &io.horizontal_extension_um, &io.vertical_layer, &io.horizontal_layer,
        &io.north.minimum_distance_um, &io.south.minimum_distance_um,
        &io.east.minimum_distance_um, &io.west.minimum_distance_um})
    Optional(field);
  value.advanced_overrides_json = Trim(value.advanced_overrides_json);
  if (value.advanced_overrides_json.empty())
    value.advanced_overrides_json = "{}";
  value.orfs.advanced_variables_json = Trim(value.orfs.advanced_variables_json);
  if (value.orfs.advanced_variables_json.empty())
    value.orfs.advanced_variables_json = "{}";
  value.pnr_sdc_path = Trim(value.pnr_sdc_path);
  value.signoff_sdc_path = Trim(value.signoff_sdc_path);
  std::sort(value.automatic_fields.begin(), value.automatic_fields.end());
  auto status = core::ValidatePhysicalImplementationConfiguration(value);
  if (!status.Ok()) return status;
  return core::ResolvePhysicalDefaults(std::move(value));
}
}  // namespace designpp::application

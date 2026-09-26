// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_ADAPTERS_ORFS_FLAKE_INPUTS_H_
#define DESIGNPP_ADAPTERS_ORFS_FLAKE_INPUTS_H_

namespace designpp::adapters {

// Schema 1 and earlier environments use the original Git flake inputs. Schema
// 2 environments use content-based local paths so shallow nested submodules do
// not require Git revCount during Nix evaluation.
inline constexpr wchar_t kOrfsFlakeInputSelectionScript[] =
    L"yosys_input=\"git+file://$root/tools/yosys?submodules=1\"; "
    L"openroad_input=\"git+file://$root/tools/OpenROAD?submodules=1\"; "
    L"eqy_input=\"git+file://$root/tools/eqy\"; "
    L"marker=\"$root/.designpp-environment\"; "
    L"if [ -f \"$marker\" ] && grep -Fxq 'schema_version=2' \"$marker\" "
    L"&& grep -Fxq 'input_mode=path' \"$marker\"; then "
    L"yosys_input=\"path:$root/tools/yosys\"; "
    L"openroad_input=\"path:$root/tools/OpenROAD\"; "
    L"eqy_input=\"path:$root/tools/eqy\"; fi; ";

}  // namespace designpp::adapters

#endif  // DESIGNPP_ADAPTERS_ORFS_FLAKE_INPUTS_H_

// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_ADAPTERS_TOOLCHAIN_COMPATIBILITY_PROBE_H_
#define DESIGNPP_ADAPTERS_TOOLCHAIN_COMPATIBILITY_PROBE_H_

#include <string_view>

#include "designpp/runtime/wsl_executor.h"

namespace designpp::adapters {

// Extends an adapter-owned bash probe whose first positional argument is the
// checkout root. All generated literals come from the compiled-in catalog.
// Paths remain positional arguments and are never interpolated into source.
[[nodiscard]] runtime::WslCommand WithToolchainCompatibilityEvidence(
    runtime::WslCommand command, std::string_view provider_id);

}  // namespace designpp::adapters

#endif  // DESIGNPP_ADAPTERS_TOOLCHAIN_COMPATIBILITY_PROBE_H_

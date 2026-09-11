// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_APPLICATION_LAYOUT_SETUP_DRAFT_H_
#define DESIGNPP_APPLICATION_LAYOUT_SETUP_DRAFT_H_

#include <string>
#include <vector>

#include "designpp/core/project.h"
#include "designpp/core/status.h"

namespace designpp::application {

// Raw text is retained until validation succeeds; failed edits never replace
// the saved project snapshot.
struct LayoutSetupDraft {
  core::PhysicalImplementationConfiguration values;
  std::string utilization;
};

[[nodiscard]] LayoutSetupDraft MakeLayoutSetupDraft(
    const core::PhysicalImplementationConfiguration& configuration);
[[nodiscard]] core::Result<core::PhysicalImplementationConfiguration>
ValidateLayoutSetupDraft(LayoutSetupDraft draft);

}  // namespace designpp::application
#endif  // DESIGNPP_APPLICATION_LAYOUT_SETUP_DRAFT_H_

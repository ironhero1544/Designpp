// Copyright 2026 The Design++ Authors

#include <CppUnitTest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "designpp/adapters/verilator_adapter.h"
#include "designpp/application/project_service.h"
#include "designpp/core/library.h"
#include "designpp/core/project.h"
#include "designpp/runtime/path_mapper.h"

using Microsoft::VisualStudio::CppUnitTestFramework::Assert;

namespace designpp::tests {

TEST_CLASS(VerilatorAdapterSourceFilterTests) {
 public:
  TEST_METHOD(OnlyVerilogViewsArePassedToLint) {
    core::Project project;
    project.top_module = "vm80a";

    application::ResolvedSource rtl;
    rtl.view_kind = core::ViewKind::kVerilog;
    rtl.windows_path = L"C:\\Design\\vm80a.sv";
    rtl.library_directory = L"C:\\Design";
    rtl.relative_path = "cells/vm80/views/rtl/files/vm80a.sv";
    rtl.enabled = true;
    rtl.exists = true;

    application::ResolvedSource liberty = rtl;
    liberty.view_kind = core::ViewKind::kSynthesis;
    liberty.windows_path = L"C:\\Design\\timing.lib";
    liberty.relative_path = "cells/vm80/views/synthesis/files/timing.lib";

    application::ResolvedSource timing_liberty = liberty;
    timing_liberty.view_kind = core::ViewKind::kTiming;
    timing_liberty.relative_path = "cells/vm80/views/timing/files/timing.lib";

    application::ResolvedSource constraints_liberty = liberty;
    constraints_liberty.view_kind = core::ViewKind::kConstraints;
    constraints_liberty.relative_path =
        "cells/vm80/views/constraints/files/timing.lib";

    adapters::VerilatorAdapter adapter;
    runtime::PathMapper mapper;
    const std::vector<application::ResolvedSource> sources = {
        rtl, liberty, timing_liberty, constraints_liberty};
    auto command = adapter.BuildCommand(
        project, sources, mapper,
        adapters::ToolCapabilities{"Verilator", "5.020", true});

    Assert::IsTrue(command.Ok());
    Assert::IsTrue(std::find(command.Value().arguments.begin(),
                             command.Value().arguments.end(),
                             L"/mnt/c/Design/vm80a.sv") !=
                   command.Value().arguments.end());
    Assert::IsTrue(std::none_of(
        command.Value().arguments.begin(), command.Value().arguments.end(),
        [](const std::wstring& argument) { return argument.ends_with(L".lib"); }));
  }

  TEST_METHOD(MissingNonRtlToolInputDoesNotFailLintValidation) {
    core::Project project;
    project.top_module = "vm80a";

    application::ResolvedSource rtl;
    rtl.view_kind = core::ViewKind::kVerilog;
    rtl.relative_path = "vm80a.sv";
    rtl.enabled = true;
    rtl.exists = true;

    application::ResolvedSource missing_liberty;
    missing_liberty.view_kind = core::ViewKind::kSynthesis;
    missing_liberty.relative_path = "missing.lib";
    missing_liberty.enabled = true;
    missing_liberty.exists = false;

    adapters::VerilatorAdapter adapter;
    Assert::IsTrue(adapter.Validate(project, {rtl, missing_liberty}).Ok());
  }
};

}  // namespace designpp::tests

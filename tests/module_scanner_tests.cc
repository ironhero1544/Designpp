// Copyright 2026 The Design++ Authors

#include <CppUnitTest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "designpp/application/module_scanner.h"

using Microsoft::VisualStudio::CppUnitTestFramework::Assert;

namespace designpp::tests {
namespace {

std::filesystem::path WriteScannerFixture(std::wstring_view name,
                                          std::string_view source) {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / name;
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << source;
  return path;
}

void RemoveFixture(const std::filesystem::path& path) {
  std::error_code error;
  std::filesystem::remove(path, error);
}

}  // namespace

TEST_CLASS(ModuleScannerTests){public : TEST_METHOD(IgnoresCommentsAndStrings){
    const std::filesystem::path path =
        WriteScannerFixture(L"designpp-module-scan.sv",
                            "// module ignored;\n"
                            "string s = \"module hidden;\";\n"
                            "module real_top(input logic clk); endmodule\n");
application::SystemVerilogModuleScanner scanner;
const auto modules = scanner.FindModuleNames({path});
Assert::AreEqual(static_cast<std::size_t>(1), modules.size());
Assert::AreEqual(std::string("real_top"), modules[0]);
RemoveFixture(path);
}  // namespace designpp::tests

TEST_METHOD(SkipsModuleLifetime) {
  const std::filesystem::path path =
      WriteScannerFixture(L"designpp-module-lifetime-scan.sv",
                          "module automatic automatic_module; endmodule\n"
                          "module static static_module; endmodule\n");
  application::SystemVerilogModuleScanner scanner;
  const auto modules = scanner.FindModuleNames({path});
  Assert::AreEqual(static_cast<std::size_t>(2), modules.size());
  Assert::AreEqual(std::string("automatic_module"), modules[0]);
  Assert::AreEqual(std::string("static_module"), modules[1]);
  RemoveFixture(path);
}

TEST_METHOD(FindsModuleParameterDefaults) {
  const std::filesystem::path path =
      WriteScannerFixture(L"designpp-module-parameters-scan.sv",
                          "module nmos #(\n"
                          "  parameter int WIDTH = 8,\n"
                          "  parameter logic [3:0] RESET_VALUE = 4'h0,\n"
                          "  DEPTH = (1 << WIDTH),\n"
                          "  parameter type DATA_T = logic\n"
                          ")(input logic clk); endmodule\n");
  application::SystemVerilogModuleScanner scanner;
  const auto modules = scanner.FindModules({path});
  Assert::AreEqual(static_cast<std::size_t>(1), modules.size());
  Assert::AreEqual(std::string("nmos"), modules[0].name);
  Assert::AreEqual(static_cast<std::size_t>(3),
                   modules[0].parameter_defaults.size());
  Assert::AreEqual(std::string("WIDTH=8"), modules[0].parameter_defaults[0]);
  Assert::AreEqual(std::string("RESET_VALUE=4'h0"),
                   modules[0].parameter_defaults[1]);
  Assert::AreEqual(std::string("DEPTH=(1 << WIDTH)"),
                   modules[0].parameter_defaults[2]);
  RemoveFixture(path);
}
}
;

}  // namespace designpp::tests

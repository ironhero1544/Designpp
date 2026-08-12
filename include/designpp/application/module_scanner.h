// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_APPLICATION_MODULE_SCANNER_H_
#define DESIGNPP_APPLICATION_MODULE_SCANNER_H_

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace designpp::application {

struct ModuleDeclaration {
  std::string name;
  std::vector<std::string> parameter_defaults;
};

class SystemVerilogModuleScanner final {
 public:
  [[nodiscard]] std::vector<ModuleDeclaration> FindModulesInText(
      std::string_view source) const;
  [[nodiscard]] std::vector<ModuleDeclaration> FindModules(
      const std::vector<std::filesystem::path>& source_files) const;
  [[nodiscard]] std::vector<std::string> FindModuleNames(
      const std::vector<std::filesystem::path>& source_files) const;
};

}  // namespace designpp::application

#endif  // DESIGNPP_APPLICATION_MODULE_SCANNER_H_

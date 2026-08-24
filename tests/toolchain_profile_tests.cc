// Copyright 2026 The Design++ Authors

#include <CppUnitTest.h>
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "designpp/application/toolchain_profile_store.h"

using Microsoft::VisualStudio::CppUnitTestFramework::Assert;

namespace designpp::tests {
namespace {

class TemporaryProfileDirectory final {
 public:
  TemporaryProfileDirectory()
      : path_(std::filesystem::temp_directory_path() /
              (L"designpp-profile-tests-" +
               std::to_wstring(GetCurrentProcessId()) + L"-" +
               std::to_wstring(GetTickCount64()))) {
    std::filesystem::create_directories(path_);
  }

  TemporaryProfileDirectory(const TemporaryProfileDirectory&) = delete;
  TemporaryProfileDirectory& operator=(const TemporaryProfileDirectory&) =
      delete;

  ~TemporaryProfileDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] std::filesystem::path SettingsPath() const {
    return path_ / L"toolchain-profiles.json";
  }

 private:
  std::filesystem::path path_;
};

}  // namespace

TEST_CLASS(ToolchainProfileTests){
  public : TEST_METHOD(DefaultProfileIsValidAndReservesAProcessor){
      const core::ToolchainSettings settings =
          application::ToolchainProfileStore::CreateDefaults();
Assert::IsTrue(core::ValidateToolchainSettings(settings).Ok());
Assert::AreEqual(std::string("default"), settings.selected_profile_id);
Assert::AreEqual<std::size_t>(1U, settings.profiles.size());
Assert::IsTrue(settings.profiles.front().cpu_budget >= 1);
Assert::AreEqual(std::string("~/.designpp/toolchains/openlane2"),
                 settings.profiles.front().openlane_root);
}  // namespace designpp::tests

TEST_METHOD(RoundTripPreservesDistributionPathsAndCpuBudget) {
  TemporaryProfileDirectory directory;
  application::ToolchainProfileStore store(directory.SettingsPath());
  core::ToolchainSettings settings =
      application::ToolchainProfileStore::CreateDefaults();
  settings.profiles.front().wsl_distribution = "Ubuntu-24.04";
  settings.profiles.front().pdk_root = "/opt/pdk/sky130";
  settings.profiles.front().cpu_budget = 3;
  Assert::IsTrue(store.Save(settings).Ok());
  auto loaded = store.Load();
  Assert::IsTrue(loaded.Ok());
  Assert::AreEqual(std::string("Ubuntu-24.04"),
                   loaded.Value().profiles.front().wsl_distribution);
  Assert::AreEqual(std::string("/opt/pdk/sky130"),
                   loaded.Value().profiles.front().pdk_root);
  Assert::AreEqual<std::uint32_t>(3,
                                  loaded.Value().profiles.front().cpu_budget);
}

TEST_METHOD(UnsafePathsAndMissingSelectionAreRejected) {
  core::ToolchainSettings settings =
      application::ToolchainProfileStore::CreateDefaults();
  settings.profiles.front().pdk_root = "../../outside";
  Assert::IsFalse(core::ValidateToolchainSettings(settings).Ok());
  settings.profiles.front().pdk_root = "/opt/pdk";
  settings.selected_profile_id = "missing";
  Assert::IsFalse(core::ValidateToolchainSettings(settings).Ok());
}

TEST_METHOD(UnknownAndMalformedSchemasAreNotSilentlyReplaced) {
  TemporaryProfileDirectory directory;
  const std::filesystem::path path = directory.SettingsPath();
  {
    std::ofstream output(path, std::ios::binary);
    output << "{\"schema_version\":99,\"selected_profile_id\":"
              "\"default\",\"profiles\":[]}";
  }
  application::ToolchainProfileStore store(path);
  auto unsupported = store.Load();
  Assert::IsFalse(unsupported.Ok());
  Assert::IsTrue(unsupported.GetStatus().code ==
                 core::ErrorCode::kUnsupportedSchema);
  {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << "{broken";
  }
  auto malformed = store.Load();
  Assert::IsFalse(malformed.Ok());
  Assert::IsTrue(malformed.GetStatus().code == core::ErrorCode::kCorruptData);
}
}
;

}  // namespace designpp::tests

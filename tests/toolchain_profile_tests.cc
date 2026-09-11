// Copyright 2026 The Design++ Authors

#include <CppUnitTest.h>
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "designpp/application/toolchain_environment_service.h"
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

TEST_METHOD(SchemaOneMigratesWithoutRewritingDisk) {
  TemporaryProfileDirectory directory;
  const auto path = directory.SettingsPath();
  const std::string original =
      R"({"schema_version":1,"selected_profile_id":"p","profiles":[{"id":"p","name":"Legacy","wsl_distribution":"Ubuntu","openlane_root":"~/ol","orfs_root":"~/orfs","pdk_root":"~/pdk","cpu_budget":2}]})";
  {
    std::ofstream output(path);
    output << original;
  }
  application::ToolchainProfileStore store(path);
  const auto loaded = store.Load();
  Assert::IsTrue(loaded.Ok());
  Assert::AreEqual(std::uint32_t(3), loaded.Value().schema_version);
  Assert::AreEqual(std::string("custom"),
                   loaded.Value().profiles.front().orfs_mode);
  std::ifstream input(path);
  const std::string actual((std::istreambuf_iterator<char>(input)), {});
  Assert::AreEqual(original, actual);
}

TEST_METHOD(BundleSelectionRoundTripsAndRejectsInvalidModes) {
  TemporaryProfileDirectory directory;
  application::ToolchainProfileStore store(directory.SettingsPath());
  auto settings = application::ToolchainProfileStore::CreateDefaults();
  auto& profile = settings.profiles.front();
  profile.orfs_mode = "managed";
  profile.orfs_bundle_id = "orfs-26Q2-candidate";
  Assert::IsTrue(store.Save(settings).Ok());
  const auto loaded = store.Load();
  Assert::IsTrue(loaded.Ok());
  Assert::AreEqual(profile.orfs_bundle_id,
                   loaded.Value().profiles.front().orfs_bundle_id);
  profile.orfs_mode = "latest";
  Assert::IsFalse(core::ValidateToolchainSettings(settings).Ok());
}

TEST_METHOD(EnvironmentAndVerificationRecipeRoundTrip) {
  TemporaryProfileDirectory directory;
  application::ToolchainProfileStore store(directory.SettingsPath());
  auto settings = application::ToolchainProfileStore::CreateDefaults();
  settings.revision = 1;
  core::InstalledToolchainEnvironment environment;
  environment.id = "orfs-26q2-local";
  environment.provider_id = "orfs";
  environment.bundle_id = "orfs-26Q2";
  environment.root = "/home/test/.designpp/toolchains/orfs";
  environment.executable = "/nix/store/test/bin/openroad";
  environment.version = "036d106273e66855cd5214d49518fd0f0df7de61";
  environment.fingerprint = "sha256:environment";
  settings.environments.push_back(environment);
  settings.profiles.front().active_orfs_environment_id = environment.id;

  core::VerificationRecipe recipe;
  recipe.id = "sky130hd-klayout-drc";
  recipe.name = "Sky130HD KLayout DRC";
  recipe.engine = "klayout_drc";
  recipe.provider_id = "orfs";
  recipe.platform = "sky130hd";
  recipe.root = "/home/test/.designpp/toolchains/orfs/flow/platforms/sky130hd";
  recipe.entrypoint = "drc/sky130hd.lydrc";
  recipe.output_format = "lyrdb";
  recipe.content_hash = "sha256:recipe";
  recipe.trusted = true;
  settings.verification_recipes.push_back(recipe);

  Assert::IsTrue(store.Save(settings).Ok());
  auto loaded = store.Load();
  Assert::IsTrue(loaded.Ok());
  Assert::AreEqual<std::size_t>(1, loaded.Value().environments.size());
  Assert::AreEqual(environment.id,
                   loaded.Value().profiles.front().active_orfs_environment_id);
  Assert::AreEqual<std::size_t>(1, loaded.Value().verification_recipes.size());
  Assert::AreEqual(recipe.content_hash,
                   loaded.Value().verification_recipes.front().content_hash);
}

TEST_METHOD(InventoryResolvesOnlyTheProfileActiveEnvironment) {
  auto settings = application::ToolchainProfileStore::CreateDefaults();
  core::InstalledToolchainEnvironment environment;
  environment.id = "orfs-26q2-local";
  environment.provider_id = "orfs";
  environment.bundle_id = "orfs-26Q2";
  environment.root = "/home/test/orfs";
  environment.executable = "/nix/store/test/bin/openroad";
  environment.version = "26Q2";
  environment.fingerprint = "sha256:environment";
  settings.environments.push_back(environment);
  settings.profiles.front().active_orfs_environment_id = environment.id;

  application::ToolchainInventoryService inventory;
  auto resolved =
      inventory.Resolve(settings, settings.profiles.front(), "orfs");
  Assert::IsTrue(resolved.Ok());
  Assert::AreEqual(environment.id, resolved.Value().installation_id);
  Assert::AreEqual(environment.fingerprint, resolved.Value().fingerprint);
  Assert::IsFalse(
      inventory.Resolve(settings, settings.profiles.front(), "openlane2").Ok());
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

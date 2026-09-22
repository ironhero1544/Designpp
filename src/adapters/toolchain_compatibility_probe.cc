// Copyright 2026 The Design++ Authors

#include "designpp/adapters/toolchain_compatibility_probe.h"

#include <string>
#include <utility>

#include "designpp/core/toolchain_compatibility.h"

namespace designpp::adapters {
namespace {

std::wstring Literal(std::string_view value) {
  return std::wstring(value.begin(), value.end());
}

}  // namespace

runtime::WslCommand WithToolchainCompatibilityEvidence(
    runtime::WslCommand command, std::string_view provider_id) {
  const core::ToolchainCompatibilityEntry* selected = nullptr;
  for (const auto& entry : core::ToolchainCompatibilityCatalog::Entries()) {
    if (entry.provider_id == provider_id) selected = &entry;
  }
  if (selected == nullptr || command.arguments.size() < 4 ||
      command.program != L"/bin/bash" || command.arguments.front() != L"-lc") {
    command.program = L"/bin/false";
    command.arguments.clear();
    return command;
  }
  const auto& entry = *selected;
  std::wstring guard =
      L"root=\"$1\"; case \"$root\" in '~/'*) root=\"$HOME/${root#\\~/}\";; "
      L"esac; "
      L"test -d \"$root\" || { echo 'Configured toolchain directory does not "
      L"exist; select the installed environment in Tool Check or update the "
      L"framework path in Toolchain Doctor.' >&2; exit 44; }; "
      L"revision=$(git -C \"$root\" rev-parse HEAD) || exit 78; "
      L"test \"$revision\" = '" +
      Literal(entry.revision) +
      L"' || { echo 'Unsupported framework revision' >&2; exit 78; }; "
      L"test -s \"$root/flake.lock\" || exit 78; "
      L"lock_hash=$(sha256sum -- \"$root/flake.lock\") || exit 78; "
      L"lock_hash=${lock_hash%% *}; "
      L"expected_lock=$(git -C \"$root\" show HEAD:flake.lock | sha256sum); "
      L"test \"$lock_hash\" = \"${expected_lock%% *}\" || "
      L"{ echo 'Framework lock was modified' >&2; exit 78; }; ";
  std::wstring evidence =
      L"printf '%s\\n' 'DESIGNPP_COMPAT_SCHEMA=1' 'DESIGNPP_COMPAT_PROVIDER=" +
      Literal(entry.provider_id) + L"' 'DESIGNPP_COMPAT_BUNDLE=" +
      Literal(entry.bundle_id) + L"' 'DESIGNPP_COMPAT_CONTRACT=" +
      Literal(entry.command_contract_id) + L"' 'DESIGNPP_COMPAT_REVISION=" +
      Literal(entry.revision) +
      L"'; "
      L"printf 'DESIGNPP_COMPAT_LOCK=%s\\n' \"$lock_hash\"; ";
  std::wstring fingerprint = L"printf '%s\\n' '" +
                             Literal(entry.command_contract_id) +
                             L"' \"$revision\" \"$lock_hash\"; "
                             L"git -C \"$root\" diff HEAD --binary; ";
  for (const auto& dependency : entry.dependencies) {
    std::wstring path;
    if (dependency.name == "openroad") path = L"tools/OpenROAD";
    if (dependency.name == "yosys") path = L"tools/yosys";
    if (dependency.name == "abc") path = L"tools/yosys/abc";
    if (dependency.name == "eqy") path = L"tools/eqy";
    guard += L"test \"$(git -C \"$root/" + path + L"\" rev-parse HEAD)\" = '" +
             Literal(dependency.revision) +
             L"' || { echo 'Toolchain dependency mismatch: " +
             Literal(dependency.name) + L"' >&2; exit 78; }; ";
    fingerprint += L"printf '%s\\n' '" + Literal(dependency.revision) +
                   L"'; git -C \"$root/" + path + L"\" diff HEAD --binary; ";
    evidence += L"printf '%s\\n' 'DESIGNPP_COMPAT_DEPENDENCY_" +
                Literal(dependency.name) + L"=" + Literal(dependency.revision) +
                L"'; ";
  }
  guard += L"compat_fingerprint() { { " + fingerprint +
           L"} | sha256sum; }; before=$(compat_fingerprint) || exit 78; ";
  // Existing adapter feature checks must succeed before the protocol is
  // emitted.
  std::wstring script =
      guard + L"( " + command.arguments[1] + L" ) || exit $?; ";
  if (provider_id == "openlane2") {
    script +=
        L"cd \"$root\" || exit 44; "
        L"nix-shell shell.nix --run "
        L"'python3 -c \"import openlane; assert openlane.__version__ == "
        L"\\\"2.3.10\\\"\"' || exit 78; "
        L"nix-shell shell.nix --run 'openlane --help' | "
        L"grep -Fq -- --run-tag || exit 78; "
        L"nix-shell shell.nix --run "
        L"'python3 -c \"from openlane.flows import Flow; "
        L"assert Flow.factory.get(\\\"Classic\\\") is not None\"' || exit 78; ";
  } else {
    script +=
        L"for target in all synth floorplan place cts route finish; do "
        L"grep -Eq \"^$target:\" \"$root/flow/Makefile\" || exit 78; done; ";
  }
  script +=
      L"after=$(compat_fingerprint) || exit 78; "
      L"test \"$before\" = \"$after\" || "
      L"{ echo 'Toolchain changed during probe' >&2; exit 78; }; " +
      evidence;
  for (const auto feature : entry.required_features) {
    script += L"printf '%s\\n' 'DESIGNPP_COMPAT_FEATURE_" + Literal(feature) +
              L"=1'; ";
  }
  script += L"printf 'DESIGNPP_COMPAT_FINGERPRINT=%s\\n' \"${after%% *}\"";
  command.arguments[1] = std::move(script);
  return command;
}

}  // namespace designpp::adapters

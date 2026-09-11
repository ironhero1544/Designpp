// Copyright 2026 The Design++ Authors

#include "designpp/adapters/physical_verification_adapter.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <memory>
#include <string>

namespace designpp::adapters {
namespace {

std::wstring Utf8ToWide(std::string_view text) {
  if (text.empty()) return {};
  const int size =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), nullptr, 0);
  if (size <= 0) return {};
  std::wstring result(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                      static_cast<int>(text.size()), result.data(), size);
  return result;
}

std::wstring JoinLinuxPath(std::string_view root, std::string_view relative) {
  std::string result(root);
  if (!result.empty() && result.back() != '/') result.push_back('/');
  result.append(relative);
  return Utf8ToWide(result);
}

runtime::WslCommand Command(std::wstring program,
                            const VerificationCommandInput& input) {
  runtime::WslCommand command;
  command.program = std::move(program);
  command.distribution = input.distribution;
  return command;
}

bool HasTclDelimiter(std::wstring_view path) {
  return path.find(L'{') != std::wstring_view::npos ||
         path.find(L'}') != std::wstring_view::npos;
}

std::string NarrowAscii(std::wstring_view text) {
  std::string result;
  result.reserve(text.size());
  for (wchar_t character : text) {
    if (character > 0x7f) return {};
    result.push_back(static_cast<char>(character));
  }
  return result;
}

std::size_t Count(std::string_view text, std::string_view needle) {
  std::size_t count = 0;
  std::size_t position = 0;
  while ((position = text.find(needle, position)) != std::string_view::npos) {
    ++count;
    position += needle.size();
  }
  return count;
}

std::string XmlValue(std::string_view text, std::string_view name,
                     std::size_t start) {
  const std::string open = "<" + std::string(name) + ">";
  const std::string close = "</" + std::string(name) + ">";
  const std::size_t begin = text.find(open, start);
  if (begin == std::string_view::npos) return {};
  const std::size_t end = text.find(close, begin + open.size());
  return end == std::string_view::npos
             ? std::string{}
             : std::string(
                   text.substr(begin + open.size(), end - begin - open.size()));
}

core::Result<VerificationResult> ParseMarkerDatabase(std::string_view report) {
  VerificationResult result;
  if (report.find("<report-database") == std::string_view::npos &&
      report.find("<layout-to-netlist") == std::string_view::npos) {
    return core::Status{core::ErrorCode::kCorruptData,
                        "KLayout report database is malformed", 0};
  }
  result.violation_count = Count(report, "<item>");
  std::size_t position = 0;
  while ((position = report.find("<item>", position)) !=
         std::string_view::npos) {
    const std::size_t end = report.find("</item>", position);
    if (end == std::string_view::npos) break;
    const std::string_view item = report.substr(position, end - position);
    VerificationMarker marker;
    marker.category = XmlValue(item, "category", 0);
    marker.cell = XmlValue(item, "cell", 0);
    marker.description = XmlValue(item, "description", 0);
    marker.geometry = XmlValue(item, "polygon", 0);
    if (marker.geometry.empty()) marker.geometry = XmlValue(item, "box", 0);
    result.markers.push_back(std::move(marker));
    position = end + 7;
  }
  result.parsed = true;
  result.passed = result.violation_count == 0;
  result.detail = result.passed ? "No violations" : "Violations found";
  return result;
}

class KLayoutAdapter final : public PhysicalVerificationAdapter {
 public:
  explicit KLayoutAdapter(VerificationCheck check) : check_(check) {}

  std::string_view Engine() const noexcept override {
    return check_ == VerificationCheck::kDrc ? "klayout_drc" : "klayout_lvs";
  }
  VerificationCheck Check() const noexcept override { return check_; }

  runtime::WslCommand BuildProbeCommand(
      const VerificationCommandInput& input) const override {
    if (check_ == VerificationCheck::kLvs) {
      runtime::WslCommand command = Command(L"/bin/bash", input);
      command.arguments = {
          L"-lc",
          L"command -v klayout >/dev/null 2>&1 && "
          L"command -v openroad >/dev/null 2>&1",
          L"designpp-lvs-probe"};
      return command;
    }
    runtime::WslCommand command = Command(L"klayout", input);
    command.arguments = {L"-v"};
    return command;
  }

  core::Result<std::string> BuildPreparationScript(
      const VerificationCommandInput& input) const override {
    if (check_ != VerificationCheck::kLvs || input.odb_path.empty() ||
        input.staged_model_path.empty() || input.design_cdl_path.empty() ||
        HasTclDelimiter(input.odb_path) ||
        HasTclDelimiter(input.staged_model_path) ||
        HasTclDelimiter(input.design_cdl_path)) {
      return core::Status{core::ErrorCode::kInvalidArgument,
                          "LVS CDL export inputs are incomplete", 0};
    }
    const std::string odb = NarrowAscii(input.odb_path);
    const std::string model = NarrowAscii(input.staged_model_path);
    const std::string design = NarrowAscii(input.design_cdl_path);
    if (odb.empty() || model.empty() || design.empty()) {
      return core::Status{core::ErrorCode::kInvalidArgument,
                          "LVS CDL export path is not ASCII-safe", 0};
    }
    return "read_db {" + odb +
           "}\nwrite_cdl -masters {" +
           model + "} {" + design +
           "}\nexit\n";
  }

  core::Result<std::vector<runtime::WslCommand>> BuildPreparationCommands(
      const VerificationCommandInput& input) const override {
    if (check_ != VerificationCheck::kLvs || input.model_path.empty() ||
        input.staged_model_path.empty() || input.preparation_script_path.empty() ||
        input.design_cdl_path.empty() || input.combined_cdl_path.empty()) {
      return core::Status{core::ErrorCode::kInvalidArgument,
                          "LVS CDL preparation paths are incomplete", 0};
    }
    runtime::WslCommand copy = Command(L"/bin/cp", input);
    copy.arguments = {input.model_path, input.staged_model_path};

    runtime::WslCommand export_cdl = Command(L"openroad", input);
    export_cdl.arguments = {L"-exit", L"-no_splash", L"-db", input.odb_path,
                            input.preparation_script_path};

    runtime::WslCommand combine = Command(L"/bin/bash", input);
    combine.arguments = {
        L"-lc", L"cat \"$1\" \"$2\" > \"$3\"", L"designpp-cdl-merge",
        input.design_cdl_path, input.staged_model_path, input.combined_cdl_path};
    return std::vector<runtime::WslCommand>{std::move(copy),
                                           std::move(export_cdl),
                                           std::move(combine)};
  }

  core::Result<runtime::WslCommand> BuildCommand(
      const VerificationCommandInput& input) const override {
    if (input.recipe.entrypoint.empty() || input.gds_path.empty() ||
        input.report_path.empty()) {
      return core::Status{core::ErrorCode::kInvalidArgument,
                          "KLayout verification input is incomplete", 0};
    }
    runtime::WslCommand command = Command(L"klayout", input);
    command.arguments = {L"-b", L"-rd", L"in_gds=" + input.gds_path, L"-rd",
                         L"report_file=" + input.report_path};
    if (check_ == VerificationCheck::kLvs) {
      if (input.schematic_path.empty()) {
        return core::Status{core::ErrorCode::kInvalidArgument,
                            "KLayout LVS requires a schematic netlist", 0};
      }
      command.arguments.insert(
          command.arguments.end(),
          {L"-rd", L"cdl_file=" + input.schematic_path, L"-rd",
           L"target_netlist=" + input.extracted_path});
    }
    if (input.recipe.root.starts_with("~/")) {
      std::vector<std::wstring> arguments = {
          L"-lc",
          L"root=$1; shift; entry=$1; shift; case \"$root\" in \"~/\"*) "
          L"root=\"$HOME/${root#\\~/}\";; esac; exec klayout \"$@\" "
          L"-r \"$root/$entry\"",
          L"designpp-klayout-verification", Utf8ToWide(input.recipe.root),
          Utf8ToWide(input.recipe.entrypoint)};
      arguments.insert(arguments.end(), command.arguments.begin(),
                       command.arguments.end());
      command.program = L"/bin/bash";
      command.arguments = std::move(arguments);
    } else {
      command.arguments.insert(
          command.arguments.end(),
          {L"-r", JoinLinuxPath(input.recipe.root, input.recipe.entrypoint)});
    }
    return command;
  }

  core::Result<VerificationResult> ParseReport(
      std::string_view report) const override {
    return ParseMarkerDatabase(report);
  }

 private:
  VerificationCheck check_;
};

class MagicDrcAdapter final : public PhysicalVerificationAdapter {
 public:
  std::string_view Engine() const noexcept override { return "magic_drc"; }
  VerificationCheck Check() const noexcept override {
    return VerificationCheck::kDrc;
  }
  runtime::WslCommand BuildProbeCommand(
      const VerificationCommandInput& input) const override {
    runtime::WslCommand command = Command(L"magic", input);
    command.arguments = {L"--version"};
    return command;
  }
  core::Result<runtime::WslCommand> BuildCommand(
      const VerificationCommandInput& input) const override {
    if (input.recipe.entrypoint.empty()) {
      return core::Status{core::ErrorCode::kInvalidArgument,
                          "Magic DRC recipe has no entrypoint", 0};
    }
    runtime::WslCommand command = Command(L"magic", input);
    command.arguments = {L"-dnull", L"-noconsole", L"-batch"};
    if (!input.recipe.technology_file.empty()) {
      command.arguments.insert(
          command.arguments.end(),
          {L"-rcfile",
           JoinLinuxPath(input.recipe.root, input.recipe.technology_file)});
    }
    command.arguments.push_back(
        JoinLinuxPath(input.recipe.root, input.recipe.entrypoint));
    return command;
  }
  core::Result<VerificationResult> ParseReport(
      std::string_view report) const override {
    if (report.empty()) {
      return core::Status{core::ErrorCode::kCorruptData,
                          "Magic DRC report is empty", 0};
    }
    VerificationResult result;
    result.parsed = true;
    result.violation_count =
        Count(report, "----------------------------------------");
    const std::size_t explicit_total = report.find("Total DRC errors found:");
    if (explicit_total != std::string_view::npos) {
      const char* begin = report.data() + explicit_total + 23;
      char* end = nullptr;
      result.violation_count = std::strtoull(begin, &end, 10);
    }
    result.passed = result.violation_count == 0;
    result.detail = result.passed ? "No violations" : "Violations found";
    return result;
  }
};

class NetgenLvsAdapter final : public PhysicalVerificationAdapter {
 public:
  std::string_view Engine() const noexcept override { return "netgen_lvs"; }
  VerificationCheck Check() const noexcept override {
    return VerificationCheck::kLvs;
  }
  runtime::WslCommand BuildProbeCommand(
      const VerificationCommandInput& input) const override {
    runtime::WslCommand command = Command(L"netgen", input);
    command.arguments = {L"-batch"};
    return command;
  }
  core::Result<runtime::WslCommand> BuildCommand(
      const VerificationCommandInput& input) const override {
    if (input.extracted_path.empty() || input.schematic_path.empty() ||
        input.top_cell.empty()) {
      return core::Status{core::ErrorCode::kInvalidArgument,
                          "Netgen LVS input is incomplete", 0};
    }
    runtime::WslCommand command = Command(L"netgen", input);
    const std::wstring setup =
        input.recipe.setup_file.empty()
            ? L"nosetup"
            : JoinLinuxPath(input.recipe.root, input.recipe.setup_file);
    command.arguments = {L"-batch",
                         L"lvs",
                         input.extracted_path + L" " + input.top_cell,
                         input.schematic_path + L" " + input.top_cell,
                         setup,
                         input.report_path,
                         L"-json"};
    return command;
  }
  core::Result<VerificationResult> ParseReport(
      std::string_view report) const override {
    if (report.empty()) {
      return core::Status{core::ErrorCode::kCorruptData,
                          "Netgen LVS report is empty", 0};
    }
    std::string lower(report);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char character) {
                     return static_cast<char>(std::tolower(character));
                   });
    VerificationResult result;
    result.parsed = lower.find("circuits match") != std::string::npos ||
                    lower.find("do not match") != std::string::npos ||
                    lower.find("mismatch") != std::string::npos;
    if (!result.parsed) {
      return core::Status{core::ErrorCode::kCorruptData,
                          "Netgen LVS result is not identifiable", 0};
    }
    result.passed =
        lower.find("circuits match uniquely") != std::string::npos ||
        (lower.find("circuits match") != std::string::npos &&
         lower.find("do not match") == std::string::npos);
    result.violation_count = result.passed ? 0 : 1;
    result.detail = result.passed ? "Circuits match" : "Circuits do not match";
    return result;
  }
};

}  // namespace

core::Result<std::string> PhysicalVerificationAdapter::BuildPreparationScript(
    const VerificationCommandInput&) const {
  return core::Status{core::ErrorCode::kInvalidArgument,
                      "This verification recipe has no CDL preparation", 0};
}

core::Result<std::vector<runtime::WslCommand>>
PhysicalVerificationAdapter::BuildPreparationCommands(
    const VerificationCommandInput&) const {
  return core::Status{core::ErrorCode::kInvalidArgument,
                      "This verification recipe has no CDL preparation", 0};
}

std::unique_ptr<PhysicalVerificationAdapter> CreatePhysicalVerificationAdapter(
    std::string_view engine) {
  if (engine == "klayout_drc") {
    return std::make_unique<KLayoutAdapter>(VerificationCheck::kDrc);
  }
  if (engine == "klayout_lvs") {
    return std::make_unique<KLayoutAdapter>(VerificationCheck::kLvs);
  }
  if (engine == "magic_drc") return std::make_unique<MagicDrcAdapter>();
  if (engine == "netgen_lvs") return std::make_unique<NetgenLvsAdapter>();
  return nullptr;
}

}  // namespace designpp::adapters

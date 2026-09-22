// Copyright 2026 The Design++ Authors

#include "designpp/adapters/physical_verification_adapter.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

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
          L"set -eu; root=\"$1\"; "
          L"case \"$root\" in '~/'*) root=\"$HOME/${root#\\~/}\";; esac; "
          L"command -v klayout >/dev/null 2>&1; "
          L"if [ -x \"$root/tools/install/OpenROAD/bin/openroad\" ]; then "
          L"exit 0; fi; "
          L"if command -v openroad >/dev/null 2>&1; then exit 0; fi; "
          L"if [ -f \"$root/flake.nix\" ] && "
          L"command -v nix >/dev/null 2>&1; then "
          L"exec nix --extra-experimental-features 'nix-command flakes' "
          L"develop \"$root\" --no-write-lock-file --offline "
          L"--max-jobs 0 --builders '' --option fallback false "
          L"--override-input yosys "
          L"\"git+file://$root/tools/yosys?submodules=1\" "
          L"--override-input openroad "
          L"\"git+file://$root/tools/OpenROAD?submodules=1\" "
          L"--override-input eqy-src \"git+file://$root/tools/eqy\" "
          L"--command /bin/bash -lc 'command -v openroad >/dev/null 2>&1'; "
          L"fi; exit 45",
          L"designpp-lvs-probe", input.toolchain_root};
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
    return "read_db {" + odb + "}\nwrite_cdl -masters {" + model + "} {" +
           design + "}\nexit\n";
  }

  core::Result<std::vector<runtime::WslCommand>> BuildPreparationCommands(
      const VerificationCommandInput& input) const override {
    if (check_ != VerificationCheck::kLvs || input.model_path.empty() ||
        input.staged_model_path.empty() ||
        input.preparation_script_path.empty() ||
        input.design_cdl_path.empty() || input.combined_cdl_path.empty()) {
      return core::Status{core::ErrorCode::kInvalidArgument,
                          "LVS CDL preparation paths are incomplete", 0};
    }
    runtime::WslCommand copy = Command(L"/bin/bash", input);
    copy.arguments = {
        L"-lc",
        L"set -eu; root=\"$1\"; destination=\"$2\"; "
        L"case \"$root\" in '~/'*) root=\"$HOME/${root#\\~/}\";; esac; "
        L"exec /bin/cp \"$root\" \"$destination\"",
        L"designpp-cdl-model-copy", input.model_path, input.staged_model_path};

    runtime::WslCommand export_cdl = Command(L"/bin/bash", input);
    export_cdl.arguments = {
        L"-lc",
        L"set -eu; root=\"$1\"; script=\"$2\"; "
        L"case \"$root\" in '~/'*) root=\"$HOME/${root#\\~/}\";; esac; "
        L"if [ -x \"$root/tools/install/OpenROAD/bin/openroad\" ]; then "
        L"exec \"$root/tools/install/OpenROAD/bin/openroad\" -exit "
        L"-no_splash \"$script\"; fi; "
        L"if command -v openroad >/dev/null 2>&1; then "
        L"exec openroad -exit -no_splash \"$script\"; fi; "
        L"if [ -f \"$root/flake.nix\" ] && command -v nix >/dev/null 2>&1; "
        L"then "
        L"exec nix --extra-experimental-features 'nix-command flakes' "
        L"develop \"$root\" --no-write-lock-file --offline --max-jobs 0 "
        L"--builders '' --option fallback false "
        L"--override-input yosys \"git+file://$root/tools/yosys?submodules=1\" "
        L"--override-input openroad "
        L"\"git+file://$root/tools/OpenROAD?submodules=1\" "
        L"--override-input eqy-src \"git+file://$root/tools/eqy\" "
        L"--command openroad -exit "
        L"-no_splash \"$script\"; fi; exit 45",
        L"designpp-cdl-export", input.toolchain_root,
        input.preparation_script_path};

    runtime::WslCommand combine = Command(L"/bin/bash", input);
    combine.arguments = {L"-lc",
                         L"cat \"$1\" \"$2\" > \"$3\"",
                         L"designpp-cdl-merge",
                         input.design_cdl_path,
                         input.staged_model_path,
                         input.combined_cdl_path};
    return std::vector<runtime::WslCommand>{
        std::move(copy), std::move(export_cdl), std::move(combine)};
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
      if (input.schematic_path.empty() ||
          input.verification_script_path.empty()) {
        return core::Status{core::ErrorCode::kInvalidArgument,
                            "KLayout LVS requires a schematic netlist", 0};
      }
      command.arguments.insert(
          command.arguments.end(),
          {L"-rd", L"cdl_file=" + input.schematic_path, L"-rd",
           L"target_netlist=" + input.extracted_path, L"-rd",
           L"top_cell=" + input.top_cell, L"-rd",
           input.recipe.managed && input.recipe.platform == "sky130hd" &&
                   input.recipe.id == "builtin.orfs.sky130hd.klayout-lvs"
               ? L"recipe_contract=sky130hd-bulk-v1"
               : L"recipe_contract=custom"});
      command.program = L"/bin/bash";
      std::vector<std::wstring> arguments = {
          L"-lc",
          L"set -eu; root=$1; entry=$2; driver=$3; shift 3; "
          L"case \"$root\" in '~/'*) root=\"$HOME/${root#\\~/}\";; esac; "
          L"exec klayout \"$@\" -rd \"recipe_path=$root/$entry\" -r "
          L"\"$driver\"",
          L"designpp-lvs-driver",
          Utf8ToWide(input.recipe.root),
          Utf8ToWide(input.recipe.entrypoint),
          input.verification_script_path};
      arguments.insert(arguments.end(), command.arguments.begin(),
                       command.arguments.end());
      command.arguments = std::move(arguments);
      return command;
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

  core::Result<VerificationResult> ParseResult(
      const VerificationResultArtifacts& artifacts) const override {
    if (check_ != VerificationCheck::kLvs) return ParseReport(artifacts.report);
    if (!artifacts.report_exists ||
        !artifacts.report.starts_with("#%lvsdb-klayout")) {
      return core::Status{core::ErrorCode::kCorruptData,
                          "LVS-COMPARISON-MISSING: native LVS database is "
                          "missing or unsupported",
                          0};
    }
    std::istringstream summary(artifacts.comparison_summary);
    std::string magic;
    std::size_t circuits = 0, mismatches = 0, skipped = 0;
    if (!(summary >> magic >> circuits >> mismatches >> skipped) ||
        magic != "DESIGNPP_LVS_XREF_V1" || circuits == 0 ||
        mismatches > circuits || skipped > circuits - mismatches) {
      return core::Status{
          core::ErrorCode::kCorruptData,
          "LVS-COMPARISON-MISSING: invalid native comparison summary", 0};
    }
    VerificationResult result;
    result.parsed = true;
    result.comparison_completed = skipped == 0;
    result.passed = mismatches == 0 && skipped == 0;
    result.violation_count = mismatches;
    result.detail = "KLayout LVS database: " + std::to_string(circuits) +
                    " circuit pairs, " + std::to_string(mismatches) +
                    " mismatched, " + std::to_string(skipped) + " not compared";
    return result;
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

std::string BuildKLayoutLvsDriver() {
  return R"ruby(require 'digest'
require 'json'
directory = File.dirname($report_file)
original = File.binread($recipe_path)
File.binwrite(File.join(directory, 'rule-original.lylvs'), original)
macro = RBA::Macro.new($recipe_path)
text = macro.text
changes = []
if $recipe_contract == 'sky130hd-bulk-v1'
  expected = '1afade11dd24ea4e64d1ffba54fc17a8f110286712ab942928d2f5e021bb3caf'
  raise 'LVS-RECIPE-INCOMPATIBLE: unsupported source rule hash' unless Digest::SHA256.hexdigest(original) == expected
  layout = RBA::Layout.new
  layout.read($in_gds)
  raise 'LVS-SUBSTRATE-UNSUPPORTED: ambiguous top cell' unless layout.top_cells.size == 1
  top = layout.top_cell
  present = lambda do |layer, datatype|
    index = layout.find_layer(layer, datatype)
    index && !RBA::Region.new(top.begin_shapes_rec(index)).is_empty?
  end
  raise 'LVS-SUBSTRATE-UNSUPPORTED: missing boundary or well' unless present.call(235, 4) && present.call(64, 20)
  raise 'LVS-SUBSTRATE-UNSUPPORTED: deep-well or isolation geometry' if present.call(64, 18) || present.call(75, 20) || present.call(64, 13)
  replace = lambda do |before, after|
    raise 'LVS-RECIPE-INCOMPATIBLE: unexpected rule structure' unless text.scan(before).size == 1
    text = text.sub(before, after)
    changes << before
  end
  # Layer symbols belong to this hash-verified process recipe, not the design.
  ['NWELL', 'POLY', 'LI', 'MET1', 'MET2', 'MET3', 'MET4', 'MET5'].each do |layer|
    replace.call("connect(#{layer}, #{layer}TXT)",
      "connect(#{layer}, #{layer}PIN)\nconnect(#{layer}PIN, #{layer}TXT)\nconnect(#{layer}, #{layer}TXT)")
  end
  replace.call('SUB = polygons(236, 0)', <<~DSL.chomp)
    SUB = BOUND - NWELL
  DSL
  replace.call('netlist.simplify\n#schematic.simplify'.gsub('\\n', "\n"), <<~DSL.chomp)
    netlist.write($report_file + '.layout-before.spice', RBA::NetlistSpiceWriter.new)
    schematic.write($report_file + '.schematic-before.spice', RBA::NetlistSpiceWriter.new)
    netlist.simplify
    schematic.simplify
    netlist.write($report_file + '.layout-after.spice', RBA::NetlistSpiceWriter.new)
    schematic.write($report_file + '.schematic-after.spice', RBA::NetlistSpiceWriter.new)
  DSL
  replace.call('report($report_file)', 'report_lvs($report_file)')
elsif $recipe_contract != 'custom'
  raise 'LVS-RECIPE-INCOMPATIBLE: unknown extraction contract'
end
raise 'LVS-RECIPE-CONTRACT: missing report_lvs output' unless text.include?('report_lvs($report_file)')
macro.text = text
macro.save_to(File.join(directory, 'rule-effective.lylvs'))
File.write(File.join(directory, 'rule-contract.json'), JSON.pretty_generate({
  'schema_version' => 1, 'contract' => $recipe_contract,
  'original_sha256' => Digest::SHA256.hexdigest(original),
  'effective_sha256' => Digest::SHA256.file(File.join(directory, 'rule-effective.lylvs')).hexdigest,
  'changes' => changes
}))
macro.run
if $recipe_contract == 'sky130hd-bulk-v1'
  counts = {}
  ['layout-before', 'schematic-before', 'layout-after', 'schematic-after'].each do |phase|
    n = RBA::Netlist.new
    n.read($report_file + '.' + phase + '.spice', RBA::NetlistSpiceReader.new)
    circuits_count = 0
    devices_count = 0
    n.each_circuit do |circuit|
      circuits_count += 1
      circuit.each_device { |_| devices_count += 1 }
    end
    counts[phase] = {'circuits' => circuits_count, 'devices' => devices_count}
  end
  File.write($report_file + '.normalization.json', JSON.pretty_generate(counts))
end
raise 'LVS-REPORT-MISSING: no comparison database' unless File.file?($report_file)
db = RBA::LayoutVsSchematic.new
db.read($report_file)
xref = db.xref
raise 'LVS-COMPARISON-MISSING: no cross reference' unless xref
circuits = 0
mismatches = 0
skipped = 0
details = []
top_seen = false
xref.each_circuit_pair do |pair|
  top_seen = true if pair.first && pair.second && $top_cell && pair.first.name == $top_cell
  circuits += 1
  status = pair.status
  if status == RBA::NetlistCrossReference::Skipped || status == RBA::NetlistCrossReference::None
    skipped += 1
  elsif status != RBA::NetlistCrossReference::Match
    mismatches += 1
  end
  detail = {'layout' => pair.first && pair.first.name,
            'schematic' => pair.second && pair.second.name, 'status' => status.to_s}
  ['pin', 'net', 'device', 'subcircuit'].each do |kind|
    entries = []
    xref.send('each_' + kind + '_pair', pair) do |item|
      next if item.status == RBA::NetlistCrossReference::Match
      entries << {'layout' => item.first && item.first.to_s,
                  'schematic' => item.second && item.second.to_s,
                  'status' => item.status.to_s}
    end
    detail[kind] = entries
  end
  details << detail
end
raise 'LVS-COMPARISON-MISSING: empty cross reference' if circuits == 0
raise 'LVS-COMPARISON-MISSING: top circuit absent' if $top_cell && !$top_cell.empty? && !top_seen
File.write($report_file + '.details.json', JSON.pretty_generate(details))
File.write($report_file + '.summary', "DESIGNPP_LVS_XREF_V1 #{circuits} #{mismatches} #{skipped}\n")
)ruby";
}

core::Result<std::string> NormalizeCdlForSpice(std::string_view contents) {
  std::string result(contents);
  std::vector<std::pair<std::size_t, std::size_t>> tokens;
  bool instance = false;
  bool resistor = false;
  const auto finish = [&]() -> bool {
    if (resistor && tokens.size() == 4) {
      const auto [offset, length] = tokens.back();
      std::string value(contents.substr(offset, length));
      std::transform(
          value.begin(), value.end(), value.begin(),
          [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      // sky130hd explicitly models tie-cell connections as zero-ohm shorts.
      if (value == "short") {
        result[offset] = '0';
        for (std::size_t i = 1; i < length; ++i) result[offset + i] = ' ';
      }
    }
    if (!instance) return true;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
      const auto [offset, length] = tokens[i];
      if (contents.substr(offset, length) != "/") continue;
      if (i < 2 || i + 2 != tokens.size()) return false;
      result[offset] = ' ';
    }
    return true;
  };
  std::size_t begin = 0;
  while (begin < contents.size()) {
    const auto newline = contents.find('\n', begin);
    const auto end =
        newline == std::string_view::npos ? contents.size() : newline;
    std::size_t cursor = begin;
    while (cursor < end &&
           std::isspace(static_cast<unsigned char>(contents[cursor])))
      ++cursor;
    if (cursor < end && contents[cursor] != '*' && contents[cursor] != ';') {
      if (contents[cursor] == '+') {
        ++cursor;
      } else {
        if (!finish()) {
          return core::Status{core::ErrorCode::kCorruptData,
                              "LVS-CDL-PARSE: ambiguous CDL instance separator",
                              0};
        }
        tokens.clear();
        instance = contents[cursor] == 'X' || contents[cursor] == 'x';
        resistor = contents[cursor] == 'R' || contents[cursor] == 'r';
      }
      while (cursor < end) {
        while (cursor < end &&
               std::isspace(static_cast<unsigned char>(contents[cursor])))
          ++cursor;
        if (cursor == end || contents[cursor] == '$' || contents[cursor] == ';')
          break;
        const auto offset = cursor;
        while (cursor < end &&
               !std::isspace(static_cast<unsigned char>(contents[cursor])))
          ++cursor;
        tokens.emplace_back(offset, cursor - offset);
      }
    }
    begin = newline == std::string_view::npos ? contents.size() : newline + 1;
  }
  if (!finish()) {
    return core::Status{core::ErrorCode::kCorruptData,
                        "LVS-CDL-PARSE: ambiguous CDL instance separator", 0};
  }
  return result;
}

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

core::Result<VerificationResult> PhysicalVerificationAdapter::ParseResult(
    const VerificationResultArtifacts& artifacts) const {
  return ParseReport(artifacts.report);
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

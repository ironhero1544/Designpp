// Copyright 2026 The Design++ Authors

#include "designpp/application/prepared_physical_inputs.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <sstream>
#include <utility>

#include "designpp/application/managed_flow_run_service.h"
#include "designpp/application/physical_implementation_service.h"
#include "designpp/application/synthesis_fingerprint.h"

namespace designpp::application {
namespace {

std::filesystem::path Utf8Path(std::string_view text) {
  std::u8string value;
  value.reserve(text.size());
  for (char character : text) value.push_back(static_cast<char8_t>(character));
  return std::filesystem::path(value);
}

core::Result<std::string> ReadFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return core::Status{core::ErrorCode::kNotFound,
                        "Physical implementation input is missing", 0};
  }
  std::ostringstream output;
  output << input.rdbuf();
  if (!input.eof() && input.fail()) {
    return core::Status{core::ErrorCode::kIoError,
                        "Cannot read physical implementation input", 0};
  }
  return output.str();
}

std::string StripTclComments(std::string_view input) {
  std::string result;
  result.reserve(input.size());
  bool quoted = false;
  bool escaped = false;
  for (char character : input) {
    if (character == '\n') {
      result.push_back(character);
      quoted = false;
      escaped = false;
      continue;
    }
    if (!quoted && !escaped && character == '#') {
      while (!result.empty() && result.back() == ' ') result.pop_back();
      result.append(" #");
      escaped = false;
      continue;
    }
    if (!escaped && character == '"') quoted = !quoted;
    escaped = !escaped && character == '\\';
    if (character != '\r') result.push_back(character);
  }
  std::istringstream lines(result);
  std::string clean;
  std::string line;
  while (std::getline(lines, line)) {
    const std::size_t comment = line.find(" #");
    if (comment != std::string::npos) line.resize(comment);
    clean.append(line);
    clean.push_back('\n');
  }
  return clean;
}

std::string TrimToken(std::string token) {
  while (!token.empty() && (token.front() == '{' || token.front() == '"' ||
                            token.front() == '[')) {
    token.erase(token.begin());
  }
  while (!token.empty() && (token.back() == '}' || token.back() == '"' ||
                            token.back() == ']' || token.back() == ';')) {
    token.pop_back();
  }
  return token;
}

std::optional<double> TimeUnitToNanoseconds(std::string_view unit) {
  if (unit == "s") return 1.0e9;
  if (unit == "ms") return 1.0e6;
  if (unit == "us") return 1.0e3;
  if (unit == "ns") return 1.0;
  if (unit == "ps") return 1.0e-3;
  if (unit == "fs") return 1.0e-6;
  return std::nullopt;
}

struct ParsedClockContract {
  std::string final_time_unit = "ns";
  std::optional<double> shortest_period_ns;
  std::vector<PreparedPhysicalClock> clocks;
};

core::Result<ParsedClockContract> ParseClockContract(std::string_view sdc) {
  ParsedClockContract result;
  double scale = 1.0;
  std::string normalized = StripTclComments(sdc);
  std::replace(normalized.begin(), normalized.end(), '\\', ' ');
  std::istringstream lines(normalized);
  std::string line;
  while (std::getline(lines, line)) {
    std::istringstream tokens(line);
    std::vector<std::string> words;
    std::string word;
    while (tokens >> word) words.push_back(TrimToken(std::move(word)));
    if (words.empty()) continue;
    const auto time_option =
        std::find(words.begin(), words.end(), std::string("-time"));
    if (words.front() == "set_cmd_units" && time_option != words.end() &&
        std::next(time_option) != words.end()) {
      const std::string unit = *std::next(time_option);
      const auto next_scale = TimeUnitToNanoseconds(unit);
      if (!next_scale) {
        return core::Status{
            core::ErrorCode::kInvalidArgument,
            "Managed SDC uses an unsupported time unit: " + unit, 0};
      }
      scale = *next_scale;
      result.final_time_unit = unit;
      continue;
    }
    if (words.front() != "create_clock") continue;
    const auto period_option =
        std::find(words.begin(), words.end(), std::string("-period"));
    if (period_option == words.end() ||
        std::next(period_option) == words.end()) {
      continue;
    }
    const std::string& value = *std::next(period_option);
    double period = 0.0;
    const auto parsed =
        std::from_chars(value.data(), value.data() + value.size(), period);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
        !std::isfinite(period) || period <= 0.0) {
      continue;
    }
    const double period_ns = period * scale;
    PreparedPhysicalClock clock;
    const auto name_option =
        std::find(words.begin(), words.end(), std::string("-name"));
    if (name_option != words.end() && std::next(name_option) != words.end()) {
      clock.name = *std::next(name_option);
    }
    const auto get_ports =
        std::find(words.begin(), words.end(), std::string("get_ports"));
    if (get_ports != words.end() && std::next(get_ports) != words.end()) {
      clock.target_port = *std::next(get_ports);
    }
    if (clock.name.empty()) clock.name = clock.target_port;
    clock.period_ns = period_ns;
    result.clocks.push_back(std::move(clock));
    if (!result.shortest_period_ns || period_ns < *result.shortest_period_ns) {
      result.shortest_period_ns = period_ns;
    }
  }
  return result;
}

std::string FormatDouble(double value) {
  std::ostringstream output;
  output << std::setprecision(15) << value;
  return output.str();
}

core::Result<PreparedPhysicalFile> ReadPreparedFile(
    std::string relative_path, const std::filesystem::path& source,
    std::filesystem::path staged_path) {
  auto contents = ReadFile(source);
  if (!contents.Ok()) return contents.GetStatus();
  auto digest = CalculateSha256(contents.Value());
  if (!digest.Ok()) return digest.GetStatus();
  return PreparedPhysicalFile{std::move(relative_path), std::move(staged_path),
                              std::move(contents).Value(),
                              std::move(digest).Value()};
}

}  // namespace

core::Result<std::shared_ptr<const PreparedPhysicalInputs>>
PrepareOrfsPhysicalInputs(const ManagedFlowRunRequest& request,
                          std::string_view tool_version) {
  auto prepared = std::make_shared<PreparedPhysicalInputs>();
  prepared->tool_version = tool_version;
  prepared->configuration_contract_hex = EncodePhysicalImplementationContract(
      BuildPhysicalImplementationConfigurationContract(request, tool_version));
  std::ostringstream manifest;
  manifest << "input_contract:6\n"
           << "backend:orfs\n"
           << "orfs_root:" << request.profile.orfs_root << '\n'
           << "tool_version:" << tool_version << '\n'
           << "platform:"
           << request.project.physical_implementation.orfs.platform << '\n'
           << "variant:"
           << request.project.physical_implementation.orfs.flow_variant << '\n'
           << "top:" << request.project.top_module << '\n';

  std::size_t source_index = 0;
  for (const ResolvedSource& source : request.sources) {
    if (!source.enabled || !source.exists ||
        source.view_kind != core::ViewKind::kVerilog) {
      continue;
    }
    std::filesystem::path extension = source.windows_path.extension();
    if (extension.empty()) extension = ".sv";
    auto file = ReadPreparedFile(
        source.relative_path, source.windows_path,
        std::filesystem::path("source_" + std::to_string(source_index++) +
                              extension.string()));
    if (!file.Ok()) return file.GetStatus();
    manifest << "source:" << file.Value().relative_path << ':'
             << file.Value().sha256 << '\n';
    prepared->rtl_sources.push_back(std::move(file).Value());
  }
  if (prepared->rtl_sources.empty()) {
    return core::Status{core::ErrorCode::kInvalidArgument,
                        "No enabled RTL is available for ORFS", 0};
  }

  for (std::size_t index = 0;
       index < request.project.include_directories.size(); ++index) {
    PreparedPhysicalInclude include;
    include.configured_path = request.project.include_directories[index];
    const std::filesystem::path root =
        request.library_directory / Utf8Path(include.configured_path);
    std::error_code error;
    if (!std::filesystem::is_directory(root, error) || error) {
      return core::Status{core::ErrorCode::kNotFound,
                          "ORFS include directory is missing", 0};
    }
    std::vector<std::filesystem::path> files;
    for (std::filesystem::recursive_directory_iterator iterator(
             root, std::filesystem::directory_options::skip_permission_denied,
             error),
         end;
         !error && iterator != end; iterator.increment(error)) {
      if (iterator->is_regular_file(error) && !error) {
        files.push_back(iterator->path());
      }
    }
    if (error) {
      return core::Status{core::ErrorCode::kIoError,
                          "Cannot enumerate an ORFS include directory",
                          static_cast<unsigned long>(error.value())};
    }
    std::sort(files.begin(), files.end(),
              [&root](const auto& left, const auto& right) {
                return std::filesystem::relative(left, root).generic_string() <
                       std::filesystem::relative(right, root).generic_string();
              });
    manifest << "include_" << index << ".count:" << files.size() << '\n';
    for (const std::filesystem::path& path : files) {
      const std::filesystem::path relative =
          std::filesystem::relative(path, root, error);
      if (error || relative.empty()) {
        return core::Status{core::ErrorCode::kIoError,
                            "Cannot resolve an ORFS include path", 0};
      }
      auto file = ReadPreparedFile(relative.generic_string(), path, relative);
      if (!file.Ok()) return file.GetStatus();
      manifest << "include_" << index << ':' << relative.generic_string() << ':'
               << file.Value().sha256 << '\n';
      include.files.push_back(std::move(file).Value());
    }
    prepared->include_directories.push_back(std::move(include));
  }

  const std::string sdc_relative =
      request.project.physical_implementation.pnr_sdc_path.empty()
          ? request.project.physical_implementation.signoff_sdc_path
          : request.project.physical_implementation.pnr_sdc_path;
  if (!sdc_relative.empty()) {
    auto source = ReadFile(request.library_directory / Utf8Path(sdc_relative));
    if (!source.Ok()) return source.GetStatus();
    prepared->sdc_contents =
        "# Design++ managed SDC command units\n"
        "set_cmd_units -time ns\n" +
        source.Value();
    prepared->sdc_provenance = sdc_relative;
    manifest << "sdc:managed:" << sdc_relative << '\n';
  } else {
    std::ostringstream generated;
    generated << "# Generated minimal SDC by Design++\n"
              << "set_cmd_units -time ns\n";
    for (const std::string& port :
         request.project.physical_implementation.clock_ports) {
      generated << "create_clock -name {" << port << "} -period "
                << request.project.physical_implementation.clock_period_ns
                << " [get_ports {" << port << "}]\n";
    }
    prepared->sdc_contents = generated.str();
    prepared->sdc_provenance = "auto_generated";
    prepared->sdc_auto_generated = true;
    manifest << "sdc:auto_generated\n";
  }
  prepared->unconstrained_warning =
      request.project.physical_implementation.clock_ports.empty() &&
      sdc_relative.empty();
  auto clock = ParseClockContract(prepared->sdc_contents);
  if (!clock.Ok()) return clock.GetStatus();
  prepared->sdc_time_unit = clock.Value().final_time_unit;
  prepared->clocks = std::move(clock).Value().clocks;
  prepared->effective_clock_period_ns = clock.Value().shortest_period_ns;
  if (prepared->effective_clock_period_ns) {
    prepared->effective_clock_period_text =
        FormatDouble(*prepared->effective_clock_period_ns);
  }
  auto sdc_hash = CalculateSha256(prepared->sdc_contents);
  if (!sdc_hash.Ok()) return sdc_hash.GetStatus();
  prepared->sdc_sha256 = std::move(sdc_hash).Value();
  manifest << "sdc_sha256:" << prepared->sdc_sha256 << '\n'
           << "effective_clock_period_ns:"
           << prepared->effective_clock_period_text << '\n';
  for (const PreparedPhysicalClock& prepared_clock : prepared->clocks) {
    manifest << "clock:" << prepared_clock.name << ':'
             << prepared_clock.target_port << ':'
             << FormatDouble(prepared_clock.period_ns) << '\n';
  }
  manifest
      << "environment:" << request.environment_id << ':'
      << request.environment_fingerprint << '\n'
      << "advanced:"
      << request.project.physical_implementation.orfs.advanced_variables_json
      << '\n'
      << "configuration_contract_hex:" << prepared->configuration_contract_hex
      << '\n';
  prepared->fingerprint_manifest = manifest.str();
  auto fingerprint = CalculateSha256(prepared->fingerprint_manifest);
  if (!fingerprint.Ok()) return fingerprint.GetStatus();
  prepared->fingerprint = std::move(fingerprint).Value();
  return std::shared_ptr<const PreparedPhysicalInputs>(std::move(prepared));
}

}  // namespace designpp::application

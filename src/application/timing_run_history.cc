// Copyright 2026 The Design++ Authors

#include "designpp/application/timing_run_history.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <optional>
#include <sstream>

namespace designpp::application {
namespace {

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  std::ostringstream contents;
  contents << input.rdbuf();
  return input || input.eof() ? contents.str() : std::string{};
}

std::optional<double> JsonNumber(std::string_view json, std::string_view key) {
  const std::string marker = "\"" + std::string(key) + "\":";
  const std::size_t position = json.find(marker);
  if (position == std::string_view::npos) return std::nullopt;
  const char* begin = json.data() + position + marker.size();
  double value = 0.0;
  const auto parsed = std::from_chars(begin, json.data() + json.size(), value);
  return parsed.ec == std::errc{} ? std::optional<double>(value) : std::nullopt;
}

std::string JsonString(std::string_view json, std::string_view key) {
  const std::string marker = "\"" + std::string(key) + "\":\"";
  const std::size_t begin = json.find(marker);
  if (begin == std::string_view::npos) return {};
  const std::size_t value_begin = begin + marker.size();
  const std::size_t end = json.find('"', value_begin);
  return end == std::string_view::npos
             ? std::string{}
             : std::string(json.substr(value_begin, end - value_begin));
}

const RunArtifact* FindArtifact(const RunRecord& run, std::string_view kind,
                                std::string_view format) {
  const auto found =
      std::find_if(run.artifacts.begin(), run.artifacts.end(),
                   [kind, format](const RunArtifact& artifact) {
                     return artifact.kind == kind && artifact.format == format;
                   });
  return found == run.artifacts.end() ? nullptr : &*found;
}

void RestoreSummary(std::string_view json, TimingRunSnapshot* snapshot) {
  const std::string corner = JsonString(json, "corner");
  if (!corner.empty()) snapshot->corner = corner;
  const auto setup_wns = JsonNumber(json, "setup_wns");
  const auto setup_tns = JsonNumber(json, "setup_tns");
  const auto hold_wns = JsonNumber(json, "hold_wns");
  const auto hold_tns = JsonNumber(json, "hold_tns");
  if (setup_wns && setup_tns) {
    snapshot->metrics.setup.wns = *setup_wns;
    snapshot->metrics.setup.tns = *setup_tns;
    snapshot->metrics.setup.has_paths = true;
  }
  if (hold_wns && hold_tns) {
    snapshot->metrics.hold.wns = *hold_wns;
    snapshot->metrics.hold.tns = *hold_tns;
    snapshot->metrics.hold.has_paths = true;
  }
}

}  // namespace

core::Result<std::vector<TimingRunSnapshot>> LoadTimingRunHistory(
    const std::filesystem::path& cell_directory,
    std::string_view fallback_corner) {
  RunStore store;
  auto loaded = store.List(cell_directory);
  if (!loaded.Ok()) return loaded.GetStatus();

  adapters::OpenStaAdapter adapter;
  std::vector<TimingRunSnapshot> snapshots;
  for (RunRecord& run : std::move(loaded).Value()) {
    if (run.stage != "StaticTimingAnalysis") continue;
    TimingRunSnapshot snapshot;
    snapshot.run = std::move(run);
    snapshot.corner = std::string(fallback_corner);
    const RunArtifact* summary = FindArtifact(snapshot.run, "summary", "json");
    if (summary != nullptr) {
      RestoreSummary(ReadFile(snapshot.run.directory / summary->relative_path),
                     &snapshot);
    }
    const RunArtifact* script = FindArtifact(snapshot.run, "script", "tcl");
    if (script != nullptr) {
      snapshot.script_text =
          ReadFile(snapshot.run.directory / script->relative_path);
    }
    const std::string raw =
        ReadFile(snapshot.run.directory / L"logs" / L"raw.log");
    snapshot.diagnostics = adapter.ParseDiagnostics(raw);
    const RunArtifact* report = FindArtifact(snapshot.run, "report", "text");
    if (report != nullptr) {
      auto parsed = adapter.ParseReport(
          raw, ReadFile(snapshot.run.directory / report->relative_path),
          snapshot.corner);
      if (parsed.Ok()) snapshot.metrics = std::move(parsed).Value();
    }
    snapshots.push_back(std::move(snapshot));
  }
  return snapshots;
}

}  // namespace designpp::application

// Copyright 2026 The Design++ Authors

#include "designpp/application/wsl_distribution_service.h"

#include <windows.h>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <mutex>
#include <utility>

#include "designpp/runtime/process_runner.h"

namespace designpp::application {
namespace {

std::wstring Trim(std::wstring_view text) {
  const auto is_space = [](wchar_t character) {
    return character == L' ' || character == L'\t' || character == L'\r' ||
           character == L'\n' || character == 0xfeff;
  };
  while (!text.empty() && is_space(text.front())) text.remove_prefix(1);
  while (!text.empty() && is_space(text.back())) text.remove_suffix(1);
  return std::wstring(text);
}

std::string WideToUtf8(std::wstring_view text) {
  if (text.empty()) return {};
  const int size = WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
      nullptr, 0, nullptr, nullptr);
  if (size <= 0) return {};
  std::string result(static_cast<std::size_t>(size), '\0');
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), result.data(), size,
                          nullptr, nullptr) <= 0) {
    return {};
  }
  return result;
}

core::Result<std::wstring> DecodeOutput(std::string_view raw_output) {
  if (raw_output.empty()) {
    return core::Status{core::ErrorCode::kNotFound,
                        "WSL did not return a distribution list", 0};
  }
  const bool has_utf16_bom =
      raw_output.size() >= 2 &&
      static_cast<unsigned char>(raw_output[0]) == 0xff &&
      static_cast<unsigned char>(raw_output[1]) == 0xfe;
  const bool looks_utf16 =
      raw_output.size() >= 4 &&
      std::count(raw_output.begin(), raw_output.end(), '\0') >=
          static_cast<std::ptrdiff_t>(raw_output.size() / 8);
  if (has_utf16_bom || looks_utf16) {
    const std::size_t offset = has_utf16_bom ? 2 : 0;
    if ((raw_output.size() - offset) % 2 != 0) {
      return core::Status{core::ErrorCode::kCorruptData,
                          "WSL distribution output has invalid UTF-16 data", 0};
    }
    std::wstring decoded;
    decoded.reserve((raw_output.size() - offset) / 2);
    for (std::size_t index = offset; index < raw_output.size(); index += 2) {
      const auto low = static_cast<unsigned char>(raw_output[index]);
      const auto high = static_cast<unsigned char>(raw_output[index + 1]);
      decoded.push_back(static_cast<wchar_t>(low | (high << 8)));
    }
    return decoded;
  }

  const int size =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw_output.data(),
                          static_cast<int>(raw_output.size()), nullptr, 0);
  if (size <= 0) {
    return core::Status{core::ErrorCode::kInvalidEncoding,
                        "WSL distribution output is not valid UTF-8 or UTF-16",
                        GetLastError()};
  }
  std::wstring decoded(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw_output.data(),
                      static_cast<int>(raw_output.size()), decoded.data(),
                      size);
  return decoded;
}

}  // namespace

core::Result<std::vector<WslDistribution>> ParseWslDistributionList(
    std::string_view raw_output) {
  auto decoded = DecodeOutput(raw_output);
  if (!decoded.Ok()) return decoded.GetStatus();

  std::vector<WslDistribution> result;
  std::wstring_view remaining = decoded.Value();
  while (!remaining.empty()) {
    const std::size_t newline = remaining.find(L'\n');
    std::wstring line = Trim(remaining.substr(0, newline));
    if (newline == std::wstring_view::npos) {
      remaining = {};
    } else {
      remaining.remove_prefix(newline + 1);
    }
    if (line.empty()) continue;

    bool is_default = false;
    if (line.front() == L'*') {
      is_default = true;
      line = Trim(std::wstring_view(line).substr(1));
    }

    const std::size_t version_separator = line.find_last_of(L" \t");
    if (version_separator == std::wstring::npos) continue;
    const std::wstring version_text =
        Trim(std::wstring_view(line).substr(version_separator + 1));
    std::uint32_t version = 0;
    const std::string version_utf8 = WideToUtf8(version_text);
    const auto converted =
        std::from_chars(version_utf8.data(),
                        version_utf8.data() + version_utf8.size(), version);
    if (converted.ec != std::errc{} ||
        converted.ptr != version_utf8.data() + version_utf8.size() ||
        (version != 1 && version != 2)) {
      continue;
    }

    line = Trim(std::wstring_view(line).substr(0, version_separator));
    std::size_t state_separator = std::wstring::npos;
    for (std::size_t index = 1; index < line.size(); ++index) {
      if ((line[index] == L' ' || line[index] == L'\t') &&
          (line[index - 1] == L' ' || line[index - 1] == L'\t')) {
        state_separator = index - 1;
      }
    }
    if (state_separator == std::wstring::npos) {
      state_separator = line.find_last_of(L" \t");
    }
    if (state_separator == std::wstring::npos) continue;
    std::size_t state_start = state_separator;
    while (state_start < line.size() &&
           (line[state_start] == L' ' || line[state_start] == L'\t')) {
      ++state_start;
    }
    const std::wstring state =
        Trim(std::wstring_view(line).substr(state_start));
    const std::wstring name =
        Trim(std::wstring_view(line).substr(0, state_separator));
    if (name.empty() || state.empty()) continue;

    result.push_back(
        {WideToUtf8(name), WideToUtf8(state), version, is_default});
  }
  if (result.empty()) {
    return core::Status{core::ErrorCode::kNotFound,
                        "No installed WSL distributions were found", 0};
  }
  return result;
}

struct WslDistributionService::Implementation final
    : std::enable_shared_from_this<Implementation> {
  void Complete(runtime::ProcessResult process_result) {
    WslDistributionEventSink callback;
    WslDistributionEvent event;
    {
      std::scoped_lock lock(mutex);
      if (shutdown || !active || terminal_delivered || !sink) return;
      terminal_delivered = true;
      active = false;
      event.generation = generation;
      callback = sink;
    }
    if (process_result.cancelled) {
      event.status = {core::ErrorCode::kCancelled,
                      "WSL distribution discovery cancelled", 0};
    } else if (!process_result.started ||
               !process_result.error_message.empty() ||
               process_result.exit_code != 0) {
      event.status = {core::ErrorCode::kIoError,
                      "Cannot enumerate WSL distributions",
                      process_result.exit_code};
    } else {
      auto parsed = ParseWslDistributionList(process_result.output);
      event.status = parsed.Ok() ? core::Status::Success() : parsed.GetStatus();
      if (parsed.Ok()) event.distributions = std::move(parsed).Value();
    }
    callback(std::move(event));
  }

  mutable std::mutex mutex;
  runtime::ProcessSession session;
  WslDistributionEventSink sink;
  std::uint64_t generation = 0;
  bool active = false;
  bool terminal_delivered = false;
  bool shutdown = false;
};

WslDistributionService::WslDistributionService()
    : implementation_(std::make_shared<Implementation>()) {}

WslDistributionService::~WslDistributionService() { Shutdown(); }

core::Status WslDistributionService::Start(std::uint64_t generation,
                                           WslDistributionEventSink sink) {
  if (!implementation_ || generation == 0 || !sink) {
    return {core::ErrorCode::kInvalidArgument,
            "WSL distribution discovery request is incomplete", 0};
  }
  {
    std::scoped_lock lock(implementation_->mutex);
    if (implementation_->shutdown) {
      return {core::ErrorCode::kCancelled,
              "WSL distribution discovery is shutting down", 0};
    }
    if (implementation_->active) {
      return {core::ErrorCode::kConflict,
              "WSL distribution discovery is already active", 0};
    }
    implementation_->generation = generation;
    implementation_->sink = std::move(sink);
    implementation_->active = true;
    implementation_->terminal_delivered = false;
  }

  runtime::ProcessRequest request;
  request.executable = L"wsl.exe";
  request.arguments = {L"--list", L"--verbose"};
  const auto implementation = implementation_;
  runtime::ProcessLaunchResult launch = runtime::ProcessRunner::RunAsync(
      std::move(request), {}, [implementation](runtime::ProcessResult result) {
        implementation->Complete(std::move(result));
      });
  if (!launch.IsValid()) {
    std::scoped_lock lock(implementation_->mutex);
    implementation_->active = false;
    implementation_->terminal_delivered = true;
    return {core::ErrorCode::kIoError,
            "Cannot start WSL distribution discovery", 0};
  }
  {
    std::scoped_lock lock(implementation_->mutex);
    implementation_->session = std::move(launch.session);
    if (!implementation_->active) implementation_->session.Cancel();
  }
  return core::Status::Success();
}

void WslDistributionService::Cancel() noexcept {
  if (!implementation_) return;
  std::scoped_lock lock(implementation_->mutex);
  if (implementation_->active) implementation_->session.Cancel();
}

void WslDistributionService::Shutdown() noexcept {
  if (!implementation_) return;
  std::scoped_lock lock(implementation_->mutex);
  if (implementation_->shutdown) return;
  implementation_->shutdown = true;
  implementation_->active = false;
  implementation_->sink = {};
  implementation_->session.Cancel();
}

bool WslDistributionService::IsActive() const {
  if (!implementation_) return false;
  std::scoped_lock lock(implementation_->mutex);
  return implementation_->active;
}

}  // namespace designpp::application

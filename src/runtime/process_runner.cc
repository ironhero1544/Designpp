#include "designpp/runtime/process_runner.h"

// Shell API declarations require the base Windows declarations first.
// clang-format off
#include <windows.h>
#include <shellapi.h>
// clang-format on

#include <array>
#include <atomic>
#include <mutex>
#include <thread>
#include <utility>

namespace designpp::runtime {
namespace {

class UniqueHandle final {
 public:
  UniqueHandle() noexcept = default;
  explicit UniqueHandle(HANDLE handle) noexcept : handle_(handle) {}
  UniqueHandle(const UniqueHandle&) = delete;
  UniqueHandle& operator=(const UniqueHandle&) = delete;

  UniqueHandle(UniqueHandle&& other) noexcept
      : handle_(std::exchange(other.handle_, nullptr)) {}

  UniqueHandle& operator=(UniqueHandle&& other) noexcept {
    if (this != &other) {
      Reset();
      handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
  }

  ~UniqueHandle() { Reset(); }

  [[nodiscard]] HANDLE get() const noexcept { return handle_; }

  [[nodiscard]] bool valid() const noexcept {
    return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
  }

  HANDLE release() noexcept { return std::exchange(handle_, nullptr); }

  void Reset(HANDLE handle = nullptr) noexcept {
    if (valid()) {
      CloseHandle(handle_);
    }
    handle_ = handle;
  }

 private:
  HANDLE handle_{nullptr};
};

[[nodiscard]] std::wstring QuoteWindowsArgument(const std::wstring& argument) {
  if (argument.empty()) {
    return L"\"\"";
  }

  const bool needs_quotes =
      argument.find_first_of(L" \t\n\v\"") != std::wstring::npos;
  if (!needs_quotes) {
    return argument;
  }

  std::wstring quoted{L'\"'};
  std::size_t backslashes = 0;
  for (const wchar_t character : argument) {
    if (character == L'\\') {
      ++backslashes;
      continue;
    }
    if (character == L'\"') {
      quoted.append(backslashes * 2 + 1, L'\\');
      quoted.push_back(L'\"');
      backslashes = 0;
      continue;
    }
    quoted.append(backslashes, L'\\');
    backslashes = 0;
    quoted.push_back(character);
  }
  quoted.append(backslashes * 2, L'\\');
  quoted.push_back(L'\"');
  return quoted;
}

[[nodiscard]] std::wstring BuildCommandLine(const ProcessRequest& request) {
  std::wstring command_line =
      QuoteWindowsArgument(request.executable.wstring());
  for (const auto& argument : request.arguments) {
    command_line.push_back(L' ');
    command_line.append(QuoteWindowsArgument(argument));
  }
  return command_line;
}

[[nodiscard]] std::wstring BuildArgumentLine(const ProcessRequest& request) {
  std::wstring arguments;
  for (const std::wstring& argument : request.arguments) {
    if (!arguments.empty()) {
      arguments.push_back(L' ');
    }
    arguments.append(QuoteWindowsArgument(argument));
  }
  return arguments;
}

[[nodiscard]] std::wstring ResolveExecutablePath(
    const std::filesystem::path& executable) {
  if (executable.has_parent_path()) {
    return executable.wstring();
  }

  std::array<wchar_t, 32768> resolved{};
  const std::wstring name = executable.wstring();
  const DWORD length = SearchPathW(nullptr, name.c_str(), nullptr,
                                   static_cast<DWORD>(resolved.size()),
                                   resolved.data(), nullptr);
  if (length > 0 && length < resolved.size()) {
    return std::wstring(resolved.data(), length);
  }
  return name;
}

[[nodiscard]] std::wstring FormatWindowsError(const DWORD error) {
  wchar_t* message_buffer = nullptr;
  const DWORD length = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, error, 0, reinterpret_cast<wchar_t*>(&message_buffer), 0,
      nullptr);
  if (length == 0 || message_buffer == nullptr) {
    return L"Windows 오류 " + std::to_wstring(error);
  }

  std::wstring message(message_buffer, length);
  LocalFree(message_buffer);
  return message;
}

}  // namespace

struct ProcessSession::Implementation final {
  std::mutex handles_mutex;
  UniqueHandle job;
  UniqueHandle process;
  std::jthread worker;
  std::atomic_bool cancel_requested{false};
  std::atomic_bool running{true};
};

ProcessSession::ProcessSession(std::shared_ptr<Implementation> implementation)
    : implementation_(std::move(implementation)) {}

ProcessSession::ProcessSession(ProcessSession&& other) noexcept = default;

ProcessSession& ProcessSession::operator=(ProcessSession&& other) noexcept {
  if (this != &other) {
    StopAndJoin();
    implementation_ = std::move(other.implementation_);
  }
  return *this;
}

ProcessSession::~ProcessSession() { StopAndJoin(); }

void ProcessSession::Cancel() noexcept {
  if (!implementation_) {
    return;
  }

  implementation_->cancel_requested.store(true);
  std::scoped_lock lock(implementation_->handles_mutex);
  if (implementation_->job.valid()) {
    TerminateJobObject(implementation_->job.get(), ERROR_CANCELLED);
  } else if (implementation_->process.valid()) {
    TerminateProcess(implementation_->process.get(), ERROR_CANCELLED);
  }
}

bool ProcessSession::IsRunning() const noexcept {
  return implementation_ && implementation_->running.load();
}

bool ProcessSession::IsValid() const noexcept {
  return implementation_ != nullptr;
}

void ProcessSession::StopAndJoin() noexcept {
  if (!implementation_) {
    return;
  }
  Cancel();
  if (implementation_->worker.joinable()) {
    implementation_->worker.join();
  }
  implementation_.reset();
}

ProcessLaunchResult ProcessRunner::RunAsync(ProcessRequest request,
                                            OutputCallback on_output,
                                            CompletionCallback on_complete) {
  if (request.executable.empty()) {
    return {ProcessSession(), L"Process executable cannot be empty."};
  }

  auto implementation = std::make_shared<ProcessSession::Implementation>();
  auto* const state = implementation.get();

  implementation->worker = std::jthread([state, request = std::move(request),
                                         on_output = std::move(on_output),
                                         on_complete =
                                             std::move(on_complete)]() mutable {
    ProcessResult result;
    UniqueHandle output_read;
    UniqueHandle output_write;

    SECURITY_ATTRIBUTES security_attributes{};
    security_attributes.nLength = sizeof(security_attributes);
    security_attributes.bInheritHandle = TRUE;

    HANDLE raw_read = nullptr;
    HANDLE raw_write = nullptr;
    if (!CreatePipe(&raw_read, &raw_write, &security_attributes, 0)) {
      result.error_message = FormatWindowsError(GetLastError());
      state->running.store(false);
      if (on_complete) {
        on_complete(std::move(result));
      }
      return;
    }
    output_read.Reset(raw_read);
    output_write.Reset(raw_write);
    SetHandleInformation(output_read.get(), HANDLE_FLAG_INHERIT, 0);

    UniqueHandle job(CreateJobObjectW(nullptr, nullptr));
    if (job.valid()) {
      JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
      limits.BasicLimitInformation.LimitFlags =
          JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
      SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation,
                              &limits, sizeof(limits));
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = output_write.get();
    startup.hStdError = output_write.get();

    PROCESS_INFORMATION process_information{};
    std::wstring command_line = BuildCommandLine(request);
    const std::wstring executable = ResolveExecutablePath(request.executable);
    const std::wstring working_directory =
        request.working_directory ? request.working_directory->wstring()
                                  : std::wstring{};

    const BOOL created = CreateProcessW(
        executable.c_str(), command_line.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr,
        working_directory.empty() ? nullptr : working_directory.c_str(),
        &startup, &process_information);
    if (!created) {
      result.error_message = FormatWindowsError(GetLastError());
      state->running.store(false);
      if (on_complete) {
        on_complete(std::move(result));
      }
      return;
    }

    result.started = true;
    UniqueHandle thread_handle(process_information.hThread);
    UniqueHandle process_handle(process_information.hProcess);
    if (job.valid()) {
      AssignProcessToJobObject(job.get(), process_handle.get());
    }

    {
      std::scoped_lock lock(state->handles_mutex);
      state->job = std::move(job);
      state->process = std::move(process_handle);
      if (state->cancel_requested.load()) {
        if (state->job.valid()) {
          TerminateJobObject(state->job.get(), ERROR_CANCELLED);
        } else {
          TerminateProcess(state->process.get(), ERROR_CANCELLED);
        }
      }
    }

    output_write.Reset();
    std::array<char, 4096> buffer{};
    for (;;) {
      DWORD bytes_read = 0;
      if (!ReadFile(output_read.get(), buffer.data(),
                    static_cast<DWORD>(buffer.size()), &bytes_read, nullptr) ||
          bytes_read == 0) {
        break;
      }
      std::string chunk(buffer.data(), bytes_read);
      result.output.append(chunk);
      if (on_output) {
        on_output(std::move(chunk));
      }
    }

    HANDLE process = nullptr;
    {
      std::scoped_lock lock(state->handles_mutex);
      process = state->process.get();
    }
    if (process != nullptr) {
      WaitForSingleObject(process, INFINITE);
      DWORD exit_code = 0;
      if (GetExitCodeProcess(process, &exit_code)) {
        result.exit_code = exit_code;
      }
    }
    result.cancelled = state->cancel_requested.load();

    {
      std::scoped_lock lock(state->handles_mutex);
      state->process.Reset();
      state->job.Reset();
    }
    state->running.store(false);
    if (on_complete) {
      on_complete(std::move(result));
    }
  });

  return {ProcessSession(std::move(implementation)), {}};
}

ProcessLaunchResult ProcessRunner::RunElevatedAsync(
    ProcessRequest request, CompletionCallback on_complete) {
  if (request.executable.empty()) {
    return {ProcessSession(), L"Process executable cannot be empty."};
  }

  const std::wstring executable = ResolveExecutablePath(request.executable);
  const std::wstring arguments = BuildArgumentLine(request);
  const std::wstring working_directory =
      request.working_directory ? request.working_directory->wstring()
                                : std::wstring{};
  SHELLEXECUTEINFOW execute{};
  execute.cbSize = sizeof(execute);
  execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
  execute.lpVerb = L"runas";
  execute.lpFile = executable.c_str();
  execute.lpParameters = arguments.empty() ? nullptr : arguments.c_str();
  execute.lpDirectory =
      working_directory.empty() ? nullptr : working_directory.c_str();
  execute.nShow = SW_HIDE;
  if (!ShellExecuteExW(&execute) || execute.hProcess == nullptr) {
    return {ProcessSession(), FormatWindowsError(GetLastError())};
  }

  auto implementation = std::make_shared<ProcessSession::Implementation>();
  auto* const state = implementation.get();
  {
    std::scoped_lock lock(state->handles_mutex);
    state->process.Reset(execute.hProcess);
  }
  implementation->worker =
      std::jthread([state, on_complete = std::move(on_complete)]() mutable {
        ProcessResult result;
        result.started = true;
        HANDLE process = nullptr;
        {
          std::scoped_lock lock(state->handles_mutex);
          process = state->process.get();
          if (state->cancel_requested.load() && process != nullptr) {
            TerminateProcess(process, ERROR_CANCELLED);
          }
        }
        if (process != nullptr) {
          WaitForSingleObject(process, INFINITE);
          DWORD exit_code = 0;
          if (GetExitCodeProcess(process, &exit_code)) {
            result.exit_code = exit_code;
          }
        }
        result.cancelled = state->cancel_requested.load();
        {
          std::scoped_lock lock(state->handles_mutex);
          state->process.Reset();
        }
        state->running.store(false);
        if (on_complete) {
          on_complete(std::move(result));
        }
      });
  return {ProcessSession(std::move(implementation)), {}};
}

}  // namespace designpp::runtime

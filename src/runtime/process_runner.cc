#include "designpp/runtime/process_runner.h"

// Shell API declarations require the base Windows declarations first.
// clang-format off
#include <windows.h>
#include <shellapi.h>
// clang-format on

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace designpp::runtime {
namespace {

[[nodiscard]] std::size_t Utf8SequenceLength(unsigned char lead) {
  if ((lead & 0x80U) == 0) return 1;
  if ((lead & 0xE0U) == 0xC0U) return 2;
  if ((lead & 0xF0U) == 0xE0U) return 3;
  if ((lead & 0xF8U) == 0xF0U) return 4;
  return 1;
}

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

  std::vector<wchar_t> resolved(32768);
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

std::string TakeCompleteUtf8Chunk(std::string_view bytes,
                                  std::string* remainder) {
  if (remainder == nullptr) return std::string(bytes);

  std::string complete = std::move(*remainder);
  remainder->clear();
  complete.append(bytes);
  if (complete.empty()) return complete;

  std::size_t lead_index = complete.size() - 1;
  std::size_t continuation_count = 0;
  while (lead_index > 0 && continuation_count < 3 &&
         (static_cast<unsigned char>(complete[lead_index]) & 0xC0U) == 0x80U) {
    --lead_index;
    ++continuation_count;
  }
  const std::size_t available = complete.size() - lead_index;
  const std::size_t required =
      Utf8SequenceLength(static_cast<unsigned char>(complete[lead_index]));
  if (required > available) {
    *remainder = complete.substr(lead_index);
    complete.resize(lead_index);
  }
  return complete;
}

struct ProcessSession::Implementation final {
  static constexpr std::size_t kMaximumQueuedInputBytes = 64 * 1024;

  std::mutex handles_mutex;
  UniqueHandle job;
  UniqueHandle process;
  std::mutex input_mutex;
  std::condition_variable_any input_ready;
  std::deque<std::string> input_queue;
  std::size_t queued_input_bytes = 0;
  UniqueHandle input_write;
  std::jthread input_worker;
  std::jthread worker;
  std::atomic_bool cancel_requested{false};
  std::atomic_bool running{true};
  bool interactive_input = false;
  bool input_closed = true;
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
  {
    std::scoped_lock input_lock(implementation_->input_mutex);
    implementation_->input_closed = true;
    implementation_->input_queue.clear();
    implementation_->queued_input_bytes = 0;
  }
  implementation_->input_ready.notify_all();
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

InputWriteResult ProcessSession::WriteInput(std::string bytes) {
  if (!implementation_ || !implementation_->interactive_input) {
    return InputWriteResult::kNotInteractive;
  }
  if (bytes.empty()) return InputWriteResult::kAccepted;
  std::scoped_lock lock(implementation_->input_mutex);
  if (implementation_->input_closed || !implementation_->running.load()) {
    return InputWriteResult::kClosed;
  }
  if (bytes.size() > Implementation::kMaximumQueuedInputBytes ||
      implementation_->queued_input_bytes >
          Implementation::kMaximumQueuedInputBytes - bytes.size()) {
    return InputWriteResult::kQueueFull;
  }
  implementation_->queued_input_bytes += bytes.size();
  implementation_->input_queue.push_back(std::move(bytes));
  implementation_->input_ready.notify_one();
  return InputWriteResult::kAccepted;
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
  implementation->interactive_input = request.interactive_input;
  implementation->input_closed = !request.interactive_input;
  auto* const state = implementation.get();

  implementation->worker = std::jthread([state, request = std::move(request),
                                         on_output = std::move(on_output),
                                         on_complete =
                                             std::move(on_complete)]() mutable {
    ProcessResult result;
    UniqueHandle output_read;
    UniqueHandle output_write;
    UniqueHandle input_read;

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
    if (!SetHandleInformation(output_read.get(), HANDLE_FLAG_INHERIT, 0)) {
      result.error_message = FormatWindowsError(GetLastError());
      state->running.store(false);
      if (on_complete) on_complete(std::move(result));
      return;
    }

    if (request.interactive_input) {
      raw_read = nullptr;
      raw_write = nullptr;
      if (!CreatePipe(&raw_read, &raw_write, &security_attributes, 0)) {
        result.error_message = FormatWindowsError(GetLastError());
        state->running.store(false);
        if (on_complete) on_complete(std::move(result));
        return;
      }
      input_read.Reset(raw_read);
      {
        std::scoped_lock lock(state->input_mutex);
        state->input_write.Reset(raw_write);
      }
      if (!SetHandleInformation(state->input_write.get(), HANDLE_FLAG_INHERIT,
                                0)) {
        result.error_message = FormatWindowsError(GetLastError());
        {
          std::scoped_lock lock(state->input_mutex);
          state->input_closed = true;
          state->input_write.Reset();
        }
        state->running.store(false);
        if (on_complete) on_complete(std::move(result));
        return;
      }
    } else {
      input_read.Reset(CreateFileW(
          L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
          &security_attributes, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
      if (!input_read.valid()) {
        result.error_message = FormatWindowsError(GetLastError());
        state->running.store(false);
        if (on_complete) on_complete(std::move(result));
        return;
      }
    }

    UniqueHandle job(CreateJobObjectW(nullptr, nullptr));
    if (!job.valid()) {
      result.error_message = FormatWindowsError(GetLastError());
      state->running.store(false);
      if (on_complete) on_complete(std::move(result));
      return;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation,
                                 &limits, sizeof(limits))) {
      result.error_message = FormatWindowsError(GetLastError());
      state->running.store(false);
      if (on_complete) on_complete(std::move(result));
      return;
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input_read.get();
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
        CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
        nullptr,
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
    input_read.Reset();
    UniqueHandle thread_handle(process_information.hThread);
    UniqueHandle process_handle(process_information.hProcess);
    if (!AssignProcessToJobObject(job.get(), process_handle.get())) {
      const DWORD error = GetLastError();
      TerminateProcess(process_handle.get(), error);
      WaitForSingleObject(process_handle.get(), INFINITE);
      result.error_message = FormatWindowsError(error);
      {
        std::scoped_lock lock(state->input_mutex);
        state->input_closed = true;
        state->input_write.Reset();
      }
      state->running.store(false);
      if (on_complete) on_complete(std::move(result));
      return;
    }

    bool cancelled_before_start = false;
    {
      std::scoped_lock lock(state->handles_mutex);
      state->job = std::move(job);
      state->process = std::move(process_handle);
      cancelled_before_start = state->cancel_requested.load();
      if (cancelled_before_start) {
        TerminateJobObject(state->job.get(), ERROR_CANCELLED);
      }
    }

    if (!cancelled_before_start &&
        ResumeThread(thread_handle.get()) == static_cast<DWORD>(-1)) {
      const DWORD error = GetLastError();
      result.error_message = FormatWindowsError(error);
      std::scoped_lock lock(state->handles_mutex);
      TerminateJobObject(state->job.get(), error);
    }

    output_write.Reset();
    if (request.interactive_input) {
      state->input_worker = std::jthread([state](std::stop_token stop_token) {
        for (;;) {
          std::string bytes;
          HANDLE input = nullptr;
          {
            std::unique_lock lock(state->input_mutex);
            state->input_ready.wait(lock, stop_token, [state] {
              return state->input_closed || !state->input_queue.empty();
            });
            if ((stop_token.stop_requested() || state->input_closed) &&
                state->input_queue.empty()) {
              break;
            }
            bytes = std::move(state->input_queue.front());
            state->input_queue.pop_front();
            state->queued_input_bytes -= bytes.size();
            input = state->input_write.get();
          }
          DWORD written = 0;
          if (input == nullptr ||
              !WriteFile(input, bytes.data(), static_cast<DWORD>(bytes.size()),
                         &written, nullptr) ||
              written != bytes.size()) {
            std::scoped_lock lock(state->input_mutex);
            state->input_closed = true;
            state->input_queue.clear();
            state->queued_input_bytes = 0;
            break;
          }
        }
      });
    }
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
      std::scoped_lock lock(state->input_mutex);
      state->input_closed = true;
      state->input_queue.clear();
      state->queued_input_bytes = 0;
      state->input_write.Reset();
    }
    state->input_ready.notify_all();
    if (state->input_worker.joinable()) {
      state->input_worker.request_stop();
      state->input_worker.join();
    }

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
  // Elevated setup cannot redirect output through ShellExecuteEx. Keep its
  // console visible so the user can observe Windows/WSL installation progress.
  execute.nShow = SW_SHOWNORMAL;
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

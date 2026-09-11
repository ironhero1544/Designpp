#ifndef DESIGNPP_RUNTIME_PROCESS_RUNNER_H_
#define DESIGNPP_RUNTIME_PROCESS_RUNNER_H_

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace designpp::runtime {

// Describes a child process without shell-specific quoting.
struct ProcessRequest {
  std::filesystem::path executable;
  std::vector<std::wstring> arguments;
  std::optional<std::filesystem::path> working_directory;
  bool interactive_input = false;
};

enum class InputWriteResult {
  kAccepted,
  kNotInteractive,
  kClosed,
  kQueueFull,
};

// Contains the final state and combined raw output of a child process.
struct ProcessResult {
  std::uint32_t exit_code{0};
  bool started{false};
  bool cancelled{false};
  std::string output;
  std::wstring error_message;
};

using OutputCallback = std::function<void(std::string)>;
using CompletionCallback = std::function<void(ProcessResult)>;

// Returns bytes ending on a complete UTF-8 code-point boundary. An incomplete
// trailing sequence is retained in `remainder` and prepended to the next call.
// This does not normalize or replace malformed complete input.
[[nodiscard]] std::string TakeCompleteUtf8Chunk(std::string_view bytes,
                                                std::string* remainder);

// Owns one asynchronous Windows process and its cancellation job object.
class ProcessSession final {
 public:
  ProcessSession() = default;
  ProcessSession(const ProcessSession&) = delete;
  ProcessSession& operator=(const ProcessSession&) = delete;
  ProcessSession(ProcessSession&& other) noexcept;
  ProcessSession& operator=(ProcessSession&& other) noexcept;
  ~ProcessSession();

  // Terminates the complete child process tree.
  void Cancel() noexcept;

  // Returns true while the worker still owns an active run.
  [[nodiscard]] bool IsRunning() const noexcept;

  // Returns true when this session owns a process implementation.
  [[nodiscard]] bool IsValid() const noexcept;

  // Queues UTF-8 bytes for an interactive child without blocking the caller.
  [[nodiscard]] InputWriteResult WriteInput(std::string bytes);

 private:
  struct Implementation;

  explicit ProcessSession(std::shared_ptr<Implementation> implementation);
  void StopAndJoin() noexcept;

  std::shared_ptr<Implementation> implementation_;

  friend class ProcessRunner;
};

// Contains either a valid process session or a launch validation error.
struct ProcessLaunchResult {
  ProcessSession session;
  std::wstring error_message;

  // Returns true when a process worker was created.
  [[nodiscard]] bool IsValid() const noexcept { return session.IsValid(); }
};

// Starts external tools with redirected output and process-tree cancellation.
class ProcessRunner final {
 public:
  // Starts a process and invokes callbacks on the worker thread.
  [[nodiscard]] static ProcessLaunchResult RunAsync(
      ProcessRequest request, OutputCallback on_output,
      CompletionCallback on_complete);

  // Starts only the requested child process with UAC elevation. No GUI is
  // relaunched and elevated stdout/stderr is not redirected.
  [[nodiscard]] static ProcessLaunchResult RunElevatedAsync(
      ProcessRequest request, CompletionCallback on_complete);
};

}  // namespace designpp::runtime

#endif  // DESIGNPP_RUNTIME_PROCESS_RUNNER_H_

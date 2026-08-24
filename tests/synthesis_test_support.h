// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_TESTS_SYNTHESIS_TEST_SUPPORT_H_
#define DESIGNPP_TESTS_SYNTHESIS_TEST_SUPPORT_H_

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "designpp/application/synthesis_run_service.h"
#include "designpp/runtime/execution_provider.h"

namespace designpp::tests {

class TemporarySynthesisWorkspace final {
 public:
  TemporarySynthesisWorkspace() {
    static std::atomic_uint64_t sequence = 0;
    root_ =
        std::filesystem::temp_directory_path() /
        (L"designpp-synthesis-test-" + std::to_wstring(GetCurrentProcessId()) +
         L"-" + std::to_wstring(++sequence));
    library_ = root_ / L"Library 한글";
    cell_ = library_ / L"cells" / L"cell";
    source_ = cell_ / L"top.sv";
    std::filesystem::create_directories(cell_);
    std::ofstream(source_, std::ios::binary)
        << "module top(input a, input b, output y); assign y = a ^ b; "
           "endmodule\n";
  }

  TemporarySynthesisWorkspace(const TemporarySynthesisWorkspace&) = delete;
  TemporarySynthesisWorkspace& operator=(const TemporarySynthesisWorkspace&) =
      delete;

  ~TemporarySynthesisWorkspace() {
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }

  [[nodiscard]] application::SynthesisRunRequest Request(
      std::uint64_t generation = 41) const {
    application::SynthesisRunRequest request;
    request.project.id = "project";
    request.project.library_id = "library";
    request.project.cell_id = "cell";
    request.project.name = "top";
    request.project.top_module = "top";
    request.project.cpu_budget = 1;
    request.library_directory = library_;
    request.cell_directory = cell_;
    request.generation = generation;
    application::ResolvedSource source;
    source.view_kind = core::ViewKind::kVerilog;
    source.windows_path = source_;
    source.library_directory = library_;
    source.relative_path = "cells/cell/top.sv";
    source.enabled = true;
    source.exists = true;
    request.sources.push_back(std::move(source));
    return request;
  }

  [[nodiscard]] const std::filesystem::path& cell() const { return cell_; }

 private:
  std::filesystem::path root_;
  std::filesystem::path library_;
  std::filesystem::path cell_;
  std::filesystem::path source_;
};

struct FakeExecutionState {
  std::atomic_bool cancelled = false;
  std::atomic_bool completion_callback_active = false;
  std::atomic_bool destroyed_during_completion_callback = false;
};

class ControlledExecutionHandle final : public runtime::ExecutionHandle {
 public:
  explicit ControlledExecutionHandle(std::shared_ptr<FakeExecutionState> state)
      : state_(std::move(state)) {}

  ~ControlledExecutionHandle() override {
    if (state_->completion_callback_active.load()) {
      state_->destroyed_during_completion_callback = true;
    }
  }

  void Cancel() noexcept override { state_->cancelled = true; }
  [[nodiscard]] bool IsRunning() const noexcept override {
    return !state_->cancelled;
  }
  [[nodiscard]] runtime::InputWriteResult WriteInput(std::string) override {
    return runtime::InputWriteResult::kNotInteractive;
  }

 private:
  std::shared_ptr<FakeExecutionState> state_;
};

class ControlledExecutionProvider final : public runtime::ExecutionProvider {
 public:
  struct PendingExecution {
    runtime::WslCommand command;
    runtime::ExecutionOutputCallback output;
    runtime::ExecutionCompletionCallback complete;
    std::shared_ptr<FakeExecutionState> state;
  };

  [[nodiscard]] runtime::ExecutionStartResult Start(
      const runtime::WslCommand& command,
      runtime::ExecutionOutputCallback output,
      runtime::ExecutionCompletionCallback complete) override {
    std::scoped_lock lock(mutex_);
    if (fail_next_start_) {
      fail_next_start_ = false;
      return {nullptr,
              {core::ErrorCode::kIoError, "Controlled start failure", 5}};
    }
    auto state = std::make_shared<FakeExecutionState>();
    pending_.push_back(
        {command, std::move(output), std::move(complete), state});
    changed_.notify_all();
    return {std::make_unique<ControlledExecutionHandle>(state),
            core::Status::Success()};
  }

  void FailNextStart() {
    std::scoped_lock lock(mutex_);
    fail_next_start_ = true;
  }

  [[nodiscard]] bool WaitForStarts(std::size_t count) {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, std::chrono::seconds(5),
                             [&] { return pending_.size() >= count; });
  }

  [[nodiscard]] std::size_t StartCount() const {
    std::scoped_lock lock(mutex_);
    return pending_.size();
  }

  [[nodiscard]] runtime::WslCommand Command(std::size_t index) const {
    std::scoped_lock lock(mutex_);
    return pending_.at(index).command;
  }

  [[nodiscard]] bool Cancelled(std::size_t index) const {
    std::scoped_lock lock(mutex_);
    return pending_.at(index).state->cancelled;
  }

  [[nodiscard]] bool DestroyedDuringCompletionCallback(
      std::size_t index) const {
    std::scoped_lock lock(mutex_);
    return pending_.at(index).state->destroyed_during_completion_callback;
  }

  void Output(std::size_t index, std::string output) {
    runtime::ExecutionOutputCallback callback;
    {
      std::scoped_lock lock(mutex_);
      callback = pending_.at(index).output;
    }
    callback(std::move(output));
  }

  void Complete(std::size_t index, runtime::ProcessResult result,
                int repetitions = 1) {
    runtime::ExecutionCompletionCallback callback;
    {
      std::scoped_lock lock(mutex_);
      callback = pending_.at(index).complete;
    }
    for (int repetition = 0; repetition < repetitions; ++repetition) {
      std::shared_ptr<FakeExecutionState> state;
      {
        std::scoped_lock lock(mutex_);
        state = pending_.at(index).state;
      }
      state->completion_callback_active = true;
      callback(result);
      state->completion_callback_active = false;
    }
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::vector<PendingExecution> pending_;
  bool fail_next_start_ = false;
};

inline std::filesystem::path WindowsWorkingDirectory(
    const runtime::WslCommand& command) {
  std::wstring path = command.working_directory.value_or(L"");
  if (path.size() >= 7 && path.starts_with(L"/mnt/") && path[6] == L'/') {
    const wchar_t drive = static_cast<wchar_t>(std::towupper(path[5]));
    std::wstring windows{drive, L':'};
    windows.append(path.substr(6));
    std::replace(windows.begin(), windows.end(), L'/', L'\\');
    return windows;
  }
  return {};
}

struct ArtifactOptions {
  std::wstring omitted_file;
  std::wstring empty_file;
  bool malformed_statistics = false;
  bool malformed_structural = false;
};

inline void WriteSynthesisArtifacts(const runtime::WslCommand& command,
                                    const ArtifactOptions& options = {}) {
  const std::filesystem::path directory = WindowsWorkingDirectory(command);
  const auto write = [&](const wchar_t* name, std::string_view contents) {
    if (options.omitted_file == name) return;
    std::ofstream(directory / name, std::ios::binary | std::ios::trunc)
        << (options.empty_file == name ? std::string_view{} : contents);
  };
  write(
      L"netlist.json",
      R"json({"modules":{"top":{"ports":{"a":{"direction":"input","bits":[2]},"b":{"direction":"input","bits":[3]},"y":{"direction":"output","bits":[4]}},"cells":{"xor":{"type":"$_XOR_","port_directions":{"A":"input","B":"input","Y":"output"},"connections":{"A":[2],"B":[3],"Y":[4]}}}}}})json");
  write(
      L"netlist.v",
      "module top(input a, input b, output y); assign y = a ^ b; endmodule\n");
  write(L"statistics.json",
        options.malformed_statistics
            ? "{malformed"
            : R"json({"modules":{"top":{"num_cells":1,"area":4.5}}})json");
  write(L"synthesis.rpt", "Number of cells: 1\nChip area: 4.5\n");
  write(
      L"schematic-structural.json",
      options.malformed_structural
          ? "{malformed"
          : R"json({"modules":{"top":{"ports":{"a":{"direction":"input","bits":[2]},"y":{"direction":"output","bits":[4]}},"cells":{}}}})json");
}

inline runtime::ProcessResult SuccessfulProcess(std::string output = {}) {
  runtime::ProcessResult result;
  result.started = true;
  result.exit_code = 0;
  result.output = std::move(output);
  return result;
}

}  // namespace designpp::tests

#endif  // DESIGNPP_TESTS_SYNTHESIS_TEST_SUPPORT_H_

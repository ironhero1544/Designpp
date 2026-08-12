// Copyright 2026 The Design++ Authors

#ifndef DESIGNPP_RUNTIME_TASK_SCHEDULER_H_
#define DESIGNPP_RUNTIME_TASK_SCHEDULER_H_

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

namespace designpp::runtime {

class TaskScheduler final {
 public:
  using Task = std::function<void(std::stop_token)>;

  explicit TaskScheduler(std::size_t worker_count = 0);
  TaskScheduler(const TaskScheduler&) = delete;
  TaskScheduler& operator=(const TaskScheduler&) = delete;
  ~TaskScheduler();

  [[nodiscard]] bool Submit(Task task);
  void RequestStop();

 private:
  void WorkerMain(std::stop_token stop_token);

  static constexpr std::size_t kMaximumQueuedTasks = 128;
  std::mutex mutex_;
  std::condition_variable_any condition_;
  std::deque<Task> tasks_;
  std::vector<std::jthread> workers_;
  bool stopping_ = false;
};

}  // namespace designpp::runtime

#endif  // DESIGNPP_RUNTIME_TASK_SCHEDULER_H_

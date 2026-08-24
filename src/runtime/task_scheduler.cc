// Copyright 2026 The Design++ Authors

#include "designpp/runtime/task_scheduler.h"

#include <algorithm>

namespace designpp::runtime {

TaskScheduler::TaskScheduler(std::size_t worker_count) {
  if (worker_count == 0) {
    const unsigned int logical_cpus = std::thread::hardware_concurrency();
    worker_count = logical_cpus > 1 ? logical_cpus - 1 : 1;
  }
  worker_count = std::clamp<std::size_t>(worker_count, 1, 4);
  workers_.reserve(worker_count);
  for (std::size_t index = 0; index < worker_count; ++index) {
    workers_.emplace_back(
        [this](std::stop_token stop_token) { WorkerMain(stop_token); });
  }
}

TaskScheduler::~TaskScheduler() { RequestStop(); }

bool TaskScheduler::Submit(Task task) {
  {
    std::scoped_lock lock(mutex_);
    if (stopping_ || tasks_.size() >= kMaximumQueuedTasks) {
      return false;
    }
    tasks_.push_back(std::move(task));
  }
  condition_.notify_one();
  return true;
}

void TaskScheduler::RequestStop() {
  {
    std::scoped_lock lock(mutex_);
    if (stopping_) return;
    stopping_ = true;
    tasks_.clear();
  }
  for (std::jthread& worker : workers_) worker.request_stop();
  condition_.notify_all();
  workers_.clear();
}

void TaskScheduler::WorkerMain(std::stop_token stop_token) {
  while (!stop_token.stop_requested()) {
    Task task;
    {
      std::unique_lock lock(mutex_);
      condition_.wait(lock, stop_token,
                      [this] { return stopping_ || !tasks_.empty(); });
      if (stopping_ || stop_token.stop_requested()) return;
      task = std::move(tasks_.front());
      tasks_.pop_front();
    }
    task(stop_token);
  }
}

}  // namespace designpp::runtime

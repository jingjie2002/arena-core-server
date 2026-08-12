#include "arena_core/storage/storage_executor.hpp"

#include <stdexcept>
#include <utility>

namespace arena_core::storage {

StorageExecutor::StorageExecutor(const std::size_t thread_count, const std::size_t queue_capacity)
    : queue_capacity_(queue_capacity) {
  if (thread_count == 0U || queue_capacity == 0U) {
    throw std::invalid_argument("StorageExecutor requires non-zero limits");
  }
  workers_.reserve(thread_count);
  for (std::size_t index = 0; index < thread_count; ++index) {
    workers_.emplace_back([this] { worker_loop(); });
  }
}

StorageExecutor::~StorageExecutor() { stop(); }

bool StorageExecutor::try_submit(Task task) {
  if (!task) {
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!accepting_ || tasks_.size() >= queue_capacity_) {
      return false;
    }
    tasks_.push_back(std::move(task));
  }
  ready_.notify_one();
  return true;
}

void StorageExecutor::stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) {
      return;
    }
    accepting_ = false;
    stopping_ = true;
  }
  ready_.notify_all();
  for (auto& worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  workers_.clear();
}

std::size_t StorageExecutor::pending() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return tasks_.size();
}

std::size_t StorageExecutor::in_flight() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return in_flight_;
}

void StorageExecutor::worker_loop() {
  for (;;) {
    Task task;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      ready_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
      if (tasks_.empty()) {
        if (stopping_) {
          return;
        }
        continue;
      }
      task = std::move(tasks_.front());
      tasks_.pop_front();
      ++in_flight_;
    }
    try {
      task();
    } catch (...) {
      // Individual services convert failures to value errors. Keep the executor alive if a task violates that rule.
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      --in_flight_;
    }
  }
}

}  // namespace arena_core::storage

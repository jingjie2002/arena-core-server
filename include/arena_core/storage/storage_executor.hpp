#pragma once

#include "arena_core/core/cpp17_guard.hpp"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace arena_core::storage {

class StorageExecutor final {
 public:
  using Task = std::function<void()>;

  StorageExecutor(std::size_t thread_count, std::size_t queue_capacity);
  ~StorageExecutor();

  StorageExecutor(const StorageExecutor&) = delete;
  StorageExecutor& operator=(const StorageExecutor&) = delete;

  bool try_submit(Task task);
  void stop();
  std::size_t pending() const;
  std::size_t in_flight() const;

 private:
  void worker_loop();

  const std::size_t queue_capacity_;
  mutable std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<Task> tasks_;
  std::vector<std::thread> workers_;
  std::size_t in_flight_{0};
  bool accepting_{true};
  bool stopping_{false};
};

}  // namespace arena_core::storage

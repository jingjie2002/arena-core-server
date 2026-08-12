#include "arena_core/storage/storage_executor.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <memory>

#include <gtest/gtest.h>

namespace arena_core::storage {
namespace {

using namespace std::chrono_literals;

TEST(StorageExecutorTest, RejectsWorkWhenBoundedQueueIsFullAndDrainsOnStop) {
  StorageExecutor executor(1, 1);
  std::promise<void> release;
  auto gate = release.get_future().share();
  std::promise<void> started;
  std::atomic<int> completed{0};
  ASSERT_TRUE(executor.try_submit([&] {
    started.set_value();
    gate.wait();
    ++completed;
  }));
  ASSERT_EQ(started.get_future().wait_for(1s), std::future_status::ready);
  ASSERT_TRUE(executor.try_submit([&] { ++completed; }));
  EXPECT_FALSE(executor.try_submit([&] { ++completed; }));
  release.set_value();
  executor.stop();
  EXPECT_EQ(completed.load(), 2);
  EXPECT_FALSE(executor.try_submit([] {}));
}

}  // namespace
}  // namespace arena_core::storage

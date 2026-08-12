#pragma once

#include "arena_core/core/cpp17_guard.hpp"

#include <chrono>

namespace arena_core::domain {

using MonotonicClock = std::chrono::steady_clock;
using TimePoint = MonotonicClock::time_point;

class Clock {
 public:
  virtual ~Clock() = default;
  virtual TimePoint now() const noexcept = 0;
};

class SteadyClock final : public Clock {
 public:
  TimePoint now() const noexcept override { return MonotonicClock::now(); }
};

class FakeClock final : public Clock {
 public:
  explicit FakeClock(TimePoint initial = TimePoint{}) noexcept : current_(initial) {}

  TimePoint now() const noexcept override { return current_; }
  void set(TimePoint value) noexcept { current_ = value; }

  template <typename Rep, typename Period>
  void advance(std::chrono::duration<Rep, Period> duration) noexcept {
    current_ += std::chrono::duration_cast<MonotonicClock::duration>(duration);
  }

 private:
  TimePoint current_;
};

}  // namespace arena_core::domain

#pragma once

#include "arena_core/core/cpp17_guard.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

namespace arena_core::domain {

bool is_valid_player_id(std::string_view player_id) noexcept;
bool is_valid_resource_id(std::string_view resource_id) noexcept;

class IdGenerator {
 public:
  virtual ~IdGenerator() = default;
  virtual std::string next(std::string_view prefix) = 0;
};

class SequentialIdGenerator final : public IdGenerator {
 public:
  explicit SequentialIdGenerator(std::uint64_t initial = 0) noexcept : sequence_(initial) {}
  std::string next(std::string_view prefix) override;

 private:
  std::atomic<std::uint64_t> sequence_;
};

}  // namespace arena_core::domain

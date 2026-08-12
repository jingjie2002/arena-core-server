#pragma once

#include "arena_core/core/cpp17_guard.hpp"

#include <cstdint>
#include <mutex>
#include <random>
#include <string>

namespace arena_core::session {

class TokenGenerator {
 public:
  virtual ~TokenGenerator() = default;
  virtual std::string next_token() = 0;
};

class RandomTokenGenerator final : public TokenGenerator {
 public:
  RandomTokenGenerator();
  std::string next_token() override;

 private:
  std::mutex mutex_;
  std::random_device source_;
};

class DeterministicTokenGenerator final : public TokenGenerator {
 public:
  explicit DeterministicTokenGenerator(std::uint64_t initial = 0) noexcept : next_(initial) {}
  std::string next_token() override;

 private:
  std::uint64_t next_;
};

}  // namespace arena_core::session

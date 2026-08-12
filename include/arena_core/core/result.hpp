#pragma once

#include "arena_core/core/cpp17_guard.hpp"
#include "arena_core/core/error.hpp"

#include <optional>
#include <utility>

namespace arena_core {

template <typename T>
struct Result {
  std::optional<T> value;
  Error error;

  bool ok() const noexcept { return value.has_value() && !error; }

  static Result success(T result) { return {std::move(result), {}}; }
  static Result failure(Error failure) { return {std::nullopt, std::move(failure)}; }
};

}  // namespace arena_core

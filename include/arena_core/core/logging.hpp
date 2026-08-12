#pragma once

#include "arena_core/core/config.hpp"
#include "arena_core/core/cpp17_guard.hpp"

#include <string>

namespace arena_core {

Error initialize_logging(const LoggingConfig& config, const std::string& service_name);

}  // namespace arena_core

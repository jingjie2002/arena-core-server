#include "arena_core/core/logging.hpp"

#include <filesystem>
#include <memory>
#include <vector>

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace arena_core {

Error initialize_logging(const LoggingConfig& config, const std::string& service_name) {
  try {
    std::vector<spdlog::sink_ptr> sinks;
    sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());

    if (!config.file.empty()) {
      const std::filesystem::path log_path(config.file);
      if (log_path.has_parent_path()) {
        std::filesystem::create_directories(log_path.parent_path());
      }
      sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(config.file, true));
    }

    auto logger = std::make_shared<spdlog::logger>(service_name, sinks.begin(), sinks.end());
    logger->set_level(spdlog::level::from_str(config.level));
    logger->set_pattern("%Y-%m-%dT%H:%M:%S.%e%z level=%l service=" + service_name + " message=%v");
    logger->flush_on(spdlog::level::warn);
    spdlog::set_default_logger(std::move(logger));
    return {};
  } catch (const std::exception&) {
    return make_error(ErrorCode::kConfigValidationFailed, "logging initialization failed");
  }
}

}  // namespace arena_core

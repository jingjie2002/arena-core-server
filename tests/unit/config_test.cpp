#include "arena_core/core/config.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

namespace arena_core {
namespace {

class TemporaryConfig {
 public:
  explicit TemporaryConfig(const std::string& contents) {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() / ("arena-core-config-" + std::to_string(suffix) + ".json");
    std::ofstream output(path_);
    output << contents;
  }

  ~TemporaryConfig() {
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
  }

  const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

TEST(ConfigTest, LoadsValidConfigurationAndDefaultsMissingFields) {
  TemporaryConfig file(R"({
    "environment": "test",
    "network": {"tcp_port": 9200, "observer_port": 8200},
    "execution": {"io_threads": 3},
    "logging": {"level": "debug"}
  })");

  const auto result = load_app_config(file.path());
  ASSERT_TRUE(result.ok()) << result.error.message;
  EXPECT_EQ(result.value->environment, "test");
  EXPECT_EQ(result.value->network.tcp_port, 9200);
  EXPECT_EQ(result.value->network.max_frame_bytes, 65'536U);
  EXPECT_EQ(result.value->execution.io_threads, 3U);
  EXPECT_EQ(result.value->execution.storage_threads, 4U);
  EXPECT_EQ(result.value->logging.level, "debug");
  EXPECT_FALSE(result.value->data.enabled);
  EXPECT_TRUE(result.value->observer.enabled);
  EXPECT_FALSE(result.value->observer.demo_mode);
}

TEST(ConfigTest, LoadsDataConnectionNamesWithoutEmbeddingPasswords) {
  TemporaryConfig file(R"({
    "data": {
      "enabled": true,
      "mysql": {"uri": "tcp://db:3306", "user": "arena", "password_env": "MYSQL_SECRET", "schema": "arena"},
      "redis": {"host": "cache", "port": 6380, "password_env": "REDIS_SECRET", "key_prefix": "arena:test:"},
      "rules": {"match_script": "rules/match_rule.lua", "score_script": "rules/score_rule.lua"}
    }
  })");
  const auto result = load_app_config(file.path());
  ASSERT_TRUE(result.ok()) << result.error.message;
  EXPECT_TRUE(result.value->data.enabled);
  EXPECT_EQ(result.value->data.mysql.password_env, "MYSQL_SECRET");
  EXPECT_EQ(result.value->data.redis.password_env, "REDIS_SECRET");
  EXPECT_EQ(result.value->data.redis.port, 6380);
}

TEST(ConfigTest, RejectsInvalidJsonWithoutLeakingParserDetails) {
  TemporaryConfig file("{not-json");
  const auto result = load_app_config(file.path());
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error.code, ErrorCode::kConfigParseFailed);
  EXPECT_EQ(result.error.message, "configuration JSON is invalid");
}

TEST(ConfigTest, RejectsConflictingPorts) {
  AppConfig config;
  config.network.tcp_port = 9000;
  config.network.observer_port = 9000;
  const auto error = validate_app_config(config);
  EXPECT_EQ(error.code, ErrorCode::kConfigValidationFailed);
}

TEST(ConfigTest, RejectsDemoModeOnNonLoopbackAddress) {
  AppConfig config;
  config.network.bind_address = "0.0.0.0";
  config.observer.demo_mode = true;
  const auto error = validate_app_config(config);
  EXPECT_EQ(error.code, ErrorCode::kConfigValidationFailed);
}

TEST(ConfigTest, ReportsMissingFile) {
  const auto result = load_app_config(std::filesystem::temp_directory_path() / "arena-core-does-not-exist.json");
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error.code, ErrorCode::kConfigFileNotFound);
}

}  // namespace
}  // namespace arena_core

#include "arena_core/qa/lab_app_config.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>

namespace arena_core::qa {
namespace {

TEST(LabAppConfigTest, ProtectsNonLoopbackListener) {
  LabAppConfig config;
  config.http.bind_address = "0.0.0.0";
  EXPECT_EQ(validate_lab_app_config(config).code, ErrorCode::kConfigValidationFailed);
  config.http.allow_non_loopback = true;
  EXPECT_FALSE(validate_lab_app_config(config));
}

TEST(LabAppConfigTest, RejectsUnsafeProxyEndpoint) {
  LabAppConfig config;
  config.toxiproxy.enabled = true;
  config.toxiproxy.listen = "../redis:6379";
  EXPECT_EQ(validate_lab_app_config(config).code, ErrorCode::kConfigValidationFailed);
}

TEST(LabAppConfigTest, LoadsConfigWithoutCredentialValue) {
  const auto path = std::filesystem::temp_directory_path() / "arena-lab-config-test.json";
  {
    std::ofstream output(path);
    output << R"({
      "environment":"test",
      "http":{"bind_address":"127.0.0.1","port":18081,"static_root":"web/lab"},
      "reports":{"directory":"lab-runs"},
      "trusted_result":{"service_token_env":"ARENA_LAB_TEST_TOKEN"},
      "toxiproxy":{"enabled":true,"host":"toxiproxy","port":8474,
                    "proxy_name":"redis","listen":"0.0.0.0:26379","upstream":"redis:6379"}
    })";
  }
  const auto loaded = load_lab_app_config(path);
  std::filesystem::remove(path);
  ASSERT_TRUE(loaded.ok()) << loaded.error.message;
  EXPECT_EQ(loaded.value->trusted_result.service_token_env, "ARENA_LAB_TEST_TOKEN");
  EXPECT_TRUE(loaded.value->toxiproxy.enabled);
}

}  // namespace
}  // namespace arena_core::qa

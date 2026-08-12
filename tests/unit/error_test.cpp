#include "arena_core/core/error.hpp"

#include <gtest/gtest.h>

namespace arena_core {
namespace {

TEST(ErrorCatalogTest, ProtocolConflictIsFatalAndNotRetryable) {
  const auto error = make_error(ErrorCode::kProtocolRequestIdConflict);
  EXPECT_EQ(error_code_name(error.code), "PROTOCOL_REQUEST_ID_CONFLICT");
  EXPECT_FALSE(error.retryable);
  EXPECT_TRUE(error.close_connection);
}

TEST(ErrorCatalogTest, DependencyFailuresAreRetryableAndKeepConnection) {
  const auto error = make_error(ErrorCode::kDependencyMysqlUnavailable, "database unavailable");
  EXPECT_EQ(error.message, "database unavailable");
  EXPECT_TRUE(error.retryable);
  EXPECT_FALSE(error.close_connection);
}

TEST(ErrorCatalogTest, SuccessHasNoFailureFlags) {
  const auto error = make_error(ErrorCode::kOk);
  EXPECT_FALSE(error);
  EXPECT_FALSE(error.retryable);
  EXPECT_FALSE(error.close_connection);
}

}  // namespace
}  // namespace arena_core

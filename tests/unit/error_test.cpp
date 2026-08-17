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

TEST(ErrorCatalogTest, LabRunErrorsHaveStablePublicNames) {
  EXPECT_EQ(error_code_name(ErrorCode::kQaScenarioInvalid), "QA_SCENARIO_INVALID");
  EXPECT_EQ(error_code_name(ErrorCode::kQaRunAlreadyActive), "QA_RUN_ALREADY_ACTIVE");
  EXPECT_EQ(error_code_name(ErrorCode::kQaRunNotFound), "QA_RUN_NOT_FOUND");
  EXPECT_EQ(error_code_name(ErrorCode::kQaRunNotCancellable), "QA_RUN_NOT_CANCELLABLE");
  EXPECT_EQ(error_code_name(ErrorCode::kQaRunCancelled), "QA_RUN_CANCELLED");
  EXPECT_EQ(error_code_name(ErrorCode::kQaRunTimedOut), "QA_RUN_TIMED_OUT");
  EXPECT_EQ(error_code_name(ErrorCode::kQaCleanupFailed), "QA_CLEANUP_FAILED");
}

}  // namespace
}  // namespace arena_core

#pragma once

#include "arena_core/core/cpp17_guard.hpp"

#include <string>
#include <string_view>

namespace arena_core {

enum class ErrorCode {
  kOk = 0,
  kConfigFileNotFound,
  kConfigParseFailed,
  kConfigValidationFailed,
  kProtocolUnsupportedVersion,
  kProtocolUnknownMessage,
  kProtocolFrameTooLarge,
  kProtocolDecodeFailed,
  kProtocolRequestIdConflict,
  kSessionHelloRequired,
  kSessionPlayerIdInvalid,
  kSessionTokenInvalid,
  kSessionReconnectExpired,
  kSessionNotActive,
  kMatchMmrInvalid,
  kMatchModeUnsupported,
  kMatchWaitInvalid,
  kMatchAlreadyQueued,
  kMatchTicketNotFound,
  kMatchTicketNotQueued,
  kMatchTicketExpired,
  kMatchCapacityUnavailable,
  kRoomNotFound,
  kRoomJoinTokenInvalid,
  kRoomPlayerNotAssigned,
  kRoomAlreadyStarted,
  kRoomNotJoined,
  kRoomInvalidState,
  kSettlementNotAllowed,
  kSettlementNotFound,
  kSettlementConflict,
  kSettlementAccessDenied,
  kSettlementPlayerSetInvalid,
  kSettlementStorageFailed,
  kRankPlayerNotFound,
  kRankQueryInvalid,
  kRankCacheUnavailable,
  kRuleLoadFailed,
  kRuleExecutionFailed,
  kRuleVersionMismatch,
  kDependencyRedisUnavailable,
  kDependencyMysqlUnavailable,
  kQaScenarioInvalid,
  kQaRunAlreadyActive,
  kQaRunNotFound,
  kQaRunNotCancellable,
  kQaRunCancelled,
  kQaRunTimedOut,
  kQaCleanupFailed,
  kServerBusy,
  kServerShuttingDown,
  kInternalError,
};

struct Error {
  ErrorCode code{ErrorCode::kOk};
  std::string message;
  bool retryable{false};
  bool close_connection{false};

  explicit operator bool() const noexcept { return code != ErrorCode::kOk; }
};

std::string_view error_code_name(ErrorCode code) noexcept;
bool error_is_retryable(ErrorCode code) noexcept;
bool error_closes_connection(ErrorCode code) noexcept;
Error make_error(ErrorCode code, std::string message = {});

}  // namespace arena_core

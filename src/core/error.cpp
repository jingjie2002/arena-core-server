#include "arena_core/core/error.hpp"

#include <utility>

namespace arena_core {

std::string_view error_code_name(const ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::kOk: return "OK";
    case ErrorCode::kConfigFileNotFound: return "CONFIG_FILE_NOT_FOUND";
    case ErrorCode::kConfigParseFailed: return "CONFIG_PARSE_FAILED";
    case ErrorCode::kConfigValidationFailed: return "CONFIG_VALIDATION_FAILED";
    case ErrorCode::kProtocolUnsupportedVersion: return "PROTOCOL_UNSUPPORTED_VERSION";
    case ErrorCode::kProtocolUnknownMessage: return "PROTOCOL_UNKNOWN_MESSAGE";
    case ErrorCode::kProtocolFrameTooLarge: return "PROTOCOL_FRAME_TOO_LARGE";
    case ErrorCode::kProtocolDecodeFailed: return "PROTOCOL_DECODE_FAILED";
    case ErrorCode::kProtocolRequestIdConflict: return "PROTOCOL_REQUEST_ID_CONFLICT";
    case ErrorCode::kSessionHelloRequired: return "SESSION_HELLO_REQUIRED";
    case ErrorCode::kSessionPlayerIdInvalid: return "SESSION_PLAYER_ID_INVALID";
    case ErrorCode::kSessionTokenInvalid: return "SESSION_TOKEN_INVALID";
    case ErrorCode::kSessionReconnectExpired: return "SESSION_RECONNECT_EXPIRED";
    case ErrorCode::kSessionNotActive: return "SESSION_NOT_ACTIVE";
    case ErrorCode::kMatchMmrInvalid: return "MATCH_MMR_INVALID";
    case ErrorCode::kMatchModeUnsupported: return "MATCH_MODE_UNSUPPORTED";
    case ErrorCode::kMatchWaitInvalid: return "MATCH_WAIT_INVALID";
    case ErrorCode::kMatchAlreadyQueued: return "MATCH_ALREADY_QUEUED";
    case ErrorCode::kMatchTicketNotFound: return "MATCH_TICKET_NOT_FOUND";
    case ErrorCode::kMatchTicketNotQueued: return "MATCH_TICKET_NOT_QUEUED";
    case ErrorCode::kMatchTicketExpired: return "MATCH_TICKET_EXPIRED";
    case ErrorCode::kMatchCapacityUnavailable: return "MATCH_CAPACITY_UNAVAILABLE";
    case ErrorCode::kRoomNotFound: return "ROOM_NOT_FOUND";
    case ErrorCode::kRoomJoinTokenInvalid: return "ROOM_JOIN_TOKEN_INVALID";
    case ErrorCode::kRoomPlayerNotAssigned: return "ROOM_PLAYER_NOT_ASSIGNED";
    case ErrorCode::kRoomAlreadyStarted: return "ROOM_ALREADY_STARTED";
    case ErrorCode::kRoomNotJoined: return "ROOM_NOT_JOINED";
    case ErrorCode::kRoomInvalidState: return "ROOM_INVALID_STATE";
    case ErrorCode::kSettlementNotAllowed: return "SETTLEMENT_NOT_ALLOWED";
    case ErrorCode::kSettlementNotFound: return "SETTLEMENT_NOT_FOUND";
    case ErrorCode::kSettlementConflict: return "SETTLEMENT_CONFLICT";
    case ErrorCode::kSettlementAccessDenied: return "SETTLEMENT_ACCESS_DENIED";
    case ErrorCode::kSettlementPlayerSetInvalid: return "SETTLEMENT_PLAYER_SET_INVALID";
    case ErrorCode::kSettlementStorageFailed: return "SETTLEMENT_STORAGE_FAILED";
    case ErrorCode::kRankPlayerNotFound: return "RANK_PLAYER_NOT_FOUND";
    case ErrorCode::kRankQueryInvalid: return "RANK_QUERY_INVALID";
    case ErrorCode::kRankCacheUnavailable: return "RANK_CACHE_UNAVAILABLE";
    case ErrorCode::kRuleLoadFailed: return "RULE_LOAD_FAILED";
    case ErrorCode::kRuleExecutionFailed: return "RULE_EXECUTION_FAILED";
    case ErrorCode::kRuleVersionMismatch: return "RULE_VERSION_MISMATCH";
    case ErrorCode::kDependencyRedisUnavailable: return "DEPENDENCY_REDIS_UNAVAILABLE";
    case ErrorCode::kDependencyMysqlUnavailable: return "DEPENDENCY_MYSQL_UNAVAILABLE";
    case ErrorCode::kServerBusy: return "SERVER_BUSY";
    case ErrorCode::kServerShuttingDown: return "SERVER_SHUTTING_DOWN";
    case ErrorCode::kInternalError: return "INTERNAL_ERROR";
  }
  return "INTERNAL_ERROR";
}

bool error_is_retryable(const ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::kSessionReconnectExpired:
    case ErrorCode::kSessionNotActive:
    case ErrorCode::kMatchTicketExpired:
    case ErrorCode::kMatchCapacityUnavailable:
    case ErrorCode::kSettlementNotFound:
    case ErrorCode::kSettlementStorageFailed:
    case ErrorCode::kRankCacheUnavailable:
    case ErrorCode::kRuleExecutionFailed:
    case ErrorCode::kDependencyRedisUnavailable:
    case ErrorCode::kDependencyMysqlUnavailable:
    case ErrorCode::kServerBusy:
    case ErrorCode::kServerShuttingDown:
    case ErrorCode::kInternalError:
      return true;
    default:
      return false;
  }
}

bool error_closes_connection(const ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::kProtocolUnsupportedVersion:
    case ErrorCode::kProtocolUnknownMessage:
    case ErrorCode::kProtocolFrameTooLarge:
    case ErrorCode::kProtocolDecodeFailed:
    case ErrorCode::kProtocolRequestIdConflict:
    case ErrorCode::kSessionHelloRequired:
    case ErrorCode::kSessionTokenInvalid:
      return true;
    default:
      return false;
  }
}

Error make_error(const ErrorCode code, std::string message) {
  if (message.empty() && code != ErrorCode::kOk) {
    message = std::string(error_code_name(code));
  }
  return Error{code, std::move(message), error_is_retryable(code), error_closes_connection(code)};
}

}  // namespace arena_core

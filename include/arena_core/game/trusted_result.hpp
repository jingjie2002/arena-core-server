#pragma once

#include "arena_core/core/cpp17_guard.hpp"
#include "arena_core/core/result.hpp"
#include "arena_core/domain/room.hpp"
#include "arena_core/settlement/settlement_gateway.hpp"

#include <string>
#include <string_view>

#include "arena_core.pb.h"

namespace arena_core::game {

struct TrustedMatchResult {
  std::string result_id;
  std::string match_id;
  std::string room_id;
  std::string winner_id;
  std::string loser_id;
  std::string rule_version;
  std::string finished_at;
};

Result<domain::FrozenRoomResult> freeze_trusted_result(const TrustedMatchResult& result);
protocol::v1::MatchResultView make_match_result_view(const settlement::SettlementReceipt& receipt);
bool receipt_contains_player(const settlement::SettlementReceipt& receipt,
                             std::string_view player_id) noexcept;

}  // namespace arena_core::game

#include "arena_core/game/trusted_result.hpp"

#include "arena_core/domain/identifiers.hpp"

#include <cctype>
#include <cstdint>
#include <iomanip>
#include <sstream>

#include <boost/uuid/detail/sha1.hpp>

namespace arena_core::game {
namespace {

bool valid_finished_at(const std::string_view value) noexcept {
  if ((value.size() != 20U && (value.size() < 22U || value.size() > 30U)) ||
      value.back() != 'Z' ||
      value[4] != '-' || value[7] != '-' || value[10] != 'T' ||
      value[13] != ':' || value[16] != ':') {
    return false;
  }
  if (value.size() > 20U && value[19] != '.') {
    return false;
  }
  for (std::size_t index = 0; index + 1U < value.size(); ++index) {
    if (index == 4U || index == 7U || index == 10U || index == 13U || index == 16U) {
      continue;
    }
    if (index == 19U && value[index] == '.') {
      continue;
    }
    if (!std::isdigit(static_cast<unsigned char>(value[index]))) {
      return false;
    }
  }
  const auto pair = [value](const std::size_t offset) noexcept {
    return static_cast<unsigned>((value[offset] - '0') * 10 + (value[offset + 1U] - '0'));
  };
  const auto year = static_cast<unsigned>((value[0] - '0') * 1000 +
                                          (value[1] - '0') * 100 +
                                          (value[2] - '0') * 10 + value[3] - '0');
  const auto month = pair(5U);
  const auto day = pair(8U);
  const auto hour = pair(11U);
  const auto minute = pair(14U);
  const auto second = pair(17U);
  if (month == 0U || month > 12U || hour > 23U || minute > 59U || second > 59U) {
    return false;
  }
  constexpr unsigned kMonthDays[] = {0U, 31U, 28U, 31U, 30U, 31U, 30U,
                                      31U, 31U, 30U, 31U, 30U, 31U};
  auto maximum_day = kMonthDays[month];
  const bool leap = year % 4U == 0U && (year % 100U != 0U || year % 400U == 0U);
  if (month == 2U && leap) {
    maximum_day = 29U;
  }
  return day > 0U && day <= maximum_day;
}

std::string sha1_hex(const std::string_view value) {
  boost::uuids::detail::sha1 sha1;
  sha1.process_bytes(value.data(), value.size());
  unsigned int digest[5]{};
  sha1.get_digest(digest);
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (const auto word : digest) {
    output << std::setw(8) << word;
  }
  return output.str();
}

std::uint64_t stable_seed(const std::string_view value) noexcept {
  std::uint64_t hash = 14'695'981'039'346'656'037ULL;
  for (const unsigned char byte : value) {
    hash ^= byte;
    hash *= 1'099'511'628'211ULL;
  }
  return hash;
}

}  // namespace

Result<domain::FrozenRoomResult> freeze_trusted_result(const TrustedMatchResult& result) {
  if (!domain::is_valid_resource_id(result.result_id) || result.result_id.size() > 64U ||
      !domain::is_valid_resource_id(result.match_id) ||
      !domain::is_valid_resource_id(result.room_id)) {
    return Result<domain::FrozenRoomResult>::failure(
        make_error(ErrorCode::kSettlementPlayerSetInvalid, "result, match, or room id is invalid"));
  }
  if (!domain::is_valid_player_id(result.winner_id) ||
      !domain::is_valid_player_id(result.loser_id) || result.winner_id == result.loser_id) {
    return Result<domain::FrozenRoomResult>::failure(
        make_error(ErrorCode::kSettlementPlayerSetInvalid, "result roster is invalid"));
  }
  if (result.rule_version.empty() || result.rule_version.size() > 32U) {
    return Result<domain::FrozenRoomResult>::failure(
        make_error(ErrorCode::kRuleVersionMismatch, "result rule version is invalid"));
  }
  if (!valid_finished_at(result.finished_at)) {
    return Result<domain::FrozenRoomResult>::failure(
        make_error(ErrorCode::kSettlementPlayerSetInvalid, "finished_at must be UTC RFC3339"));
  }
  const auto canonical = result.result_id + "\n" + result.match_id + "\n" + result.room_id + "\n" +
                         result.winner_id + "\n" + result.loser_id + "\n" + result.rule_version + "\n" +
                         result.finished_at;
  return Result<domain::FrozenRoomResult>::success(
      {result.winner_id, result.loser_id, result.rule_version, stable_seed(canonical),
       sha1_hex(canonical), result.finished_at});
}

protocol::v1::MatchResultView make_match_result_view(const settlement::SettlementReceipt& receipt) {
  protocol::v1::MatchResultView payload;
  payload.set_result_id(receipt.settlement_id);
  payload.set_match_id(receipt.match_id);
  payload.set_room_id(receipt.room_id);
  payload.set_winner_id(receipt.winner_id);
  payload.set_loser_id(receipt.loser_id);
  payload.set_rule_version(receipt.rule_version);
  payload.set_finished_at(receipt.finished_at);
  payload.set_cache_state(receipt.cache_state);
  payload.set_idempotent(receipt.idempotent);
  for (const auto& player : receipt.players) {
    auto* item = payload.add_players();
    item->set_player_id(player.player_id);
    item->set_before_score(player.before_score);
    item->set_after_score(player.after_score);
    item->set_outcome(player.outcome);
  }
  return payload;
}

bool receipt_contains_player(const settlement::SettlementReceipt& receipt,
                             const std::string_view player_id) noexcept {
  for (const auto& player : receipt.players) {
    if (player.player_id == player_id) {
      return true;
    }
  }
  return false;
}

}  // namespace arena_core::game

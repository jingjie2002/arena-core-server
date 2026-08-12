#include "arena_core/storage/settlement_repository.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

#include <cppconn/connection.h>
#include <cppconn/exception.h>
#include <cppconn/prepared_statement.h>
#include <cppconn/resultset.h>
#include <cppconn/statement.h>
#include <mysql_connection.h>
#include <mysql_driver.h>

namespace arena_core::storage {
namespace {

using Connection = std::unique_ptr<sql::Connection>;
using PreparedStatement = std::unique_ptr<sql::PreparedStatement>;
using ResultSet = std::unique_ptr<sql::ResultSet>;
using Statement = std::unique_ptr<sql::Statement>;

Result<Connection> connect(const MySqlConfig& config) {
  try {
    auto* driver = sql::mysql::get_mysql_driver_instance();
    Connection connection(driver->connect(config.uri, config.user, config.password));
    connection->setSchema(config.schema);
    connection->setAutoCommit(true);
    return Result<Connection>::success(std::move(connection));
  } catch (const sql::SQLException&) {
    return Result<Connection>::failure(
        make_error(ErrorCode::kDependencyMysqlUnavailable, "MySQL connection failed"));
  }
}

Error rollback(sql::Connection& connection, Error error) {
  try {
    connection.rollback();
    connection.setAutoCommit(true);
  } catch (const sql::SQLException&) {
  }
  return error;
}

Result<settlement::SettlementReceipt> read_existing(sql::Connection& connection,
                                                     const PreparedSettlement& prepared) {
  PreparedStatement statement(connection.prepareStatement(
      "SELECT settlement_id,fingerprint,rule_version FROM settlements WHERE match_id=? FOR UPDATE"));
  statement->setString(1, prepared.command.match_id);
  ResultSet rows(statement->executeQuery());
  if (!rows->next()) {
    return Result<settlement::SettlementReceipt>::failure({});
  }
  if (rows->getString("fingerprint") != prepared.command.result.fingerprint) {
    return Result<settlement::SettlementReceipt>::failure(make_error(ErrorCode::kSettlementConflict));
  }

  settlement::SettlementReceipt receipt;
  receipt.settlement_id = rows->getString("settlement_id");
  receipt.match_id = prepared.command.match_id;
  receipt.fingerprint = rows->getString("fingerprint");
  receipt.rule_version = rows->getString("rule_version");
  receipt.idempotent = true;
  receipt.cache_state = "stale";
  PreparedStatement players(connection.prepareStatement(
      "SELECT player_id,after_score FROM match_players WHERE match_id=? ORDER BY seat"));
  players->setString(1, prepared.command.match_id);
  ResultSet player_rows(players->executeQuery());
  while (player_rows->next()) {
    receipt.scores.push_back(
        rank::RankScore{player_rows->getString("player_id"), player_rows->getInt("after_score")});
  }
  if (receipt.scores.size() != 2U) {
    return Result<settlement::SettlementReceipt>::failure(
        make_error(ErrorCode::kSettlementStorageFailed, "stored settlement player set is incomplete"));
  }
  return Result<settlement::SettlementReceipt>::success(std::move(receipt));
}

void ensure_player(sql::Connection& connection, const std::string& player_id) {
  PreparedStatement statement(
      connection.prepareStatement("INSERT IGNORE INTO players(player_id,score,version) VALUES (?,1000,0)"));
  statement->setString(1, player_id);
  statement->executeUpdate();
}

std::unordered_map<std::string, std::int32_t> lock_scores(sql::Connection& connection,
                                                          const std::string& first,
                                                          const std::string& second) {
  PreparedStatement statement(connection.prepareStatement(
      "SELECT player_id,score FROM players WHERE player_id IN (?,?) ORDER BY player_id FOR UPDATE"));
  statement->setString(1, first);
  statement->setString(2, second);
  ResultSet rows(statement->executeQuery());
  std::unordered_map<std::string, std::int32_t> scores;
  while (rows->next()) {
    scores.emplace(rows->getString("player_id"), rows->getInt("score"));
  }
  return scores;
}

void update_player(sql::Connection& connection, const std::string& player_id, const std::int32_t score) {
  PreparedStatement statement(
      connection.prepareStatement("UPDATE players SET score=?,version=version+1 WHERE player_id=?"));
  statement->setInt(1, score);
  statement->setString(2, player_id);
  statement->executeUpdate();
}

void insert_match(sql::Connection& connection, const PreparedSettlement& prepared) {
  PreparedStatement statement(connection.prepareStatement(
      "INSERT INTO matches(match_id,room_id,mode,rule_version,status,started_at) "
      "VALUES (?,?,'duel',?,'Settling',CURRENT_TIMESTAMP(6)) "
      "ON DUPLICATE KEY UPDATE match_id=VALUES(match_id)"));
  statement->setString(1, prepared.command.match_id);
  statement->setString(2, prepared.command.room_id);
  statement->setString(3, prepared.command.result.rule_version);
  statement->executeUpdate();

  PreparedStatement verify(connection.prepareStatement(
      "SELECT room_id,mode,rule_version FROM matches WHERE match_id=? FOR UPDATE"));
  verify->setString(1, prepared.command.match_id);
  ResultSet rows(verify->executeQuery());
  if (!rows->next() || rows->getString("room_id") != prepared.command.room_id || rows->getString("mode") != "duel" ||
      rows->getString("rule_version") != prepared.command.result.rule_version) {
    throw sql::SQLException("match identity conflicts with frozen settlement");
  }
}

void insert_match_player(sql::Connection& connection,
                         const PreparedSettlement& prepared,
                         const std::string& player_id,
                         const std::uint32_t seat,
                         const std::int32_t before_score,
                         const std::int32_t after_score,
                         const std::string& outcome) {
  PreparedStatement statement(connection.prepareStatement(
      "INSERT INTO match_players(match_id,player_id,seat,before_score,after_score,outcome) VALUES (?,?,?,?,?,?)"));
  statement->setString(1, prepared.command.match_id);
  statement->setString(2, player_id);
  statement->setUInt(3, seat);
  statement->setInt(4, before_score);
  statement->setInt(5, after_score);
  statement->setString(6, outcome);
  statement->executeUpdate();
}

void insert_dirty(sql::Connection& connection,
                  const PreparedSettlement& prepared,
                  const std::string& player_id,
                  const std::int32_t target_score) {
  PreparedStatement statement(connection.prepareStatement(
      "INSERT INTO rank_cache_dirty(leaderboard,player_id,target_score,settlement_id,status,attempts,last_error) "
      "VALUES (?,?,?,?,'pending',0,'') ON DUPLICATE KEY UPDATE target_score=VALUES(target_score),"
      "settlement_id=VALUES(settlement_id),status='pending',attempts=0,last_error='',next_retry_at=CURRENT_TIMESTAMP(6)"));
  statement->setString(1, prepared.command.leaderboard);
  statement->setString(2, player_id);
  statement->setInt(3, target_score);
  statement->setString(4, prepared.command.settlement_id);
  statement->executeUpdate();
}

}  // namespace

MySqlSettlementRepository::MySqlSettlementRepository(MySqlConfig config) : config_(std::move(config)) {}

Result<settlement::SettlementReceipt> MySqlSettlementRepository::settle(const PreparedSettlement& prepared) {
  if (prepared.command.match_id.empty() || prepared.command.room_id.empty() ||
      prepared.command.result.winner_id == prepared.command.result.loser_id) {
    return Result<settlement::SettlementReceipt>::failure(make_error(ErrorCode::kSettlementPlayerSetInvalid));
  }
  auto connection_result = connect(config_);
  if (!connection_result.ok()) {
    return Result<settlement::SettlementReceipt>::failure(std::move(connection_result.error));
  }
  auto& connection = **connection_result.value;
  try {
    connection.setAutoCommit(false);
    auto existing = read_existing(connection, prepared);
    if (existing.ok()) {
      connection.commit();
      connection.setAutoCommit(true);
      return existing;
    }
    if (existing.error) {
      return Result<settlement::SettlementReceipt>::failure(rollback(connection, std::move(existing.error)));
    }

    ensure_player(connection, prepared.command.result.winner_id);
    ensure_player(connection, prepared.command.result.loser_id);
    auto scores = lock_scores(connection, prepared.command.result.winner_id, prepared.command.result.loser_id);
    if (scores.size() != 2U) {
      return Result<settlement::SettlementReceipt>::failure(rollback(
          connection, make_error(ErrorCode::kSettlementStorageFailed, "player rows could not be locked")));
    }
    const auto winner_before = scores.at(prepared.command.result.winner_id);
    const auto loser_before = scores.at(prepared.command.result.loser_id);
    const auto winner_after = winner_before + prepared.winner_delta;
    const auto loser_after = loser_before + prepared.loser_delta;

    insert_match(connection, prepared);
    update_player(connection, prepared.command.result.winner_id, winner_after);
    update_player(connection, prepared.command.result.loser_id, loser_after);

    PreparedStatement settlement_statement(connection.prepareStatement(
        "INSERT INTO settlements(settlement_id,match_id,fingerprint,rule_version,result_json) VALUES (?,?,?,?,?)"));
    settlement_statement->setString(1, prepared.command.settlement_id);
    settlement_statement->setString(2, prepared.command.match_id);
    settlement_statement->setString(3, prepared.command.result.fingerprint);
    settlement_statement->setString(4, prepared.command.result.rule_version);
    settlement_statement->setString(5, prepared.result_json);
    settlement_statement->executeUpdate();

    insert_match_player(connection, prepared, prepared.command.result.winner_id, 0, winner_before, winner_after,
                        "win");
    insert_match_player(connection, prepared, prepared.command.result.loser_id, 1, loser_before, loser_after,
                        "loss");
    insert_dirty(connection, prepared, prepared.command.result.winner_id, winner_after);
    insert_dirty(connection, prepared, prepared.command.result.loser_id, loser_after);
    PreparedStatement close_match(
        connection.prepareStatement("UPDATE matches SET status='Closed',ended_at=CURRENT_TIMESTAMP(6) WHERE match_id=?"));
    close_match->setString(1, prepared.command.match_id);
    close_match->executeUpdate();
    connection.commit();
    connection.setAutoCommit(true);

    settlement::SettlementReceipt receipt{prepared.command.settlement_id,
                                          prepared.command.match_id,
                                          prepared.command.result.fingerprint,
                                          prepared.command.result.rule_version,
                                          {{prepared.command.result.winner_id, winner_after},
                                           {prepared.command.result.loser_id, loser_after}},
                                          false,
                                          "stale"};
    return Result<settlement::SettlementReceipt>::success(std::move(receipt));
  } catch (const sql::SQLException&) {
    return Result<settlement::SettlementReceipt>::failure(rollback(
        connection, make_error(ErrorCode::kSettlementStorageFailed, "MySQL settlement transaction failed")));
  }
}

Result<std::vector<DirtyRankRecord>> MySqlSettlementRepository::pending_dirty(const std::size_t limit) {
  auto connection = connect(config_);
  if (!connection.ok()) {
    return Result<std::vector<DirtyRankRecord>>::failure(std::move(connection.error));
  }
  try {
    PreparedStatement statement((*connection.value)->prepareStatement(
        "SELECT leaderboard,player_id,target_score,settlement_id,attempts FROM rank_cache_dirty "
        "WHERE status='pending' AND next_retry_at<=CURRENT_TIMESTAMP(6) "
        "ORDER BY next_retry_at,leaderboard,player_id LIMIT ?"));
    statement->setUInt(1, static_cast<unsigned int>(std::min<std::size_t>(limit, 1'000U)));
    ResultSet rows(statement->executeQuery());
    std::vector<DirtyRankRecord> records;
    while (rows->next()) {
      records.push_back(DirtyRankRecord{rows->getString("leaderboard"), rows->getString("player_id"),
                                        rows->getInt("target_score"), rows->getString("settlement_id"),
                                        rows->getUInt("attempts")});
    }
    return Result<std::vector<DirtyRankRecord>>::success(std::move(records));
  } catch (const sql::SQLException&) {
    return Result<std::vector<DirtyRankRecord>>::failure(
        make_error(ErrorCode::kSettlementStorageFailed, "MySQL dirty query failed"));
  }
}

Error MySqlSettlementRepository::resolve_dirty(const DirtyRankRecord& record) {
  auto connection = connect(config_);
  if (!connection.ok()) {
    return std::move(connection.error);
  }
  try {
    PreparedStatement statement((*connection.value)->prepareStatement(
        "DELETE FROM rank_cache_dirty WHERE leaderboard=? AND player_id=? AND settlement_id=? AND target_score=?"));
    statement->setString(1, record.leaderboard);
    statement->setString(2, record.player_id);
    statement->setString(3, record.settlement_id);
    statement->setInt(4, record.target_score);
    statement->executeUpdate();
    return {};
  } catch (const sql::SQLException&) {
    return make_error(ErrorCode::kSettlementStorageFailed, "MySQL dirty resolution failed");
  }
}

Error MySqlSettlementRepository::resolve_dirty_score(const std::string& leaderboard,
                                                      const rank::RankScore& score) {
  auto connection = connect(config_);
  if (!connection.ok()) {
    return std::move(connection.error);
  }
  try {
    PreparedStatement statement((*connection.value)->prepareStatement(
        "DELETE FROM rank_cache_dirty WHERE leaderboard=? AND player_id=? AND target_score=?"));
    statement->setString(1, leaderboard);
    statement->setString(2, score.player_id);
    statement->setInt(3, score.score);
    statement->executeUpdate();
    return {};
  } catch (const sql::SQLException&) {
    return make_error(ErrorCode::kSettlementStorageFailed, "MySQL rebuilt dirty resolution failed");
  }
}

Error MySqlSettlementRepository::fail_dirty(const DirtyRankRecord& record, const std::string& message) {
  auto connection = connect(config_);
  if (!connection.ok()) {
    return std::move(connection.error);
  }
  try {
    const auto retry_seconds = std::min<std::uint32_t>(60U, 1U << std::min<std::uint32_t>(record.attempts, 5U));
    PreparedStatement statement((*connection.value)->prepareStatement(
        "UPDATE rank_cache_dirty SET attempts=attempts+1,last_error=?,"
        "next_retry_at=DATE_ADD(CURRENT_TIMESTAMP(6),INTERVAL ? SECOND) "
        "WHERE leaderboard=? AND player_id=? AND settlement_id=?"));
    statement->setString(1, message.substr(0, 255));
    statement->setUInt(2, retry_seconds);
    statement->setString(3, record.leaderboard);
    statement->setString(4, record.player_id);
    statement->setString(5, record.settlement_id);
    statement->executeUpdate();
    return {};
  } catch (const sql::SQLException&) {
    return make_error(ErrorCode::kSettlementStorageFailed, "MySQL dirty failure update failed");
  }
}

Result<std::vector<rank::RankScore>> MySqlSettlementRepository::all_scores() {
  auto connection = connect(config_);
  if (!connection.ok()) {
    return Result<std::vector<rank::RankScore>>::failure(std::move(connection.error));
  }
  try {
    Statement statement((*connection.value)->createStatement());
    ResultSet rows(statement->executeQuery("SELECT player_id,score FROM players ORDER BY player_id"));
    std::vector<rank::RankScore> scores;
    while (rows->next()) {
      scores.push_back(rank::RankScore{rows->getString("player_id"), rows->getInt("score")});
    }
    return Result<std::vector<rank::RankScore>>::success(std::move(scores));
  } catch (const sql::SQLException&) {
    return Result<std::vector<rank::RankScore>>::failure(
        make_error(ErrorCode::kSettlementStorageFailed, "MySQL score scan failed"));
  }
}

Error MySqlSettlementRepository::check_health() {
  auto connection = connect(config_);
  if (!connection.ok()) {
    return std::move(connection.error);
  }
  try {
    Statement statement((*connection.value)->createStatement());
    ResultSet rows(statement->executeQuery("SELECT 1"));
    return rows->next() ? Error{} : make_error(ErrorCode::kDependencyMysqlUnavailable);
  } catch (const sql::SQLException&) {
    return make_error(ErrorCode::kDependencyMysqlUnavailable, "MySQL health query failed");
  }
}

}  // namespace arena_core::storage

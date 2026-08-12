#include "arena_core/game/room_actor.hpp"
#include "arena_core/protocol/frame.hpp"
#include "arena_core/rules/rule_engine.hpp"
#include "arena_core/settlement/settlement_gateway.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include <boost/asio.hpp>
#include <gtest/gtest.h>

namespace arena_core::game {
namespace {

using namespace std::chrono_literals;
namespace wire = arena_core::protocol::v1;

class FlakySettlementGateway final : public settlement::SettlementGateway {
 public:
  void async_settle(boost::asio::any_io_executor reply_executor,
                    settlement::SettlementCommand command,
                    SettlementCompletion completion) override {
    const auto call = ++calls;
    boost::asio::post(reply_executor,
                      [call, command = std::move(command), completion = std::move(completion)]() mutable {
                        if (call == 1) {
                          completion(Result<settlement::SettlementReceipt>::failure(
                              make_error(ErrorCode::kSettlementStorageFailed)));
                          return;
                        }
                        settlement::SettlementReceipt receipt;
                        receipt.settlement_id = command.settlement_id;
                        receipt.match_id = command.match_id;
                        receipt.fingerprint = command.result.fingerprint;
                        receipt.rule_version = command.result.rule_version;
                        receipt.scores = {{command.result.winner_id, 1'016},
                                          {command.result.loser_id, 984}};
                        receipt.cache_state = "fresh";
                        receipt.room_id = command.room_id;
                        receipt.winner_id = command.result.winner_id;
                        receipt.loser_id = command.result.loser_id;
                        receipt.finished_at = command.result.finished_at;
                        receipt.players = {{receipt.winner_id, 1'000, 1'016, "win"},
                                           {receipt.loser_id, 1'000, 984, "loss"}};
                        completion(Result<settlement::SettlementReceipt>::success(
                            std::move(receipt)));
                      });
  }

  void async_query_rank(boost::asio::any_io_executor reply_executor,
                        rank::RankQuery,
                        RankCompletion completion) override {
    boost::asio::post(reply_executor, [completion = std::move(completion)]() mutable {
      completion(Result<rank::RankView>::failure(make_error(ErrorCode::kRankPlayerNotFound)));
    });
  }

  void async_query_result(boost::asio::any_io_executor reply_executor,
                          settlement::SettlementQuery,
                          ResultQueryCompletion completion) override {
    boost::asio::post(reply_executor, [completion = std::move(completion)]() mutable {
      completion(Result<settlement::SettlementReceipt>::failure(
          make_error(ErrorCode::kSettlementNotFound)));
    });
  }

  Result<std::int32_t> allowed_mmr_delta(const std::int64_t waited_seconds) const override {
    return rules.allowed_mmr_delta(waited_seconds);
  }

  const std::string& score_rule_version() const noexcept override { return rules.score_rule_version(); }

  std::atomic<int> calls{0};
  rules::BuiltInRuleEngine rules;
};

class RoomHarness {
 public:
  RoomHarness() : guard(boost::asio::make_work_guard(io)), thread([this] { io.run(); }) {}

  ~RoomHarness() {
    guard.reset();
    io.stop();
    thread.join();
  }

  Result<wire::Envelope> join(const std::shared_ptr<RoomActor>& room,
                              const std::string& player,
                              const std::string& token) {
    wire::JoinRoom command;
    command.set_match_id("match-1");
    command.set_room_id("room-1");
    command.set_room_server("local");
    command.set_player_id(player);
    command.set_join_token(token);
    std::promise<Result<wire::Envelope>> promise;
    auto future = promise.get_future();
    room->async_join(player, std::move(command),
                     [&promise](Result<wire::Envelope> result) { promise.set_value(std::move(result)); });
    return future.get();
  }

  Result<wire::Envelope> ready(const std::shared_ptr<RoomActor>& room, const std::string& player) {
    wire::ReadyRoom command;
    command.set_room_id("room-1");
    std::promise<Result<wire::Envelope>> promise;
    auto future = promise.get_future();
    room->async_ready(player, std::move(command),
                      [&promise](Result<wire::Envelope> result) { promise.set_value(std::move(result)); });
    return future.get();
  }

  Result<settlement::SettlementReceipt> submit(const std::shared_ptr<RoomActor>& room) {
    domain::FrozenRoomResult result{"player-1", "player-2", "score-v1", 42,
                                    "fingerprint-1", "2026-08-12T05:30:00Z"};
    std::promise<Result<settlement::SettlementReceipt>> promise;
    auto future = promise.get_future();
    room->async_submit_result(
        "result-1", std::move(result),
        [&promise](Result<settlement::SettlementReceipt> value) {
          promise.set_value(std::move(value));
        });
    return future.get();
  }

  boost::asio::io_context io;
  boost::asio::executor_work_guard<boost::asio::io_context::executor_type> guard;
  std::thread thread;
};

TEST(RoomSettlementTest, RemainsSettlingAfterStorageFailureAndClosesOnlyAfterRetryCommits) {
  RoomHarness harness;
  auto gateway = std::make_shared<FlakySettlementGateway>();
  std::atomic<int> closed{0};
  auto room = RoomActor::create(harness.io, "room-1", "match-1", "local", "player-1", "player-2",
                                "token-1", "token-2", domain::MonotonicClock::now() + 5s, 5s,
                                {}, gateway, [&closed](std::string) { ++closed; });
  ASSERT_TRUE(room.ok()) << room.error.message;
  ASSERT_TRUE(harness.join(*room.value, "player-1", "token-1").ok());
  ASSERT_TRUE(harness.join(*room.value, "player-2", "token-2").ok());
  ASSERT_TRUE(harness.ready(*room.value, "player-1").ok());

  const auto awaiting = harness.ready(*room.value, "player-2");
  ASSERT_TRUE(awaiting.ok());
  auto state = protocol::parse_payload<wire::RoomStateChanged>(*awaiting.value);
  ASSERT_TRUE(state.ok());
  EXPECT_EQ(state.value->state(), "AwaitingResult");

  const auto failed = harness.submit(*room.value);
  ASSERT_FALSE(failed.ok());
  EXPECT_EQ(failed.error.code, ErrorCode::kSettlementStorageFailed);
  EXPECT_EQ(closed.load(), 0);

  const auto retried = harness.submit(*room.value);
  ASSERT_TRUE(retried.ok()) << retried.error.message;
  EXPECT_EQ(retried.value->settlement_id, "result-1");
  for (int attempt = 0; attempt < 50 && closed.load() == 0; ++attempt) {
    std::this_thread::sleep_for(2ms);
  }
  EXPECT_EQ(closed.load(), 1);
  EXPECT_EQ(gateway->calls.load(), 2);
}

TEST(RoomSettlementTest, ResultTimeoutClosesWithoutCallingSettlement) {
  RoomHarness harness;
  auto gateway = std::make_shared<FlakySettlementGateway>();
  std::atomic<int> closed{0};
  auto room = RoomActor::create(harness.io, "room-1", "match-1", "local", "player-1", "player-2",
                                "token-1", "token-2", domain::MonotonicClock::now() + 5s, 10ms,
                                {}, gateway, [&closed](std::string) { ++closed; });
  ASSERT_TRUE(room.ok()) << room.error.message;
  ASSERT_TRUE(harness.join(*room.value, "player-1", "token-1").ok());
  ASSERT_TRUE(harness.join(*room.value, "player-2", "token-2").ok());
  ASSERT_TRUE(harness.ready(*room.value, "player-1").ok());
  ASSERT_TRUE(harness.ready(*room.value, "player-2").ok());
  for (int attempt = 0; attempt < 100 && closed.load() == 0; ++attempt) {
    std::this_thread::sleep_for(2ms);
  }
  EXPECT_EQ(closed.load(), 1);
  EXPECT_EQ(gateway->calls.load(), 0);
}

}  // namespace
}  // namespace arena_core::game

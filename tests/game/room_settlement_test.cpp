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
                        completion(Result<settlement::SettlementReceipt>::success(
                            settlement::SettlementReceipt{command.settlement_id,
                                                          command.match_id,
                                                          command.result.fingerprint,
                                                          command.result.rule_version,
                                                          {{command.result.winner_id, 1'016},
                                                           {command.result.loser_id, 984}},
                                                          false,
                                                          "fresh"}));
                      });
  }

  void async_query_rank(boost::asio::any_io_executor reply_executor,
                        rank::RankQuery,
                        RankCompletion completion) override {
    boost::asio::post(reply_executor, [completion = std::move(completion)]() mutable {
      completion(Result<rank::RankView>::failure(make_error(ErrorCode::kRankPlayerNotFound)));
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

  boost::asio::io_context io;
  boost::asio::executor_work_guard<boost::asio::io_context::executor_type> guard;
  std::thread thread;
};

TEST(RoomSettlementTest, RemainsSettlingAfterStorageFailureAndClosesOnlyAfterRetryCommits) {
  RoomHarness harness;
  auto gateway = std::make_shared<FlakySettlementGateway>();
  std::atomic<int> closed{0};
  auto room = RoomActor::create(harness.io, "room-1", "match-1", "local", "player-1", "player-2",
                                "token-1", "token-2", domain::MonotonicClock::now() + 5s, {}, gateway,
                                [&closed](std::string) { ++closed; });
  ASSERT_TRUE(room.ok()) << room.error.message;
  ASSERT_TRUE(harness.join(*room.value, "player-1", "token-1").ok());
  ASSERT_TRUE(harness.join(*room.value, "player-2", "token-2").ok());
  ASSERT_TRUE(harness.ready(*room.value, "player-1").ok());

  const auto failed = harness.ready(*room.value, "player-2");
  ASSERT_FALSE(failed.ok());
  EXPECT_EQ(failed.error.code, ErrorCode::kSettlementStorageFailed);
  EXPECT_EQ(closed.load(), 0);

  const auto retried = harness.ready(*room.value, "player-2");
  ASSERT_TRUE(retried.ok()) << retried.error.message;
  auto state = protocol::parse_payload<wire::RoomStateChanged>(*retried.value);
  ASSERT_TRUE(state.ok());
  EXPECT_EQ(state.value->state(), "Closed");
  EXPECT_EQ(closed.load(), 1);
  EXPECT_EQ(gateway->calls.load(), 2);
}

}  // namespace
}  // namespace arena_core::game

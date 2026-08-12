#include "arena_core/domain/clock.hpp"
#include "arena_core/domain/identifiers.hpp"
#include "arena_core/domain/match_ticket.hpp"
#include "arena_core/domain/room.hpp"
#include "arena_core/domain/session.hpp"

#include <chrono>
#include <string>

#include <gtest/gtest.h>

namespace arena_core::domain {
namespace {

using namespace std::chrono_literals;

TEST(IdentifierTest, AcceptsOnlyTheLockedPlayerAlphabetAndLength) {
  EXPECT_TRUE(is_valid_player_id("player_1:test.demo"));
  EXPECT_FALSE(is_valid_player_id(""));
  EXPECT_FALSE(is_valid_player_id("player with space"));
  EXPECT_FALSE(is_valid_player_id(std::string(65, 'x')));
}

TEST(IdentifierTest, SequentialGeneratorIsDeterministic) {
  SequentialIdGenerator generator(40);
  EXPECT_EQ(generator.next("ticket"), "ticket-00000041");
  EXPECT_EQ(generator.next("ticket"), "ticket-00000042");
}

TEST(FakeClockTest, AdvancesWithoutSleeping) {
  FakeClock clock;
  clock.advance(1500ms);
  EXPECT_EQ(clock.now(), TimePoint{} + 1500ms);
}

TEST(SessionStateMachineTest, ActivatesBeforeHelloDeadlineAndRefreshesActivity) {
  FakeClock clock;
  SessionStateMachine session(clock.now());
  clock.advance(4s);
  EXPECT_FALSE(session.activate("player-1", clock.now()));
  EXPECT_EQ(session.state(), SessionState::kActive);
  EXPECT_EQ(session.player_id(), "player-1");
  const auto first_deadline = session.deadline();
  clock.advance(10s);
  EXPECT_FALSE(session.record_activity(clock.now()));
  EXPECT_GT(session.deadline(), first_deadline);
}

TEST(SessionStateMachineTest, HelloAtDeadlineClosesSession) {
  FakeClock clock;
  SessionStateMachine session(clock.now());
  clock.advance(5s);
  const auto error = session.activate("player-1", clock.now());
  EXPECT_EQ(error.code, ErrorCode::kSessionHelloRequired);
  EXPECT_EQ(session.state(), SessionState::kClosed);
}

TEST(SessionStateMachineTest, RejectsInvalidPlayerWithoutClosingBeforeDeadline) {
  FakeClock clock;
  SessionStateMachine session(clock.now());
  const auto error = session.activate("bad player", clock.now());
  EXPECT_EQ(error.code, ErrorCode::kSessionPlayerIdInvalid);
  EXPECT_EQ(session.state(), SessionState::kConnected);
}

TEST(SessionStateMachineTest, ResumesSamePlayerAndRotatesGeneration) {
  FakeClock clock;
  SessionStateMachine session(clock.now());
  ASSERT_FALSE(session.activate("player-1", clock.now()));
  clock.advance(1s);
  ASSERT_FALSE(session.connection_lost(clock.now()));
  EXPECT_EQ(session.state(), SessionState::kReconnecting);
  clock.advance(14s);
  EXPECT_FALSE(session.resume("player-1", clock.now()));
  EXPECT_EQ(session.state(), SessionState::kActive);
  EXPECT_EQ(session.generation(), 2U);
}

TEST(SessionStateMachineTest, RejectsWrongIdentityAndExpiresAtReconnectBoundary) {
  FakeClock clock;
  SessionStateMachine session(clock.now());
  ASSERT_FALSE(session.activate("player-1", clock.now()));
  ASSERT_FALSE(session.connection_lost(clock.now()));
  EXPECT_EQ(session.resume("player-2", clock.now()).code, ErrorCode::kSessionTokenInvalid);
  clock.advance(15s);
  EXPECT_EQ(session.resume("player-1", clock.now()).code, ErrorCode::kSessionReconnectExpired);
  EXPECT_EQ(session.state(), SessionState::kClosed);
}

TEST(SessionStateMachineTest, IdleDeadlineAndCloseAreIdempotent) {
  FakeClock clock;
  SessionStateMachine session(clock.now());
  ASSERT_FALSE(session.activate("player-1", clock.now()));
  clock.advance(30s);
  EXPECT_TRUE(session.expire_if_due(clock.now()));
  EXPECT_FALSE(session.expire_if_due(clock.now()));
  session.close();
  session.close();
  EXPECT_EQ(session.state(), SessionState::kClosed);
}

Result<MatchTicket> make_ticket(FakeClock& clock, const std::chrono::milliseconds wait = 30s) {
  return MatchTicket::create("ticket-1", "player-1", "duel", 1000, clock.now(), wait);
}

TEST(MatchTicketTest, CreatesValidDuelTicketAtMmrBoundaries) {
  FakeClock clock;
  auto low = MatchTicket::create("ticket-low", "player-low", "duel", 0, clock.now(), 1s);
  auto high = MatchTicket::create("ticket-high", "player-high", "duel", 10'000, clock.now(), 10min);
  ASSERT_TRUE(low.ok());
  ASSERT_TRUE(high.ok());
  EXPECT_EQ(low.value->state(), MatchTicketState::kQueued);
  EXPECT_EQ(high.value->mmr(), 10'000);
}

TEST(MatchTicketTest, RejectsModeMmrAndWaitOutsideV1Contract) {
  FakeClock clock;
  EXPECT_EQ(MatchTicket::create("ticket-1", "p1", "ranked", 1000, clock.now(), 30s).error.code,
            ErrorCode::kMatchModeUnsupported);
  EXPECT_EQ(MatchTicket::create("ticket-1", "p1", "duel", -1, clock.now(), 30s).error.code,
            ErrorCode::kMatchMmrInvalid);
  EXPECT_EQ(MatchTicket::create("ticket-1", "p1", "duel", 1000, clock.now(), 999ms).error.code,
            ErrorCode::kMatchWaitInvalid);
}

TEST(MatchTicketTest, OwnerCanCancelIdempotentlyButOutsiderCannot) {
  FakeClock clock;
  auto created = make_ticket(clock);
  ASSERT_TRUE(created.ok());
  auto& ticket = *created.value;
  EXPECT_EQ(ticket.cancel("outsider", clock.now()).code, ErrorCode::kMatchTicketNotFound);
  EXPECT_FALSE(ticket.cancel("player-1", clock.now()));
  EXPECT_FALSE(ticket.cancel("player-1", clock.now()));
  EXPECT_EQ(ticket.state(), MatchTicketState::kCancelled);
}

TEST(MatchTicketTest, ExpirationWinsAtExactDeadline) {
  FakeClock clock;
  auto created = make_ticket(clock, 1s);
  ASSERT_TRUE(created.ok());
  clock.advance(1s);
  EXPECT_EQ(created.value->mark_matched("match-1", clock.now()).code, ErrorCode::kMatchTicketExpired);
  EXPECT_EQ(created.value->state(), MatchTicketState::kExpired);
}

TEST(MatchTicketTest, MatchedTicketIsTerminal) {
  FakeClock clock;
  auto created = make_ticket(clock);
  ASSERT_TRUE(created.ok());
  EXPECT_FALSE(created.value->mark_matched("match-1", clock.now()));
  EXPECT_EQ(created.value->cancel("player-1", clock.now()).code, ErrorCode::kMatchTicketNotQueued);
  EXPECT_EQ(created.value->match_id(), "match-1");
}

Room create_room() {
  auto result = Room::create("room-1", "match-1", "player-1", "player-2");
  EXPECT_TRUE(result.ok());
  return std::move(*result.value);
}

TEST(RoomTest, RequiresTwoDistinctValidAssignedPlayers) {
  const auto result = Room::create("room-1", "match-1", "player-1", "player-1");
  EXPECT_EQ(result.error.code, ErrorCode::kSettlementPlayerSetInvalid);
}

TEST(RoomTest, JoinIsAssignedAndIdempotent) {
  auto room = create_room();
  EXPECT_EQ(room.join("outsider").code, ErrorCode::kRoomPlayerNotAssigned);
  EXPECT_FALSE(room.join("player-1"));
  EXPECT_FALSE(room.join("player-1"));
  EXPECT_EQ(room.state(), RoomState::kWaiting);
  EXPECT_TRUE(room.members()[0].joined);
}

TEST(RoomTest, ReadyRequiresJoinAndBothPlayers) {
  auto room = create_room();
  ASSERT_FALSE(room.join("player-1"));
  EXPECT_EQ(room.set_ready("player-2").code, ErrorCode::kRoomNotJoined);
  ASSERT_FALSE(room.join("player-2"));
  ASSERT_FALSE(room.set_ready("player-1"));
  EXPECT_EQ(room.state(), RoomState::kWaiting);
  ASSERT_FALSE(room.set_ready("player-2"));
  EXPECT_EQ(room.state(), RoomState::kReady);
}

TEST(RoomTest, CompletesExactlyOneValidLifecycle) {
  auto room = create_room();
  ASSERT_FALSE(room.join("player-1"));
  ASSERT_FALSE(room.join("player-2"));
  ASSERT_FALSE(room.set_ready("player-1"));
  ASSERT_FALSE(room.set_ready("player-2"));
  ASSERT_FALSE(room.start());
  EXPECT_FALSE(room.start());

  FrozenRoomResult result{"player-1", "player-2", "score-v1", 42, "fingerprint-1",
                          "2026-08-12T05:30:00Z"};
  ASSERT_FALSE(room.freeze_result(result));
  EXPECT_FALSE(room.freeze_result(result));
  EXPECT_EQ(room.state(), RoomState::kSettling);
  ASSERT_FALSE(room.mark_settlement_committed());
  EXPECT_FALSE(room.mark_settlement_committed());
  EXPECT_EQ(room.state(), RoomState::kClosed);
}

TEST(RoomTest, RejectsResultWithWrongPlayerSet) {
  auto room = create_room();
  ASSERT_FALSE(room.join("player-1"));
  ASSERT_FALSE(room.join("player-2"));
  ASSERT_FALSE(room.set_ready("player-1"));
  ASSERT_FALSE(room.set_ready("player-2"));
  ASSERT_FALSE(room.start());
  const auto error = room.freeze_result({"player-1", "outsider", "score-v1", 42,
                                         "fingerprint-1", "2026-08-12T05:30:00Z"});
  EXPECT_EQ(error.code, ErrorCode::kSettlementPlayerSetInvalid);
  EXPECT_EQ(room.state(), RoomState::kAwaitingResult);
}

TEST(RoomTest, AbortIsIdempotentBeforeStartAndForbiddenAfterStart) {
  auto early_room = create_room();
  EXPECT_FALSE(early_room.abort());
  EXPECT_FALSE(early_room.abort());
  EXPECT_EQ(early_room.state(), RoomState::kAborted);

  auto running_room = create_room();
  ASSERT_FALSE(running_room.join("player-1"));
  ASSERT_FALSE(running_room.join("player-2"));
  ASSERT_FALSE(running_room.set_ready("player-1"));
  ASSERT_FALSE(running_room.set_ready("player-2"));
  ASSERT_FALSE(running_room.start());
  EXPECT_EQ(running_room.abort().code, ErrorCode::kRoomInvalidState);
  EXPECT_EQ(running_room.state(), RoomState::kAwaitingResult);
}

TEST(RoomTest, ExpiresWhileAwaitingTrustedResult) {
  auto room = create_room();
  ASSERT_FALSE(room.join("player-1"));
  ASSERT_FALSE(room.join("player-2"));
  ASSERT_FALSE(room.set_ready("player-1"));
  ASSERT_FALSE(room.set_ready("player-2"));
  ASSERT_FALSE(room.start());

  ASSERT_FALSE(room.expire_result_wait());
  EXPECT_EQ(room.state(), RoomState::kResultTimedOut);
  EXPECT_EQ(room.freeze_result({"player-1", "player-2", "score-v1", 42,
                                "fingerprint-1", "2026-08-12T05:30:00Z"}).code,
            ErrorCode::kSettlementNotAllowed);
}

}  // namespace
}  // namespace arena_core::domain

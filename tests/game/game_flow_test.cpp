#include "arena_core/domain/identifiers.hpp"
#include "arena_core/game/game_service.hpp"
#include "arena_core/observer/observer_store.hpp"
#include "arena_core/protocol/frame.hpp"
#include "arena_core/session/tcp_server.hpp"
#include "arena_core/session/token_generator.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio.hpp>
#include <gtest/gtest.h>

#include "arena_core.pb.h"

namespace arena_core::game {
namespace {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;
namespace wire = arena_core::protocol::v1;
using namespace std::chrono_literals;

class RunningGameServer {
 public:
  explicit RunningGameServer(GameServiceOptions game_options = {}) {
    game_options.match_tick = 20ms;
    auto token_generator = std::make_shared<session::DeterministicTokenGenerator>();
    observer_store_ = std::make_shared<observer::ObserverStore>();
    game_service_ = std::make_shared<GameService>(
        io_context_, std::move(game_options), std::make_shared<domain::SequentialIdGenerator>(), token_generator,
        nullptr, observer_store_);

    session::SessionOptions session_options;
    session_options.durations.hello_deadline = 2s;
    session_options.durations.idle_timeout = 5s;
    session_options.durations.reconnect_window = 1s;
    server_ = std::make_shared<session::TcpServer>(
        io_context_, tcp::endpoint(asio::ip::address_v4::loopback(), 0), session_options, token_generator,
        game_service_, observer_store_);
    const auto error = server_->start();
    if (error) {
      throw std::runtime_error(error.message);
    }
    port_ = server_->bound_port();
    thread_ = std::thread([this] { io_context_.run(); });
  }

  ~RunningGameServer() {
    if (!thread_.joinable()) {
      return;
    }
    if (!io_context_.stopped()) {
      std::promise<void> stopped;
      auto completion = stopped.get_future();
      asio::post(io_context_, [this, &stopped] {
        server_->stop();
        asio::post(io_context_, [&stopped] { stopped.set_value(); });
      });
      completion.wait_for(250ms);
    }
    io_context_.stop();
    thread_.join();
  }

  std::uint16_t port() const noexcept { return port_; }
  std::shared_ptr<GameService> game_service() const { return game_service_; }
  std::shared_ptr<observer::ObserverStore> observer_store() const { return observer_store_; }

  Result<settlement::SettlementReceipt> submit_result(const wire::MatchFound& match,
                                                       std::string winner_id,
                                                       std::string loser_id) {
    TrustedMatchResult result;
    result.result_id = "result-" + match.match_id();
    result.match_id = match.match_id();
    result.room_id = match.room_id();
    result.winner_id = std::move(winner_id);
    result.loser_id = std::move(loser_id);
    result.rule_version = "score-v1";
    result.finished_at = "2026-08-12T05:30:00Z";
    std::promise<Result<settlement::SettlementReceipt>> promise;
    auto future = promise.get_future();
    game_service_->async_submit_trusted_result(
        std::move(result), [&promise](Result<settlement::SettlementReceipt> receipt) {
          promise.set_value(std::move(receipt));
        });
    if (future.wait_for(2s) != std::future_status::ready) {
      return Result<settlement::SettlementReceipt>::failure(
          make_error(ErrorCode::kInternalError, "result submission timed out"));
    }
    return future.get();
  }

  bool stop_and_wait_for_connections() {
    std::promise<void> requested;
    auto completion = requested.get_future();
    asio::post(io_context_, [this, &requested] {
      server_->stop();
      server_->stop();
      asio::post(io_context_, [&requested] { requested.set_value(); });
    });
    if (completion.wait_for(1s) != std::future_status::ready) {
      return false;
    }
    for (std::uint32_t attempt = 0; attempt < 100U; ++attempt) {
      if (server_->active_connection_count() == 0U) {
        return true;
      }
      std::this_thread::sleep_for(10ms);
    }
    return false;
  }

 private:
  asio::io_context io_context_;
  std::shared_ptr<GameService> game_service_;
  std::shared_ptr<observer::ObserverStore> observer_store_;
  std::shared_ptr<session::TcpServer> server_;
  std::thread thread_;
  std::uint16_t port_{0};
};

class GameClient {
 public:
  GameClient(const std::uint16_t port, std::string player_id, const std::string& reconnect_token = {})
      : player_id_(std::move(player_id)), socket_(io_context_) {
    socket_.connect(tcp::endpoint(asio::ip::address_v4::loopback(), port));
    wire::Hello hello;
    hello.set_player_id(player_id_);
    hello.set_reconnect_token(reconnect_token);
    const auto request_id = send_without_session(wire::HELLO, hello);
    const auto response = receive_response(request_id);
    if (response.message_type() != wire::HELLO_ACK) {
      throw std::runtime_error("Hello failed");
    }
    auto ack = protocol::parse_payload<wire::HelloAck>(response);
    if (!ack.ok()) {
      throw std::runtime_error("HelloAck invalid");
    }
    session_token_ = ack.value->session_token();
    reconnect_token_ = ack.value->reconnect_token();
  }

  template <typename Message>
  std::uint64_t send(const wire::MessageType type, const Message& payload) {
    wire::Envelope envelope;
    envelope.set_protocol_version(1);
    envelope.set_message_type(type);
    envelope.set_request_id(next_request_id_++);
    envelope.set_session_token(session_token_);
    protocol::set_payload(envelope, payload);
    write(envelope);
    return envelope.request_id();
  }

  wire::Envelope receive_response(const std::uint64_t request_id) {
    for (;;) {
      auto envelope = read();
      if (envelope.request_id() == request_id) {
        return envelope;
      }
      pushes_.push_back(std::move(envelope));
    }
  }

  wire::Envelope receive_push(const wire::MessageType type) {
    const auto stored = std::find_if(pushes_.begin(), pushes_.end(), [type](const wire::Envelope& envelope) {
      return envelope.request_id() == 0U && envelope.message_type() == type;
    });
    if (stored != pushes_.end()) {
      auto result = std::move(*stored);
      pushes_.erase(stored);
      return result;
    }
    for (;;) {
      auto envelope = read();
      if (envelope.request_id() == 0U && envelope.message_type() == type) {
        return envelope;
      }
      pushes_.push_back(std::move(envelope));
    }
  }

  wire::RoomStateChanged receive_room_state(const std::string& expected_state) {
    for (;;) {
      auto event = receive_push(wire::ROOM_STATE_CHANGED);
      auto state = protocol::parse_payload<wire::RoomStateChanged>(event);
      if (!state.ok()) {
        throw std::runtime_error("RoomStateChanged invalid");
      }
      if (state.value->state() == expected_state) {
        return std::move(*state.value);
      }
    }
  }

  const std::string& player_id() const noexcept { return player_id_; }
  const std::string& reconnect_token() const noexcept { return reconnect_token_; }
  void close() {
    boost::system::error_code ignored;
    socket_.shutdown(tcp::socket::shutdown_both, ignored);
    socket_.close(ignored);
  }

 private:
  template <typename Message>
  std::uint64_t send_without_session(const wire::MessageType type, const Message& payload) {
    wire::Envelope envelope;
    envelope.set_protocol_version(1);
    envelope.set_message_type(type);
    envelope.set_request_id(next_request_id_++);
    protocol::set_payload(envelope, payload);
    write(envelope);
    return envelope.request_id();
  }

  void write(const wire::Envelope& envelope) {
    auto frame = protocol::frame_message(envelope);
    if (!frame.ok()) {
      throw std::runtime_error(frame.error.message);
    }
    asio::write(socket_, asio::buffer(*frame.value));
  }

  wire::Envelope read() {
    std::array<std::uint8_t, 4> header{};
    asio::read(socket_, asio::buffer(header));
    auto length = protocol::decode_frame_length(header);
    if (!length.ok()) {
      throw std::runtime_error(length.error.message);
    }
    std::string payload(*length.value, '\0');
    asio::read(socket_, asio::buffer(payload));
    auto envelope = protocol::parse_envelope(payload);
    if (!envelope.ok()) {
      throw std::runtime_error(envelope.error.message);
    }
    return std::move(*envelope.value);
  }

  std::string player_id_;
  asio::io_context io_context_;
  tcp::socket socket_;
  std::string session_token_;
  std::string reconnect_token_;
  std::uint64_t next_request_id_{1};
  std::vector<wire::Envelope> pushes_;
};

wire::QueueMatch queue_command(const std::int32_t mmr, const std::uint32_t wait_ms = 3'000) {
  wire::QueueMatch command;
  command.set_mode("duel");
  command.set_mmr(mmr);
  command.set_max_wait_ms(wait_ms);
  return command;
}

wire::ErrorResponse parse_error(const wire::Envelope& response, const std::string& expected_code) {
  EXPECT_EQ(response.message_type(), wire::ERROR_RESPONSE);
  auto error = protocol::parse_payload<wire::ErrorResponse>(response);
  EXPECT_TRUE(error.ok()) << error.error.message;
  if (error.ok()) {
    EXPECT_EQ(error.value->code(), expected_code);
    return std::move(*error.value);
  }
  return {};
}

wire::MatchFound parse_match_found(const wire::Envelope& envelope) {
  auto found = protocol::parse_payload<wire::MatchFound>(envelope);
  EXPECT_TRUE(found.ok()) << found.error.message;
  return found.ok() ? std::move(*found.value) : wire::MatchFound{};
}

TEST(GameFlowTest, CompletesMatchJoinReadyAndTrustedResultEnd) {
  RunningGameServer server;
  GameClient first(server.port(), "player-1");
  GameClient second(server.port(), "player-2");

  const auto first_queue = first.send(wire::QUEUE_MATCH, queue_command(1000));
  EXPECT_EQ(first.receive_response(first_queue).message_type(), wire::MATCH_QUEUED);
  const auto second_queue = second.send(wire::QUEUE_MATCH, queue_command(1050));
  EXPECT_EQ(second.receive_response(second_queue).message_type(), wire::MATCH_QUEUED);

  const auto first_found = parse_match_found(first.receive_push(wire::MATCH_FOUND));
  const auto second_found = parse_match_found(second.receive_push(wire::MATCH_FOUND));
  EXPECT_EQ(first_found.room_id(), second_found.room_id());
  EXPECT_EQ(first_found.match_id(), second_found.match_id());
  EXPECT_EQ(first_found.opponent_id(), second.player_id());
  EXPECT_NE(first_found.join_token(), second_found.join_token());

  GameClient outsider(server.port(), "outsider");
  wire::JoinRoom stolen_join;
  stolen_join.set_match_id(first_found.match_id());
  stolen_join.set_room_id(first_found.room_id());
  stolen_join.set_room_server(first_found.room_server());
  stolen_join.set_player_id(first.player_id());
  stolen_join.set_join_token(first_found.join_token());
  const auto stolen_join_id = outsider.send(wire::JOIN_ROOM, stolen_join);
  parse_error(outsider.receive_response(stolen_join_id), "ROOM_JOIN_TOKEN_INVALID");

  wire::JoinRoom invalid_join;
  invalid_join.set_match_id(first_found.match_id());
  invalid_join.set_room_id(first_found.room_id());
  invalid_join.set_room_server(first_found.room_server());
  invalid_join.set_player_id(first.player_id());
  invalid_join.set_join_token("tampered");
  const auto invalid_join_id = first.send(wire::JOIN_ROOM, invalid_join);
  parse_error(first.receive_response(invalid_join_id), "ROOM_JOIN_TOKEN_INVALID");

  auto first_join = invalid_join;
  first_join.set_join_token(first_found.join_token());
  const auto first_join_id = first.send(wire::JOIN_ROOM, first_join);
  EXPECT_EQ(first.receive_response(first_join_id).message_type(), wire::ROOM_JOINED);

  wire::ReadyRoom early_ready;
  early_ready.set_room_id(first_found.room_id());
  const auto early_ready_id = second.send(wire::READY_ROOM, early_ready);
  parse_error(second.receive_response(early_ready_id), "ROOM_NOT_JOINED");

  wire::JoinRoom second_join;
  second_join.set_match_id(second_found.match_id());
  second_join.set_room_id(second_found.room_id());
  second_join.set_room_server(second_found.room_server());
  second_join.set_player_id(second.player_id());
  second_join.set_join_token(second_found.join_token());
  const auto second_join_id = second.send(wire::JOIN_ROOM, second_join);
  EXPECT_EQ(second.receive_response(second_join_id).message_type(), wire::ROOM_JOINED);

  wire::ReadyRoom ready;
  ready.set_room_id(first_found.room_id());
  const auto first_ready_id = first.send(wire::READY_ROOM, ready);
  EXPECT_EQ(first.receive_response(first_ready_id).message_type(), wire::ROOM_STATE_CHANGED);
  const auto second_ready_id = second.send(wire::READY_ROOM, ready);
  const auto final_response = second.receive_response(second_ready_id);
  ASSERT_EQ(final_response.message_type(), wire::ROOM_STATE_CHANGED);
  auto final_state = protocol::parse_payload<wire::RoomStateChanged>(final_response);
  ASSERT_TRUE(final_state.ok());
  EXPECT_EQ(final_state.value->state(), "AwaitingResult");

  const auto committed = server.submit_result(first_found, first.player_id(), second.player_id());
  ASSERT_TRUE(committed.ok()) << committed.error.message;
  EXPECT_FALSE(committed.value->idempotent);

  const auto first_result = protocol::parse_payload<wire::MatchResultView>(
      first.receive_push(wire::MATCH_RESULT_COMMITTED));
  const auto second_result = protocol::parse_payload<wire::MatchResultView>(
      second.receive_push(wire::MATCH_RESULT_COMMITTED));
  ASSERT_TRUE(first_result.ok());
  ASSERT_TRUE(second_result.ok());
  EXPECT_EQ(first_result.value->winner_id(), first.player_id());
  EXPECT_EQ(second_result.value->result_id(), committed.value->settlement_id);

  const auto first_closed = first.receive_room_state("Closed");
  const auto second_closed = second.receive_room_state("Closed");
  EXPECT_EQ(first_closed.winner_id(), first.player_id());
  EXPECT_EQ(second_closed.winner_id(), first.player_id());

  wire::GetMatchResult query;
  query.set_match_id(first_found.match_id());
  const auto query_response = first.receive_response(first.send(wire::GET_MATCH_RESULT, query));
  ASSERT_EQ(query_response.message_type(), wire::MATCH_RESULT);
  const auto queried = protocol::parse_payload<wire::MatchResultView>(query_response);
  ASSERT_TRUE(queried.ok());
  EXPECT_EQ(queried.value->result_id(), committed.value->settlement_id);

  const auto observer_rooms = server.observer_store()->rooms_json();
  ASSERT_EQ(observer_rooms.at("total"), 1);
  EXPECT_EQ(observer_rooms.at("items").at(0).at("state"), "Closed");
  EXPECT_EQ(server.observer_store()->tickets_json().at("total"), 2);
  const auto observer_rankings = server.observer_store()->rankings_json();
  EXPECT_EQ(observer_rankings.at("settlements").size(), 1U);
  EXPECT_EQ(observer_rankings.at("leaderboards").size(), 1U);
  const auto trace_events = server.observer_store()->events_json(0, 100, first_found.match_id());
  EXPECT_GE(trace_events.at("items").size(), 5U);
}

TEST(GameFlowTest, RejectsDuplicateQueueAndCancelsIdempotently) {
  RunningGameServer server;
  GameClient client(server.port(), "player-1");
  const auto queue_id = client.send(wire::QUEUE_MATCH, queue_command(1000));
  const auto queued_response = client.receive_response(queue_id);
  auto queued = protocol::parse_payload<wire::MatchQueued>(queued_response);
  ASSERT_TRUE(queued.ok());

  const auto duplicate_id = client.send(wire::QUEUE_MATCH, queue_command(1000));
  parse_error(client.receive_response(duplicate_id), "MATCH_ALREADY_QUEUED");

  wire::CancelMatch cancel;
  cancel.set_ticket_id(queued.value->ticket_id());
  const auto cancel_id = client.send(wire::CANCEL_MATCH, cancel);
  EXPECT_EQ(client.receive_response(cancel_id).message_type(), wire::MATCH_CANCELLED);
  const auto repeat_id = client.send(wire::CANCEL_MATCH, cancel);
  EXPECT_EQ(client.receive_response(repeat_id).message_type(), wire::MATCH_CANCELLED);
  EXPECT_EQ(server.game_service()->queued_ticket_count(), 0U);
}

TEST(GameFlowTest, ExpiresTicketAndNotifiesOwner) {
  RunningGameServer server;
  GameClient client(server.port(), "player-1");
  const auto queue_id = client.send(wire::QUEUE_MATCH, queue_command(1000, 1'000));
  EXPECT_EQ(client.receive_response(queue_id).message_type(), wire::MATCH_QUEUED);
  const auto expired = client.receive_push(wire::MATCH_CANCELLED);
  auto payload = protocol::parse_payload<wire::MatchCancelled>(expired);
  ASSERT_TRUE(payload.ok());
  EXPECT_EQ(payload.value->state(), "Expired");
  EXPECT_EQ(server.game_service()->queued_ticket_count(), 0U);
}

TEST(GameFlowTest, ExpandsOlderTicketWindowDeterministically) {
  RunningGameServer server;
  GameClient first(server.port(), "player-1");
  GameClient second(server.port(), "player-2");
  auto first_command = queue_command(1000, 3'000);
  auto second_command = queue_command(1125, 3'000);
  EXPECT_EQ(first.receive_response(first.send(wire::QUEUE_MATCH, first_command)).message_type(), wire::MATCH_QUEUED);
  EXPECT_EQ(second.receive_response(second.send(wire::QUEUE_MATCH, second_command)).message_type(), wire::MATCH_QUEUED);
  const auto first_found = parse_match_found(first.receive_push(wire::MATCH_FOUND));
  const auto second_found = parse_match_found(second.receive_push(wire::MATCH_FOUND));
  EXPECT_EQ(first_found.match_id(), second_found.match_id());
}

TEST(GameFlowTest, KeepsTicketsQueuedWhenRoomCapacityIsUnavailable) {
  GameServiceOptions options;
  options.maximum_active_rooms = 0;
  RunningGameServer server(options);
  GameClient first(server.port(), "player-1");
  GameClient second(server.port(), "player-2");
  EXPECT_EQ(first.receive_response(first.send(wire::QUEUE_MATCH, queue_command(1000))).message_type(),
            wire::MATCH_QUEUED);
  EXPECT_EQ(second.receive_response(second.send(wire::QUEUE_MATCH, queue_command(1000))).message_type(),
            wire::MATCH_QUEUED);
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(server.game_service()->queued_ticket_count(), 2U);
  EXPECT_EQ(server.game_service()->active_room_count(), 0U);
}

TEST(GameFlowTest, RejectsJoinTokenAfterItsDeadline) {
  GameServiceOptions options;
  options.join_token_ttl = 100ms;
  RunningGameServer server(options);
  GameClient first(server.port(), "player-1");
  GameClient second(server.port(), "player-2");
  first.receive_response(first.send(wire::QUEUE_MATCH, queue_command(1000)));
  second.receive_response(second.send(wire::QUEUE_MATCH, queue_command(1000)));
  const auto found = parse_match_found(first.receive_push(wire::MATCH_FOUND));
  second.receive_push(wire::MATCH_FOUND);
  std::this_thread::sleep_for(150ms);

  wire::JoinRoom join;
  join.set_match_id(found.match_id());
  join.set_room_id(found.room_id());
  join.set_room_server(found.room_server());
  join.set_player_id(first.player_id());
  join.set_join_token(found.join_token());
  const auto join_id = first.send(wire::JOIN_ROOM, join);
  parse_error(first.receive_response(join_id), "ROOM_JOIN_TOKEN_INVALID");
}

TEST(GameFlowTest, KeepsRoomMembershipAcrossSessionReconnect) {
  RunningGameServer server;
  GameClient first(server.port(), "player-1");
  GameClient second(server.port(), "player-2");
  first.receive_response(first.send(wire::QUEUE_MATCH, queue_command(1000)));
  second.receive_response(second.send(wire::QUEUE_MATCH, queue_command(1000)));
  const auto first_found = parse_match_found(first.receive_push(wire::MATCH_FOUND));
  const auto second_found = parse_match_found(second.receive_push(wire::MATCH_FOUND));

  wire::JoinRoom first_join;
  first_join.set_match_id(first_found.match_id());
  first_join.set_room_id(first_found.room_id());
  first_join.set_room_server(first_found.room_server());
  first_join.set_player_id(first.player_id());
  first_join.set_join_token(first_found.join_token());
  first.receive_response(first.send(wire::JOIN_ROOM, first_join));

  wire::JoinRoom second_join;
  second_join.set_match_id(second_found.match_id());
  second_join.set_room_id(second_found.room_id());
  second_join.set_room_server(second_found.room_server());
  second_join.set_player_id(second.player_id());
  second_join.set_join_token(second_found.join_token());
  second.receive_response(second.send(wire::JOIN_ROOM, second_join));

  const auto reconnect_token = first.reconnect_token();
  first.close();
  std::this_thread::sleep_for(30ms);
  GameClient resumed(server.port(), "player-1", reconnect_token);
  wire::ReadyRoom ready;
  ready.set_room_id(first_found.room_id());
  EXPECT_EQ(resumed.receive_response(resumed.send(wire::READY_ROOM, ready)).message_type(),
            wire::ROOM_STATE_CHANGED);
  const auto final_response = second.receive_response(second.send(wire::READY_ROOM, ready));
  auto final_state = protocol::parse_payload<wire::RoomStateChanged>(final_response);
  ASSERT_TRUE(final_state.ok());
  EXPECT_EQ(final_state.value->state(), "AwaitingResult");
  const auto committed = server.submit_result(first_found, resumed.player_id(), second.player_id());
  ASSERT_TRUE(committed.ok()) << committed.error.message;
  EXPECT_EQ(resumed.receive_room_state("Closed").winner_id(), resumed.player_id());
  EXPECT_EQ(second.receive_room_state("Closed").winner_id(), resumed.player_id());
}

TEST(GameFlowTest, GracefulStopDrainsQueuedAndJoinedStatesIdempotently) {
  RunningGameServer server;
  GameClient first(server.port(), "stop-player-1");
  GameClient second(server.port(), "stop-player-2");
  GameClient queued(server.port(), "stop-queued");

  EXPECT_EQ(first.receive_response(first.send(wire::QUEUE_MATCH, queue_command(1000))).message_type(),
            wire::MATCH_QUEUED);
  EXPECT_EQ(second.receive_response(second.send(wire::QUEUE_MATCH, queue_command(1000))).message_type(),
            wire::MATCH_QUEUED);
  const auto first_found = parse_match_found(first.receive_push(wire::MATCH_FOUND));
  const auto second_found = parse_match_found(second.receive_push(wire::MATCH_FOUND));

  wire::JoinRoom first_join;
  first_join.set_match_id(first_found.match_id());
  first_join.set_room_id(first_found.room_id());
  first_join.set_room_server(first_found.room_server());
  first_join.set_player_id(first.player_id());
  first_join.set_join_token(first_found.join_token());
  EXPECT_EQ(first.receive_response(first.send(wire::JOIN_ROOM, first_join)).message_type(), wire::ROOM_JOINED);

  wire::JoinRoom second_join;
  second_join.set_match_id(second_found.match_id());
  second_join.set_room_id(second_found.room_id());
  second_join.set_room_server(second_found.room_server());
  second_join.set_player_id(second.player_id());
  second_join.set_join_token(second_found.join_token());
  EXPECT_EQ(second.receive_response(second.send(wire::JOIN_ROOM, second_join)).message_type(), wire::ROOM_JOINED);

  EXPECT_EQ(queued.receive_response(queued.send(wire::QUEUE_MATCH, queue_command(5000))).message_type(),
            wire::MATCH_QUEUED);
  EXPECT_TRUE(server.stop_and_wait_for_connections());
}

}  // namespace
}  // namespace arena_core::game

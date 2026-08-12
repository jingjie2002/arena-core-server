#include "arena_core/qa/robot_scenario.hpp"

#include "arena_core/core/version.hpp"
#include "arena_core/protocol/frame.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <boost/asio.hpp>

#include "arena_core.pb.h"

namespace arena_core::qa {
namespace {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;
namespace wire = arena_core::protocol::v1;
using SteadyClock = std::chrono::steady_clock;

Error scenario_error(std::string message) {
  return make_error(ErrorCode::kConfigValidationFailed, std::move(message));
}

std::uint64_t unix_time_ms() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string player_name(const RobotScenario& scenario, const std::uint32_t client_index) {
  std::ostringstream stream;
  stream << "robot-" << scenario.seed << "-p" << std::setw(4) << std::setfill('0')
         << client_index;
  return stream.str();
}

wire::Envelope read_envelope(tcp::socket& socket) {
  std::array<std::uint8_t, 4> header{};
  asio::read(socket, asio::buffer(header));
  auto length = protocol::decode_frame_length(header);
  if (!length.ok()) {
    throw std::runtime_error(length.error.message);
  }
  std::string payload(*length.value, '\0');
  asio::read(socket, asio::buffer(payload));
  auto envelope = protocol::parse_envelope(payload);
  if (!envelope.ok()) {
    throw std::runtime_error(envelope.error.message);
  }
  return std::move(*envelope.value);
}

void write_envelope(tcp::socket& socket, const wire::Envelope& envelope) {
  auto frame = protocol::frame_message(envelope);
  if (!frame.ok()) {
    throw std::runtime_error(frame.error.message);
  }
  asio::write(socket, asio::buffer(*frame.value));
}

std::string describe_response(const wire::Envelope& envelope) {
  if (envelope.message_type() != wire::ERROR_RESPONSE) {
    return wire::MessageType_Name(envelope.message_type());
  }
  auto error = protocol::parse_payload<wire::ErrorResponse>(envelope);
  if (!error.ok()) {
    return "invalid ERROR_RESPONSE";
  }
  return error.value->code() + ": " + error.value->message();
}

void require_type(const wire::Envelope& envelope,
                  const wire::MessageType expected,
                  const std::string& step) {
  if (envelope.message_type() != expected) {
    throw std::runtime_error(step + " expected " + wire::MessageType_Name(expected) +
                             ", got " + describe_response(envelope));
  }
}

template <typename Message>
Message parse_payload_or_throw(const wire::Envelope& envelope, const std::string& step) {
  auto parsed = protocol::parse_payload<Message>(envelope);
  if (!parsed.ok()) {
    throw std::runtime_error(step + " payload invalid: " + parsed.error.message);
  }
  return std::move(*parsed.value);
}

class RobotClient final {
 public:
  RobotClient(asio::io_context& io_context,
              const std::string& host,
              const std::uint16_t port,
              std::string player_id)
      : player_id_(std::move(player_id)), socket_(io_context) {
    tcp::resolver resolver(io_context);
    asio::connect(socket_, resolver.resolve(host, std::to_string(port)));

    wire::Hello hello;
    hello.set_player_id(player_id_);
    const auto request_id = send_without_session(wire::HELLO, hello);
    const auto response = receive_response(request_id);
    require_type(response, wire::HELLO_ACK, "hello");
    const auto ack = parse_payload_or_throw<wire::HelloAck>(response, "hello");
    session_token_ = ack.session_token();
  }

  template <typename Message>
  std::uint64_t send(const wire::MessageType type, const Message& payload) {
    wire::Envelope envelope;
    envelope.set_protocol_version(1);
    envelope.set_message_type(type);
    envelope.set_request_id(next_request_id_++);
    envelope.set_session_token(session_token_);
    protocol::set_payload(envelope, payload);
    write_envelope(socket_, envelope);
    return envelope.request_id();
  }

  wire::Envelope receive_response(const std::uint64_t request_id) {
    for (;;) {
      auto envelope = read_envelope(socket_);
      if (envelope.request_id() == request_id) {
        return envelope;
      }
      pending_.push_back(std::move(envelope));
    }
  }

  wire::Envelope receive_push(const wire::MessageType type) {
    const auto stored = std::find_if(pending_.begin(), pending_.end(),
        [type](const wire::Envelope& envelope) {
          return envelope.request_id() == 0U && envelope.message_type() == type;
        });
    if (stored != pending_.end()) {
      auto result = std::move(*stored);
      pending_.erase(stored);
      return result;
    }
    for (;;) {
      auto envelope = read_envelope(socket_);
      if (envelope.request_id() == 0U && envelope.message_type() == type) {
        return envelope;
      }
      pending_.push_back(std::move(envelope));
    }
  }

  wire::RoomStateChanged receive_room_state(const std::string& expected_state) {
    for (;;) {
      auto envelope = receive_push(wire::ROOM_STATE_CHANGED);
      auto state = parse_payload_or_throw<wire::RoomStateChanged>(envelope, "room push");
      if (state.state() == expected_state) {
        return state;
      }
    }
  }

  void heartbeat(const std::uint64_t client_time_ms) {
    wire::Heartbeat heartbeat;
    heartbeat.set_client_time_ms(client_time_ms);
    const auto response = receive_response(send(wire::HEARTBEAT, heartbeat));
    require_type(response, wire::HEARTBEAT_ACK, "heartbeat");
  }

  const std::string& player_id() const noexcept { return player_id_; }

 private:
  template <typename Message>
  std::uint64_t send_without_session(const wire::MessageType type, const Message& payload) {
    wire::Envelope envelope;
    envelope.set_protocol_version(1);
    envelope.set_message_type(type);
    envelope.set_request_id(next_request_id_++);
    protocol::set_payload(envelope, payload);
    write_envelope(socket_, envelope);
    return envelope.request_id();
  }

  std::string player_id_;
  tcp::socket socket_;
  std::string session_token_;
  std::uint64_t next_request_id_{1};
  std::vector<wire::Envelope> pending_;
};

wire::QueueMatch queue_command(const std::int32_t mmr, const std::uint32_t wait_ms) {
  wire::QueueMatch command;
  command.set_mode("duel");
  command.set_mmr(mmr);
  command.set_max_wait_ms(wait_ms);
  return command;
}

void trace_step(const RobotScenario& scenario,
                const std::uint32_t pair_index,
                const std::string& step) {
  if (scenario.pair_count == 1U) {
    std::cerr << "robot pair=" << pair_index << " step=" << step << '\n';
  }
}

wire::MatchFound receive_match(RobotClient& client) {
  const auto envelope = client.receive_push(wire::MATCH_FOUND);
  return parse_payload_or_throw<wire::MatchFound>(envelope, "match found");
}

void join_room(RobotClient& client, const wire::MatchFound& found) {
  wire::JoinRoom command;
  command.set_match_id(found.match_id());
  command.set_room_id(found.room_id());
  command.set_room_server(found.room_server());
  command.set_player_id(client.player_id());
  command.set_join_token(found.join_token());
  const auto response = client.receive_response(client.send(wire::JOIN_ROOM, command));
  require_type(response, wire::ROOM_JOINED, "join room");
}

wire::RoomStateChanged ready_room(RobotClient& client, const std::string& room_id) {
  wire::ReadyRoom command;
  command.set_room_id(room_id);
  const auto response = client.receive_response(client.send(wire::READY_ROOM, command));
  require_type(response, wire::ROOM_STATE_CHANGED, "ready room");
  return parse_payload_or_throw<wire::RoomStateChanged>(response, "ready room");
}

wire::RankResult get_rank(RobotClient& client, const std::string& player_id) {
  wire::GetRank command;
  command.set_leaderboard("duel:season-1");
  command.set_player_id(player_id);
  command.set_neighbor_count(2);
  const auto response = client.receive_response(client.send(wire::GET_RANK, command));
  require_type(response, wire::RANK_RESULT, "get rank");
  auto result = parse_payload_or_throw<wire::RankResult>(response, "get rank");
  if (result.player().player_id() != player_id) {
    throw std::runtime_error("rank response belongs to a different player");
  }
  return result;
}

class RoomJoinBarrier final {
 public:
  void arrive_and_wait(const std::string& match_id) {
    std::unique_lock<std::mutex> lock(mutex_);
    auto& count = joined_[match_id];
    ++count;
    ready_.notify_all();
    if (!ready_.wait_for(lock, std::chrono::seconds(10), [&] { return joined_[match_id] == 2U; })) {
      throw std::runtime_error("opponent did not join the matched room within 10 seconds");
    }
  }

 private:
  std::mutex mutex_;
  std::condition_variable ready_;
  std::map<std::string, std::uint32_t> joined_;
};

struct ClientOutcome {
  std::uint32_t client_index{0};
  std::uint32_t round_index{0};
  std::string player_id;
  std::string opponent_id;
  std::string match_id;
  std::string room_id;
  std::string winner_id;
  std::string loser_id;
  std::int32_t own_score{0};
  std::string cache_state;
  std::uint64_t latency_us{0};
};

ClientOutcome run_client_round(const RobotScenario& scenario,
                               const std::uint32_t client_index,
                               const std::uint32_t round_index,
                               RobotClient& client,
                               RoomJoinBarrier& join_barrier) {
  const auto started = SteadyClock::now();
  const auto bucket = (client_index / 2U) % scenario.mmr_bucket_count;
  const auto mmr = static_cast<std::int32_t>(
      static_cast<std::int64_t>(scenario.mmr_base) +
      static_cast<std::int64_t>(bucket) * scenario.mmr_stride);

  trace_step(scenario, client_index, "queue");
  const auto queued = client.receive_response(
      client.send(wire::QUEUE_MATCH, queue_command(mmr, scenario.match_wait_ms)));
  require_type(queued, wire::MATCH_QUEUED, "queue");

  trace_step(scenario, client_index, "wait-match");
  const auto found = receive_match(client);
  if (found.opponent_id().empty() || found.opponent_id() == client.player_id() ||
      found.match_id().empty() || found.room_id().empty() || found.join_token().empty()) {
    throw std::runtime_error("match response contains invalid opponent or room credentials");
  }

  trace_step(scenario, client_index, "join");
  join_room(client, found);
  join_barrier.arrive_and_wait(found.match_id());

  trace_step(scenario, client_index, "ready");
  static_cast<void>(ready_room(client, found.room_id()));
  trace_step(scenario, client_index, "wait-closed-push");
  const auto closed = client.receive_room_state("Closed");
  if (closed.winner_id() == closed.loser_id() ||
      (closed.winner_id() != client.player_id() && closed.loser_id() != client.player_id())) {
    throw std::runtime_error("closed room result does not contain this player exactly once");
  }

  trace_step(scenario, client_index, "rank");
  const auto rank = get_rank(client, client.player_id());

  ClientOutcome outcome;
  outcome.client_index = client_index;
  outcome.round_index = round_index;
  outcome.player_id = client.player_id();
  outcome.opponent_id = found.opponent_id();
  outcome.match_id = found.match_id();
  outcome.room_id = found.room_id();
  outcome.winner_id = closed.winner_id();
  outcome.loser_id = closed.loser_id();
  outcome.own_score = rank.player().score();
  outcome.cache_state = rank.cache_state();
  outcome.latency_us = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(SteadyClock::now() - started).count());
  trace_step(scenario, client_index, "complete");
  return outcome;
}

std::vector<RobotOperation> aggregate_operations(std::vector<ClientOutcome> outcomes) {
  std::map<std::pair<std::uint32_t, std::string>, std::vector<ClientOutcome>> rooms;
  for (auto& outcome : outcomes) {
    rooms[{outcome.round_index, outcome.match_id}].push_back(std::move(outcome));
  }

  std::vector<RobotOperation> operations;
  operations.reserve(rooms.size());
  std::uint32_t current_round = std::numeric_limits<std::uint32_t>::max();
  std::uint32_t pair_index = 0;
  for (auto& [key, members] : rooms) {
    if (members.size() != 2U) {
      throw std::runtime_error("recorded match does not contain exactly two Robot outcomes");
    }
    std::sort(members.begin(), members.end(), [](const ClientOutcome& left, const ClientOutcome& right) {
      return left.player_id < right.player_id;
    });
    const auto& first = members[0];
    const auto& second = members[1];
    if (first.opponent_id != second.player_id || second.opponent_id != first.player_id ||
        first.room_id != second.room_id || first.winner_id != second.winner_id ||
        first.loser_id != second.loser_id || first.cache_state != second.cache_state) {
      throw std::runtime_error("the two Robot views of a matched room are inconsistent");
    }
    if (current_round != key.first) {
      current_round = key.first;
      pair_index = 0;
    }
    const auto& winner = first.player_id == first.winner_id ? first : second;
    const auto& loser = first.player_id == first.loser_id ? first : second;
    operations.push_back({pair_index++, key.first, first.player_id, second.player_id,
                          key.second, first.room_id, first.winner_id, first.loser_id,
                          winner.own_score, loser.own_score, first.cache_state,
                          std::max(first.latency_us, second.latency_us)});
  }
  return operations;
}

nlohmann::json operation_json(const RobotOperation& operation) {
  return {{"pair_index", operation.pair_index},
          {"round_index", operation.round_index},
          {"first_player", operation.first_player},
          {"second_player", operation.second_player},
          {"match_id", operation.match_id},
          {"room_id", operation.room_id},
          {"winner_id", operation.winner_id},
          {"loser_id", operation.loser_id},
          {"winner_score", operation.winner_score},
          {"loser_score", operation.loser_score},
          {"cache_state", operation.cache_state},
          {"latency_us", operation.latency_us}};
}

RobotOperation parse_operation(const nlohmann::json& value) {
  RobotOperation operation;
  operation.pair_index = value.at("pair_index").get<std::uint32_t>();
  operation.round_index = value.at("round_index").get<std::uint32_t>();
  operation.first_player = value.at("first_player").get<std::string>();
  operation.second_player = value.at("second_player").get<std::string>();
  operation.match_id = value.at("match_id").get<std::string>();
  operation.room_id = value.at("room_id").get<std::string>();
  operation.winner_id = value.at("winner_id").get<std::string>();
  operation.loser_id = value.at("loser_id").get<std::string>();
  operation.winner_score = value.at("winner_score").get<std::int32_t>();
  operation.loser_score = value.at("loser_score").get<std::int32_t>();
  operation.cache_state = value.at("cache_state").get<std::string>();
  operation.latency_us = value.value("latency_us", 0ULL);
  return operation;
}

bool same_replay_result(const RobotOperation& expected, const RobotOperation& actual) {
  return expected.pair_index == actual.pair_index &&
         expected.round_index == actual.round_index &&
         expected.first_player == actual.first_player &&
         expected.second_player == actual.second_player &&
         expected.match_id == actual.match_id &&
         expected.room_id == actual.room_id &&
         expected.winner_id == actual.winner_id &&
         expected.loser_id == actual.loser_id &&
         expected.winner_score == actual.winner_score &&
         expected.loser_score == actual.loser_score &&
         expected.cache_state == actual.cache_state;
}

}  // namespace

Result<RobotScenario> parse_robot_scenario(const nlohmann::json& value) {
  try {
    RobotScenario scenario;
    scenario.name = value.value("name", scenario.name);
    scenario.host = value.value("host", scenario.host);
    const auto port = value.value("port", static_cast<std::uint32_t>(scenario.port));
    scenario.seed = value.value("seed", scenario.seed);
    scenario.pair_count = value.value("pair_count", scenario.pair_count);
    scenario.rounds_per_pair = value.value("rounds_per_pair", scenario.rounds_per_pair);
    scenario.duration_seconds = value.value("duration_seconds", scenario.duration_seconds);
    scenario.heartbeat_interval_ms = value.value("heartbeat_interval_ms", scenario.heartbeat_interval_ms);
    scenario.queue_stagger_ms = value.value("queue_stagger_ms", scenario.queue_stagger_ms);
    scenario.match_wait_ms = value.value("match_wait_ms", scenario.match_wait_ms);
    scenario.mmr_base = value.value("mmr_base", scenario.mmr_base);
    scenario.mmr_stride = value.value("mmr_stride", scenario.mmr_stride);
    scenario.mmr_bucket_count = value.value("mmr_bucket_count", scenario.mmr_bucket_count);

    if (scenario.name.empty() || scenario.host.empty()) {
      return Result<RobotScenario>::failure(scenario_error("scenario name and host must not be empty"));
    }
    if (port == 0U || port > 65'535U) {
      return Result<RobotScenario>::failure(scenario_error("scenario port must be between 1 and 65535"));
    }
    scenario.port = static_cast<std::uint16_t>(port);
    if (scenario.pair_count == 0U || scenario.pair_count > 500U) {
      return Result<RobotScenario>::failure(scenario_error("pair_count must be between 1 and 500"));
    }
    if (scenario.rounds_per_pair == 0U && scenario.duration_seconds == 0U) {
      return Result<RobotScenario>::failure(
          scenario_error("rounds_per_pair or duration_seconds must request work"));
    }
    if (scenario.rounds_per_pair > 10'000U || scenario.duration_seconds > 3'600U) {
      return Result<RobotScenario>::failure(scenario_error("scenario duration or rounds exceed safety limits"));
    }
    if (scenario.heartbeat_interval_ms < 100U || scenario.heartbeat_interval_ms > 30'000U) {
      return Result<RobotScenario>::failure(
          scenario_error("heartbeat_interval_ms must be between 100 and 30000"));
    }
    if (scenario.queue_stagger_ms > 100U ||
        static_cast<std::uint64_t>(scenario.queue_stagger_ms) * scenario.pair_count * 2U > 30'000U) {
      return Result<RobotScenario>::failure(
          scenario_error("queue_stagger_ms must keep total startup staggering within 30 seconds"));
    }
    if (scenario.match_wait_ms < 100U || scenario.match_wait_ms > 60'000U) {
      return Result<RobotScenario>::failure(scenario_error("match_wait_ms must be between 100 and 60000"));
    }
    if (scenario.mmr_bucket_count == 0U || scenario.mmr_bucket_count > scenario.pair_count) {
      return Result<RobotScenario>::failure(
          scenario_error("mmr_bucket_count must be between 1 and pair_count"));
    }
    const auto last_mmr = static_cast<std::int64_t>(scenario.mmr_base) +
                          static_cast<std::int64_t>(scenario.mmr_bucket_count - 1U) * scenario.mmr_stride;
    if (scenario.mmr_base < 0 || scenario.mmr_stride < 0 || last_mmr > 10'000) {
      return Result<RobotScenario>::failure(
          scenario_error("MMR buckets must stay inside the service range 0..10000"));
    }
    return Result<RobotScenario>::success(std::move(scenario));
  } catch (const std::exception& error) {
    return Result<RobotScenario>::failure(scenario_error(std::string("invalid scenario JSON: ") + error.what()));
  }
}

Result<RobotScenario> load_robot_scenario(const std::string& path) {
  std::ifstream input(path);
  if (!input) {
    return Result<RobotScenario>::failure(scenario_error("cannot open robot scenario: " + path));
  }
  try {
    nlohmann::json value;
    input >> value;
    return parse_robot_scenario(value);
  } catch (const std::exception& error) {
    return Result<RobotScenario>::failure(scenario_error(std::string("cannot parse robot scenario: ") + error.what()));
  }
}

Result<RobotRunReport> load_robot_report(const std::string& path) {
  std::ifstream input(path);
  if (!input) {
    return Result<RobotRunReport>::failure(scenario_error("cannot open robot report: " + path));
  }
  try {
    nlohmann::json value;
    input >> value;
    if (value.value("schema_version", 0U) != 1U ||
        value.value("kind", std::string{}) != "arena-core-robot-report") {
      return Result<RobotRunReport>::failure(scenario_error("unsupported robot report schema"));
    }
    auto scenario = parse_robot_scenario(value.at("scenario"));
    if (!scenario.ok()) {
      return Result<RobotRunReport>::failure(std::move(scenario.error));
    }
    RobotRunReport report;
    report.scenario = std::move(*scenario.value);
    for (const auto& item : value.at("operations")) {
      report.operations.push_back(parse_operation(item));
    }
    return Result<RobotRunReport>::success(std::move(report));
  } catch (const std::exception& error) {
    return Result<RobotRunReport>::failure(scenario_error(std::string("invalid robot report: ") + error.what()));
  }
}

LatencySummary summarize_latencies(const std::vector<std::uint64_t>& latency_us) {
  if (latency_us.empty()) {
    return {};
  }
  auto sorted = latency_us;
  std::sort(sorted.begin(), sorted.end());
  const auto percentile = [&sorted](const double ratio) {
    const auto index = static_cast<std::size_t>(
        std::ceil(ratio * static_cast<double>(sorted.size())) - 1.0);
    return static_cast<double>(sorted[std::min(index, sorted.size() - 1U)]) / 1'000.0;
  };
  return {percentile(0.50), percentile(0.95), percentile(0.99),
          static_cast<double>(sorted.back()) / 1'000.0};
}

RobotRunReport run_robot_scenario(const RobotScenario& scenario) {
  RobotRunReport report;
  report.scenario = scenario;
  report.started_unix_ms = unix_time_ms();
  const auto client_count = scenario.pair_count * 2U;

  std::mutex gate_mutex;
  std::condition_variable gate_ready;
  std::condition_variable gate_start;
  std::uint32_t ready_workers = 0;
  bool start_workers = false;
  SteadyClock::time_point started;

  std::mutex result_mutex;
  asio::io_context network_context;
  std::atomic<std::uint64_t> successful_connections{0};
  std::atomic<std::uint64_t> heartbeat_count{0};
  std::atomic<bool> stop_requested{false};
  RoomJoinBarrier join_barrier;
  std::vector<ClientOutcome> outcomes;
  std::vector<std::thread> workers;
  workers.reserve(client_count);
  outcomes.reserve(static_cast<std::size_t>(client_count) * scenario.rounds_per_pair);

  for (std::uint32_t client_index = 0; client_index < client_count; ++client_index) {
    workers.emplace_back([&, client_index] {
      bool announced_ready = false;
      try {
        RobotClient client(network_context, scenario.host, scenario.port,
                           player_name(scenario, client_index));
        successful_connections.fetch_add(1, std::memory_order_relaxed);

        {
          std::unique_lock<std::mutex> lock(gate_mutex);
          ++ready_workers;
          announced_ready = true;
          gate_ready.notify_one();
          gate_start.wait(lock, [&] { return start_workers; });
        }
        if (stop_requested.load(std::memory_order_relaxed)) {
          return;
        }

        const auto deadline = started + std::chrono::seconds(scenario.duration_seconds);
        for (std::uint32_t round = 0; round < scenario.rounds_per_pair; ++round) {
          if (stop_requested.load(std::memory_order_relaxed)) {
            return;
          }
          if (scenario.queue_stagger_ms > 0U) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(static_cast<std::uint64_t>(scenario.queue_stagger_ms) * client_index));
          }
          auto outcome = run_client_round(scenario, client_index, round, client, join_barrier);
          std::lock_guard<std::mutex> lock(result_mutex);
          outcomes.push_back(std::move(outcome));
        }

        while (scenario.duration_seconds > 0U && SteadyClock::now() < deadline &&
               !stop_requested.load(std::memory_order_relaxed)) {
          const auto next_heartbeat = std::min(
              deadline, SteadyClock::now() + std::chrono::milliseconds(scenario.heartbeat_interval_ms));
          std::this_thread::sleep_until(next_heartbeat);
          if (SteadyClock::now() >= deadline) {
            break;
          }
          const auto client_time = static_cast<std::uint64_t>(
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  SteadyClock::now().time_since_epoch()).count());
          client.heartbeat(client_time);
          heartbeat_count.fetch_add(1, std::memory_order_relaxed);
        }
      } catch (const std::exception& error) {
        stop_requested.store(true, std::memory_order_relaxed);
        {
          std::lock_guard<std::mutex> lock(result_mutex);
          report.failures.push_back({client_index, error.what()});
        }
        if (!announced_ready) {
          std::lock_guard<std::mutex> lock(gate_mutex);
          ++ready_workers;
          gate_ready.notify_one();
        }
      }
    });
  }

  {
    std::unique_lock<std::mutex> lock(gate_mutex);
    gate_ready.wait(lock, [&] { return ready_workers == client_count; });
    started = SteadyClock::now();
    start_workers = true;
  }
  gate_start.notify_all();

  for (auto& worker : workers) {
    worker.join();
  }

  report.elapsed_ms = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now() - started).count());
  report.successful_connections = successful_connections.load(std::memory_order_relaxed);
  report.heartbeat_count = heartbeat_count.load(std::memory_order_relaxed);
  try {
    report.operations = aggregate_operations(std::move(outcomes));
  } catch (const std::exception& error) {
    report.failures.push_back({0U, error.what()});
  }
  std::sort(report.operations.begin(), report.operations.end(),
            [](const RobotOperation& left, const RobotOperation& right) {
              return std::tie(left.pair_index, left.round_index) <
                     std::tie(right.pair_index, right.round_index);
            });
  std::sort(report.failures.begin(), report.failures.end(),
            [](const RobotFailure& left, const RobotFailure& right) {
              return left.pair_index < right.pair_index;
            });
  std::vector<std::uint64_t> latencies;
  latencies.reserve(report.operations.size());
  for (const auto& operation : report.operations) {
    latencies.push_back(operation.latency_us);
  }
  report.latency = summarize_latencies(latencies);
  const auto expected_operations = static_cast<std::size_t>(scenario.pair_count) * scenario.rounds_per_pair;
  report.passed = report.failures.empty() &&
                  report.successful_connections == client_count &&
                  report.operations.size() == expected_operations;
  const auto elapsed_seconds = static_cast<double>(std::max<std::uint64_t>(1U, report.elapsed_ms)) / 1'000.0;
  report.throughput_rounds_per_second = static_cast<double>(report.operations.size()) / elapsed_seconds;
  return report;
}

Error verify_robot_replay(const RobotRunReport& expected, const RobotRunReport& actual) {
  if (!actual.passed) {
    return make_error(ErrorCode::kInternalError, "replayed robot scenario did not pass");
  }
  if (expected.operations.size() != actual.operations.size()) {
    return make_error(ErrorCode::kInternalError, "replay operation count differs from recording");
  }
  for (std::size_t index = 0; index < expected.operations.size(); ++index) {
    if (!same_replay_result(expected.operations[index], actual.operations[index])) {
      return make_error(ErrorCode::kInternalError,
                        "replay result differs at operation " + std::to_string(index));
    }
  }
  return {};
}

nlohmann::json robot_scenario_json(const RobotScenario& scenario) {
  return {{"name", scenario.name},
          {"host", scenario.host},
          {"port", scenario.port},
          {"seed", scenario.seed},
          {"pair_count", scenario.pair_count},
          {"rounds_per_pair", scenario.rounds_per_pair},
          {"duration_seconds", scenario.duration_seconds},
          {"heartbeat_interval_ms", scenario.heartbeat_interval_ms},
          {"queue_stagger_ms", scenario.queue_stagger_ms},
          {"match_wait_ms", scenario.match_wait_ms},
          {"mmr_base", scenario.mmr_base},
          {"mmr_stride", scenario.mmr_stride},
          {"mmr_bucket_count", scenario.mmr_bucket_count}};
}

nlohmann::json robot_report_json(const RobotRunReport& report) {
  nlohmann::json operations = nlohmann::json::array();
  for (const auto& operation : report.operations) {
    operations.push_back(operation_json(operation));
  }
  nlohmann::json failures = nlohmann::json::array();
  for (const auto& failure : report.failures) {
    failures.push_back({{"pair_index", failure.pair_index}, {"message", failure.message}});
  }
  nlohmann::json summary = {
      {"passed", report.passed},
      {"successful_connections", report.successful_connections},
      {"completed_rooms", report.operations.size()},
      {"heartbeat_count", report.heartbeat_count},
      {"business_errors", report.failures.size()},
      {"elapsed_ms", report.elapsed_ms},
      {"throughput_rounds_per_second", report.throughput_rounds_per_second},
      {"latency_ms", {{"p50", report.latency.p50_ms},
                       {"p95", report.latency.p95_ms},
                       {"p99", report.latency.p99_ms},
                       {"maximum", report.latency.maximum_ms}}}};
  if (!report.replay_source.empty()) {
    summary["replay_source"] = report.replay_source;
    summary["replay_verified"] = report.replay_verified;
  }
  return {{"schema_version", 1},
          {"kind", "arena-core-robot-report"},
          {"project_version", std::string(kProjectVersion)},
          {"started_unix_ms", report.started_unix_ms},
          {"scenario", robot_scenario_json(report.scenario)},
          {"summary", std::move(summary)},
          {"operations", std::move(operations)},
          {"failures", std::move(failures)}};
}

Error write_robot_report(const std::string& path, const RobotRunReport& report) {
  std::ofstream output(path, std::ios::trunc);
  if (!output) {
    return make_error(ErrorCode::kInternalError, "cannot write robot report: " + path);
  }
  output << robot_report_json(report).dump(2) << '\n';
  if (!output) {
    return make_error(ErrorCode::kInternalError, "robot report write failed: " + path);
  }
  return {};
}

}  // namespace arena_core::qa

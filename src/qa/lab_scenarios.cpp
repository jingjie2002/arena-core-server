#include "arena_core/qa/lab_scenarios.hpp"

#include "arena_core/protocol/frame.hpp"
#include "arena_core/qa/demo_battle_adapter.hpp"
#include "arena_core/qa/lab_protocol_client.hpp"
#include "arena_core/qa/toxiproxy_client.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace arena_core::qa {
namespace {

namespace wire = arena_core::protocol::v1;
using SteadyClock = std::chrono::steady_clock;

Error scenario_error(std::string message) {
  return make_error(ErrorCode::kQaScenarioInvalid, std::move(message));
}

Error cancelled_or_timed_out(const LabScenarioExecution& execution) {
  if (execution.cancellation && execution.cancellation->requested()) {
    return make_error(ErrorCode::kQaRunCancelled);
  }
  if (SteadyClock::now() >= execution.deadline) {
    return make_error(ErrorCode::kQaRunTimedOut);
  }
  return {};
}

Error pause(const LabScenarioExecution& execution, const std::uint32_t milliseconds) {
  if (const auto error = cancelled_or_timed_out(execution)) return error;
  if (milliseconds == 0U) return {};
  const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
      execution.deadline - SteadyClock::now());
  if (remaining <= std::chrono::milliseconds::zero()) {
    return make_error(ErrorCode::kQaRunTimedOut);
  }
  const auto requested = std::chrono::milliseconds(milliseconds);
  const auto delay = std::min(requested, remaining);
  if (execution.cancellation && execution.cancellation->wait_for(delay)) {
    return make_error(ErrorCode::kQaRunCancelled);
  }
  if (delay < requested || SteadyClock::now() >= execution.deadline) {
    return make_error(ErrorCode::kQaRunTimedOut);
  }
  return {};
}

std::uint32_t bounded_http_timeout_ms(const LabScenarioExecution& execution) {
  const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
      execution.deadline - SteadyClock::now());
  if (remaining <= std::chrono::milliseconds::zero()) return 1U;
  const auto value = std::min<std::int64_t>(5'000, remaining.count());
  return static_cast<std::uint32_t>(std::max<std::int64_t>(1, value));
}

Error error_from_response(const wire::Envelope& envelope) {
  if (envelope.message_type() != wire::ERROR_RESPONSE) {
    return scenario_error("server response has an unexpected message type");
  }
  auto payload = protocol::parse_payload<wire::ErrorResponse>(envelope);
  if (!payload.ok()) return payload.error;
  const auto& code = payload.value->code();
  const std::array<ErrorCode, 19> supported{
      ErrorCode::kProtocolRequestIdConflict,
      ErrorCode::kSessionTokenInvalid,
      ErrorCode::kSessionReconnectExpired,
      ErrorCode::kSessionNotActive,
      ErrorCode::kMatchAlreadyQueued,
      ErrorCode::kMatchTicketNotFound,
      ErrorCode::kRoomNotFound,
      ErrorCode::kRoomJoinTokenInvalid,
      ErrorCode::kRoomNotJoined,
      ErrorCode::kRoomInvalidState,
      ErrorCode::kSettlementNotAllowed,
      ErrorCode::kSettlementNotFound,
      ErrorCode::kSettlementConflict,
      ErrorCode::kSettlementAccessDenied,
      ErrorCode::kSettlementStorageFailed,
      ErrorCode::kRankPlayerNotFound,
      ErrorCode::kRankCacheUnavailable,
      ErrorCode::kDependencyRedisUnavailable,
      ErrorCode::kServerBusy};
  for (const auto candidate : supported) {
    if (code == error_code_name(candidate)) {
      auto error = make_error(candidate, payload.value->message());
      error.retryable = payload.value->retryable();
      return error;
    }
  }
  return scenario_error("server rejected the request: " + code);
}

Result<wire::Envelope> receive_expected(LabProtocolClient& client,
                                        const std::uint64_t request_id,
                                        const wire::MessageType expected) {
  auto response = client.receive_response(request_id);
  if (!response.ok()) return response;
  if (response.value->message_type() == wire::ERROR_RESPONSE) {
    return Result<wire::Envelope>::failure(error_from_response(*response.value));
  }
  if (response.value->message_type() != expected) {
    return Result<wire::Envelope>::failure(scenario_error(
        "expected " + wire::MessageType_Name(expected) + ", received " +
        wire::MessageType_Name(response.value->message_type())));
  }
  return response;
}

template <typename Command>
Result<wire::Envelope> transact(LabProtocolClient& client,
                                const wire::MessageType request_type,
                                const Command& command,
                                const wire::MessageType response_type) {
  auto prepared = client.prepare_request(request_type, command);
  if (!prepared.ok()) return Result<wire::Envelope>::failure(prepared.error);
  if (const auto error = client.send(*prepared.value)) {
    return Result<wire::Envelope>::failure(error);
  }
  return receive_expected(client, prepared.value->request_id(), response_type);
}

template <typename Payload>
Result<Payload> parse_expected_payload(const Result<wire::Envelope>& response) {
  if (!response.ok()) return Result<Payload>::failure(response.error);
  return protocol::parse_payload<Payload>(*response.value);
}

wire::QueueMatch queue_command(const std::int32_t mmr, const std::uint32_t wait_ms) {
  wire::QueueMatch command;
  command.set_mode("duel");
  command.set_mmr(mmr);
  command.set_max_wait_ms(wait_ms);
  return command;
}

wire::GetRank rank_command(const std::string& player_id) {
  wire::GetRank command;
  command.set_leaderboard("duel:season-1");
  command.set_player_id(player_id);
  command.set_neighbor_count(2U);
  return command;
}

struct RepeatedResponses {
  std::vector<wire::Envelope> responses;
  bool byte_identical{false};
};

Result<RepeatedResponses> repeat_request(LabProtocolClient& client,
                                         const LabPreparedEnvelope& prepared,
                                         const std::uint32_t count,
                                         const wire::MessageType expected) {
  RepeatedResponses result;
  result.responses.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    if (const auto error = client.send(prepared)) {
      return Result<RepeatedResponses>::failure(error);
    }
  }
  for (std::uint32_t index = 0; index < count; ++index) {
    auto response = receive_expected(client, prepared.request_id(), expected);
    if (!response.ok()) return Result<RepeatedResponses>::failure(response.error);
    result.responses.push_back(std::move(*response.value));
  }
  result.byte_identical = !result.responses.empty();
  if (result.byte_identical) {
    const auto first = result.responses.front().SerializeAsString();
    result.byte_identical = std::all_of(
        result.responses.begin() + 1, result.responses.end(),
        [&first](const wire::Envelope& response) {
          return response.SerializeAsString() == first;
        });
  }
  return Result<RepeatedResponses>::success(std::move(result));
}

class BasicMatchFlow final {
 public:
  BasicMatchFlow(const LabScenarioRequest& request,
                 const LabRunEnvironment& environment,
                 const LabScenarioExecution& execution,
                 const std::uint32_t pair_index)
      : request_(request), environment_(environment), execution_(execution), pair_index_(pair_index) {
    static std::atomic<std::uint64_t> nonce{
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count()) * 1'000U};
    unique_ = nonce.fetch_add(1U, std::memory_order_relaxed) + 1U;
    const auto prefix = "lab-" + std::to_string(request.seed) + '-' +
                        std::to_string(unique_) + '-' + std::to_string(pair_index);
    for (std::size_t index = 0; index < clients_.size(); ++index) {
      player_ids_[index] = prefix + "-p" + std::to_string(index);
      LabProtocolClientOptions options;
      options.operation_timeout = std::chrono::milliseconds(
          std::max<std::uint32_t>(1'000U, std::min<std::uint32_t>(15'000U, request.timeout_ms)));
      clients_[index] = std::make_unique<LabProtocolClient>(
          environment.player_host, environment.player_port, player_ids_[index],
          execution.cancellation, execution.deadline, options);
    }
  }

  LabProtocolClient& client(const std::size_t index) { return *clients_.at(index); }
  const std::string& player_id(const std::size_t index) const { return player_ids_.at(index); }
  const wire::MatchFound& found(const std::size_t index) const { return found_.at(index); }
  const wire::MatchQueued& queued(const std::size_t index) const { return queued_.at(index); }
  const DemoBattleResult& result() const { return result_; }

  Error connect_one(const std::size_t index) {
    if (const auto error = cancelled_or_timed_out(execution_)) return error;
    const auto error = clients_.at(index)->connect();
    if (error) return error;
    return cancelled_or_timed_out(execution_);
  }

  Error connect_both() {
    for (std::size_t index = 0; index < clients_.size(); ++index) {
      if (const auto error = connect_one(index)) return error;
    }
    return {};
  }

  Error queue_one(const std::size_t index, const std::int32_t mmr) {
    if (const auto error = cancelled_or_timed_out(execution_)) return error;
    auto parsed = parse_expected_payload<wire::MatchQueued>(transact(
        client(index), wire::QUEUE_MATCH, queue_command(mmr, request_.timeout_ms),
        wire::MATCH_QUEUED));
    if (!parsed.ok()) return parsed.error;
    if (parsed.value->ticket_id().empty()) {
      return scenario_error("MatchQueued did not contain a ticket id");
    }
    queued_[index] = std::move(*parsed.value);
    return {};
  }

  Error queue_both(const std::int32_t mmr) {
    if (const auto error = queue_one(0U, mmr)) return error;
    if (const auto error = queue_one(1U, mmr)) return error;
    return receive_matches();
  }

  Error queue_first_repeated(const std::int32_t mmr,
                             const std::uint32_t count,
                             bool& identical) {
    auto prepared = client(0U).prepare_request(
        wire::QUEUE_MATCH, queue_command(mmr, request_.timeout_ms));
    if (!prepared.ok()) return prepared.error;
    auto repeated = repeat_request(client(0U), *prepared.value, count, wire::MATCH_QUEUED);
    if (!repeated.ok()) return repeated.error;
    identical = repeated.value->byte_identical;
    auto parsed = protocol::parse_payload<wire::MatchQueued>(repeated.value->responses.front());
    if (!parsed.ok()) return parsed.error;
    if (parsed.value->ticket_id().empty()) {
      return scenario_error("replayed MatchQueued did not contain a ticket id");
    }
    queued_[0U] = std::move(*parsed.value);
    return {};
  }

  Error receive_matches() {
    for (std::size_t index = 0; index < clients_.size(); ++index) {
      if (const auto error = cancelled_or_timed_out(execution_)) return error;
      auto envelope = client(index).receive_push(wire::MATCH_FOUND);
      if (!envelope.ok()) return envelope.error;
      auto parsed = protocol::parse_payload<wire::MatchFound>(*envelope.value);
      if (!parsed.ok()) return parsed.error;
      found_[index] = std::move(*parsed.value);
    }
    if (found_[0U].match_id().empty() || found_[0U].room_id().empty() ||
        found_[0U].room_server().empty() || found_[0U].join_token().empty() ||
        found_[1U].join_token().empty() ||
        found_[0U].match_id() != found_[1U].match_id() ||
        found_[0U].room_id() != found_[1U].room_id() ||
        found_[0U].opponent_id() != player_ids_[1U] ||
        found_[1U].opponent_id() != player_ids_[0U] ||
        found_[0U].ticket_id() != queued_[0U].ticket_id() ||
        found_[1U].ticket_id() != queued_[1U].ticket_id()) {
      return scenario_error("MatchFound did not describe one consistent two-player room");
    }
    return {};
  }

  Error join_one(const std::size_t index) {
    wire::JoinRoom command;
    command.set_match_id(found_[index].match_id());
    command.set_room_id(found_[index].room_id());
    command.set_room_server(found_[index].room_server());
    command.set_player_id(player_ids_[index]);
    command.set_join_token(found_[index].join_token());
    auto joined = parse_expected_payload<wire::RoomJoined>(
        transact(client(index), wire::JOIN_ROOM, command, wire::ROOM_JOINED));
    if (!joined.ok()) return joined.error;
    if (joined.value->room_id() != found_[index].room_id()) {
      return scenario_error("RoomJoined belongs to a different room");
    }
    return {};
  }

  Error join_both() {
    if (const auto error = join_one(0U)) return error;
    return join_one(1U);
  }

  Error leave_one(const std::size_t index) {
    wire::LeaveRoom command;
    command.set_room_id(found_[index].room_id());
    auto left = parse_expected_payload<wire::RoomLeft>(
        transact(client(index), wire::LEAVE_ROOM, command, wire::ROOM_LEFT));
    if (!left.ok()) return left.error;
    if (left.value->room_id() != found_[index].room_id() ||
        left.value->state() != "Aborted") {
      return scenario_error("room cleanup did not reach Aborted");
    }
    return {};
  }

  Error ready_one(const std::size_t index, wire::RoomStateChanged& state) {
    wire::ReadyRoom command;
    command.set_room_id(found_[index].room_id());
    auto parsed = parse_expected_payload<wire::RoomStateChanged>(
        transact(client(index), wire::READY_ROOM, command, wire::ROOM_STATE_CHANGED));
    if (!parsed.ok()) return parsed.error;
    if (parsed.value->room_id() != found_[index].room_id()) {
      return scenario_error("Ready response belongs to a different room");
    }
    state = std::move(*parsed.value);
    return {};
  }

  Error ready_both(const std::uint32_t delay_ms = 0U) {
    if (const auto error = pause(execution_, delay_ms)) return error;
    wire::RoomStateChanged first;
    wire::RoomStateChanged second;
    if (const auto error = ready_one(0U, first)) return error;
    if (const auto error = ready_one(1U, second)) return error;
    if (second.state() != "AwaitingResult") {
      return scenario_error("the second Ready did not move the room to AwaitingResult");
    }
    auto first_final = client(0U).receive_room_state("AwaitingResult");
    if (!first_final.ok()) return first_final.error;
    return {};
  }

  Error ready_first_repeated(const std::uint32_t count, bool& identical) {
    wire::ReadyRoom command;
    command.set_room_id(found_[0U].room_id());
    auto prepared = client(0U).prepare_request(wire::READY_ROOM, command);
    if (!prepared.ok()) return prepared.error;
    auto repeated = repeat_request(client(0U), *prepared.value, count, wire::ROOM_STATE_CHANGED);
    if (!repeated.ok()) return repeated.error;
    identical = repeated.value->byte_identical;
    auto parsed = protocol::parse_payload<wire::RoomStateChanged>(
        repeated.value->responses.front());
    if (!parsed.ok()) return parsed.error;
    if (parsed.value->room_id() != found_[0U].room_id() ||
        parsed.value->ready_players_size() != 1) {
      return scenario_error("replayed Ready response does not show one ready player");
    }
    return {};
  }

  Error ready_second_after_replay() {
    wire::RoomStateChanged second;
    if (const auto error = ready_one(1U, second)) return error;
    if (second.state() != "AwaitingResult") {
      return scenario_error("room did not reach AwaitingResult after replayed Ready");
    }
    auto first_final = client(0U).receive_room_state("AwaitingResult");
    return first_final.ok() ? Error{} : first_final.error;
  }

  DemoBattleResult make_result() {
    const bool first_wins = (request_.seed + pair_index_) % 2U == 0U;
    result_.result_id = "lab-result-s" + std::to_string(request_.seed % 100'000'000ULL) +
                        '-' + std::to_string(unique_ % 1'000'000'000'000ULL) + '-' +
                        found_[0U].match_id();
    result_.match_id = found_[0U].match_id();
    result_.room_id = found_[0U].room_id();
    result_.winner_id = player_ids_[first_wins ? 0U : 1U];
    result_.loser_id = player_ids_[first_wins ? 1U : 0U];
    result_.rule_version = "score-v1";
    result_.finished_at = current_utc_rfc3339();
    return result_;
  }

  Result<DemoBattleReceipt> submit_result() {
    if (const auto error = cancelled_or_timed_out(execution_)) {
      return Result<DemoBattleReceipt>::failure(error);
    }
    DemoBattleAdapter adapter({environment_.result_host, environment_.result_port,
                               environment_.result_token,
                               bounded_http_timeout_ms(execution_)});
    if (result_.result_id.empty()) make_result();
    auto receipt = adapter.submit_with_receipt(result_);
    if (!receipt.ok()) return receipt;
    if (const auto error = cancelled_or_timed_out(execution_)) {
      return Result<DemoBattleReceipt>::failure(error);
    }
    if (receipt.value->result_id != result_.result_id ||
        receipt.value->match_id != result_.match_id ||
        receipt.value->room_id != result_.room_id ||
        receipt.value->winner_id != result_.winner_id ||
        receipt.value->loser_id != result_.loser_id) {
      return Result<DemoBattleReceipt>::failure(
          scenario_error("trusted result receipt differs from the submitted result"));
    }
    return receipt;
  }

  Error receive_settlement() {
    if (result_.result_id.empty()) {
      return scenario_error("result must be constructed before settlement pushes are read");
    }
    for (std::size_t index = 0; index < clients_.size(); ++index) {
      auto envelope = client(index).receive_push(wire::MATCH_RESULT_COMMITTED);
      if (!envelope.ok()) return envelope.error;
      auto committed = protocol::parse_payload<wire::MatchResultView>(*envelope.value);
      if (!committed.ok()) return committed.error;
      if (committed.value->result_id() != result_.result_id ||
          committed.value->match_id() != result_.match_id ||
          committed.value->room_id() != result_.room_id ||
          committed.value->winner_id() != result_.winner_id ||
          committed.value->loser_id() != result_.loser_id) {
        return scenario_error("MATCH_RESULT_COMMITTED differs from the trusted result");
      }
      committed_[index] = std::move(*committed.value);
    }
    for (std::size_t index = 0; index < clients_.size(); ++index) {
      auto closed = client(index).receive_room_state("Closed");
      if (!closed.ok()) return closed.error;
      if (closed.value->winner_id() != result_.winner_id ||
          closed.value->loser_id() != result_.loser_id) {
        return scenario_error("Closed room state differs from the trusted result");
      }
      closed_[index] = std::move(*closed.value);
    }
    return {};
  }

  Result<wire::RankResult> query_rank(const std::size_t index) {
    auto response = transact(client(index), wire::GET_RANK, rank_command(player_ids_[index]),
                             wire::RANK_RESULT);
    auto rank = parse_expected_payload<wire::RankResult>(response);
    if (rank.ok() && rank.value->player().player_id() != player_ids_[index]) {
      return Result<wire::RankResult>::failure(
          scenario_error("rank response belongs to a different player"));
    }
    return rank;
  }

  Result<wire::MatchResultView> query_match_result(const std::size_t index) {
    wire::GetMatchResult command;
    command.set_match_id(found_[index].match_id());
    auto result = parse_expected_payload<wire::MatchResultView>(transact(
        client(index), wire::GET_MATCH_RESULT, command, wire::MATCH_RESULT));
    if (result.ok() && result.value->result_id() != result_.result_id) {
      return Result<wire::MatchResultView>::failure(
          scenario_error("match-result query returned a different settlement"));
    }
    return result;
  }

  Result<RepeatedResponses> repeat_rank(const std::size_t index,
                                        const std::uint32_t count) {
    auto prepared = client(index).prepare_request(
        wire::GET_RANK, rank_command(player_ids_[index]));
    if (!prepared.ok()) return Result<RepeatedResponses>::failure(prepared.error);
    return repeat_request(client(index), *prepared.value, count, wire::RANK_RESULT);
  }

  Result<RepeatedResponses> repeat_match_result(const std::size_t index,
                                                const std::uint32_t count) {
    wire::GetMatchResult command;
    command.set_match_id(found_[index].match_id());
    auto prepared = client(index).prepare_request(wire::GET_MATCH_RESULT, command);
    if (!prepared.ok()) return Result<RepeatedResponses>::failure(prepared.error);
    return repeat_request(client(index), *prepared.value, count, wire::MATCH_RESULT);
  }

 private:
  const LabScenarioRequest& request_;
  const LabRunEnvironment& environment_;
  const LabScenarioExecution& execution_;
  std::uint32_t pair_index_{0U};
  std::uint64_t unique_{0U};
  std::array<std::unique_ptr<LabProtocolClient>, 2> clients_;
  std::array<std::string, 2> player_ids_;
  std::array<wire::MatchQueued, 2> queued_;
  std::array<wire::MatchFound, 2> found_;
  DemoBattleResult result_;
  std::array<wire::MatchResultView, 2> committed_;
  std::array<wire::RoomStateChanged, 2> closed_;
};

nlohmann::json room_evidence(const BasicMatchFlow& flow,
                             const DemoBattleReceipt& receipt) {
  return {{"match_id", flow.found(0U).match_id()},
          {"room_id", flow.found(0U).room_id()},
          {"players", {flow.player_id(0U), flow.player_id(1U)}},
          {"result_id", receipt.result_id},
          {"receipt_status", receipt.status},
          {"cache_state", receipt.cache_state}};
}

bool receipt_has_scores(const DemoBattleReceipt& receipt,
                        const wire::RankResult& first,
                        const wire::RankResult& second) {
  if (!receipt.body.contains("players") || !receipt.body.at("players").is_array() ||
      receipt.body.at("players").size() != 2U) {
    return false;
  }
  std::map<std::string, std::int32_t> scores;
  for (const auto& player : receipt.body.at("players")) {
    if (!player.is_object() || !player.contains("player_id") ||
        !player.contains("after_score") || !player.at("player_id").is_string() ||
        !player.at("after_score").is_number_integer()) {
      return false;
    }
    scores[player.at("player_id").get<std::string>()] =
        player.at("after_score").get<std::int32_t>();
  }
  const auto first_found = scores.find(first.player().player_id());
  const auto second_found = scores.find(second.player().player_id());
  return first_found != scores.end() && second_found != scores.end() &&
         first_found->second == first.player().score() &&
         second_found->second == second.player().score();
}

Error complete_match_pair(BasicMatchFlow& flow,
                          const std::int32_t mmr,
                          const std::uint32_t ready_delay_ms,
                          DemoBattleReceipt& receipt,
                          std::array<wire::RankResult, 2>& ranks) {
  if (const auto error = flow.queue_both(mmr)) return error;
  if (const auto error = flow.join_both()) return error;
  if (const auto error = flow.ready_both(ready_delay_ms)) return error;
  flow.make_result();
  auto submitted = flow.submit_result();
  if (!submitted.ok()) return submitted.error;
  if (submitted.value->http_status != 201U || submitted.value->idempotent) {
    return scenario_error("first trusted result submission was not a newly created settlement");
  }
  receipt = std::move(*submitted.value);
  if (const auto error = flow.receive_settlement()) return error;
  for (std::size_t index = 0; index < ranks.size(); ++index) {
    auto rank = flow.query_rank(index);
    if (!rank.ok()) return rank.error;
    ranks[index] = std::move(*rank.value);
  }
  auto match_result = flow.query_match_result(0U);
  if (!match_result.ok()) return match_result.error;
  return {};
}

Error run_complete(const LabScenarioRequest& request,
                   const LabRunEnvironment& environment,
                   const LabScenarioExecution& execution) {
  const auto player_count = request.parameters.at("player_count").get<std::uint32_t>();
  const auto pair_count = player_count / 2U;
  const auto mmr_base = request.parameters.at("mmr_base").get<std::int32_t>();
  const auto stair = request.parameters.at("mmr_mode").get<std::string>() == "stair";
  const auto mmr_stride = request.parameters.at("mmr_stride").get<std::int32_t>();
  const auto connection_interval =
      request.parameters.at("connect_interval_ms").get<std::uint32_t>();
  const auto ready_delay = request.parameters.at("ready_delay_ms").get<std::uint32_t>();

  execution.reporter->event("scenario.started", "开始真实连接玩家并逐对完成比赛",
                            {{"player_count", player_count}, {"pair_count", pair_count}});
  std::vector<std::unique_ptr<BasicMatchFlow>> flows;
  flows.reserve(pair_count);
  nlohmann::json rooms = nlohmann::json::array();
  std::set<std::string> room_ids;
  std::set<std::string> result_ids;
  std::uint32_t successful_connections = 0U;
  std::uint32_t fresh_rank_queries = 0U;
  auto next_connection = SteadyClock::now();

  for (std::uint32_t pair = 0U; pair < pair_count; ++pair) {
    auto flow = std::make_unique<BasicMatchFlow>(request, environment, execution, pair);
    for (std::size_t player = 0U; player < 2U; ++player) {
      const auto now = SteadyClock::now();
      if (now < next_connection) {
        const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(
            next_connection - now).count();
        if (const auto error = pause(execution, static_cast<std::uint32_t>(
                std::max<std::int64_t>(0, wait)))) return error;
      }
      if (const auto error = flow->connect_one(player)) return error;
      ++successful_connections;
      next_connection = SteadyClock::now() + std::chrono::milliseconds(connection_interval);
    }

    const auto pair_mmr = static_cast<std::int32_t>(
        static_cast<std::int64_t>(mmr_base) +
        (stair ? static_cast<std::int64_t>(pair) * mmr_stride : 0));
    DemoBattleReceipt receipt;
    std::array<wire::RankResult, 2> ranks;
    if (const auto error = complete_match_pair(*flow, pair_mmr, ready_delay,
                                               receipt, ranks)) {
      return error;
    }
    for (const auto& rank : ranks) {
      if (rank.cache_state() == "fresh") ++fresh_rank_queries;
    }
    room_ids.insert(flow->found(0U).room_id());
    result_ids.insert(receipt.result_id);
    rooms.push_back(room_evidence(*flow, receipt));
    execution.reporter->event("scenario.pair_completed", "一组玩家已完成匹配、结算和排行查询",
                              {{"pair", pair + 1U},
                               {"room_id", flow->found(0U).room_id()},
                               {"cache_state", receipt.cache_state}});
    flows.push_back(std::move(flow));
  }

  const bool connections_ok = successful_connections == player_count;
  const bool rooms_ok = room_ids.size() == pair_count;
  const bool settlements_ok = result_ids.size() == pair_count;
  const bool ranking_ok = fresh_rank_queries == player_count;
  execution.reporter->assertion("connections", "全部玩家都完成真实 TCP Hello",
                                player_count, successful_connections, connections_ok);
  execution.reporter->assertion("rooms", "每一对玩家只进入一个独立房间",
                                pair_count, room_ids.size(), rooms_ok);
  execution.reporter->assertion("settlements", "每个房间只生成一个结算事实",
                                pair_count, result_ids.size(), settlements_ok);
  execution.reporter->assertion("ranking", "结算后每名玩家都读到新鲜排行",
                                player_count, fresh_rank_queries, ranking_ok);
  execution.reporter->evidence("rooms", std::move(rooms));
  if (!connections_ok || !rooms_ok || !settlements_ok || !ranking_ok) {
    return scenario_error("完整对局的连接、房间、结算或排行断言失败");
  }
  return {};
}

Error resume_at_selected_stage(BasicMatchFlow& flow,
                               const std::size_t player_index,
                               const std::uint32_t delay_ms,
                               const bool inside_window,
                               const LabScenarioExecution& execution) {
  auto& client = flow.client(player_index);
  const auto previous_generation = client.generation();
  if (const auto error = client.disconnect()) return error;
  execution.reporter->event("scenario.disconnected", "目标玩家的旧 TCP 连接已关闭",
                            {{"player_index", player_index}, {"delay_ms", delay_ms}});
  if (const auto error = pause(execution, delay_ms)) return error;

  auto reconnect_error = client.reconnect();
  if (inside_window && reconnect_error.code == ErrorCode::kSessionTokenInvalid) {
    const auto settle_deadline = std::min(execution.deadline,
                                         SteadyClock::now() + std::chrono::milliseconds(200));
    while (reconnect_error.code == ErrorCode::kSessionTokenInvalid &&
           SteadyClock::now() < settle_deadline) {
      if (const auto error = pause(execution, 10U)) return error;
      reconnect_error = client.reconnect();
    }
  }
  if (!inside_window) {
    const bool expired = reconnect_error.code == ErrorCode::kSessionReconnectExpired;
    execution.reporter->assertion("reconnect_expired", "窗口外重连返回明确的过期错误",
                                  "SESSION_RECONNECT_EXPIRED",
                                  std::string(error_code_name(reconnect_error.code)), expired);
    execution.reporter->evidence(
        "reconnect",
        {{"player_index", player_index}, {"delay_ms", delay_ms},
         {"outcome", expired ? "expired_as_expected" : "unexpected_response"}});
    return expired ? Error{} : scenario_error(
        "outside-window reconnect did not return SESSION_RECONNECT_EXPIRED");
  }
  if (reconnect_error) return reconnect_error;
  const bool metadata_ok = client.connected() && client.resumed() &&
                           client.credentials_rotated() &&
                           client.generation() == previous_generation + 1U;
  execution.reporter->assertion("reconnect_metadata", "窗口内恢复身份并递增 generation",
                                {{"resumed", true}, {"credentials_rotated", true},
                                 {"generation", previous_generation + 1U}},
                                {{"resumed", client.resumed()},
                                 {"credentials_rotated", client.credentials_rotated()},
                                 {"generation", client.generation()}}, metadata_ok);
  execution.reporter->event("scenario.reconnected", "新 TCP 连接已接管玩家会话",
                            {{"player_index", player_index},
                             {"generation", client.generation()}});
  execution.reporter->evidence(
      "reconnect",
      {{"player_index", player_index}, {"delay_ms", delay_ms},
       {"outcome", "resumed"}, {"credentials_rotated", client.credentials_rotated()},
       {"generation", client.generation()}});
  return metadata_ok ? Error{} : scenario_error("reconnect metadata is inconsistent");
}

Error run_reconnect(const LabScenarioRequest& request,
                    const LabRunEnvironment& environment,
                    const LabScenarioExecution& execution) {
  const auto player_index = request.parameters.at("player_index").get<std::uint32_t>();
  const auto stage = request.parameters.at("disconnect_stage").get<std::string>();
  const auto delay = request.parameters.at("reconnect_delay_ms").get<std::uint32_t>();
  const bool inside = delay + 250U <= environment.reconnect_window_ms;
  const bool outside = delay >= environment.reconnect_window_ms + 250U;
  if (!inside && !outside) {
    execution.reporter->assertion("stable_boundary", "重连延迟与窗口边界至少相距 250ms",
                                  true, false, false);
    return scenario_error("reconnect delay is too close to the configured boundary");
  }

  BasicMatchFlow flow(request, environment, execution, 0U);
  if (const auto error = flow.connect_both()) return error;
  execution.reporter->event("scenario.started", "两名真实玩家已连接，准备在指定阶段断线",
                            {{"stage", stage}, {"player_index", player_index}});

  if (stage == "before_queue") {
    if (const auto error = resume_at_selected_stage(flow, player_index, delay, inside, execution)) {
      return error;
    }
    if (outside) return {};
  }

  if (const auto error = flow.queue_both(1'000)) return error;
  if (stage == "after_match") {
    if (const auto error = resume_at_selected_stage(flow, player_index, delay, inside, execution)) {
      return error;
    }
    if (outside) {
      const auto remaining_player = player_index == 0U ? 1U : 0U;
      if (const auto error = flow.join_one(remaining_player)) return error;
      if (const auto error = flow.leave_one(remaining_player)) return error;
      execution.reporter->event("scenario.room_cleaned", "窗口外拒绝后，仍在线玩家已退出并关闭房间");
      return {};
    }
  }
  if (const auto error = flow.join_both()) return error;
  if (stage == "after_join") {
    if (const auto error = resume_at_selected_stage(flow, player_index, delay, inside, execution)) {
      return error;
    }
    if (outside) {
      const auto remaining_player = player_index == 0U ? 1U : 0U;
      if (const auto error = flow.leave_one(remaining_player)) return error;
      execution.reporter->event("scenario.room_cleaned", "窗口外拒绝后，仍在线玩家已退出并关闭房间");
      return {};
    }
  }
  if (const auto error = flow.ready_both()) return error;
  flow.make_result();
  auto receipt = flow.submit_result();
  if (!receipt.ok()) return receipt.error;
  if (const auto error = flow.receive_settlement()) return error;
  auto first_rank = flow.query_rank(0U);
  auto second_rank = flow.query_rank(1U);
  const bool completed = first_rank.ok() && second_rank.ok() &&
                         first_rank.value->cache_state() == "fresh" &&
                         second_rank.value->cache_state() == "fresh";
  execution.reporter->assertion("continued_match", "恢复后继续完成同一场比赛和结算",
                                true, completed, completed);
  execution.reporter->assertion("active_transport", "旧连接关闭后只有新 transport 继续业务",
                                true, flow.client(player_index).connected(),
                                flow.client(player_index).connected());
  execution.reporter->evidence("room", room_evidence(flow, *receipt.value));
  return completed ? Error{} : scenario_error("reconnected match did not complete");
}

Error finish_duplicate_request_flow(BasicMatchFlow& flow,
                                    DemoBattleReceipt& receipt) {
  flow.make_result();
  auto submitted = flow.submit_result();
  if (!submitted.ok()) return submitted.error;
  receipt = std::move(*submitted.value);
  return flow.receive_settlement();
}

Error run_duplicate_request(const LabScenarioRequest& request,
                            const LabRunEnvironment& environment,
                            const LabScenarioExecution& execution) {
  const auto message = request.parameters.at("message").get<std::string>();
  const auto repeat_count = request.parameters.at("repeat_count").get<std::uint32_t>();
  BasicMatchFlow flow(request, environment, execution, 0U);
  if (const auto error = flow.connect_both()) return error;
  bool byte_identical = false;
  DemoBattleReceipt receipt;

  if (message == "queue_match") {
    if (const auto error = flow.queue_first_repeated(1'000, repeat_count, byte_identical)) {
      return error;
    }
    if (const auto error = flow.queue_one(1U, 1'000)) return error;
    if (const auto error = flow.receive_matches()) return error;
    if (const auto error = flow.join_both()) return error;
    if (const auto error = flow.ready_both()) return error;
    if (const auto error = finish_duplicate_request_flow(flow, receipt)) return error;
  } else if (message == "ready_room") {
    if (const auto error = flow.queue_both(1'000)) return error;
    if (const auto error = flow.join_both()) return error;
    if (const auto error = flow.ready_first_repeated(repeat_count, byte_identical)) return error;
    if (const auto error = flow.ready_second_after_replay()) return error;
    if (const auto error = finish_duplicate_request_flow(flow, receipt)) return error;
  } else {
    if (const auto error = flow.queue_both(1'000)) return error;
    if (const auto error = flow.join_both()) return error;
    if (const auto error = flow.ready_both()) return error;
    if (const auto error = finish_duplicate_request_flow(flow, receipt)) return error;
    Result<RepeatedResponses> repeated = message == "get_rank"
        ? flow.repeat_rank(0U, repeat_count)
        : flow.repeat_match_result(0U, repeat_count);
    if (!repeated.ok()) return repeated.error;
    byte_identical = repeated.value->byte_identical;
  }

  const bool one_ticket = !flow.queued(0U).ticket_id().empty() &&
                          flow.found(0U).ticket_id() == flow.queued(0U).ticket_id();
  const bool one_room = !flow.found(0U).room_id().empty() &&
                        flow.found(0U).room_id() == flow.found(1U).room_id();
  const bool one_settlement = receipt.http_status == 201U && !receipt.idempotent &&
                              receipt.result_id == flow.result().result_id;
  execution.reporter->event("scenario.request_replayed", "同一个 request_id 和载荷已在真实连接上重复发送",
                            {{"message", message}, {"repeat_count", repeat_count}});
  execution.reporter->assertion("byte_identical", "每次重复请求返回完全相同的 Envelope 字节",
                                true, byte_identical, byte_identical);
  execution.reporter->assertion("single_ticket", "重复请求只产生一张目标票据",
                                true, one_ticket, one_ticket);
  execution.reporter->assertion("single_room", "后续业务只进入一个房间",
                                true, one_room, one_room);
  execution.reporter->assertion("single_settlement", "后续业务只创建一个结算事实",
                                true, one_settlement, one_settlement);
  execution.reporter->evidence("room", room_evidence(flow, receipt));
  return byte_identical && one_ticket && one_room && one_settlement
      ? Error{} : scenario_error("duplicate player request assertions failed");
}

bool same_receipt_core(const DemoBattleReceipt& first,
                       const DemoBattleReceipt& replay) {
  if (first.result_id != replay.result_id || first.match_id != replay.match_id ||
      first.room_id != replay.room_id || first.winner_id != replay.winner_id ||
      first.loser_id != replay.loser_id) {
    return false;
  }
  const auto first_fingerprint = first.body.value("fingerprint", std::string{});
  const auto replay_fingerprint = replay.body.value("fingerprint", std::string{});
  return !first_fingerprint.empty() && first_fingerprint == replay_fingerprint &&
         first.body.value("finished_at", std::string{}) ==
             replay.body.value("finished_at", std::string{}) &&
         first.body.value("rule_version", std::string{}) ==
             replay.body.value("rule_version", std::string{});
}

Error run_duplicate_result(const LabScenarioRequest& request,
                           const LabRunEnvironment& environment,
                           const LabScenarioExecution& execution) {
  const auto mode = request.parameters.at("mode").get<std::string>();
  const auto total_submissions = request.parameters.at("repeat_count").get<std::uint32_t>();
  BasicMatchFlow flow(request, environment, execution, 0U);
  if (const auto error = flow.connect_both()) return error;
  if (const auto error = flow.queue_both(1'000)) return error;
  if (const auto error = flow.join_both()) return error;
  if (const auto error = flow.ready_both()) return error;
  const auto original = flow.make_result();
  auto first = flow.submit_result();
  if (!first.ok()) return first.error;
  if (first.value->http_status != 201U || first.value->idempotent) {
    return scenario_error("the first result submission was not created exactly once");
  }

  DemoBattleAdapter adapter({environment.result_host, environment.result_port,
                             environment.result_token,
                             bounded_http_timeout_ms(execution)});
  std::uint32_t idempotent_replays = 0U;
  std::uint32_t conflicts = 0U;
  bool stable_receipt = true;
  for (std::uint32_t index = 1U; index < total_submissions; ++index) {
    if (const auto error = cancelled_or_timed_out(execution)) return error;
    if (mode == "exact") {
      auto replay = adapter.submit_with_receipt(original);
      if (!replay.ok()) return replay.error;
      const bool accepted = replay.value->http_status == 200U && replay.value->idempotent;
      if (accepted) ++idempotent_replays;
      stable_receipt = stable_receipt && accepted && same_receipt_core(*first.value, *replay.value);
    } else {
      auto conflicting = original;
      std::swap(conflicting.winner_id, conflicting.loser_id);
      auto replay = adapter.submit_with_receipt(conflicting);
      if (!replay.ok() && replay.error.code == ErrorCode::kSettlementConflict) {
        ++conflicts;
      } else {
        stable_receipt = false;
      }
    }
  }
  if (const auto error = flow.receive_settlement()) return error;
  auto first_rank = flow.query_rank(0U);
  auto second_rank = flow.query_rank(1U);
  if (!first_rank.ok()) return first_rank.error;
  if (!second_rank.ok()) return second_rank.error;
  const bool score_once = receipt_has_scores(*first.value, *first_rank.value, *second_rank.value);
  const auto expected_duplicates = total_submissions - 1U;
  const bool replay_semantics = mode == "exact"
      ? idempotent_replays == expected_duplicates && stable_receipt
      : conflicts == expected_duplicates && stable_receipt;

  execution.reporter->assertion("first_submission", "首次结果创建非幂等结算",
                                {{"http_status", 201}, {"idempotent", false}},
                                {{"http_status", first.value->http_status},
                                 {"idempotent", first.value->idempotent}},
                                first.value->http_status == 201U && !first.value->idempotent);
  execution.reporter->assertion("replay_semantics", "后续提交遵守所选的幂等或冲突语义",
                                expected_duplicates,
                                mode == "exact" ? idempotent_replays : conflicts,
                                replay_semantics);
  execution.reporter->assertion("single_score_change", "最终排行与首次 receipt 的 after_score 一致",
                                true, score_once, score_once);
  execution.reporter->evidence("settlement",
      {{"mode", mode}, {"total_submissions", total_submissions},
       {"result_id", first.value->result_id}, {"match_id", first.value->match_id},
       {"room_id", first.value->room_id}, {"winner_id", first.value->winner_id},
       {"loser_id", first.value->loser_id},
       {"first_cache_state", first.value->cache_state}});
  return replay_semantics && score_once
      ? Error{} : scenario_error("duplicate trusted result assertions failed");
}

ToxiproxyFaultKind fault_kind(const std::string& value) {
  if (value == "latency") return ToxiproxyFaultKind::kLatency;
  if (value == "timeout") return ToxiproxyFaultKind::kTimeout;
  if (value == "reset_peer") return ToxiproxyFaultKind::kResetPeer;
  return ToxiproxyFaultKind::kDisconnect;
}

class ProxyResetGuard final {
 public:
  ProxyResetGuard(const ToxiproxyClient& proxy,
                  const std::shared_ptr<LabRunReporter>& reporter)
      : proxy_(proxy), reporter_(reporter) {}

  ~ProxyResetGuard() {
    if (!armed_) return;
    const auto error = proxy_.reset();
    try {
      if (reporter_) {
        reporter_->event(error ? "dependency.reset_failed" : "dependency.reset_complete",
                         error ? "离开场景时恢复 Redis 代理失败" : "离开场景时已恢复 Redis 代理");
      }
    } catch (...) {
      // Cleanup must remain noexcept even if report allocation fails.
    }
  }

  Error reset_now() {
    if (!armed_) return {};
    const auto error = proxy_.reset();
    if (!error) armed_ = false;
    if (reporter_) {
      reporter_->event(error ? "dependency.reset_failed" : "dependency.reset_complete",
                       error ? "恢复 Redis 代理失败" : "Redis 代理和 toxic 已恢复");
    }
    return error;
  }

 private:
  const ToxiproxyClient& proxy_;
  std::shared_ptr<LabRunReporter> reporter_;
  bool armed_{true};
};

Error run_redis_failure(const LabScenarioRequest& request,
                        const LabRunEnvironment& environment,
                        const LabScenarioExecution& execution) {
  if (environment.toxiproxy_host.empty()) {
    return scenario_error("Toxiproxy is unavailable in this Lab profile");
  }
  ToxiproxyClientConfig proxy_config;
  proxy_config.host = environment.toxiproxy_host;
  proxy_config.api_port = environment.toxiproxy_port;
  proxy_config.proxy_name = environment.toxiproxy_proxy_name;
  proxy_config.listen = environment.toxiproxy_listen;
  proxy_config.upstream = environment.toxiproxy_upstream;
  const auto remaining_http_budget = bounded_http_timeout_ms(execution);
  proxy_config.request_timeout_ms = std::max<std::uint32_t>(
      100U, std::min<std::uint32_t>(1'000U, remaining_http_budget / 4U));
  ToxiproxyClient proxy(std::move(proxy_config));
  if (const auto error = proxy.prepare()) return error;
  ProxyResetGuard reset_guard(proxy, execution.reporter);
  execution.reporter->event("dependency.proxy_ready", "Redis 代理已恢复到无故障状态");

  BasicMatchFlow flow(request, environment, execution, 0U);
  if (const auto error = flow.connect_both()) return error;
  if (const auto error = flow.queue_both(1'000)) return error;
  if (const auto error = flow.join_both()) return error;
  if (const auto error = flow.ready_both()) return error;
  flow.make_result();

  const auto kind = request.parameters.at("fault_kind").get<std::string>();
  const auto fault_value = request.parameters.at("fault_value_ms").get<std::uint32_t>();
  const auto fault_duration = request.parameters.at("fault_duration_ms").get<std::uint32_t>();
  if (const auto error = cancelled_or_timed_out(execution)) return error;
  if (const auto error = proxy.inject(fault_kind(kind), fault_value)) return error;
  execution.reporter->event("dependency.fault_injected", "已向真实 Redis TCP 路径注入故障",
                            {{"kind", kind}, {"value_ms", fault_value}});

  auto stale_receipt = flow.submit_result();
  if (!stale_receipt.ok()) return stale_receipt.error;
  if (const auto error = flow.receive_settlement()) return error;
  DemoBattleAdapter adapter({environment.result_host, environment.result_port,
                             environment.result_token,
                             bounded_http_timeout_ms(execution)});
  auto stale_query = adapter.query(stale_receipt.value->result_id);
  if (!stale_query.ok()) return stale_query.error;
  const bool mysql_committed = stale_query.value->result_id == stale_receipt.value->result_id &&
                               stale_query.value->match_id == stale_receipt.value->match_id;
  const bool stale_visible = stale_receipt.value->cache_state == "stale" &&
                             stale_query.value->cache_state == "stale";
  execution.reporter->event("dependency.mysql_committed", "Redis 故障期间 MySQL 已保存结算事实",
                            {{"result_id", stale_receipt.value->result_id},
                             {"cache_state", stale_receipt.value->cache_state}});

  if (const auto error = pause(execution, fault_duration)) return error;
  if (const auto error = reset_guard.reset_now()) return error;

  std::array<wire::RankResult, 2> recovered_ranks;
  std::array<bool, 2> recovered{{false, false}};
  while ((!recovered[0U] || !recovered[1U]) &&
         SteadyClock::now() < execution.deadline) {
    for (std::size_t index = 0; index < recovered.size(); ++index) {
      if (recovered[index]) continue;
      auto rank = flow.query_rank(index);
      if (rank.ok() && rank.value->cache_state() == "fresh") {
        recovered_ranks[index] = std::move(*rank.value);
        recovered[index] = true;
      } else if (!rank.ok() && rank.error.code != ErrorCode::kRankCacheUnavailable &&
                 rank.error.code != ErrorCode::kDependencyRedisUnavailable &&
                 rank.error.code != ErrorCode::kRankPlayerNotFound &&
                 rank.error.code != ErrorCode::kServerBusy) {
        return rank.error;
      }
    }
    if (!recovered[0U] || !recovered[1U]) {
      if (const auto error = pause(execution, 100U)) return error;
    }
  }
  auto fresh_query = adapter.query(stale_receipt.value->result_id);
  if (!fresh_query.ok()) return fresh_query.error;
  // The repair worker updates each player's sorted-set score before deleting
  // that player's dirty row. Both rank reads can therefore become fresh a few
  // milliseconds before the settlement-level receipt has no dirty rows left.
  // Wait for the durable MySQL repair evidence as a separate condition.
  while (fresh_query.value->cache_state != "fresh" &&
         SteadyClock::now() < execution.deadline) {
    if (const auto error = pause(execution, 50U)) return error;
    fresh_query = adapter.query(stale_receipt.value->result_id);
    if (!fresh_query.ok()) return fresh_query.error;
  }
  const bool ranks_fresh = recovered[0U] && recovered[1U];
  const bool receipt_repaired = fresh_query.value->cache_state == "fresh";
  const bool scores_match = ranks_fresh && receipt_has_scores(
      *fresh_query.value, recovered_ranks[0U], recovered_ranks[1U]);

  execution.reporter->assertion("mysql_fact", "故障期间 MySQL 结算事实可查询",
                                true, mysql_committed, mysql_committed);
  execution.reporter->assertion("cache_stale", "故障期间 receipt 明确标记排行缓存落后",
                                "stale", stale_receipt.value->cache_state, stale_visible);
  execution.reporter->assertion("ranking_repaired", "恢复代理后两名玩家都读到新鲜排行",
                                true, ranks_fresh, ranks_fresh);
  execution.reporter->assertion("receipt_repaired", "MySQL receipt 查询确认脏记录已清理",
                                "fresh", fresh_query.value->cache_state, receipt_repaired);
  execution.reporter->assertion("scores_consistent", "修复后的排行与 receipt 分数一致",
                                true, scores_match, scores_match);
  execution.reporter->evidence("settlement",
      {{"result_id", stale_receipt.value->result_id},
       {"match_id", stale_receipt.value->match_id},
       {"room_id", stale_receipt.value->room_id},
       {"fault_cache_state", stale_receipt.value->cache_state},
       {"recovered_cache_state", fresh_query.value->cache_state}});
  return mysql_committed && stale_visible && ranks_fresh && receipt_repaired && scores_match
      ? Error{} : scenario_error("Redis failure recovery assertions failed");
}

LabScenarioHandler guarded(LabScenarioHandler handler) {
  return [handler = std::move(handler)](const LabScenarioRequest& request,
                                        const LabRunEnvironment& environment,
                                        const LabScenarioExecution& execution) {
    try {
      return handler(request, environment, execution);
    } catch (const std::exception& exception) {
      return scenario_error("scenario execution raised an exception: " +
                            std::string(exception.what()));
    } catch (...) {
      return scenario_error("scenario execution raised an unknown exception");
    }
  };
}

}  // namespace

std::map<LabScenarioKind, LabScenarioHandler> make_lab_scenario_handlers() {
  return {{LabScenarioKind::kCompleteMatch, guarded(run_complete)},
          {LabScenarioKind::kReconnectRecovery, guarded(run_reconnect)},
          {LabScenarioKind::kDuplicatePlayerRequest, guarded(run_duplicate_request)},
          {LabScenarioKind::kDuplicateTrustedResult, guarded(run_duplicate_result)},
          {LabScenarioKind::kRedisFailureRecovery, guarded(run_redis_failure)}};
}

}  // namespace arena_core::qa

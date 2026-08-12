#include "arena_core/game/game_service.hpp"

#include "arena_core/protocol/frame.hpp"
#include "arena_core/observer/observer_store.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

namespace arena_core::game {
namespace {

namespace asio = boost::asio;
namespace wire = arena_core::protocol::v1;

wire::Envelope response_envelope(const wire::MessageType type) {
  wire::Envelope response;
  response.set_protocol_version(1);
  response.set_message_type(type);
  return response;
}

template <typename Message>
Result<wire::Envelope> response_with_payload(const wire::MessageType type, const Message& payload) {
  auto response = response_envelope(type);
  if (!protocol::set_payload(response, payload)) {
    return Result<wire::Envelope>::failure(make_error(ErrorCode::kProtocolDecodeFailed));
  }
  return Result<wire::Envelope>::success(std::move(response));
}

}  // namespace

GameService::GameService(asio::io_context& io_context,
                         GameServiceOptions options,
                         std::shared_ptr<domain::IdGenerator> id_generator,
                         std::shared_ptr<session::TokenGenerator> token_generator,
                         std::shared_ptr<settlement::SettlementGateway> settlement_gateway,
                         std::shared_ptr<observer::ObserverStore> observer_store)
    : io_context_(io_context),
      strand_(asio::make_strand(io_context)),
      tick_timer_(strand_),
      options_(std::move(options)),
      id_generator_(std::move(id_generator)),
      token_generator_(std::move(token_generator)),
      settlement_gateway_(settlement_gateway ? std::move(settlement_gateway)
                                             : settlement::make_in_memory_settlement_gateway()),
      observer_store_(std::move(observer_store)) {}

void GameService::start() {
  asio::dispatch(strand_, [self = shared_from_this()] {
    if (self->started_) {
      return;
    }
    self->started_ = true;
    self->stopped_ = false;
    self->settlement_gateway_->start();
    self->arm_tick();
  });
}

void GameService::stop() {
  asio::dispatch(strand_, [self = shared_from_this()] {
    if (self->stopped_) {
      return;
    }
    self->stopped_ = true;
    self->tick_timer_.cancel();
    for (const auto& [room_id, room] : self->rooms_) {
      static_cast<void>(room_id);
      room->stop();
    }
    self->settlement_gateway_->stop();
  });
}

void GameService::set_player_notifier(PlayerNotifier notifier) { notifier_ = std::move(notifier); }

void GameService::handle(std::string player_id, wire::Envelope request, Completion completion) {
  asio::dispatch(strand_, [self = shared_from_this(), player_id = std::move(player_id),
                           request = std::move(request), completion = std::move(completion)]() mutable {
    if (self->stopped_) {
      completion(Result<wire::Envelope>::failure(make_error(ErrorCode::kServerShuttingDown)));
      return;
    }
    switch (request.message_type()) {
      case wire::QUEUE_MATCH:
        self->handle_queue(std::move(player_id), std::move(request), std::move(completion));
        return;
      case wire::CANCEL_MATCH:
        self->handle_cancel(std::move(player_id), std::move(request), std::move(completion));
        return;
      case wire::JOIN_ROOM:
        self->handle_join(std::move(player_id), std::move(request), std::move(completion));
        return;
      case wire::READY_ROOM:
        self->handle_ready(std::move(player_id), std::move(request), std::move(completion));
        return;
      case wire::LEAVE_ROOM:
        self->handle_leave(std::move(player_id), std::move(request), std::move(completion));
        return;
      case wire::GET_RANK:
        self->handle_rank(std::move(player_id), std::move(request), std::move(completion));
        return;
      default:
        completion(Result<wire::Envelope>::failure(make_error(ErrorCode::kProtocolUnknownMessage)));
        return;
    }
  });
}

void GameService::handle_queue(std::string player_id, wire::Envelope request, Completion completion) {
  auto command = protocol::parse_payload<wire::QueueMatch>(request);
  if (!command.ok()) {
    completion(Result<wire::Envelope>::failure(std::move(command.error)));
    return;
  }
  if (active_ticket_by_player_.find(player_id) != active_ticket_by_player_.end()) {
    completion(Result<wire::Envelope>::failure(make_error(ErrorCode::kMatchAlreadyQueued)));
    return;
  }

  const auto wait = command.value->max_wait_ms() == 0U
      ? std::chrono::milliseconds(30'000)
      : std::chrono::milliseconds(command.value->max_wait_ms());
  const auto now = domain::MonotonicClock::now();
  const auto ticket_id = id_generator_->next("ticket");
  auto ticket = domain::MatchTicket::create(ticket_id, player_id, command.value->mode(), command.value->mmr(), now,
                                             wait);
  if (!ticket.ok()) {
    completion(Result<wire::Envelope>::failure(std::move(ticket.error)));
    return;
  }

  wire::MatchQueued payload;
  payload.set_ticket_id(ticket_id);
  payload.set_expires_at_ms(static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(ticket.value->expires_at().time_since_epoch()).count()));
  tickets_.emplace(ticket_id, std::move(*ticket.value));
  active_ticket_by_player_[player_id] = ticket_id;
  queue_.push_back(ticket_id);
  queued_ticket_count_.store(queue_.size());
  if (observer_store_) {
    observer_store_->upsert_ticket({ticket_id, player_id, command.value->mode(), "Queued", ticket_id,
                                    command.value->mmr(), 0});
    observer_store_->publish_event("match", "ticket.queued", ticket_id, ticket_id,
                                   player_id + " entered duel queue");
  }
  completion(response_with_payload(wire::MATCH_QUEUED, payload));
  while (try_match_once(now)) {
  }
}

void GameService::handle_cancel(std::string player_id, wire::Envelope request, Completion completion) {
  auto command = protocol::parse_payload<wire::CancelMatch>(request);
  if (!command.ok()) {
    completion(Result<wire::Envelope>::failure(std::move(command.error)));
    return;
  }
  const auto ticket = tickets_.find(command.value->ticket_id());
  if (ticket == tickets_.end()) {
    completion(Result<wire::Envelope>::failure(make_error(ErrorCode::kMatchTicketNotFound)));
    return;
  }
  if (auto error = ticket->second.cancel(player_id, domain::MonotonicClock::now())) {
    completion(Result<wire::Envelope>::failure(std::move(error)));
    return;
  }

  queue_.erase(std::remove(queue_.begin(), queue_.end(), ticket->first), queue_.end());
  active_ticket_by_player_.erase(player_id);
  queued_ticket_count_.store(queue_.size());
  if (observer_store_) {
    observer_store_->upsert_ticket({ticket->second.ticket_id(), ticket->second.player_id(),
                                    ticket->second.mode(), "Cancelled", ticket->second.ticket_id(),
                                    ticket->second.mmr(), 0});
    observer_store_->publish_event("match", "ticket.cancelled", ticket->second.ticket_id(),
                                   ticket->second.ticket_id(), "ticket cancelled by owner", "warning");
  }
  wire::MatchCancelled payload;
  payload.set_ticket_id(ticket->first);
  payload.set_state(std::string(domain::match_ticket_state_name(ticket->second.state())));
  completion(response_with_payload(wire::MATCH_CANCELLED, payload));
}

void GameService::handle_join(std::string player_id, wire::Envelope request, Completion completion) {
  auto command = protocol::parse_payload<wire::JoinRoom>(request);
  if (!command.ok()) {
    completion(Result<wire::Envelope>::failure(std::move(command.error)));
    return;
  }
  const auto room = rooms_.find(command.value->room_id());
  if (room == rooms_.end()) {
    completion(Result<wire::Envelope>::failure(make_error(ErrorCode::kRoomNotFound)));
    return;
  }
  room->second->async_join(std::move(player_id), std::move(*command.value), std::move(completion));
}

void GameService::handle_ready(std::string player_id, wire::Envelope request, Completion completion) {
  auto command = protocol::parse_payload<wire::ReadyRoom>(request);
  if (!command.ok()) {
    completion(Result<wire::Envelope>::failure(std::move(command.error)));
    return;
  }
  const auto room = rooms_.find(command.value->room_id());
  if (room == rooms_.end()) {
    completion(Result<wire::Envelope>::failure(make_error(ErrorCode::kRoomNotFound)));
    return;
  }
  room->second->async_ready(std::move(player_id), std::move(*command.value), std::move(completion));
}

void GameService::handle_leave(std::string player_id, wire::Envelope request, Completion completion) {
  auto command = protocol::parse_payload<wire::LeaveRoom>(request);
  if (!command.ok()) {
    completion(Result<wire::Envelope>::failure(std::move(command.error)));
    return;
  }
  const auto room = rooms_.find(command.value->room_id());
  if (room == rooms_.end()) {
    completion(Result<wire::Envelope>::failure(make_error(ErrorCode::kRoomNotFound)));
    return;
  }
  room->second->async_leave(std::move(player_id), std::move(*command.value), std::move(completion));
}

void GameService::handle_rank(std::string player_id, wire::Envelope request, Completion completion) {
  auto command = protocol::parse_payload<wire::GetRank>(request);
  if (!command.ok()) {
    completion(Result<wire::Envelope>::failure(std::move(command.error)));
    return;
  }
  rank::RankQuery query;
  query.leaderboard = command.value->leaderboard().empty() ? "duel:season-1" : command.value->leaderboard();
  query.player_id = command.value->player_id().empty() ? std::move(player_id) : command.value->player_id();
  query.neighbor_count = command.value->neighbor_count();
  settlement_gateway_->async_query_rank(
      strand_, std::move(query),
      [completion = std::move(completion)](Result<rank::RankView> result) mutable {
        if (!result.ok()) {
          completion(Result<wire::Envelope>::failure(std::move(result.error)));
          return;
        }
        wire::RankResult payload;
        payload.set_leaderboard(result.value->leaderboard);
        payload.set_cache_state(result.value->cache_state);
        auto* player = payload.mutable_player();
        player->set_player_id(result.value->player.player_id);
        player->set_score(result.value->player.score);
        player->set_rank(result.value->player.rank);
        for (const auto& neighbor : result.value->neighbors) {
          auto* entry = payload.add_neighbors();
          entry->set_player_id(neighbor.player_id);
          entry->set_score(neighbor.score);
          entry->set_rank(neighbor.rank);
        }
        completion(response_with_payload(wire::RANK_RESULT, payload));
      });
}

void GameService::sweep_and_match() {
  const auto now = domain::MonotonicClock::now();
  for (auto iterator = queue_.begin(); iterator != queue_.end();) {
    auto ticket = tickets_.find(*iterator);
    if (ticket != tickets_.end() && ticket->second.expire_if_due(now)) {
      active_ticket_by_player_.erase(ticket->second.player_id());
      notify_expired(ticket->second);
      iterator = queue_.erase(iterator);
    } else {
      ++iterator;
    }
  }
  queued_ticket_count_.store(queue_.size());
  while (try_match_once(now)) {
  }
}

bool GameService::try_match_once(const domain::TimePoint now) {
  if (queue_.size() < 2U || active_room_count_.load() >= options_.maximum_active_rooms) {
    return false;
  }

  for (std::size_t first_index = 0; first_index + 1U < queue_.size(); ++first_index) {
    const auto first = tickets_.find(queue_[first_index]);
    if (first == tickets_.end() || first->second.state() != domain::MatchTicketState::kQueued) {
      continue;
    }
    if (now >= first->second.expires_at()) {
      continue;
    }
    std::size_t best_index = queue_.size();
    auto best_gap = std::numeric_limits<std::int32_t>::max();
    for (std::size_t candidate_index = first_index + 1U; candidate_index < queue_.size(); ++candidate_index) {
      const auto candidate = tickets_.find(queue_[candidate_index]);
      if (candidate == tickets_.end() || candidate->second.state() != domain::MatchTicketState::kQueued ||
          candidate->second.mode() != first->second.mode() || now >= candidate->second.expires_at()) {
        continue;
      }
      const auto gap = static_cast<std::int32_t>(std::abs(candidate->second.mmr() - first->second.mmr()));
      const auto allowed = allowed_mmr_delta(first->second, now);
      if (allowed.ok() && gap <= *allowed.value &&
          (gap < best_gap || (gap == best_gap && candidate->second.ticket_id() < queue_[best_index]))) {
        best_gap = gap;
        best_index = candidate_index;
      }
    }
    if (best_index == queue_.size()) {
      continue;
    }

    auto second = tickets_.find(queue_[best_index]);
    const auto match_id = id_generator_->next("match");
    const auto room_id = id_generator_->next("room");
    const auto first_join_token = token_generator_->next_token();
    const auto second_join_token = token_generator_->next_token();
    std::weak_ptr<GameService> weak_self = shared_from_this();
    auto room = RoomActor::create(io_context_, room_id, match_id, options_.room_server,
                                  first->second.player_id(), second->second.player_id(), first_join_token,
                                  second_join_token, now + options_.join_token_ttl, notifier_, settlement_gateway_,
                                  [weak_self](std::string closed_room_id) {
                                    if (const auto self = weak_self.lock()) {
                                      asio::post(self->strand_, [self, closed_room_id = std::move(closed_room_id)]() mutable {
                                        self->room_closed(std::move(closed_room_id));
                                      });
                                    }
                                  }, observer_store_);
    if (!room.ok()) {
      return false;
    }
    if (auto error = first->second.mark_matched(match_id, now)) {
      static_cast<void>(error);
      return false;
    }
    if (auto error = second->second.mark_matched(match_id, now)) {
      static_cast<void>(error);
      return false;
    }

    const auto first_ticket_id = first->first;
    const auto second_ticket_id = second->first;
    rooms_.emplace(room_id, *room.value);
    active_room_count_.fetch_add(1);
    active_ticket_by_player_.erase(first->second.player_id());
    active_ticket_by_player_.erase(second->second.player_id());
    remove_from_queue(first_ticket_id, second_ticket_id);
    if (observer_store_) {
      observer_store_->upsert_ticket({first->second.ticket_id(), first->second.player_id(),
                                      first->second.mode(), "Matched", match_id,
                                      first->second.mmr(), 0});
      observer_store_->upsert_ticket({second->second.ticket_id(), second->second.player_id(),
                                      second->second.mode(), "Matched", match_id,
                                      second->second.mmr(), 0});
      observer_store_->upsert_room({room_id, match_id, "AwaitingJoin", match_id,
                                    {first->second.player_id(), second->second.player_id()}, {}, "", ""});
      observer_store_->publish_event("match", "match.created", match_id, room_id,
                                     "two queued tickets allocated to room", "success");
    }
    notify_match_found(first->second, second->second, match_id, room_id, first_join_token);
    notify_match_found(second->second, first->second, match_id, room_id, second_join_token);
    return true;
  }
  return false;
}

Result<std::int32_t> GameService::allowed_mmr_delta(const domain::MatchTicket& ticket,
                                                    const domain::TimePoint now) const {
  const auto waited_seconds = std::chrono::duration_cast<std::chrono::seconds>(now - ticket.queued_at()).count();
  return settlement_gateway_->allowed_mmr_delta(waited_seconds);
}

void GameService::remove_from_queue(const std::string& first_ticket, const std::string& second_ticket) {
  queue_.erase(std::remove_if(queue_.begin(), queue_.end(), [&](const std::string& ticket_id) {
    return ticket_id == first_ticket || ticket_id == second_ticket;
  }), queue_.end());
  queued_ticket_count_.store(queue_.size());
}

void GameService::notify_expired(const domain::MatchTicket& ticket) {
  if (observer_store_) {
    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
        ticket.expires_at() - ticket.queued_at()).count();
    observer_store_->upsert_ticket({ticket.ticket_id(), ticket.player_id(), ticket.mode(), "Expired",
                                    ticket.ticket_id(), ticket.mmr(),
                                    static_cast<std::uint64_t>(std::max<std::int64_t>(0, waited))});
    observer_store_->publish_event("match", "ticket.expired", ticket.ticket_id(),
                                   ticket.ticket_id(), "ticket reached its wait deadline", "warning");
  }
  if (!notifier_) {
    return;
  }
  wire::MatchCancelled payload;
  payload.set_ticket_id(ticket.ticket_id());
  payload.set_state("Expired");
  auto event = response_envelope(wire::MATCH_CANCELLED);
  protocol::set_payload(event, payload);
  notifier_(ticket.player_id(), std::move(event));
}

void GameService::notify_match_found(const domain::MatchTicket& ticket,
                                     const domain::MatchTicket& opponent,
                                     const std::string& match_id,
                                     const std::string& room_id,
                                     const std::string& join_token) {
  if (!notifier_) {
    return;
  }
  wire::MatchFound payload;
  payload.set_ticket_id(ticket.ticket_id());
  payload.set_match_id(match_id);
  payload.set_room_id(room_id);
  payload.set_room_server(options_.room_server);
  payload.set_join_token(join_token);
  payload.set_opponent_id(opponent.player_id());
  auto event = response_envelope(wire::MATCH_FOUND);
  protocol::set_payload(event, payload);
  notifier_(ticket.player_id(), std::move(event));
}

void GameService::room_closed(std::string room_id) {
  const auto room = rooms_.find(room_id);
  if (room != rooms_.end() && active_room_count_.load() > 0U) {
    active_room_count_.fetch_sub(1);
  }
}

void GameService::arm_tick() {
  if (stopped_) {
    return;
  }
  tick_timer_.expires_after(options_.match_tick);
  tick_timer_.async_wait(asio::bind_executor(strand_, [self = shared_from_this()](const boost::system::error_code& error) {
    if (error || self->stopped_) {
      return;
    }
    self->sweep_and_match();
    self->arm_tick();
  }));
}

}  // namespace arena_core::game

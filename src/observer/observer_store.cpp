#include "arena_core/observer/observer_store.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <utility>

namespace arena_core::observer {
namespace {

std::string now_iso8601() {
  const auto now = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  std::tm utc{};
#if defined(_WIN32)
  gmtime_s(&utc, &time);
#else
  gmtime_r(&time, &utc);
#endif
  std::ostringstream output;
  output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
  return output.str();
}

nlohmann::json session_json(const SessionView& value) {
  return {{"session_id", value.session_id},
          {"player_id", value.player_id},
          {"state", value.state},
          {"trace_id", value.trace_id},
          {"generation", value.generation},
          {"last_heartbeat_ms", value.last_heartbeat_ms}};
}

nlohmann::json ticket_json(const TicketView& value) {
  return {{"ticket_id", value.ticket_id},
          {"player_id", value.player_id},
          {"mode", value.mode},
          {"state", value.state},
          {"trace_id", value.trace_id},
          {"mmr", value.mmr},
          {"waiting_ms", value.waiting_ms}};
}

nlohmann::json room_json(const RoomView& value) {
  return {{"room_id", value.room_id},
          {"match_id", value.match_id},
          {"state", value.state},
          {"trace_id", value.trace_id},
          {"players", value.players},
          {"ready_players", value.ready_players},
          {"winner_id", value.winner_id},
          {"rejection_reason", value.rejection_reason}};
}

nlohmann::json settlement_json(const SettlementView& value) {
  return {{"settlement_id", value.settlement_id},
          {"match_id", value.match_id},
          {"winner_id", value.winner_id},
          {"loser_id", value.loser_id},
          {"status", value.status},
          {"trace_id", value.trace_id},
          {"idempotent_replay", value.idempotent_replay}};
}

nlohmann::json rank_json(const RankEntryView& value) {
  return {{"player_id", value.player_id}, {"score", value.score}, {"rank", value.rank}};
}

nlohmann::json qa_json(const QaRunView& value) {
  return {{"run_id", value.run_id},
          {"scenario_id", value.scenario_id},
          {"name", value.name},
          {"status", value.status},
          {"bug_id", value.bug_id},
          {"detail", value.detail},
          {"duration_ms", value.duration_ms}};
}

}  // namespace

ObserverStore::ObserverStore(const std::size_t maximum_events)
    : maximum_events_(std::max<std::size_t>(1, maximum_events)) {}

void ObserverStore::set_health(std::string dependency, const bool healthy, std::string detail) {
  std::lock_guard<std::mutex> lock(mutex_);
  health_[std::move(dependency)] = HealthView{healthy, std::move(detail)};
}

void ObserverStore::upsert_session(SessionView session) {
  std::lock_guard<std::mutex> lock(mutex_);
  sessions_[session.session_id] = std::move(session);
}

void ObserverStore::remove_session(const std::string_view session_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  sessions_.erase(std::string(session_id));
}

void ObserverStore::upsert_ticket(TicketView ticket) {
  std::lock_guard<std::mutex> lock(mutex_);
  tickets_[ticket.ticket_id] = std::move(ticket);
}

void ObserverStore::upsert_room(RoomView room) {
  std::lock_guard<std::mutex> lock(mutex_);
  rooms_[room.room_id] = std::move(room);
}

void ObserverStore::add_settlement(SettlementView settlement) {
  std::lock_guard<std::mutex> lock(mutex_);
  settlements_[settlement.settlement_id] = std::move(settlement);
}

void ObserverStore::replace_rankings(std::string leaderboard,
                                     std::vector<RankEntryView> entries,
                                     std::string cache_state) {
  std::lock_guard<std::mutex> lock(mutex_);
  rankings_[std::move(leaderboard)] = RankingView{std::move(cache_state), std::move(entries)};
}

void ObserverStore::add_qa_run(QaRunView run) {
  std::lock_guard<std::mutex> lock(mutex_);
  qa_runs_[run.run_id] = std::move(run);
}

std::uint64_t ObserverStore::publish_event(std::string category,
                                           std::string type,
                                           std::string trace_id,
                                           std::string entity_id,
                                           std::string message,
                                           std::string severity) {
  std::lock_guard<std::mutex> lock(mutex_);
  return append_event_locked(std::move(category), std::move(type), std::move(trace_id),
                             std::move(entity_id), std::move(message), std::move(severity));
}

std::uint64_t ObserverStore::append_event_locked(std::string category,
                                                  std::string type,
                                                  std::string trace_id,
                                                  std::string entity_id,
                                                  std::string message,
                                                  std::string severity) {
  const auto sequence = next_sequence_++;
  events_.push_back(EventView{sequence, now_iso8601(), std::move(category), std::move(type),
                              std::move(trace_id), std::move(entity_id), std::move(message),
                              std::move(severity)});
  while (events_.size() > maximum_events_) {
    events_.pop_front();
  }
  return sequence;
}

void ObserverStore::reset_demo_locked() {
  sessions_.clear();
  tickets_.clear();
  rooms_.clear();
  settlements_.clear();
  rankings_.clear();
  qa_runs_.clear();
  events_.clear();
  health_["tcp"] = HealthView{true, "accepting"};
  health_["mysql"] = HealthView{true, "transaction source ready"};
  health_["redis"] = HealthView{true, "rank cache fresh"};
}

void ObserverStore::run_eight_player_demo_locked() {
  reset_demo_locked();
  std::vector<RankEntryView> ranking;
  ranking.reserve(8);
  for (int index = 1; index <= 8; ++index) {
    const auto player = "player-" + std::to_string(index);
    const auto trace = "trace-demo-" + std::to_string((index + 1) / 2);
    const auto session = "session-" + std::to_string(index);
    const auto ticket = "ticket-" + std::to_string(index);
    sessions_[session] = SessionView{session, player, "Active", trace, 1,
                                     static_cast<std::uint64_t>(1'723'000'000'000LL + index)};
    tickets_[ticket] = TicketView{ticket, player, "duel", "Matched", trace,
                                  1'000 + index * 12, 140U + static_cast<std::uint64_t>(index * 15)};
    append_event_locked("session", "session.active", trace, session,
                        player + " completed Hello", "info");
    append_event_locked("match", "ticket.matched", trace, ticket,
                        player + " matched into room", "info");
    ranking.push_back(RankEntryView{player, 1'120 - index * 13, static_cast<std::uint32_t>(index)});
  }
  for (int index = 1; index <= 4; ++index) {
    const auto room_id = "room-demo-" + std::to_string(index);
    const auto match_id = "match-demo-" + std::to_string(index);
    const auto trace = "trace-demo-" + std::to_string(index);
    const auto first = "player-" + std::to_string(index * 2 - 1);
    const auto second = "player-" + std::to_string(index * 2);
    rooms_[room_id] = RoomView{room_id, match_id, "Closed", trace, {first, second},
                               {first, second}, first, {}};
    const auto settlement_id = "settlement-demo-" + std::to_string(index);
    settlements_[settlement_id] = SettlementView{settlement_id, match_id, first, second,
                                                  "Committed", trace, false};
    append_event_locked("room", "room.ready", trace, room_id,
                        "both players ready; deterministic result frozen", "info");
    append_event_locked("settlement", "settlement.committed", trace, settlement_id,
                        "MySQL committed before Redis rank update", "success");
    append_event_locked("room", "room.closed", trace, room_id,
                        "room closed after settlement commit", "success");
  }
  rankings_["duel:season-1"] = RankingView{"fresh", std::move(ranking)};
  qa_runs_["qa-eight-player"] = QaRunView{"qa-eight-player", "eight-player-flow",
                                           "8 人匹配 / 4 房间闭环", "passed", "",
                                           "8 sessions → 8 tickets → 4 rooms → 4 settlements", 684};
  append_event_locked("qa", "scenario.passed", "trace-demo-all", "qa-eight-player",
                      "8-player / 4-room demonstration passed", "success");
}

void ObserverStore::run_duplicate_settlement_demo_locked() {
  if (rooms_.empty()) {
    run_eight_player_demo_locked();
  }
  const auto settlement = settlements_.find("settlement-demo-1");
  if (settlement != settlements_.end()) {
    settlement->second.idempotent_replay = true;
  }
  qa_runs_["qa-duplicate-settlement"] = QaRunView{
      "qa-duplicate-settlement", "duplicate-settlement", "重复结算幂等复测", "passed", "",
      "same fingerprint returned the committed receipt; score changed once", 42};
  append_event_locked("settlement", "settlement.idempotent", "trace-demo-1",
                      "settlement-demo-1", "same fingerprint replay returned original receipt", "success");
  append_event_locked("qa", "scenario.passed", "trace-demo-1", "qa-duplicate-settlement",
                      "duplicate settlement did not double-apply score", "success");
}

void ObserverStore::run_redis_repair_demo_locked() {
  if (rooms_.empty()) {
    run_eight_player_demo_locked();
  }
  health_["redis"] = HealthView{false, "simulated timeout; MySQL remains authoritative"};
  append_event_locked("rank", "rank.cache_stale", "trace-demo-repair", "duel:season-1",
                      "Redis update failed; dirty row retained", "warning");
  append_event_locked("rank", "rank.repair_started", "trace-demo-repair", "duel:season-1",
                      "repair worker replayed MySQL target scores", "info");
  health_["redis"] = HealthView{true, "repaired from MySQL; cache fresh"};
  if (const auto ranking = rankings_.find("duel:season-1"); ranking != rankings_.end()) {
    ranking->second.cache_state = "fresh";
  }
  qa_runs_["qa-redis-repair"] = QaRunView{"qa-redis-repair", "redis-repair",
                                          "Redis 故障与 dirty 修复", "passed", "BUG-M5-TZ-001",
                                          "dirty retained while unavailable and cleared after replay", 318};
  append_event_locked("rank", "rank.repaired", "trace-demo-repair", "duel:season-1",
                      "dirty cleared after cache reached MySQL state", "success");
  append_event_locked("qa", "scenario.passed", "trace-demo-repair", "qa-redis-repair",
                      "Redis failure recovery passed", "success");
}

bool ObserverStore::run_demo_scenario(const std::string_view scenario_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (scenario_id == "eight-player-flow") {
    run_eight_player_demo_locked();
    return true;
  }
  if (scenario_id == "duplicate-settlement") {
    run_duplicate_settlement_demo_locked();
    return true;
  }
  if (scenario_id == "redis-repair") {
    run_redis_repair_demo_locked();
    return true;
  }
  return false;
}

nlohmann::json ObserverStore::overview_json() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::size_t online = 0;
  std::size_t queued = 0;
  std::size_t active_rooms = 0;
  std::size_t errors = 0;
  nlohmann::json health = nlohmann::json::object();
  for (const auto& [name, value] : health_) {
    health[name] = {{"healthy", value.healthy}, {"detail", value.detail}};
    if (!value.healthy) {
      ++errors;
    }
  }
  for (const auto& [id, value] : sessions_) {
    static_cast<void>(id);
    online += value.state == "Active" ? 1U : 0U;
  }
  for (const auto& [id, value] : tickets_) {
    static_cast<void>(id);
    queued += value.state == "Queued" ? 1U : 0U;
  }
  for (const auto& [id, value] : rooms_) {
    static_cast<void>(id);
    active_rooms += value.state != "Closed" && value.state != "Aborted" ? 1U : 0U;
  }
  for (const auto& event : events_) {
    errors += event.severity == "error" ? 1U : 0U;
  }
  return {{"service", "arena-core-server"},
          {"generated_at", now_iso8601()},
          {"counts", {{"sessions_online", online},
                       {"tickets_queued", queued},
                       {"rooms_active", active_rooms},
                       {"rooms_total", rooms_.size()},
                       {"settlements", settlements_.size()},
                       {"errors", errors}}},
          {"health", std::move(health)},
          {"latest_sequence", next_sequence_ - 1}};
}

nlohmann::json ObserverStore::sessions_json() const {
  std::lock_guard<std::mutex> lock(mutex_);
  nlohmann::json items = nlohmann::json::array();
  for (const auto& [id, value] : sessions_) {
    static_cast<void>(id);
    items.push_back(session_json(value));
  }
  return {{"items", std::move(items)}, {"total", sessions_.size()}};
}

nlohmann::json ObserverStore::tickets_json() const {
  std::lock_guard<std::mutex> lock(mutex_);
  nlohmann::json items = nlohmann::json::array();
  for (const auto& [id, value] : tickets_) {
    static_cast<void>(id);
    items.push_back(ticket_json(value));
  }
  return {{"items", std::move(items)}, {"total", tickets_.size()}};
}

nlohmann::json ObserverStore::rooms_json() const {
  std::lock_guard<std::mutex> lock(mutex_);
  nlohmann::json items = nlohmann::json::array();
  for (const auto& [id, value] : rooms_) {
    static_cast<void>(id);
    items.push_back(room_json(value));
  }
  return {{"items", std::move(items)}, {"total", rooms_.size()}};
}

nlohmann::json ObserverStore::rankings_json() const {
  std::lock_guard<std::mutex> lock(mutex_);
  nlohmann::json leaderboards = nlohmann::json::array();
  for (const auto& [name, value] : rankings_) {
    nlohmann::json entries = nlohmann::json::array();
    for (const auto& entry : value.entries) {
      entries.push_back(rank_json(entry));
    }
    leaderboards.push_back({{"leaderboard", name}, {"cache_state", value.cache_state},
                            {"entries", std::move(entries)}});
  }
  nlohmann::json settlements = nlohmann::json::array();
  for (const auto& [id, value] : settlements_) {
    static_cast<void>(id);
    settlements.push_back(settlement_json(value));
  }
  return {{"leaderboards", std::move(leaderboards)}, {"settlements", std::move(settlements)}};
}

nlohmann::json ObserverStore::qa_runs_json() const {
  std::lock_guard<std::mutex> lock(mutex_);
  nlohmann::json items = nlohmann::json::array();
  for (const auto& [id, value] : qa_runs_) {
    static_cast<void>(id);
    items.push_back(qa_json(value));
  }
  return {{"items", std::move(items)}, {"total", qa_runs_.size()}};
}

nlohmann::json ObserverStore::events_json(const std::uint64_t after_sequence,
                                           const std::size_t limit,
                                           const std::string_view trace_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto effective_limit = std::max<std::size_t>(1, std::min<std::size_t>(limit, 500));
  nlohmann::json items = nlohmann::json::array();
  for (const auto& event : events_) {
    if (event.sequence <= after_sequence || (!trace_id.empty() && event.trace_id != trace_id)) {
      continue;
    }
    items.push_back({{"sequence", event.sequence},
                     {"timestamp", event.timestamp},
                     {"category", event.category},
                     {"type", event.type},
                     {"trace_id", event.trace_id},
                     {"entity_id", event.entity_id},
                     {"message", event.message},
                     {"severity", event.severity}});
    if (items.size() >= effective_limit) {
      break;
    }
  }
  const auto oldest = events_.empty() ? next_sequence_ : events_.front().sequence;
  return {{"items", std::move(items)},
          {"oldest_available_sequence", oldest},
          {"latest_sequence", next_sequence_ - 1},
          {"gap", after_sequence != 0 && after_sequence + 1 < oldest}};
}

}  // namespace arena_core::observer

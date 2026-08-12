#pragma once

#include "arena_core/core/cpp17_guard.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace arena_core::observer {

struct SessionView {
  std::string session_id;
  std::string player_id;
  std::string state;
  std::string trace_id;
  std::uint64_t generation{0};
  std::uint64_t last_heartbeat_ms{0};
};

struct TicketView {
  std::string ticket_id;
  std::string player_id;
  std::string mode;
  std::string state;
  std::string trace_id;
  std::int32_t mmr{0};
  std::uint64_t waiting_ms{0};
};

struct RoomView {
  std::string room_id;
  std::string match_id;
  std::string state;
  std::string trace_id;
  std::vector<std::string> players;
  std::vector<std::string> ready_players;
  std::string winner_id;
  std::string rejection_reason;
};

struct RankEntryView {
  std::string player_id;
  std::int32_t score{0};
  std::uint32_t rank{0};
};

struct SettlementView {
  std::string settlement_id;
  std::string match_id;
  std::string winner_id;
  std::string loser_id;
  std::string status;
  std::string trace_id;
  bool idempotent_replay{false};
};

struct QaRunView {
  std::string run_id;
  std::string scenario_id;
  std::string name;
  std::string status;
  std::string bug_id;
  std::string detail;
  std::uint64_t duration_ms{0};
};

class ObserverStore final {
 public:
  explicit ObserverStore(std::size_t maximum_events = 2'048);

  void set_health(std::string dependency, bool healthy, std::string detail);
  void upsert_session(SessionView session);
  void remove_session(std::string_view session_id);
  void upsert_ticket(TicketView ticket);
  void upsert_room(RoomView room);
  void add_settlement(SettlementView settlement);
  void replace_rankings(std::string leaderboard, std::vector<RankEntryView> entries,
                        std::string cache_state);
  void add_qa_run(QaRunView run);

  std::uint64_t publish_event(std::string category,
                              std::string type,
                              std::string trace_id,
                              std::string entity_id,
                              std::string message,
                              std::string severity = "info");

  bool run_demo_scenario(std::string_view scenario_id);

  nlohmann::json overview_json() const;
  nlohmann::json sessions_json() const;
  nlohmann::json tickets_json() const;
  nlohmann::json rooms_json() const;
  nlohmann::json rankings_json() const;
  nlohmann::json qa_runs_json() const;
  nlohmann::json events_json(std::uint64_t after_sequence,
                             std::size_t limit,
                             std::string_view trace_id) const;

 private:
  struct HealthView {
    bool healthy{false};
    std::string detail;
  };

  struct RankingView {
    std::string cache_state{"unknown"};
    std::vector<RankEntryView> entries;
  };

  struct EventView {
    std::uint64_t sequence{0};
    std::string timestamp;
    std::string category;
    std::string type;
    std::string trace_id;
    std::string entity_id;
    std::string message;
    std::string severity;
  };

  std::uint64_t append_event_locked(std::string category,
                                    std::string type,
                                    std::string trace_id,
                                    std::string entity_id,
                                    std::string message,
                                    std::string severity);
  void reset_demo_locked();
  void run_eight_player_demo_locked();
  void run_duplicate_settlement_demo_locked();
  void run_redis_repair_demo_locked();

  const std::size_t maximum_events_;
  mutable std::mutex mutex_;
  std::uint64_t next_sequence_{1};
  std::map<std::string, HealthView> health_;
  std::map<std::string, SessionView> sessions_;
  std::map<std::string, TicketView> tickets_;
  std::map<std::string, RoomView> rooms_;
  std::map<std::string, SettlementView> settlements_;
  std::map<std::string, RankingView> rankings_;
  std::map<std::string, QaRunView> qa_runs_;
  std::deque<EventView> events_;
};

}  // namespace arena_core::observer

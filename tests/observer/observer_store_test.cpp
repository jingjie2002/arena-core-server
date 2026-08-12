#include "arena_core/observer/observer_store.hpp"

#include <algorithm>
#include <string>

#include <gtest/gtest.h>

namespace arena_core::observer {
namespace {

TEST(ObserverStoreTest, ReturnedSnapshotsAreIsolatedFromLaterWrites) {
  ObserverStore store;
  store.upsert_session({"session-1", "player-1", "Active", "trace-1", 1, 10});

  const auto first = store.sessions_json();
  store.upsert_session({"session-1", "player-1", "Disconnected", "trace-1", 1, 20});
  const auto second = store.sessions_json();

  EXPECT_EQ(first.at("items").at(0).at("state"), "Active");
  EXPECT_EQ(second.at("items").at(0).at("state"), "Disconnected");
}

TEST(ObserverStoreTest, BoundedEventLogReportsGapToSlowConsumer) {
  ObserverStore store(3);
  for (int index = 1; index <= 5; ++index) {
    store.publish_event("qa", "event." + std::to_string(index), "trace-1",
                        "entity", "message");
  }

  const auto events = store.events_json(1, 100, "trace-1");
  EXPECT_TRUE(events.at("gap").get<bool>());
  EXPECT_EQ(events.at("oldest_available_sequence"), 3);
  ASSERT_EQ(events.at("items").size(), 3U);
  EXPECT_EQ(events.at("items").at(0).at("sequence"), 3);
  EXPECT_EQ(events.at("items").at(2).at("sequence"), 5);
}

TEST(ObserverStoreTest, DemoScenariosExposeEightPlayersAndIdempotentReplay) {
  ObserverStore store;
  ASSERT_TRUE(store.run_demo_scenario("eight-player-flow"));
  const auto overview = store.overview_json();
  EXPECT_EQ(overview.at("counts").at("sessions_online"), 8);
  EXPECT_EQ(overview.at("counts").at("rooms_total"), 4);
  EXPECT_EQ(overview.at("counts").at("settlements"), 4);

  ASSERT_TRUE(store.run_demo_scenario("duplicate-settlement"));
  const auto rankings = store.rankings_json();
  const auto& settlements = rankings.at("settlements");
  ASSERT_EQ(settlements.size(), 4U);
  const auto replay = std::find_if(settlements.begin(), settlements.end(), [](const auto& item) {
    return item.at("settlement_id") == "settlement-demo-1";
  });
  ASSERT_NE(replay, settlements.end());
  EXPECT_TRUE(replay->at("idempotent_replay").template get<bool>());
  EXPECT_EQ(store.events_json(0, 100, "trace-demo-1").at("items").back().at("type"),
            "scenario.passed");
}

TEST(ObserverStoreTest, RedisRepairLeavesFinalHealthFreshAndAuditable) {
  ObserverStore store;
  ASSERT_TRUE(store.run_demo_scenario("redis-repair"));
  const auto overview = store.overview_json();
  EXPECT_TRUE(overview.at("health").at("redis").at("healthy").get<bool>());
  EXPECT_EQ(store.rankings_json().at("leaderboards").at(0).at("cache_state"), "fresh");
  const auto events = store.events_json(0, 100, "trace-demo-repair");
  ASSERT_GE(events.at("items").size(), 3U);
  EXPECT_EQ(events.at("items").at(0).at("type"), "rank.cache_stale");
}

}  // namespace
}  // namespace arena_core::observer

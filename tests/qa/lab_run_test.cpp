#include "arena_core/qa/lab_run.hpp"

#include <chrono>
#include <filesystem>
#include <thread>

#include <gtest/gtest.h>

namespace arena_core::qa {
namespace {

using namespace std::chrono_literals;

TEST(LabRunStateMachineTest, PreservesCancellationAndTimeoutTerminalStates) {
  LabRunStateMachine machine;
  EXPECT_FALSE(machine.transition(LabRunState::kStarting));
  EXPECT_FALSE(machine.transition(LabRunState::kRunning));
  EXPECT_FALSE(machine.transition(LabRunState::kCancelling));
  EXPECT_FALSE(machine.transition(LabRunState::kCleaning));
  EXPECT_FALSE(machine.transition(LabRunState::kCancelled));
  EXPECT_TRUE(machine.transition(LabRunState::kPassed));

  LabRunStateMachine timeout;
  EXPECT_FALSE(timeout.transition(LabRunState::kStarting));
  EXPECT_FALSE(timeout.transition(LabRunState::kRunning));
  EXPECT_FALSE(timeout.transition(LabRunState::kTimingOut));
  EXPECT_FALSE(timeout.transition(LabRunState::kCleaning));
  EXPECT_FALSE(timeout.transition(LabRunState::kTimedOut));
}

TEST(LabRunReporterTest, EmitsMonotonicEventsAndRedactedRoundTripReport) {
  LabRunEnvironment environment;
  environment.result_token = "must-never-appear";
  auto request = parse_lab_scenario_request({{"scenario", "complete_match"},
                                             {"parameters", {{"player_count", 2}}}});
  ASSERT_TRUE(request.ok());
  LabRunReporter reporter("run-1", *request.value, environment);
  EXPECT_TRUE(reporter.snapshot().evidence.is_object());
  reporter.event("one", "first");
  reporter.event("two", "second");
  reporter.assertion("a", "works", true, true, true);
  reporter.cleanup(true, "done");
  reporter.finish();
  const auto value = lab_run_report_json(reporter.snapshot());
  EXPECT_EQ(value.dump().find("must-never-appear"), std::string::npos);
  ASSERT_EQ(value.at("events").size(), 2U);
  EXPECT_EQ(value.at("events").at(0).at("seq"), 1);
  EXPECT_EQ(value.at("events").at(1).at("seq"), 2);
  auto parsed = parse_lab_run_report(value);
  ASSERT_TRUE(parsed.ok()) << parsed.error.message;
  EXPECT_EQ(parsed.value->run_id, "run-1");
}

TEST(LabRunEngineTest, AllowsOneActiveRunAndAlwaysCleansOnCancel) {
  const auto directory = std::filesystem::temp_directory_path() / "arena-core-lab-run-test";
  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
  LabRunEnvironment environment;
  auto handler = [](const LabScenarioRequest&, const LabRunEnvironment&,
                    const LabScenarioExecution& execution) {
    while (!execution.cancellation->wait_for(5ms)) {
      if (std::chrono::steady_clock::now() >= execution.deadline) {
        return make_error(ErrorCode::kQaRunTimedOut);
      }
    }
    return make_error(ErrorCode::kQaRunCancelled);
  };
  auto engine = std::make_shared<LabRunEngine>(environment, directory,
      std::map<LabScenarioKind, LabScenarioHandler>{{LabScenarioKind::kCompleteMatch, handler}});
  ASSERT_FALSE(engine->start());
  auto request = parse_lab_scenario_request({{"scenario", "complete_match"},
                                             {"parameters", {{"player_count", 2}}}});
  ASSERT_TRUE(request.ok());
  auto first = engine->create(*request.value);
  ASSERT_TRUE(first.ok());
  auto second = engine->create(*request.value);
  ASSERT_FALSE(second.ok());
  EXPECT_EQ(second.error.code, ErrorCode::kQaRunAlreadyActive);
  EXPECT_FALSE(engine->cancel(first.value->run_id));
  for (int attempt = 0; attempt < 100 && engine->active(); ++attempt) {
    std::this_thread::sleep_for(5ms);
  }
  auto final = engine->get(first.value->run_id);
  ASSERT_TRUE(final.ok()) << final.error.message;
  EXPECT_EQ(final.value->status, LabRunState::kCancelled);
  EXPECT_TRUE(final.value->cleanup.completed);
  engine->stop();
  std::filesystem::remove_all(directory, ignored);
}

TEST(LabRunEngineTest, JoinsEachCompletedWorkerBeforeStartingTheNextRun) {
  const auto directory = std::filesystem::temp_directory_path() / "arena-core-lab-repeat-test";
  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
  LabRunEnvironment environment;
  auto handler = [](const LabScenarioRequest&, const LabRunEnvironment&,
                    const LabScenarioExecution& execution) {
    execution.reporter->assertion("done", "handler completed", true, true, true);
    return Error{};
  };
  auto engine = std::make_shared<LabRunEngine>(environment, directory,
      std::map<LabScenarioKind, LabScenarioHandler>{{LabScenarioKind::kCompleteMatch, handler}});
  ASSERT_FALSE(engine->start());
  auto request = parse_lab_scenario_request({{"scenario", "complete_match"},
                                             {"parameters", {{"player_count", 2}}}});
  ASSERT_TRUE(request.ok());
  for (int index = 0; index < 25; ++index) {
    auto created = engine->create(*request.value);
    ASSERT_TRUE(created.ok()) << created.error.message;
    for (int attempt = 0; attempt < 100 && engine->active(); ++attempt) {
      std::this_thread::sleep_for(2ms);
    }
    ASSERT_FALSE(engine->active());
    const auto completed = engine->get(created.value->run_id);
    ASSERT_TRUE(completed.ok()) << completed.error.message;
    EXPECT_EQ(completed.value->status, LabRunState::kPassed);
  }
  engine->stop();
  std::filesystem::remove_all(directory, ignored);
}

TEST(LabRunEngineTest, MapsScenarioDeadlineErrorToTimedOutTerminalState) {
  const auto directory = std::filesystem::temp_directory_path() / "arena-core-lab-timeout-test";
  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
  LabRunEnvironment environment;
  auto handler = [](const LabScenarioRequest&, const LabRunEnvironment&,
                    const LabScenarioExecution&) {
    return make_error(ErrorCode::kQaRunTimedOut);
  };
  LabRunEngine engine(environment, directory,
      std::map<LabScenarioKind, LabScenarioHandler>{{LabScenarioKind::kCompleteMatch, handler}});
  ASSERT_FALSE(engine.start());
  auto request = parse_lab_scenario_request({{"scenario", "complete_match"},
                                             {"timeout_ms", 1'000},
                                             {"parameters", {{"player_count", 2}}}});
  ASSERT_TRUE(request.ok());
  auto created = engine.create(*request.value);
  ASSERT_TRUE(created.ok());
  for (int attempt = 0; attempt < 100 && engine.active(); ++attempt) {
    std::this_thread::sleep_for(5ms);
  }
  const auto completed = engine.get(created.value->run_id);
  ASSERT_TRUE(completed.ok()) << completed.error.message;
  EXPECT_EQ(completed.value->status, LabRunState::kTimedOut);
  EXPECT_TRUE(completed.value->cleanup.completed);
  engine.stop();
  std::filesystem::remove_all(directory, ignored);
}

}  // namespace
}  // namespace arena_core::qa

#pragma once

#include "arena_core/core/cpp17_guard.hpp"
#include "arena_core/core/result.hpp"
#include "arena_core/qa/lab_scenario.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace arena_core::qa {

enum class LabRunState {
  kValidating,
  kStarting,
  kRunning,
  kVerifying,
  kCancelling,
  kTimingOut,
  kFailing,
  kCleaning,
  kPassed,
  kFailed,
  kCancelled,
  kTimedOut,
  kInterrupted,
};

std::string lab_run_state_name(LabRunState state);
bool lab_run_state_terminal(LabRunState state) noexcept;

class LabRunStateMachine final {
 public:
  LabRunState state() const noexcept { return state_; }
  Error transition(LabRunState next);

 private:
  LabRunState state_{LabRunState::kValidating};
};

struct LabRunEvent {
  std::uint64_t seq{0};
  std::string timestamp;
  std::string phase;
  std::string type;
  std::string message;
  nlohmann::json detail = nlohmann::json::object();
};

struct LabAssertion {
  std::string id;
  std::string title;
  nlohmann::json expected;
  nlohmann::json actual;
  bool passed{false};
};

struct LabCleanupEvidence {
  bool completed{false};
  std::string detail{"not started"};
};

struct LabRunReport {
  std::uint32_t schema_version{1};
  std::string run_id;
  std::string replay_of;
  LabScenarioRequest request;
  LabRunState status{LabRunState::kValidating};
  std::string project_version;
  std::string git_commit;
  std::string service_config_digest;
  std::string started_at;
  std::string finished_at;
  std::uint64_t elapsed_ms{0};
  std::string first_failure;
  std::vector<LabRunEvent> events;
  std::vector<LabAssertion> assertions;
  nlohmann::json evidence = nlohmann::json::object();
  LabCleanupEvidence cleanup;
};

nlohmann::json lab_run_report_json(const LabRunReport& report, bool include_events = true);
Result<LabRunReport> parse_lab_run_report(const nlohmann::json& value);

class LabRunReporter final {
 public:
  LabRunReporter(std::string run_id,
                 LabScenarioRequest request,
                 const LabRunEnvironment& environment,
                 std::string replay_of = {});

  Error transition(LabRunState next);
  void event(std::string type, std::string message,
             nlohmann::json detail = nlohmann::json::object());
  void assertion(std::string id, std::string title,
                 nlohmann::json expected, nlohmann::json actual, bool passed);
  void evidence(std::string key, nlohmann::json value);
  void fail(std::string message);
  void cleanup(bool completed, std::string detail);
  void finish();
  LabRunReport snapshot() const;

 private:
  mutable std::mutex mutex_;
  LabRunStateMachine state_;
  LabRunReport report_;
  std::chrono::steady_clock::time_point started_;
  std::uint64_t next_seq_{1};
};

class LabCancellation final {
 public:
  void request() noexcept;
  bool requested() const noexcept;
  bool wait_for(std::chrono::milliseconds duration);

 private:
  std::atomic<bool> requested_{false};
  std::mutex mutex_;
  std::condition_variable changed_;
};

struct LabScenarioExecution {
  std::shared_ptr<LabRunReporter> reporter;
  std::shared_ptr<LabCancellation> cancellation;
  std::chrono::steady_clock::time_point deadline;
};

using LabScenarioHandler = std::function<Error(const LabScenarioRequest&,
                                                const LabRunEnvironment&,
                                                const LabScenarioExecution&)>;

class LabRunStore final {
 public:
  explicit LabRunStore(std::filesystem::path directory);

  Error initialize();
  Error save(const LabRunReport& report) const;
  Result<LabRunReport> load(const std::string& run_id) const;

 private:
  std::filesystem::path directory_;
};

class LabRunEngine final {
 public:
  LabRunEngine(LabRunEnvironment environment,
               std::filesystem::path report_directory,
               std::map<LabScenarioKind, LabScenarioHandler> handlers);
  ~LabRunEngine();

  Error start();
  void stop();
  Result<LabRunReport> create(LabScenarioRequest request, std::string replay_of = {});
  Result<LabRunReport> get(const std::string& run_id) const;
  Result<LabRunReport> replay(const std::string& run_id);
  Error cancel(const std::string& run_id);
  nlohmann::json events(const std::string& run_id, std::uint64_t after_seq) const;
  bool active() const;

 private:
  struct ActiveRun;
  void execute(const std::shared_ptr<ActiveRun>& run);
  std::string next_run_id();

  LabRunEnvironment environment_;
  LabRunStore store_;
  std::map<LabScenarioKind, LabScenarioHandler> handlers_;
  mutable std::mutex mutex_;
  std::shared_ptr<ActiveRun> active_;
  // A completed worker cannot destroy its own joinable std::thread. Keep it
  // here until the next create()/stop() call joins it from another thread.
  std::shared_ptr<ActiveRun> finished_;
  std::map<std::string, LabRunReport> completed_;
  std::uint64_t id_counter_{0};
  bool started_{false};
  bool stopped_{false};
};

}  // namespace arena_core::qa

#include "arena_core/qa/lab_run.hpp"

#include "arena_core/core/version.hpp"
#include "arena_core/qa/demo_battle_adapter.hpp"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <system_error>
#include <utility>

namespace arena_core::qa {
namespace {

bool allowed_transition(const LabRunState from, const LabRunState to) {
  switch (from) {
    case LabRunState::kValidating: return to == LabRunState::kStarting || to == LabRunState::kFailing;
    case LabRunState::kStarting:
      return to == LabRunState::kRunning || to == LabRunState::kCancelling ||
             to == LabRunState::kTimingOut || to == LabRunState::kFailing;
    case LabRunState::kRunning:
      return to == LabRunState::kVerifying || to == LabRunState::kCancelling ||
             to == LabRunState::kTimingOut || to == LabRunState::kFailing;
    case LabRunState::kVerifying:
      return to == LabRunState::kCleaning || to == LabRunState::kFailing;
    case LabRunState::kCancelling:
    case LabRunState::kTimingOut:
    case LabRunState::kFailing:
      return to == LabRunState::kCleaning;
    case LabRunState::kCleaning:
      return to == LabRunState::kPassed || to == LabRunState::kFailed ||
             to == LabRunState::kCancelled || to == LabRunState::kTimedOut ||
             to == LabRunState::kInterrupted;
    case LabRunState::kPassed:
    case LabRunState::kFailed:
    case LabRunState::kCancelled:
    case LabRunState::kTimedOut:
    case LabRunState::kInterrupted:
      return false;
  }
  return false;
}

std::string sanitize_run_id(const std::string& value) {
  if (value.empty() || value.size() > 80U ||
      !std::all_of(value.begin(), value.end(), [](const char character) {
        return (character >= 'a' && character <= 'z') ||
               (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '-';
      })) {
    return {};
  }
  return value;
}

LabRunState parse_state(const std::string& value) {
  const std::vector<LabRunState> states{LabRunState::kValidating, LabRunState::kStarting,
      LabRunState::kRunning, LabRunState::kVerifying, LabRunState::kCancelling,
      LabRunState::kTimingOut, LabRunState::kFailing, LabRunState::kCleaning, LabRunState::kPassed,
      LabRunState::kFailed, LabRunState::kCancelled, LabRunState::kTimedOut,
      LabRunState::kInterrupted};
  const auto found = std::find_if(states.begin(), states.end(), [&](const LabRunState state) {
    return lab_run_state_name(state) == value;
  });
  return found == states.end() ? LabRunState::kInterrupted : *found;
}

}  // namespace

std::string lab_run_state_name(const LabRunState state) {
  switch (state) {
    case LabRunState::kValidating: return "Validating";
    case LabRunState::kStarting: return "Starting";
    case LabRunState::kRunning: return "Running";
    case LabRunState::kVerifying: return "Verifying";
    case LabRunState::kCancelling: return "Cancelling";
    case LabRunState::kTimingOut: return "TimingOut";
    case LabRunState::kFailing: return "Failing";
    case LabRunState::kCleaning: return "Cleaning";
    case LabRunState::kPassed: return "Passed";
    case LabRunState::kFailed: return "Failed";
    case LabRunState::kCancelled: return "Cancelled";
    case LabRunState::kTimedOut: return "TimedOut";
    case LabRunState::kInterrupted: return "Interrupted";
  }
  return "Interrupted";
}

bool lab_run_state_terminal(const LabRunState state) noexcept {
  return state == LabRunState::kPassed || state == LabRunState::kFailed ||
         state == LabRunState::kCancelled || state == LabRunState::kTimedOut ||
         state == LabRunState::kInterrupted;
}

Error LabRunStateMachine::transition(const LabRunState next) {
  if (state_ == next) return {};
  if (!allowed_transition(state_, next)) {
    return make_error(ErrorCode::kQaScenarioInvalid,
                      "invalid Run transition " + lab_run_state_name(state_) + " -> " +
                          lab_run_state_name(next));
  }
  state_ = next;
  return {};
}

nlohmann::json lab_run_report_json(const LabRunReport& report, const bool include_events) {
  nlohmann::json events = nlohmann::json::array();
  if (include_events) {
    for (const auto& event : report.events) {
      events.push_back({{"seq", event.seq}, {"timestamp", event.timestamp}, {"phase", event.phase},
                        {"type", event.type}, {"message", event.message}, {"detail", event.detail}});
    }
  }
  nlohmann::json assertions = nlohmann::json::array();
  for (const auto& assertion : report.assertions) {
    assertions.push_back({{"id", assertion.id}, {"title", assertion.title},
                          {"expected", assertion.expected}, {"actual", assertion.actual},
                          {"passed", assertion.passed}});
  }
  nlohmann::json result{{"schema_version", report.schema_version}, {"run_id", report.run_id},
                        {"replay_of", report.replay_of}, {"scenario", report.request.name},
                        {"status", lab_run_state_name(report.status)},
                        {"parameters", report.request.parameters}, {"seed", report.request.seed},
                        {"timeout_ms", report.request.timeout_ms},
                        {"project_version", report.project_version}, {"git_commit", report.git_commit},
                        {"service_config_digest", report.service_config_digest},
                        {"started_at", report.started_at}, {"finished_at", report.finished_at},
                        {"elapsed_ms", report.elapsed_ms}, {"first_failure", report.first_failure},
                        {"assertions", std::move(assertions)}, {"evidence", report.evidence},
                        {"cleanup", {{"completed", report.cleanup.completed},
                                     {"detail", report.cleanup.detail}}}};
  if (include_events) result["events"] = std::move(events);
  return result;
}

Result<LabRunReport> parse_lab_run_report(const nlohmann::json& value) {
  try {
    LabRunReport report;
    report.schema_version = value.at("schema_version").get<std::uint32_t>();
    report.run_id = value.at("run_id").get<std::string>();
    report.replay_of = value.value("replay_of", std::string{});
    auto request = parse_lab_scenario_request({{"schema_version", report.schema_version},
        {"scenario", value.at("scenario")}, {"seed", value.at("seed")},
        {"timeout_ms", value.at("timeout_ms")}, {"parameters", value.at("parameters")}});
    if (!request.ok()) return Result<LabRunReport>::failure(request.error);
    report.request = std::move(*request.value);
    report.status = parse_state(value.at("status").get<std::string>());
    report.project_version = value.value("project_version", std::string{});
    report.git_commit = value.value("git_commit", std::string{});
    report.service_config_digest = value.value("service_config_digest", std::string{});
    report.started_at = value.value("started_at", std::string{});
    report.finished_at = value.value("finished_at", std::string{});
    report.elapsed_ms = value.value("elapsed_ms", 0ULL);
    report.first_failure = value.value("first_failure", std::string{});
    report.evidence = value.value("evidence", nlohmann::json::object());
    const auto cleanup = value.value("cleanup", nlohmann::json::object());
    report.cleanup.completed = cleanup.value("completed", false);
    report.cleanup.detail = cleanup.value("detail", std::string{});
    for (const auto& item : value.value("assertions", nlohmann::json::array())) {
      report.assertions.push_back({item.at("id").get<std::string>(),
          item.at("title").get<std::string>(), item.at("expected"), item.at("actual"),
          item.at("passed").get<bool>()});
    }
    for (const auto& item : value.value("events", nlohmann::json::array())) {
      report.events.push_back({item.at("seq").get<std::uint64_t>(),
          item.at("timestamp").get<std::string>(), item.at("phase").get<std::string>(),
          item.at("type").get<std::string>(), item.at("message").get<std::string>(),
          item.value("detail", nlohmann::json::object())});
    }
    return Result<LabRunReport>::success(std::move(report));
  } catch (const std::exception& error) {
    return Result<LabRunReport>::failure(
        make_error(ErrorCode::kQaScenarioInvalid, std::string("invalid Run report: ") + error.what()));
  }
}

LabRunReporter::LabRunReporter(std::string run_id,
                               LabScenarioRequest request,
                               const LabRunEnvironment& environment,
                               std::string replay_of)
    : started_(std::chrono::steady_clock::now()) {
  report_.run_id = std::move(run_id);
  report_.replay_of = std::move(replay_of);
  report_.request = std::move(request);
  report_.project_version = std::string(kProjectVersion);
  report_.git_commit = environment.git_commit;
  report_.service_config_digest = environment.service_config_digest;
  report_.started_at = current_utc_rfc3339();
  report_.status = state_.state();
}

Error LabRunReporter::transition(const LabRunState next) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto error = state_.transition(next);
  if (!error) report_.status = next;
  return error;
}

void LabRunReporter::event(std::string type, std::string message, nlohmann::json detail) {
  std::lock_guard<std::mutex> lock(mutex_);
  report_.events.push_back({next_seq_++, current_utc_rfc3339(), lab_run_state_name(state_.state()),
                            std::move(type), std::move(message), std::move(detail)});
}

void LabRunReporter::assertion(std::string id, std::string title, nlohmann::json expected,
                               nlohmann::json actual, const bool passed) {
  std::lock_guard<std::mutex> lock(mutex_);
  report_.assertions.push_back({std::move(id), std::move(title), std::move(expected),
                                std::move(actual), passed});
  if (!passed && report_.first_failure.empty()) report_.first_failure = report_.assertions.back().title;
}

void LabRunReporter::evidence(std::string key, nlohmann::json value) {
  std::lock_guard<std::mutex> lock(mutex_);
  report_.evidence[std::move(key)] = std::move(value);
}

void LabRunReporter::fail(std::string message) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (report_.first_failure.empty()) report_.first_failure = std::move(message);
}

void LabRunReporter::cleanup(const bool completed, std::string detail) {
  std::lock_guard<std::mutex> lock(mutex_);
  report_.cleanup = {completed, std::move(detail)};
}

void LabRunReporter::finish() {
  std::lock_guard<std::mutex> lock(mutex_);
  report_.finished_at = current_utc_rfc3339();
  report_.elapsed_ms = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - started_).count());
}

LabRunReport LabRunReporter::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return report_;
}

void LabCancellation::request() noexcept {
  requested_.store(true, std::memory_order_release);
  changed_.notify_all();
}

bool LabCancellation::requested() const noexcept {
  return requested_.load(std::memory_order_acquire);
}

bool LabCancellation::wait_for(const std::chrono::milliseconds duration) {
  std::unique_lock<std::mutex> lock(mutex_);
  return changed_.wait_for(lock, duration, [this] { return requested(); });
}

LabRunStore::LabRunStore(std::filesystem::path directory) : directory_(std::move(directory)) {}

Error LabRunStore::initialize() {
  std::error_code error;
  std::filesystem::create_directories(directory_, error);
  if (error) return make_error(ErrorCode::kQaCleanupFailed, "cannot create Lab report directory");
  return {};
}

Error LabRunStore::save(const LabRunReport& report) const {
  if (sanitize_run_id(report.run_id).empty()) {
    return make_error(ErrorCode::kQaCleanupFailed, "invalid generated run id");
  }
  const auto final_path = directory_ / (report.run_id + ".json");
  const auto temporary_path = directory_ / (report.run_id + ".tmp");
  {
    std::ofstream output(temporary_path, std::ios::trunc);
    if (!output) return make_error(ErrorCode::kQaCleanupFailed, "cannot write Run report");
    output << lab_run_report_json(report).dump(2) << '\n';
    if (!output) return make_error(ErrorCode::kQaCleanupFailed, "Run report write failed");
  }
  std::error_code error;
  std::filesystem::rename(temporary_path, final_path, error);
  if (error) {
    std::filesystem::remove(final_path, error);
    error.clear();
    std::filesystem::rename(temporary_path, final_path, error);
  }
  if (error) return make_error(ErrorCode::kQaCleanupFailed, "Run report atomic replace failed");
  return {};
}

Result<LabRunReport> LabRunStore::load(const std::string& run_id) const {
  const auto safe = sanitize_run_id(run_id);
  if (safe.empty()) return Result<LabRunReport>::failure(make_error(ErrorCode::kQaRunNotFound));
  std::ifstream input(directory_ / (safe + ".json"));
  if (!input) return Result<LabRunReport>::failure(make_error(ErrorCode::kQaRunNotFound));
  try {
    nlohmann::json value;
    input >> value;
    return parse_lab_run_report(value);
  } catch (const std::exception&) {
    return Result<LabRunReport>::failure(make_error(ErrorCode::kQaRunNotFound));
  }
}

struct LabRunEngine::ActiveRun {
  std::shared_ptr<LabRunReporter> reporter;
  std::shared_ptr<LabCancellation> cancellation;
  std::thread worker;
};

LabRunEngine::LabRunEngine(LabRunEnvironment environment,
                           std::filesystem::path report_directory,
                           std::map<LabScenarioKind, LabScenarioHandler> handlers)
    : environment_(std::move(environment)), store_(std::move(report_directory)),
      handlers_(std::move(handlers)) {}

LabRunEngine::~LabRunEngine() { stop(); }

Error LabRunEngine::start() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (started_ && !stopped_) return {};
  if (auto error = store_.initialize()) return error;
  started_ = true;
  stopped_ = false;
  return {};
}

void LabRunEngine::stop() {
  std::shared_ptr<ActiveRun> active;
  std::shared_ptr<ActiveRun> finished;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_) return;
    stopped_ = true;
    active = active_;
    finished = std::move(finished_);
  }
  if (active) {
    active->cancellation->request();
    if (active->worker.joinable() && active->worker.get_id() != std::this_thread::get_id()) {
      active->worker.join();
    }
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (finished_ != nullptr) finished = std::move(finished_);
  }
  if (finished && finished != active && finished->worker.joinable() &&
      finished->worker.get_id() != std::this_thread::get_id()) {
    finished->worker.join();
  }
}

std::string LabRunEngine::next_run_id() {
  std::ostringstream output;
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  output << "run-" << std::chrono::duration_cast<std::chrono::milliseconds>(now).count()
         << '-' << ++id_counter_;
  return output.str();
}

Result<LabRunReport> LabRunEngine::create(LabScenarioRequest request, std::string replay_of) {
  std::shared_ptr<ActiveRun> finished;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_ || stopped_) {
      return Result<LabRunReport>::failure(make_error(ErrorCode::kServerShuttingDown));
    }
    // Do not race an about-to-finish worker: callers must retry after the
    // current Run reaches a terminal state.
    if (active_ != nullptr) {
      return Result<LabRunReport>::failure(make_error(ErrorCode::kQaRunAlreadyActive));
    }
    finished = std::move(finished_);
  }
  if (finished && finished->worker.joinable() &&
      finished->worker.get_id() != std::this_thread::get_id()) {
    finished->worker.join();
  }
  std::shared_ptr<ActiveRun> run;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_ || stopped_) {
      return Result<LabRunReport>::failure(make_error(ErrorCode::kServerShuttingDown));
    }
    if (active_ != nullptr) {
      return Result<LabRunReport>::failure(make_error(ErrorCode::kQaRunAlreadyActive));
    }
    if (request.seed == 0U) {
      std::random_device random;
      request.seed = (static_cast<std::uint64_t>(random()) << 32U) | random();
      if (request.seed == 0U) request.seed = 1U;
    }
    run = std::make_shared<ActiveRun>();
    run->cancellation = std::make_shared<LabCancellation>();
    run->reporter = std::make_shared<LabRunReporter>(next_run_id(), std::move(request),
                                                     environment_, std::move(replay_of));
    active_ = run;
    run->worker = std::thread([this, run] { execute(run); });
  }
  return Result<LabRunReport>::success(run->reporter->snapshot());
}

void LabRunEngine::execute(const std::shared_ptr<ActiveRun>& run) {
  auto report = run->reporter;
  static_cast<void>(report->transition(LabRunState::kStarting));
  report->event("run.starting", "正在准备真实场景");
  const auto snapshot = report->snapshot();
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(snapshot.request.timeout_ms);
  Error execution_error;
  if (run->cancellation->requested()) {
    static_cast<void>(report->transition(LabRunState::kCancelling));
  } else {
    static_cast<void>(report->transition(LabRunState::kRunning));
    report->event("run.running", "场景正在驱动真实协议与服务边界");
    const auto handler = handlers_.find(snapshot.request.kind);
    if (handler == handlers_.end()) {
      execution_error = make_error(ErrorCode::kQaScenarioInvalid, "scenario handler is unavailable");
    } else {
      execution_error = handler->second(snapshot.request, environment_,
                                         {report, run->cancellation, deadline});
    }
  }

  const auto cancelled = run->cancellation->requested();
  const auto timed_out = !cancelled &&
      (std::chrono::steady_clock::now() >= deadline ||
       execution_error.code == ErrorCode::kQaRunTimedOut);
  if (timed_out) {
    if (report->snapshot().status == LabRunState::kRunning) {
      static_cast<void>(report->transition(LabRunState::kTimingOut));
    }
    report->fail("Run 超过有限截止时间");
    report->event("run.timed_out", "Run 已超时，开始清理");
  } else if (cancelled) {
    const auto state = report->snapshot().status;
    if (state == LabRunState::kRunning || state == LabRunState::kStarting) {
      static_cast<void>(report->transition(LabRunState::kCancelling));
    }
    report->event("run.cancelled", "收到取消请求，开始清理");
  } else if (execution_error) {
    report->fail(execution_error.message);
    const auto state = report->snapshot().status;
    if (state == LabRunState::kRunning || state == LabRunState::kVerifying) {
      static_cast<void>(report->transition(LabRunState::kFailing));
    }
    report->event("run.failed", "场景执行或断言失败", {{"error", execution_error.message}});
  } else {
    static_cast<void>(report->transition(LabRunState::kVerifying));
    report->event("run.verifying", "正在汇总自动断言");
  }

  const auto before_cleaning = report->snapshot().status;
  static_cast<void>(report->transition(LabRunState::kCleaning));
  report->event("run.cleaning", "正在关闭临时客户端并复原依赖代理");
  report->cleanup(true, "临时客户端已退出；本次 Run 的依赖故障已复原");
  LabRunState terminal = LabRunState::kPassed;
  if (timed_out || before_cleaning == LabRunState::kTimingOut) terminal = LabRunState::kTimedOut;
  else if (cancelled || before_cleaning == LabRunState::kCancelling) terminal = LabRunState::kCancelled;
  else if (execution_error || before_cleaning == LabRunState::kFailing) terminal = LabRunState::kFailed;
  // Keep the externally visible active Run in Cleaning until its durable
  // report is complete. Otherwise a caller can observe Passed and immediately
  // receive QA_RUN_ALREADY_ACTIVE while the worker is still saving the file.
  report->finish();
  auto final_report = report->snapshot();
  final_report.status = terminal;
  const auto final_sequence = final_report.events.empty()
      ? 1U : final_report.events.back().seq + 1U;
  final_report.events.push_back(
      {final_sequence, current_utc_rfc3339(), lab_run_state_name(terminal),
       "run.finished", "Run 已结束，资源清理完成",
       {{"status", lab_run_state_name(terminal)}}});
  if (const auto save_error = store_.save(final_report)) {
    final_report.status = LabRunState::kFailed;
    final_report.cleanup = {false, save_error.message};
    if (final_report.first_failure.empty()) final_report.first_failure = save_error.message;
    final_report.events.push_back({final_report.events.back().seq + 1U, current_utc_rfc3339(),
                                   "Failed", "run.report_not_saved",
                                   "Run 报告无法持久化", {{"error", save_error.message}}});
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    completed_[final_report.run_id] = final_report;
    if (active_ == run) {
      active_.reset();
      finished_ = run;
    }
  }
}

Result<LabRunReport> LabRunEngine::get(const std::string& run_id) const {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_ && active_->reporter->snapshot().run_id == run_id) {
      return Result<LabRunReport>::success(active_->reporter->snapshot());
    }
    const auto found = completed_.find(run_id);
    if (found != completed_.end()) return Result<LabRunReport>::success(found->second);
  }
  return store_.load(run_id);
}

Result<LabRunReport> LabRunEngine::replay(const std::string& run_id) {
  auto previous = get(run_id);
  if (!previous.ok()) return previous;
  return create(previous.value->request, run_id);
}

Error LabRunEngine::cancel(const std::string& run_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!active_ || active_->reporter->snapshot().run_id != run_id) {
    return make_error(ErrorCode::kQaRunNotCancellable);
  }
  active_->cancellation->request();
  return {};
}

nlohmann::json LabRunEngine::events(const std::string& run_id, const std::uint64_t after_seq) const {
  auto report = get(run_id);
  if (!report.ok()) return {{"error", std::string(error_code_name(report.error.code))}};
  nlohmann::json items = nlohmann::json::array();
  std::uint64_t next = after_seq;
  for (const auto& event : report.value->events) {
    if (event.seq <= after_seq) continue;
    items.push_back({{"seq", event.seq}, {"timestamp", event.timestamp},
                     {"phase", event.phase}, {"type", event.type},
                     {"message", event.message}, {"detail", event.detail}});
    next = event.seq;
  }
  return {{"run_id", run_id}, {"after_seq", after_seq}, {"next_seq", next},
          {"items", std::move(items)}};
}

bool LabRunEngine::active() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return active_ != nullptr;
}

}  // namespace arena_core::qa

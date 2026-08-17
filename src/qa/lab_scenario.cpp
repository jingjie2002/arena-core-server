#include "arena_core/qa/lab_scenario.hpp"

#include "arena_core/core/version.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <set>
#include <utility>

namespace arena_core::qa {
namespace {

Error invalid(std::string message) {
  return make_error(ErrorCode::kQaScenarioInvalid, std::move(message));
}

bool only_fields(const nlohmann::json& value, const std::set<std::string>& allowed) {
  for (auto item = value.begin(); item != value.end(); ++item) {
    if (allowed.count(item.key()) == 0U) return false;
  }
  return true;
}

Result<std::uint32_t> bounded_uint(const nlohmann::json& value,
                                   const std::string& key,
                                   const std::uint32_t fallback,
                                   const std::uint32_t minimum,
                                   const std::uint32_t maximum) {
  if (!value.contains(key)) {
    return Result<std::uint32_t>::success(fallback);
  }
  if (!value.at(key).is_number_unsigned() && !value.at(key).is_number_integer()) {
    return Result<std::uint32_t>::failure(invalid(key + " must be an integer"));
  }
  const auto numeric = value.at(key).get<std::int64_t>();
  if (numeric < static_cast<std::int64_t>(minimum) ||
      numeric > static_cast<std::int64_t>(maximum)) {
    return Result<std::uint32_t>::failure(invalid(key + " is outside its safe range"));
  }
  return Result<std::uint32_t>::success(static_cast<std::uint32_t>(numeric));
}

Result<std::int32_t> bounded_int(const nlohmann::json& value,
                                 const std::string& key,
                                 const std::int32_t fallback,
                                 const std::int32_t minimum,
                                 const std::int32_t maximum) {
  if (!value.contains(key)) {
    return Result<std::int32_t>::success(fallback);
  }
  if (!value.at(key).is_number_integer()) {
    return Result<std::int32_t>::failure(invalid(key + " must be an integer"));
  }
  const auto numeric = value.at(key).get<std::int64_t>();
  if (numeric < minimum || numeric > maximum) {
    return Result<std::int32_t>::failure(invalid(key + " is outside its safe range"));
  }
  return Result<std::int32_t>::success(static_cast<std::int32_t>(numeric));
}

Result<std::string> enum_string(const nlohmann::json& value,
                                const std::string& key,
                                std::string fallback,
                                const std::set<std::string>& allowed) {
  if (!value.contains(key)) {
    return Result<std::string>::success(std::move(fallback));
  }
  if (!value.at(key).is_string()) {
    return Result<std::string>::failure(invalid(key + " must be a string"));
  }
  auto result = value.at(key).get<std::string>();
  if (allowed.count(result) == 0U) {
    return Result<std::string>::failure(invalid(key + " is not a supported value"));
  }
  return Result<std::string>::success(std::move(result));
}

nlohmann::json field(std::string name,
                     std::string label,
                     std::string type,
                     nlohmann::json default_value,
                     const nlohmann::json& options = nlohmann::json::object()) {
  nlohmann::json result{{"name", std::move(name)}, {"label", std::move(label)},
                        {"type", std::move(type)}, {"default", std::move(default_value)}};
  result.update(options);
  return result;
}

Result<LabScenarioRequest> validate_complete(LabScenarioRequest request) {
  static const std::set<std::string> allowed{"player_count", "mmr_mode", "mmr_base",
                                             "mmr_stride", "connect_interval_ms",
                                             "ready_delay_ms"};
  if (!only_fields(request.parameters, allowed)) {
    return Result<LabScenarioRequest>::failure(invalid("complete_match contains an unknown field"));
  }
  auto players = bounded_uint(request.parameters, "player_count", 2, 2, 32);
  auto mode = enum_string(request.parameters, "mmr_mode", "uniform", {"uniform", "stair"});
  auto base = bounded_int(request.parameters, "mmr_base", 1'000, 0, 10'000);
  auto stride = bounded_int(request.parameters, "mmr_stride", 100, 0, 10'000);
  auto connect = bounded_uint(request.parameters, "connect_interval_ms", 0, 0, 5'000);
  auto ready = bounded_uint(request.parameters, "ready_delay_ms", 0, 0, 5'000);
  if (!players.ok() || !mode.ok() || !base.ok() || !stride.ok() || !connect.ok() || !ready.ok()) {
    const std::array<const Error*, 6> errors{&players.error, &mode.error, &base.error,
                                             &stride.error, &connect.error, &ready.error};
    const auto found = std::find_if(errors.begin(), errors.end(), [](const Error* error) {
      return static_cast<bool>(*error);
    });
    return Result<LabScenarioRequest>::failure(**found);
  }
  if (*players.value % 2U != 0U) {
    return Result<LabScenarioRequest>::failure(invalid("player_count must be even"));
  }
  const auto pair_count = *players.value / 2U;
  const auto maximum_mmr = static_cast<std::int64_t>(*base.value) +
                           static_cast<std::int64_t>(pair_count - 1U) * *stride.value;
  if (*mode.value == "stair" && maximum_mmr > 10'000) {
    return Result<LabScenarioRequest>::failure(invalid("stair MMR values exceed 10000"));
  }
  request.parameters = {{"player_count", *players.value}, {"mmr_mode", *mode.value},
                        {"mmr_base", *base.value}, {"mmr_stride", *stride.value},
                        {"connect_interval_ms", *connect.value},
                        {"ready_delay_ms", *ready.value}};
  return Result<LabScenarioRequest>::success(std::move(request));
}

Result<LabScenarioRequest> validate_reconnect(LabScenarioRequest request) {
  static const std::set<std::string> allowed{"player_index", "disconnect_stage",
                                             "reconnect_delay_ms"};
  if (!only_fields(request.parameters, allowed)) {
    return Result<LabScenarioRequest>::failure(invalid("reconnect_recovery contains an unknown field"));
  }
  auto player = bounded_uint(request.parameters, "player_index", 0, 0, 1);
  auto stage = enum_string(request.parameters, "disconnect_stage", "after_join",
                           {"before_queue", "after_match", "after_join"});
  auto delay = bounded_uint(request.parameters, "reconnect_delay_ms", 500, 0, 25'000);
  if (!player.ok()) return Result<LabScenarioRequest>::failure(player.error);
  if (!stage.ok()) return Result<LabScenarioRequest>::failure(stage.error);
  if (!delay.ok()) return Result<LabScenarioRequest>::failure(delay.error);
  request.parameters = {{"player_index", *player.value}, {"disconnect_stage", *stage.value},
                        {"reconnect_delay_ms", *delay.value}};
  return Result<LabScenarioRequest>::success(std::move(request));
}

Result<LabScenarioRequest> validate_duplicate_request(LabScenarioRequest request) {
  static const std::set<std::string> allowed{"message", "repeat_count"};
  if (!only_fields(request.parameters, allowed)) {
    return Result<LabScenarioRequest>::failure(
        invalid("duplicate_player_request contains an unknown field"));
  }
  auto message = enum_string(request.parameters, "message", "get_rank",
                             {"queue_match", "ready_room", "get_rank", "get_match_result"});
  auto repeats = bounded_uint(request.parameters, "repeat_count", 3, 1, 10);
  if (!message.ok()) return Result<LabScenarioRequest>::failure(message.error);
  if (!repeats.ok()) return Result<LabScenarioRequest>::failure(repeats.error);
  request.parameters = {{"message", *message.value}, {"repeat_count", *repeats.value}};
  return Result<LabScenarioRequest>::success(std::move(request));
}

Result<LabScenarioRequest> validate_duplicate_result(LabScenarioRequest request) {
  static const std::set<std::string> allowed{"mode", "repeat_count"};
  if (!only_fields(request.parameters, allowed)) {
    return Result<LabScenarioRequest>::failure(
        invalid("duplicate_trusted_result contains an unknown field"));
  }
  auto mode = enum_string(request.parameters, "mode", "exact", {"exact", "conflicting"});
  auto repeats = bounded_uint(request.parameters, "repeat_count", 3, 1, 10);
  if (!mode.ok()) return Result<LabScenarioRequest>::failure(mode.error);
  if (!repeats.ok()) return Result<LabScenarioRequest>::failure(repeats.error);
  request.parameters = {{"mode", *mode.value}, {"repeat_count", *repeats.value}};
  return Result<LabScenarioRequest>::success(std::move(request));
}

Result<LabScenarioRequest> validate_redis(LabScenarioRequest request) {
  static const std::set<std::string> allowed{"fault_kind", "fault_value_ms",
                                             "fault_duration_ms"};
  if (!only_fields(request.parameters, allowed)) {
    return Result<LabScenarioRequest>::failure(
        invalid("redis_failure_recovery contains an unknown field"));
  }
  auto kind = enum_string(request.parameters, "fault_kind", "disconnect",
                          {"disconnect", "latency", "timeout", "reset_peer"});
  auto value = bounded_uint(request.parameters, "fault_value_ms", 1'000, 0, 30'000);
  auto duration = bounded_uint(request.parameters, "fault_duration_ms", 2'000, 1'000, 30'000);
  if (!kind.ok()) return Result<LabScenarioRequest>::failure(kind.error);
  if (!value.ok()) return Result<LabScenarioRequest>::failure(value.error);
  if (!duration.ok()) return Result<LabScenarioRequest>::failure(duration.error);
  if (*kind.value == "latency" && *value.value == 0U) {
    return Result<LabScenarioRequest>::failure(
        invalid("fault_value_ms must be greater than zero for latency"));
  }
  request.parameters = {{"fault_kind", *kind.value}, {"fault_value_ms", *value.value},
                        {"fault_duration_ms", *duration.value}};
  return Result<LabScenarioRequest>::success(std::move(request));
}

}  // namespace

Result<LabScenarioKind> parse_lab_scenario_kind(const std::string& name) {
  if (name == "complete_match") return Result<LabScenarioKind>::success(LabScenarioKind::kCompleteMatch);
  if (name == "reconnect_recovery") return Result<LabScenarioKind>::success(LabScenarioKind::kReconnectRecovery);
  if (name == "duplicate_player_request") {
    return Result<LabScenarioKind>::success(LabScenarioKind::kDuplicatePlayerRequest);
  }
  if (name == "duplicate_trusted_result") {
    return Result<LabScenarioKind>::success(LabScenarioKind::kDuplicateTrustedResult);
  }
  if (name == "redis_failure_recovery") {
    return Result<LabScenarioKind>::success(LabScenarioKind::kRedisFailureRecovery);
  }
  return Result<LabScenarioKind>::failure(invalid("unknown lab scenario"));
}

std::string lab_scenario_name(const LabScenarioKind kind) {
  switch (kind) {
    case LabScenarioKind::kCompleteMatch: return "complete_match";
    case LabScenarioKind::kReconnectRecovery: return "reconnect_recovery";
    case LabScenarioKind::kDuplicatePlayerRequest: return "duplicate_player_request";
    case LabScenarioKind::kDuplicateTrustedResult: return "duplicate_trusted_result";
    case LabScenarioKind::kRedisFailureRecovery: return "redis_failure_recovery";
  }
  return "complete_match";
}

std::vector<std::string> lab_scenario_names() {
  return {"complete_match", "reconnect_recovery", "duplicate_player_request",
          "duplicate_trusted_result", "redis_failure_recovery"};
}

Result<LabScenarioRequest> parse_lab_scenario_request(const nlohmann::json& value) {
  try {
    if (!value.is_object()) {
      return Result<LabScenarioRequest>::failure(invalid("scenario request must be an object"));
    }
    static const std::set<std::string> allowed{"schema_version", "scenario", "seed", "timeout_ms",
                                               "parameters"};
    if (!only_fields(value, allowed)) {
      return Result<LabScenarioRequest>::failure(invalid("run request contains an unknown field"));
    }
    if (value.contains("schema_version") &&
        (!value.at("schema_version").is_number_unsigned() ||
         value.at("schema_version").get<std::uint32_t>() != 1U)) {
      return Result<LabScenarioRequest>::failure(invalid("schema_version must be 1"));
    }
    if (!value.contains("scenario") || !value.at("scenario").is_string()) {
      return Result<LabScenarioRequest>::failure(invalid("scenario is required"));
    }
    auto kind = parse_lab_scenario_kind(value.at("scenario").get<std::string>());
    if (!kind.ok()) return Result<LabScenarioRequest>::failure(kind.error);
    LabScenarioRequest request;
    request.kind = *kind.value;
    request.name = lab_scenario_name(request.kind);
    request.schema_version = 1;
    if (value.contains("seed")) {
      if (!value.at("seed").is_number_unsigned() && !value.at("seed").is_number_integer()) {
        return Result<LabScenarioRequest>::failure(invalid("seed must be an unsigned integer"));
      }
      if (value.at("seed").is_number_integer()) {
        const auto seed = value.at("seed").get<std::int64_t>();
        if (seed < 0) {
          return Result<LabScenarioRequest>::failure(invalid("seed must not be negative"));
        }
        request.seed = static_cast<std::uint64_t>(seed);
      } else {
        request.seed = value.at("seed").get<std::uint64_t>();
      }
    }
    auto timeout = bounded_uint(value, "timeout_ms", 60'000, 1'000, 180'000);
    if (!timeout.ok()) return Result<LabScenarioRequest>::failure(timeout.error);
    request.timeout_ms = *timeout.value;
    request.parameters = value.value("parameters", nlohmann::json::object());
    if (!request.parameters.is_object()) {
      return Result<LabScenarioRequest>::failure(invalid("parameters must be an object"));
    }
    switch (request.kind) {
      case LabScenarioKind::kCompleteMatch: return validate_complete(std::move(request));
      case LabScenarioKind::kReconnectRecovery: return validate_reconnect(std::move(request));
      case LabScenarioKind::kDuplicatePlayerRequest:
        return validate_duplicate_request(std::move(request));
      case LabScenarioKind::kDuplicateTrustedResult:
        return validate_duplicate_result(std::move(request));
      case LabScenarioKind::kRedisFailureRecovery: return validate_redis(std::move(request));
    }
  } catch (const nlohmann::json::exception&) {
    return Result<LabScenarioRequest>::failure(invalid("run request contains an invalid value"));
  }
  return Result<LabScenarioRequest>::failure(invalid("unsupported lab scenario"));
}

Result<LabScenarioRequest> parse_lab_run_request(const nlohmann::json& value) {
  return parse_lab_scenario_request(value);
}

nlohmann::json lab_scenario_request_json(const LabScenarioRequest& request) {
  return {{"schema_version", request.schema_version}, {"scenario", request.name},
          {"seed", request.seed}, {"timeout_ms", request.timeout_ms},
          {"parameters", request.parameters}};
}

nlohmann::json lab_scenario_schema(const LabScenarioKind kind) {
  nlohmann::json result{{"schema_version", 1}, {"name", lab_scenario_name(kind)}};
  nlohmann::json fields = nlohmann::json::array();
  nlohmann::json expected = nlohmann::json::array();
  switch (kind) {
    case LabScenarioKind::kCompleteMatch:
      result["title"] = "完整对局";
      result["description"] = "真实玩家协议完成匹配、房间、可信结果、结算和排行。";
      fields = {field("player_count", "玩家数量", "integer", 2, {{"min", 2}, {"max", 32}, {"step", 2}}),
                field("mmr_mode", "MMR 模式", "enum", "uniform", {{"enum", {"uniform", "stair"}}}),
                field("mmr_base", "基础 MMR", "integer", 1000, {{"min", 0}, {"max", 10000}}),
                field("mmr_stride", "阶梯间隔", "integer", 100, {{"min", 0}, {"max", 10000}}),
                field("connect_interval_ms", "连接间隔", "integer", 0, {{"min", 0}, {"max", 5000}}),
                field("ready_delay_ms", "Ready 延迟", "integer", 0, {{"min", 0}, {"max", 5000}})};
      expected = {"所有玩家真实连接", "每个房间只结算一次", "双方收到结果且排行一致"};
      break;
    case LabScenarioKind::kReconnectRecovery:
      result["title"] = "断线恢复";
      result["description"] = "在指定阶段断开玩家并验证窗口内恢复或窗口外稳定拒绝。";
      fields = {field("player_index", "目标玩家", "integer", 0, {{"min", 0}, {"max", 1}}),
                field("disconnect_stage", "断线阶段", "enum", "after_join",
                      {{"enum", {"before_queue", "after_match", "after_join"}}}),
                field("reconnect_delay_ms", "重连延迟", "integer", 500,
                      {{"min", 0}, {"max", 25000}})};
      expected = {"身份与 generation 正确", "token 轮换", "旧连接不能覆盖新连接"};
      break;
    case LabScenarioKind::kDuplicatePlayerRequest:
      result["title"] = "重复玩家请求";
      result["description"] = "重放完全相同的 request id 与载荷，验证只产生一次副作用。";
      fields = {field("message", "消息", "enum", "get_rank",
                      {{"enum", {"queue_match", "ready_room", "get_rank", "get_match_result"}}}),
                field("repeat_count", "重复次数", "integer", 3, {{"min", 1}, {"max", 10}})};
      expected = {"每次响应一致", "状态只推进一次", "没有重复票据或结算"};
      break;
    case LabScenarioKind::kDuplicateTrustedResult:
      result["title"] = "重复可信结果";
      result["description"] = "验证完全相同结果的幂等重放和冲突结果拒绝。";
      fields = {field("mode", "重复模式", "enum", "exact",
                      {{"enum", {"exact", "conflicting"}}}),
                field("repeat_count", "提交次数", "integer", 3, {{"min", 1}, {"max", 10}})};
      expected = {"积分只变化一次", "相同结果返回原 receipt", "冲突结果稳定拒绝"};
      break;
    case LabScenarioKind::kRedisFailureRecovery:
      result["title"] = "Redis 故障恢复";
      result["description"] = "经真实依赖代理制造故障，验证 MySQL 事实和排行自动修复。";
      fields = {field("fault_kind", "故障类型", "enum", "disconnect",
                      {{"enum", {"disconnect", "latency", "timeout", "reset_peer"}}}),
                field("fault_value_ms", "故障参数", "integer", 1000,
                      {{"min", 0}, {"max", 30000}}),
                field("fault_duration_ms", "持续时间", "integer", 2000,
                      {{"min", 1000}, {"max", 30000}})};
      expected = {"MySQL 结算成功", "缓存落后证据可见", "代理复原后排行补齐"};
      break;
  }
  result["fields"] = std::move(fields);
  result["expected"] = std::move(expected);
  return result;
}

nlohmann::json lab_capabilities(const LabRunEnvironment& environment,
                                const bool redis_fault_available) {
  nlohmann::json scenarios = nlohmann::json::array();
  for (const auto& name : lab_scenario_names()) {
    auto kind = parse_lab_scenario_kind(name);
    auto schema = lab_scenario_schema(*kind.value);
    const auto available = name != "redis_failure_recovery" || redis_fault_available;
    scenarios.push_back({{"name", name}, {"title", schema.at("title")},
                         {"description", schema.at("description")}, {"available", available},
                         {"schema_url", "/api/lab/v1/scenarios/" + name + "/schema"}});
  }
  return {{"schema_version", 1},
          {"service", {{"name", "arena-core-lab"}, {"version", std::string(kProjectVersion)}}},
          {"environment", {{"git_commit", environment.git_commit},
                           {"config_digest", environment.service_config_digest}}},
          {"scenarios", std::move(scenarios)},
          {"limits", {{"single_active_run", true}, {"max_events", 2048}}}};
}

}  // namespace arena_core::qa

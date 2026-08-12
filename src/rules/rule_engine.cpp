#include "arena_core/rules/rule_engine.hpp"

#include <algorithm>
#include <fstream>
#include <limits>
#include <sstream>
#include <utility>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace arena_core::rules {
namespace {

using LuaState = std::unique_ptr<lua_State, decltype(&lua_close)>;

void instruction_limit(lua_State* state, lua_Debug*) {
  luaL_error(state, "rule instruction budget exceeded");
}

Result<std::string> read_script(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return Result<std::string>::failure(make_error(ErrorCode::kRuleLoadFailed, "rule script not found"));
  }
  std::ostringstream contents;
  contents << input.rdbuf();
  if (!input.good() && !input.eof()) {
    return Result<std::string>::failure(make_error(ErrorCode::kRuleLoadFailed, "rule script could not be read"));
  }
  return Result<std::string>::success(contents.str());
}

Result<LuaState> execute_script(const std::string& source) {
  LuaState state(luaL_newstate(), &lua_close);
  if (!state) {
    return Result<LuaState>::failure(make_error(ErrorCode::kRuleLoadFailed, "Lua state allocation failed"));
  }
  lua_sethook(state.get(), &instruction_limit, LUA_MASKCOUNT, 10'000);
  if (luaL_loadbuffer(state.get(), source.data(), source.size(), "arena-rule") != LUA_OK ||
      lua_pcall(state.get(), 0, 0, 0) != LUA_OK) {
    return Result<LuaState>::failure(make_error(ErrorCode::kRuleLoadFailed, "rule script failed validation"));
  }
  return Result<LuaState>::success(std::move(state));
}

Result<std::string> validate_script(const std::string& source, const char* function_name) {
  auto state_result = execute_script(source);
  if (!state_result.ok()) {
    return Result<std::string>::failure(std::move(state_result.error));
  }
  auto& state = *state_result.value;
  lua_getglobal(state.get(), "RULE_VERSION");
  if (!lua_isstring(state.get(), -1)) {
    return Result<std::string>::failure(make_error(ErrorCode::kRuleLoadFailed, "RULE_VERSION must be a string"));
  }
  std::string version(lua_tostring(state.get(), -1));
  lua_pop(state.get(), 1);
  if (version.empty() || version.size() > 32U) {
    return Result<std::string>::failure(make_error(ErrorCode::kRuleLoadFailed, "RULE_VERSION is invalid"));
  }
  lua_getglobal(state.get(), function_name);
  if (!lua_isfunction(state.get(), -1)) {
    return Result<std::string>::failure(make_error(ErrorCode::kRuleLoadFailed, "required rule function is missing"));
  }
  return Result<std::string>::success(std::move(version));
}

Error runtime_error() {
  return make_error(ErrorCode::kRuleExecutionFailed, "deterministic Lua rule execution failed");
}

}  // namespace

Result<std::int32_t> BuiltInRuleEngine::allowed_mmr_delta(const std::int64_t waited_seconds) const {
  const auto waited = std::max<std::int64_t>(0, waited_seconds);
  const auto expanded = 100 + std::min<std::int64_t>(36, waited) * 25;
  return Result<std::int32_t>::success(static_cast<std::int32_t>(std::min<std::int64_t>(1'000, expanded)));
}

Result<ScoreAdjustment> BuiltInRuleEngine::score(const std::uint64_t seed,
                                                  const std::string_view winner_id,
                                                  const std::string_view loser_id,
                                                  const std::string_view expected_version) const {
  static_cast<void>(seed);
  if (winner_id.empty() || loser_id.empty() || winner_id == loser_id) {
    return Result<ScoreAdjustment>::failure(make_error(ErrorCode::kSettlementPlayerSetInvalid));
  }
  if (expected_version != score_version_) {
    return Result<ScoreAdjustment>::failure(make_error(ErrorCode::kRuleVersionMismatch));
  }
  return Result<ScoreAdjustment>::success(ScoreAdjustment{16, -16, score_version_});
}

LuaRuleEngine::LuaRuleEngine(std::string match_source,
                             std::string score_source,
                             std::string match_version,
                             std::string score_version)
    : match_source_(std::move(match_source)),
      score_source_(std::move(score_source)),
      match_version_(std::move(match_version)),
      score_version_(std::move(score_version)) {}

Result<std::shared_ptr<LuaRuleEngine>> LuaRuleEngine::load(const std::filesystem::path& match_script,
                                                           const std::filesystem::path& score_script) {
  auto match_source = read_script(match_script);
  if (!match_source.ok()) {
    return Result<std::shared_ptr<LuaRuleEngine>>::failure(std::move(match_source.error));
  }
  auto score_source = read_script(score_script);
  if (!score_source.ok()) {
    return Result<std::shared_ptr<LuaRuleEngine>>::failure(std::move(score_source.error));
  }
  auto match_version = validate_script(*match_source.value, "allowed_mmr_delta");
  if (!match_version.ok()) {
    return Result<std::shared_ptr<LuaRuleEngine>>::failure(std::move(match_version.error));
  }
  auto score_version = validate_script(*score_source.value, "score");
  if (!score_version.ok()) {
    return Result<std::shared_ptr<LuaRuleEngine>>::failure(std::move(score_version.error));
  }
  auto engine = std::shared_ptr<LuaRuleEngine>(new LuaRuleEngine(
      std::move(*match_source.value), std::move(*score_source.value), std::move(*match_version.value),
      std::move(*score_version.value)));
  const auto match_probe = engine->allowed_mmr_delta(0);
  const auto score_probe = engine->score(0, "validation-winner", "validation-loser", engine->score_rule_version());
  if (!match_probe.ok() || !score_probe.ok()) {
    return Result<std::shared_ptr<LuaRuleEngine>>::failure(
        make_error(ErrorCode::kRuleLoadFailed, "rule script failed deterministic probe"));
  }
  return Result<std::shared_ptr<LuaRuleEngine>>::success(std::move(engine));
}

Result<std::int32_t> LuaRuleEngine::allowed_mmr_delta(const std::int64_t waited_seconds) const {
  auto state_result = execute_script(match_source_);
  if (!state_result.ok()) {
    return Result<std::int32_t>::failure(runtime_error());
  }
  auto& state = *state_result.value;
  lua_getglobal(state.get(), "allowed_mmr_delta");
  lua_pushinteger(state.get(), static_cast<lua_Integer>(waited_seconds));
  if (lua_pcall(state.get(), 1, 1, 0) != LUA_OK || !lua_isinteger(state.get(), -1)) {
    return Result<std::int32_t>::failure(runtime_error());
  }
  const auto value = lua_tointeger(state.get(), -1);
  if (value < 0 || value > std::numeric_limits<std::int32_t>::max()) {
    return Result<std::int32_t>::failure(runtime_error());
  }
  return Result<std::int32_t>::success(static_cast<std::int32_t>(value));
}

Result<ScoreAdjustment> LuaRuleEngine::score(const std::uint64_t seed,
                                              const std::string_view winner_id,
                                              const std::string_view loser_id,
                                              const std::string_view expected_version) const {
  if (winner_id.empty() || loser_id.empty() || winner_id == loser_id) {
    return Result<ScoreAdjustment>::failure(make_error(ErrorCode::kSettlementPlayerSetInvalid));
  }
  if (expected_version != score_version_) {
    return Result<ScoreAdjustment>::failure(make_error(ErrorCode::kRuleVersionMismatch));
  }
  auto state_result = execute_script(score_source_);
  if (!state_result.ok()) {
    return Result<ScoreAdjustment>::failure(runtime_error());
  }
  auto& state = *state_result.value;
  lua_getglobal(state.get(), "score");
  lua_pushinteger(state.get(), static_cast<lua_Integer>(seed & 0x7fff'ffff'ffff'ffffULL));
  lua_pushlstring(state.get(), winner_id.data(), winner_id.size());
  lua_pushlstring(state.get(), loser_id.data(), loser_id.size());
  if (lua_pcall(state.get(), 3, 2, 0) != LUA_OK || !lua_isinteger(state.get(), -2) ||
      !lua_isinteger(state.get(), -1)) {
    return Result<ScoreAdjustment>::failure(runtime_error());
  }
  const auto winner_delta = lua_tointeger(state.get(), -2);
  const auto loser_delta = lua_tointeger(state.get(), -1);
  if (winner_delta <= 0 || loser_delta >= 0 || winner_delta + loser_delta != 0 || winner_delta > 1'000 ||
      loser_delta < -1'000) {
    return Result<ScoreAdjustment>::failure(runtime_error());
  }
  return Result<ScoreAdjustment>::success(ScoreAdjustment{static_cast<std::int32_t>(winner_delta),
                                                           static_cast<std::int32_t>(loser_delta), score_version_});
}

}  // namespace arena_core::rules

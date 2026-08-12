#pragma once

#include "arena_core/core/cpp17_guard.hpp"
#include "arena_core/core/result.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace arena_core::rules {

struct ScoreAdjustment {
  std::int32_t winner_delta{0};
  std::int32_t loser_delta{0};
  std::string rule_version;
};

class RuleEngine {
 public:
  virtual ~RuleEngine() = default;

  virtual Result<std::int32_t> allowed_mmr_delta(std::int64_t waited_seconds) const = 0;
  virtual Result<ScoreAdjustment> score(std::uint64_t seed,
                                        std::string_view winner_id,
                                        std::string_view loser_id,
                                        std::string_view expected_version) const = 0;
  virtual const std::string& match_rule_version() const noexcept = 0;
  virtual const std::string& score_rule_version() const noexcept = 0;
};

class BuiltInRuleEngine final : public RuleEngine {
 public:
  Result<std::int32_t> allowed_mmr_delta(std::int64_t waited_seconds) const override;
  Result<ScoreAdjustment> score(std::uint64_t seed,
                                std::string_view winner_id,
                                std::string_view loser_id,
                                std::string_view expected_version) const override;
  const std::string& match_rule_version() const noexcept override { return match_version_; }
  const std::string& score_rule_version() const noexcept override { return score_version_; }

 private:
  std::string match_version_{"match-v1"};
  std::string score_version_{"score-v1"};
};

class LuaRuleEngine final : public RuleEngine {
 public:
  static Result<std::shared_ptr<LuaRuleEngine>> load(const std::filesystem::path& match_script,
                                                      const std::filesystem::path& score_script);

  Result<std::int32_t> allowed_mmr_delta(std::int64_t waited_seconds) const override;
  Result<ScoreAdjustment> score(std::uint64_t seed,
                                std::string_view winner_id,
                                std::string_view loser_id,
                                std::string_view expected_version) const override;
  const std::string& match_rule_version() const noexcept override { return match_version_; }
  const std::string& score_rule_version() const noexcept override { return score_version_; }

 private:
  LuaRuleEngine(std::string match_source,
                std::string score_source,
                std::string match_version,
                std::string score_version);

  std::string match_source_;
  std::string score_source_;
  std::string match_version_;
  std::string score_version_;
};

}  // namespace arena_core::rules

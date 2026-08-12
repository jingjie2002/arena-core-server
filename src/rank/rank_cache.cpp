#include "arena_core/rank/rank_cache.hpp"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <string_view>
#include <utility>

#include <hiredis/hiredis.h>

namespace arena_core::rank {
namespace {

using RedisContext = std::unique_ptr<redisContext, decltype(&redisFree)>;
using RedisReply = std::unique_ptr<redisReply, decltype(&freeReplyObject)>;

Error validate_leaderboard(const std::string& leaderboard) {
  if (leaderboard.empty() || leaderboard.size() > 64U) {
    return make_error(ErrorCode::kRankQueryInvalid, "leaderboard length is invalid");
  }
  return {};
}

Result<RedisContext> connect_redis(const RedisRankConfig& config) {
  const timeval timeout{static_cast<long>(config.connect_timeout_ms / 1'000U),
                        static_cast<long>((config.connect_timeout_ms % 1'000U) * 1'000U)};
  RedisContext context(redisConnectWithTimeout(config.host.c_str(), static_cast<int>(config.port), timeout),
                       &redisFree);
  if (!context || context->err != 0) {
    return Result<RedisContext>::failure(
        make_error(ErrorCode::kDependencyRedisUnavailable, "Redis connection failed"));
  }
  if (!config.password.empty()) {
    RedisReply auth(static_cast<redisReply*>(redisCommand(context.get(), "AUTH %b", config.password.data(),
                                                          config.password.size())),
                    &freeReplyObject);
    if (!auth || auth->type == REDIS_REPLY_ERROR) {
      return Result<RedisContext>::failure(
          make_error(ErrorCode::kDependencyRedisUnavailable, "Redis authentication failed"));
    }
  }
  RedisReply select(static_cast<redisReply*>(redisCommand(context.get(), "SELECT %u", config.database)),
                    &freeReplyObject);
  if (!select || select->type == REDIS_REPLY_ERROR) {
    return Result<RedisContext>::failure(
        make_error(ErrorCode::kDependencyRedisUnavailable, "Redis database selection failed"));
  }
  return Result<RedisContext>::success(std::move(context));
}

RedisReply command_argv(redisContext* context, const std::vector<std::string>& arguments) {
  std::vector<const char*> values;
  std::vector<std::size_t> lengths;
  values.reserve(arguments.size());
  lengths.reserve(arguments.size());
  for (const auto& argument : arguments) {
    values.push_back(argument.data());
    lengths.push_back(argument.size());
  }
  return RedisReply(static_cast<redisReply*>(
                        redisCommandArgv(context, static_cast<int>(values.size()), values.data(), lengths.data())),
                    &freeReplyObject);
}

Error run_score_script(redisContext* context,
                       const std::string& key,
                       const std::vector<RankScore>& scores,
                       const bool replace) {
  static const std::string kUpsertScript =
      "for i=1,#ARGV,2 do redis.call('ZADD',KEYS[1],ARGV[i],ARGV[i+1]) end return #ARGV/2";
  static const std::string kRebuildScript =
      "redis.call('DEL',KEYS[1]); for i=1,#ARGV,2 do redis.call('ZADD',KEYS[1],ARGV[i],ARGV[i+1]) end; "
      "return #ARGV/2";
  std::vector<std::string> arguments{"EVAL", replace ? kRebuildScript : kUpsertScript, "1", key};
  arguments.reserve(4U + scores.size() * 2U);
  for (const auto& score : scores) {
    arguments.push_back(std::to_string(score.score));
    arguments.push_back(score.player_id);
  }
  auto reply = command_argv(context, arguments);
  if (!reply || reply->type == REDIS_REPLY_ERROR) {
    return make_error(ErrorCode::kRankCacheUnavailable, "Redis leaderboard update failed");
  }
  return {};
}

}  // namespace

class HiredisRankCache::ConnectionPool final : public std::enable_shared_from_this<ConnectionPool> {
 public:
  using Lease = std::unique_ptr<redisContext, std::function<void(redisContext*)>>;

  explicit ConnectionPool(RedisRankConfig config)
      : config_(std::move(config)), maximum_(std::max<std::size_t>(1U, config_.pool_size)) {}

  ~ConnectionPool() {
    for (auto* context : available_) {
      redisFree(context);
    }
  }

  Result<Lease> acquire() {
    redisContext* context = nullptr;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      ready_.wait(lock, [this] { return !available_.empty() || total_ < maximum_; });
      if (!available_.empty()) {
        context = available_.back();
        available_.pop_back();
      } else {
        ++total_;
      }
    }
    if (context == nullptr) {
      auto connected = connect_redis(config_);
      if (!connected.ok()) {
        {
          std::lock_guard<std::mutex> lock(mutex_);
          --total_;
        }
        ready_.notify_one();
        return Result<Lease>::failure(std::move(connected.error));
      }
      context = connected.value->release();
    }
    auto self = shared_from_this();
    return Result<Lease>::success(Lease(context, [self = std::move(self)](redisContext* returned) {
      self->release(returned);
    }));
  }

 private:
  void release(redisContext* context) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (context == nullptr || context->err != 0) {
        if (context != nullptr) {
          redisFree(context);
        }
        --total_;
      } else {
        available_.push_back(context);
      }
    }
    ready_.notify_one();
  }

  RedisRankConfig config_;
  const std::size_t maximum_;
  std::mutex mutex_;
  std::condition_variable ready_;
  std::vector<redisContext*> available_;
  std::size_t total_{0};
};

Result<RankView> build_rank_view(std::string leaderboard,
                                 std::vector<RankScore> scores,
                                 const std::string& player_id,
                                 const std::size_t neighbor_count) {
  if (auto error = validate_leaderboard(leaderboard)) {
    return Result<RankView>::failure(std::move(error));
  }
  if (player_id.empty() || neighbor_count > 100U) {
    return Result<RankView>::failure(make_error(ErrorCode::kRankQueryInvalid));
  }
  std::sort(scores.begin(), scores.end(), [](const RankScore& left, const RankScore& right) {
    return left.score != right.score ? left.score > right.score : left.player_id < right.player_id;
  });
  const auto found = std::find_if(scores.begin(), scores.end(), [&](const RankScore& score) {
    return score.player_id == player_id;
  });
  if (found == scores.end()) {
    return Result<RankView>::failure(make_error(ErrorCode::kRankPlayerNotFound));
  }
  const auto index = static_cast<std::size_t>(std::distance(scores.begin(), found));
  RankView view;
  view.leaderboard = std::move(leaderboard);
  view.player = RankEntry{found->player_id, found->score, static_cast<std::uint32_t>(index + 1U)};
  const auto first = index > neighbor_count ? index - neighbor_count : 0U;
  const auto last = std::min(scores.size(), index + neighbor_count + 1U);
  for (auto current = first; current < last; ++current) {
    if (current == index) {
      continue;
    }
    view.neighbors.push_back(
        RankEntry{scores[current].player_id, scores[current].score, static_cast<std::uint32_t>(current + 1U)});
  }
  return Result<RankView>::success(std::move(view));
}

Error InMemoryRankCache::upsert(const std::string& leaderboard, const std::vector<RankScore>& scores) {
  if (auto error = validate_leaderboard(leaderboard)) {
    return error;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (fail_writes_) {
    return make_error(ErrorCode::kRankCacheUnavailable);
  }
  auto& values = leaderboards_[leaderboard];
  for (const auto& score : scores) {
    values[score.player_id] = score.score;
  }
  return {};
}

Result<RankView> InMemoryRankCache::query(const RankQuery& query) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (fail_reads_) {
    return Result<RankView>::failure(make_error(ErrorCode::kRankCacheUnavailable));
  }
  const auto leaderboard = leaderboards_.find(query.leaderboard);
  if (leaderboard == leaderboards_.end()) {
    return Result<RankView>::failure(make_error(ErrorCode::kRankPlayerNotFound));
  }
  std::vector<RankScore> scores;
  scores.reserve(leaderboard->second.size());
  for (const auto& [player_id, score] : leaderboard->second) {
    scores.push_back(RankScore{player_id, score});
  }
  return build_rank_view(query.leaderboard, std::move(scores), query.player_id, query.neighbor_count);
}

Error InMemoryRankCache::rebuild(const std::string& leaderboard, const std::vector<RankScore>& scores) {
  if (auto error = validate_leaderboard(leaderboard)) {
    return error;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (fail_writes_) {
    return make_error(ErrorCode::kRankCacheUnavailable);
  }
  auto& values = leaderboards_[leaderboard];
  values.clear();
  for (const auto& score : scores) {
    values[score.player_id] = score.score;
  }
  return {};
}

HiredisRankCache::HiredisRankCache(RedisRankConfig config)
    : config_(std::move(config)), pool_(std::make_shared<ConnectionPool>(config_)) {}

Error HiredisRankCache::upsert(const std::string& leaderboard, const std::vector<RankScore>& scores) {
  if (auto error = validate_leaderboard(leaderboard)) {
    return error;
  }
  auto connection = pool_->acquire();
  if (!connection.ok()) {
    return std::move(connection.error);
  }
  return run_score_script(connection.value->get(), key(leaderboard), scores, false);
}

Result<RankView> HiredisRankCache::query(const RankQuery& query_value) {
  if (auto error = validate_leaderboard(query_value.leaderboard)) {
    return Result<RankView>::failure(std::move(error));
  }
  auto connection = pool_->acquire();
  if (!connection.ok()) {
    return Result<RankView>::failure(std::move(connection.error));
  }
  auto reply = command_argv(connection.value->get(), {"ZRANGE", key(query_value.leaderboard), "0", "-1", "WITHSCORES"});
  if (!reply || reply->type == REDIS_REPLY_ERROR || reply->type != REDIS_REPLY_ARRAY || reply->elements % 2U != 0U) {
    return Result<RankView>::failure(make_error(ErrorCode::kRankCacheUnavailable, "Redis leaderboard query failed"));
  }
  std::vector<RankScore> scores;
  scores.reserve(reply->elements / 2U);
  for (std::size_t index = 0; index < reply->elements; index += 2U) {
    const auto* member = reply->element[index];
    const auto* value = reply->element[index + 1U];
    if (member == nullptr || value == nullptr || member->str == nullptr || value->str == nullptr) {
      return Result<RankView>::failure(make_error(ErrorCode::kRankCacheUnavailable, "Redis rank payload invalid"));
    }
    char* end = nullptr;
    const auto numeric = std::strtod(value->str, &end);
    if (end == value->str || !std::isfinite(numeric) || std::floor(numeric) != numeric ||
        numeric < std::numeric_limits<std::int32_t>::min() ||
        numeric > std::numeric_limits<std::int32_t>::max()) {
      return Result<RankView>::failure(make_error(ErrorCode::kRankCacheUnavailable, "Redis rank score invalid"));
    }
    scores.push_back(RankScore{std::string(member->str, member->len), static_cast<std::int32_t>(numeric)});
  }
  return build_rank_view(query_value.leaderboard, std::move(scores), query_value.player_id,
                         query_value.neighbor_count);
}

Error HiredisRankCache::rebuild(const std::string& leaderboard, const std::vector<RankScore>& scores) {
  if (auto error = validate_leaderboard(leaderboard)) {
    return error;
  }
  auto connection = pool_->acquire();
  if (!connection.ok()) {
    return std::move(connection.error);
  }
  return run_score_script(connection.value->get(), key(leaderboard), scores, true);
}

std::string HiredisRankCache::key(const std::string& leaderboard) const {
  return config_.key_prefix + "rank:" + leaderboard;
}

}  // namespace arena_core::rank

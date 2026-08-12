#pragma once

#include "arena_core/core/cpp17_guard.hpp"
#include "arena_core/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace arena_core::rank {

struct RankScore {
  std::string player_id;
  std::int32_t score{0};
};

struct RankEntry {
  std::string player_id;
  std::int32_t score{0};
  std::uint32_t rank{0};
};

struct RankQuery {
  std::string leaderboard{"duel:season-1"};
  std::string player_id;
  std::size_t neighbor_count{2};
};

struct RankView {
  std::string leaderboard;
  RankEntry player;
  std::vector<RankEntry> neighbors;
  std::string cache_state{"fresh"};
};

class RankCache {
 public:
  virtual ~RankCache() = default;

  virtual Error upsert(const std::string& leaderboard, const std::vector<RankScore>& scores) = 0;
  virtual Result<RankView> query(const RankQuery& query) = 0;
  virtual Error rebuild(const std::string& leaderboard, const std::vector<RankScore>& scores) = 0;
};

class InMemoryRankCache final : public RankCache {
 public:
  Error upsert(const std::string& leaderboard, const std::vector<RankScore>& scores) override;
  Result<RankView> query(const RankQuery& query) override;
  Error rebuild(const std::string& leaderboard, const std::vector<RankScore>& scores) override;
  void fail_writes(bool enabled) noexcept { fail_writes_ = enabled; }
  void fail_reads(bool enabled) noexcept { fail_reads_ = enabled; }

 private:
  std::mutex mutex_;
  std::map<std::string, std::map<std::string, std::int32_t>> leaderboards_;
  bool fail_writes_{false};
  bool fail_reads_{false};
};

struct RedisRankConfig {
  std::string host{"127.0.0.1"};
  std::uint16_t port{6379};
  std::uint32_t database{0};
  std::string password;
  std::string key_prefix{"arena:dev:"};
  std::uint32_t connect_timeout_ms{500};
  std::size_t pool_size{4};
};

class HiredisRankCache final : public RankCache {
 public:
  explicit HiredisRankCache(RedisRankConfig config);

  Error upsert(const std::string& leaderboard, const std::vector<RankScore>& scores) override;
  Result<RankView> query(const RankQuery& query) override;
  Error rebuild(const std::string& leaderboard, const std::vector<RankScore>& scores) override;

 private:
  class ConnectionPool;
  std::string key(const std::string& leaderboard) const;

  RedisRankConfig config_;
  std::shared_ptr<ConnectionPool> pool_;
};

Result<RankView> build_rank_view(std::string leaderboard,
                                 std::vector<RankScore> scores,
                                 const std::string& player_id,
                                 std::size_t neighbor_count);

}  // namespace arena_core::rank

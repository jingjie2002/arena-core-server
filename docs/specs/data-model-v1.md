# 竞域 v1 数据与一致性模型

- 状态：已锁定
- 事实源：MySQL
- 临时状态与缓存：Redis

## 1. MySQL 表

### players

| 字段 | 约束 |
|---|---|
| player_id | `varchar(64)` 主键 |
| score | `int`，默认 1000 |
| version | 乐观版本号 |
| created_at / updated_at | UTC 时间 |

### match_tickets

保存票据审计信息：`ticket_id` 主键、player、mode、mmr、状态、enqueue/expire/matched 时间和可选 `match_id`。活动队列仍在 Redis；该表用于演示追踪而非实时扫描撮合。

### matches

`match_id` 主键，记录 mode、room、rule version、状态、创建/开始/结束时间。一个 match 对应一个 room。

### match_players

联合唯一键 `(match_id, player_id)`，包含 seat、before_score、after_score、outcome。每个 duel match 必须恰有两条成员记录。

### settlements

`settlement_id` 主键，`match_id` 唯一，包含不可变 `fingerprint`、规则版本、规范化结果 JSON、提交时间。指纹不同不得覆盖既有记录。

### rank_cache_dirty

联合主键 `(leaderboard, player_id)`，包含 `target_score`、`settlement_id`、状态、attempts、last_error、next_retry_at、created/updated 时间。事务提交时写入 pending；Redis 同步成功后标记 resolved 或删除。

## 2. Redis 键

键必须带环境命名空间，例如 `arena:dev:`。

| 用途 | 建议键 | 结构 |
|---|---|---|
| 模式队列 | `match:q:{mode}` | ZSET，score 为 enqueue time，member 为 ticket id |
| 票据快照 | `match:ticket:{id}` | HASH，带 TTL |
| 玩家活动票据 | `match:player:{mode}:{player}` | STRING ticket id，带 TTL |
| 重连上下文 | `session:reconnect:{token_hash}` | HASH，15 秒 TTL |
| 房间快照 | `room:{room_id}` | HASH/JSON，短 TTL，仅作恢复与观测辅助 |
| 排行榜 | `rank:{leaderboard}` | ZSET，score 为玩家分数 |

创建票据、取消/过期票据和撮合提交使用 Redis Lua 原子校验及更新。Redis 丢失不能抹去已提交结算；排行榜可从 MySQL 重建。

## 3. 结算事务

1. 校验 match、room 状态和两个成员。
2. 规范化结果并计算 fingerprint。
3. 若 `match_id` 已结算：相同 fingerprint 返回原结果；不同 fingerprint 返回冲突。
4. 锁定两条 player 记录，计算并写入新分数。
5. 插入 settlement、更新 match/match_players，并为两名玩家 upsert `rank_cache_dirty=pending`。
6. 同一 MySQL 事务提交。
7. 事务外更新 Redis ZSET；成功后清理或 resolve dirty，失败保留给修复器。

任何 MySQL 步骤失败均回滚，且不得先写 Redis。Redis 失败不改变结算成功事实。

## 4. 排行与并列

- 默认 leaderboard 为 `duel:season-1`。
- 分数高者在前；分数相同按 `player_id` 字典序用于稳定展示。
- Redis ZSET 的 rank 只表达分数排序；精确并列展示由服务层对同分集合按 player id 稳定整理。
- 查询结果包含 `cache_state`，必要时可标记 stale/unavailable，不能伪装为强一致。

## 5. 清理和保留

- session、join token、活跃 ticket 等临时数据必须有 TTL。
- matches、settlements 和玩家分数默认不自动删除。
- dirty 记录采用有界指数退避；达到告警阈值后继续保留证据，不静默丢弃。
- 数据库迁移只前进，已发布迁移文件不可改写。

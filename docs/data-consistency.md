# 数据一致性

## 写入顺序

```mermaid
flowchart LR
  F["冻结的 RoomResult"] --> T["MySQL 事务"]
  T -->|"提交成功"| Z["Redis ZSET 排行"]
  T -->|"提交失败"| X["返回错误，不触碰 Redis"]
  Z -->|"更新失败"| D["MySQL dirty 行"]
  D --> R["修复任务按目标分重放"]
  R --> Z
```

MySQL 事务先锁定双方玩家行，再写 match、settlement、match_players、玩家分数和排行榜目标分 dirty 记录。事务提交成功后才更新 Redis；Redis 成功后清理 dirty。这样可能短时看到旧排行，但不会丢失真实比赛成绩。

## 幂等与冲突

- `settlements.match_id` 唯一，`settlement_id` 是主键。
- 已存在 match_id 且 fingerprint 相同：返回第一次提交的收据，分数不再次变化。
- 已存在 match_id 但 fingerprint 不同：返回稳定冲突，数据库与排行不产生第二次写入。
- MySQL 失败：整个事务回滚，Redis 不更新。
- Redis 失败：MySQL 事实保留，`rank_cache_dirty` 记录目标分、次数、错误与下次重试时间。

## 排行

Redis 使用 ZSET；分数相同按 player_id 稳定排序，查询支持本人名次和相邻玩家。修复既支持单条重放，也支持从 MySQL 全量重建。

## Compose 迁移

`migrations/001_m5_settlement.sql` 使用 `CREATE TABLE IF NOT EXISTS` 和 `INSERT IGNORE schema_migrations`。Compose 的一次性 `migrate` 服务每次启动都会执行该文件，重复执行不破坏已有数据。普通 `demo-down.sh` 保留卷；显式 `--reset` 才删除演示卷。

数据库字段与索引见 `docs/specs/data-model-v1.md` 和迁移文件。

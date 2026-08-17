# 游戏客户端与可信战斗模块接入指南

## 适用范围

本指南说明如何把竞域作为“战斗之外的对局服务”接入一个 1v1 游戏原型。竞域负责会话、匹配、房间、可信结果接收、结算和排行；你的游戏客户端负责交互与表现，可信战斗模块负责计算最终胜负。

先在本机用 `./scripts/demo-up.sh --lab` 验证流程。当前版本没有公网认证、TLS、服务发现或多租户能力，不应直接暴露到互联网。

## 接入方与信任边界

| 接入方 | 使用入口 | 可以做什么 | 不能做什么 |
|---|---|---|---|
| 游戏客户端 | TCP + Protobuf | 建立会话、匹配、入房、Ready、查询结果/排行 | 提交胜负、修改积分 |
| 可信战斗模块 | 内部 Bearer HTTP | 为已存在且等待结果的 match 提交唯一胜负事实 | 创建房间、替换成员、重复改分 |
| 运营/QA 浏览器 | Observer / 本机 Lab | 只读观察；在 Lab 白名单内运行场景 | SQL/Shell、任意配置、读取 token |

## 客户端最小流程

1. 建立 TCP，发送 `Hello(player_id)`。
2. 保存 `HelloAck` 中的 session/reconnect 凭证；后续 Envelope 带 session token。
3. 发送 `QueueMatch(mode="duel", mmr, max_wait_ms)`，保存 `MatchQueued.ticket_id`。
4. 等待服务端 push `MatchFound`，取得 match、room、opponent、room_server 与 Join Token。
5. 发送 `JoinRoom`，再发送 `ReadyRoom`。
6. 双方 Ready 后，房间进入 `AwaitingResult`；此时客户端等待可信战斗模块完成比赛。
7. 收到 `MatchResultCommitted` 后展示结果；也可用 `GetMatchResult(match_id)` 重新查询。
8. 用 `GetRank(leaderboard="duel:season-1")` 查询本人及邻近排行。

TCP 每帧是 4 字节大端长度 + Protobuf `Envelope`，最大 payload 65536 字节。相同 `request_id` 与相同载荷可以安全重发；相同 ID 搭配不同载荷会返回冲突。

## 断线恢复

短时断线时用上一次 `HelloAck.reconnect_token` 重新发送 Hello。成功后：

- `resumed=true`；
- generation 增加 1；
- session token 与 reconnect token 都会轮换；
- 旧 token 和旧 transport 不再拥有会话。

默认恢复窗口为 15 秒，实际值以服务配置为准。客户端必须以新的 HelloAck 覆盖旧凭证，不能把 token 写入日志、URL 或浏览器存储。

## 可信结果提交

可信结果 listener 是内部接口，Compose Lab 中位于核心服务私有端口 `9200`，不发布宿主机。调用方从安全环境变量读取 service token：

```http
POST /internal/v1/match-results HTTP/1.1
Authorization: Bearer <service-token>
Content-Type: application/json

{
  "result_id": "battle-result-000001",
  "match_id": "match-00000001",
  "room_id": "room-00000002",
  "winner_id": "player-a",
  "loser_id": "player-b",
  "rule_version": "score-v1",
  "finished_at": "2026-08-17T12:00:00Z"
}
```

约束：

- match/room 必须存在、对应且处于 `AwaitingResult`；
- winner/loser 必须正好是该房间的两名成员；
- `result_id` 最长 64 字符，只使用资源 ID 允许字符；
- `finished_at` 使用 UTC RFC3339，且不能超出允许时钟偏差；
- 普通玩家协议没有此写入口。

首次成功返回 HTTP 201 与 `idempotent=false`。完全相同的结果再次提交返回 HTTP 200、`idempotent=true` 和第一次 receipt；相同 result/match 搭配不同内容返回 HTTP 409，积分不变化。

只读查询：

```http
GET /internal/v1/match-results/<result_id> HTTP/1.1
Authorization: Bearer <service-token>
```

## receipt 与缓存状态

receipt 包含 result/match/room、胜负双方、rule_version、fingerprint、玩家前后分数、`cache_state` 和幂等标记。

- `fresh`：MySQL 事实已提交，Redis 已追上目标分。
- `stale`：MySQL 事实已提交，但 Redis 更新失败或尚未修复；这不是结算丢失。

客户端可以展示成绩已保存，同时把排行标为“更新中”；不要因为 `stale` 重复生成新 result_id。后台 repair 会按 MySQL 目标分重放。

## 本机验证自己的接入

1. 运行 `./scripts/demo-up.sh --lab`。
2. 在 Lab 的“场景实验台”先跑完整对局、断线恢复和重复结果。
3. 在“运行详情”确认所有断言、cleanup 和业务证据。
4. 用 Observer 只读查看会话、房间、成绩与排行。
5. 运行 `./scripts/lab-smoke.sh` 做机器门禁。

协议字段以 `proto/arena_core.proto` 为准，状态/错误以 `docs/specs/protocol-v1.md`、`docs/specs/state-machines-v1.md` 和 `docs/specs/error-catalog-v1.md` 为准。Lab API 是本机 QA 接口，不是游戏运行时依赖。

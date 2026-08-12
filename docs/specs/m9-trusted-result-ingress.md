# M9 可信比赛结果接入规格

- 状态：已确认产品方向下的实施基线
- 日期：2026-08-12
- 上位设计：`docs/specs/v1.1-product-design-proposal.md`
- 范围：只覆盖 M9，不包含 Lab 页面、Toxiproxy 或 v1.2 候选功能

## 1. 状态流

`Waiting -> Ready -> AwaitingResult -> Settling -> Closed`

- 双方 Ready 后进入 `AwaitingResult`，不再由 Room 内部生成胜负。
- 结果等待超时后进入 `ResultTimedOut`，无积分变化并清理 Room。
- 只有可信结果入口可以把 `AwaitingResult` 推进到 `Settling`。
- MySQL 事实提交成功后才能进入 `Closed` 并通知玩家。

## 2. 可信结果模型

请求字段：

- `result_id`：1–64 字节资源 ID，同时作为 settlement ID。
- `match_id`、`room_id`：必须与活动 Room 一致。
- `winner_id`、`loser_id`：必须恰好是 Room 的两名成员。
- `rule_version`：必须等于服务端已加载积分规则版本。
- `finished_at`：UTC RFC3339 时间，仅作审计事实，不参与超时决策。

服务端根据上述字段生成 SHA-1 指纹；调用方不能指定指纹、积分变化或随机种子。

## 3. 幂等与冲突

- 同一 `result_id`、同一 match、相同指纹：返回已有 receipt，`idempotent=true`。
- 同一 `result_id` 但不同内容：`SETTLEMENT_CONFLICT`。
- 同一 match 已有不同 result：`SETTLEMENT_CONFLICT`。
- 不同 match 复用同一 `result_id`：`SETTLEMENT_CONFLICT`。
- Room 不存在且 MySQL 中也没有结果：`SETTLEMENT_NOT_FOUND`。
- Room 已超时或状态不允许结算：`SETTLEMENT_NOT_ALLOWED`。

## 4. 权限边界

- 玩家 TCP/Protobuf 没有写入胜负的消息。
- 可信结果 HTTP 默认关闭。
- 启用后必须从配置指定环境变量读取 Bearer service token。
- 默认只允许 loopback；非 loopback 必须显式声明位于可信私有代理/Compose 边界。
- Observer API 保持只读且不复用可信结果路由。
- token 不写入配置示例、日志、Observer 或错误响应。

## 5. HTTP API

### 提交

`POST /internal/v1/match-results`

- `201 Created`：首次提交完成。
- `200 OK`：相同结果幂等重放。
- `400 Bad Request`：JSON 或字段格式错误。
- `401 Unauthorized`：缺少或错误的 Bearer token。
- `404 Not Found`：没有活动 Room 或已存结果。
- `409 Conflict`：result/match 指纹冲突。
- `422 Unprocessable Entity`：成员、状态或规则版本不合法。
- `503 Service Unavailable`：存储、依赖、队列或停机错误。

### 可信查询

`GET /internal/v1/match-results/{result_id}`

使用同一 service token，返回已提交 receipt；不存在时返回 404。

## 6. 玩家协议

- 新增 `GET_MATCH_RESULT` 请求，仅包含 `match_id`。
- 新增 `MATCH_RESULT` 查询响应。
- 新增 `MATCH_RESULT_COMMITTED` 服务端推送。
- 返回双方的 before/after score、outcome、rule version、cache state 和 settlement ID。
- 只有比赛参与者可以查询；非参与者返回 `SETTLEMENT_ACCESS_DENIED`。
- 查询与推送均只读，不改变比赛、分数或缓存状态。

## 7. M9 验收

1. Room Ready 后稳定停在 `AwaitingResult`。
2. 没有 token、错误 token、玩家 TCP 均不能提交胜负。
3. 合法结果完成结算并向两名玩家推送。
4. 断线/稍后查询能读到 MySQL 中的 committed 结果。
5. 三类重复/冲突语义经过内存和真实 MySQL 验证。
6. 等待超时不结算、不加分并清理 Room。
7. GCC、Clang、协议、游戏、HTTP、数据、真实 MySQL/Redis 回归通过。

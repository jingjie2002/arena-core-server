# 竞域 TCP/Protobuf 协议 v1

- 状态：已锁定
- 协议版本：1

## 1. 帧格式

TCP 字节流由连续帧组成：

| 字段 | 长度 | 说明 |
|---|---:|---|
| payload_length | 4 字节 | 无符号大端整数，不含自身。 |
| payload | `payload_length` | 序列化后的 `Envelope`。 |

- 最大 payload 为 65536 字节，0 长度或超限立即判为致命协议错误。
- 服务端必须处理拆包和粘包，不依赖一次 read 对应一帧。
- 解析失败、未知消息类型或版本不兼容时，尽力发送错误响应后关闭连接。

## 2. Envelope

所有请求和响应共享以下逻辑字段；实际 `.proto` 在 M1 固化字段编号，字段编号一经发布不复用。

```text
protocol_version: uint32 = 1
message_type: enum
request_id: uint64
session_token: bytes
payload: bytes
```

- 客户端请求 `request_id` 必须非零；服务端主动推送使用 0。
- Hello 不携带 `session_token`；其余业务请求必须携带当前令牌。
- 令牌和加入凭证至少使用 128 位密码学安全随机数；日志只允许输出不可逆摘要前缀。
- 未识别字段由 Protobuf 兼容机制忽略；消息类型和必填业务字段仍严格校验。

## 3. 消息集合

| 请求/事件 | 响应 | 关键字段与语义 |
|---|---|---|
| `Hello` | `HelloAck` | `player_id`、可选 `reconnect_token`；返回新会话令牌、重连令牌、`resumed`。 |
| `Heartbeat` | `HeartbeatAck` | 更新最后活动时间；响应带服务端时间。 |
| `QueueMatch` | `MatchQueued` | `mode=duel`、MMR、可选等待上限；返回 ticket。 |
| `CancelMatch` | `MatchCancelled` | 只允许取消当前玩家仍为 Queued 的 ticket。 |
| `MatchFound` | 无 | 服务端推送 match、room、room_server 和本玩家 join token。 |
| `JoinRoom` | `RoomJoined` | match、room、server、player 和 join token 全量校验。 |
| `ReadyRoom` | `RoomStateChanged` | 幂等设置本玩家 ready，并广播快照。 |
| `LeaveRoom` | `RoomLeft` | 根据房间状态执行离开或终止策略。 |
| `GetMatchResult` | `MatchResult` | 参与者按 match 查询已提交结果；不产生副作用。 |
| `GetRank` | `RankResult` | 查询玩家当前分数、名次和可选邻近榜单。 |
| 无 | `MatchResultCommitted` | MySQL 事务提交成功后向在线参与者推送结果与积分变化。 |
| 无 | `ErrorResponse` | `code`、安全的 `message`、`retryable`、关联 request id。 |

玩家协议不存在“提交比赛结果”消息。双方 Ready 后 Room 进入 `AwaitingResult`；只有独立、受保护的可信比赛结果 HTTP 入口可以提交胜负。玩家只能接收或查询已提交事实。

## 4. 建连与恢复

1. TCP 连接建立后进入 Connected，5 秒内只接受 Hello。
2. 新登录成功后服务端签发短期 session token 与 15 秒可恢复的 reconnect token。
3. 非主动断开进入 Reconnecting；业务身份和房间成员关系暂时保留。
4. 同一玩家在窗口内发送带有效 reconnect token 的 Hello，可绑定新连接并撤销旧连接能力。
5. 过期、玩家不一致、已使用或伪造令牌均拒绝恢复；令牌每次恢复后轮换。

## 5. 请求幂等与顺序

- 每个活动会话缓存最近 256 个请求结果，保留至少 60 秒。
- 首次请求按 `(request_id, message_type, canonical payload fingerprint)` 执行。
- 相同 request id 和相同指纹重复到达，返回缓存响应，不重复副作用。
- 相同 request id 但不同指纹视为 `PROTOCOL_REQUEST_ID_CONFLICT`，发送错误后关闭连接。
- 请求号不要求连续；服务端不得只凭“更小”判定重复。
- 业务完成通知可由服务端推送；客户端应允许同一状态快照重复到达。

## 6. 超时和默认值

| 项目 | 默认值 |
|---|---:|
| Hello 截止 | 5 秒 |
| 心跳建议间隔 | 10 秒 |
| 无活动超时 | 30 秒 |
| 重连窗口 | 15 秒 |
| 加入凭证有效期 | 30 秒或房间进入 AwaitingResult，以先到者为准 |
| 等待可信结果 | 15 分钟 |
| 单帧最大负载 | 64 KiB |

这些值允许通过配置覆盖，但测试和演示默认值固定。

## 7. 兼容策略

- v1 服务只接受 `protocol_version=1`。
- 新增字段必须是可选字段并使用新编号；已删除编号必须保留为 `reserved`。
- 新增消息类型需要旧服务明确返回 unsupported，而不是误解析。
- 协议 golden 文件在 M1 建立，用于检测字段号或编码意外变化。

# 协议与业务链路

## 传输契约

- TCP 每帧为 `4 字节大端长度 + Protobuf Envelope`，有效载荷最大 65536 字节。
- Envelope 固定 `protocol_version=1`，携带消息类型、request_id、session_token 和具体 payload。
- 零长度、超长、畸形 Protobuf、未知版本和请求 ID 指纹冲突会被明确拒绝；致命协议错误关闭连接。
- 同一会话中相同 request_id + 相同指纹返回缓存响应；相同 request_id + 不同指纹视为冲突。

## 完整业务时序

```mermaid
sequenceDiagram
  participant A as 玩家 A / Robot
  participant S as Session
  participant G as GameService
  participant R as RoomActor
  participant D as Settlement
  participant M as MySQL
  participant C as Redis
  A->>S: Hello(player_id)
  S-->>A: HelloAck(session + reconnect token)
  A->>G: QueueMatch(duel, MMR)
  G-->>A: MatchFound(match, room, join token)
  A->>R: JoinRoom + Ready
  R->>R: 双方 Ready 后冻结确定性结果
  R->>D: Commit(result + fingerprint)
  D->>M: 事务锁玩家、写结算、更新分数
  M-->>D: committed receipt
  D->>C: 更新排行榜
  alt Redis 暂时失败
    D->>M: 保留 rank_cache_dirty
    D->>C: 后台按 MySQL 目标分重放
  end
  D-->>A: RoomClosed / GetRank
```

## Session 与重连

- Hello 必须在截止时间前完成；心跳刷新空闲截止。
- 断线后 reconnect token 在 15 秒窗口内有效。成功恢复会同时轮换 session token、reconnect token 与 generation。
- 旧连接的迟到关闭不能撤销新 generation；窗口边界到期即拒绝恢复。
- Room 成员身份独立于具体 TCP 连接，合法重连不会丢失已加入房间的成员资格。

## 服务端权威结果

客户端协议没有“提交胜负”的消息。双方 Ready 后，RoomActor 使用房间身份、玩家身份、规则版本和种子生成并冻结结果；结算指纹覆盖不可变业务内容。相同指纹重放是幂等，不同指纹冲突会被拒绝。

完整字段与错误码分别见 `proto/arena_core.proto`、`docs/specs/protocol-v1.md` 和 `docs/specs/error-catalog-v1.md`。

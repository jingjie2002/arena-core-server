# 架构与线程边界

## 总体结构

竞域是一个 C++17 模块化单体。单进程承载玩家 TCP 会话、匹配、房间、可信比赛结果入口、结算与观察层，MySQL/Redis 是外部数据依赖。这样保留完整的端到端调试能力，同时用接口和执行器所有权限制“单体中的任意共享状态”。

```mermaid
flowchart TB
  subgraph IO["Asio I/O 执行域"]
    TCP["TcpServer / Session"]
    GAME["GameService / MatchCoordinator"]
    ROOM["RoomActor strand"]
    RESULT["Trusted Result HTTP"]
    OBS["ObserverStore / HTTP"]
  end
  subgraph STORAGE["有界存储执行域"]
    EXEC["StorageExecutor"]
    SETTLE["PersistentSettlementService"]
  end
  TCP --> GAME
  GAME --> ROOM
  ADAPTER["战斗服 / DemoBattleAdapter"] --> RESULT
  RESULT --> GAME
  ROOM --> SETTLE
  SETTLE --> EXEC
  EXEC --> MYSQL[("MySQL")]
  EXEC --> REDIS[("Redis")]
  TCP --> OBS
  GAME --> OBS
  SETTLE --> OBS
```

## 模块职责

| 模块 | 职责 | 不允许承担 |
|---|---|---|
| `protocol` | 4 字节大端长度帧、Protobuf Envelope、指纹 | 业务状态变化 |
| `session` | Hello、token、心跳、重连、发送队列、关闭 | 匹配/结算规则 |
| `game` | 排队、撮合、Join Token、RoomActor、可信结果校验与独立 HTTP listener | 直接阻塞数据库或让玩家写入胜负 |
| `rules` | 受限 Lua 规则加载、版本与确定性检查 | 文件/网络/系统能力 |
| `settlement/storage/rank` | MySQL 事务、幂等、Redis 排行、dirty 修复 | 客户端权威结果 |
| `observer` | 读取不可变快照、有限事件日志、本地 HTTP | 跨线程遍历可变业务容器 |
| `qa` | Robot 场景、DemoBattleAdapter、报告、回放和统计 | 修改服务端内部状态或绕过可信结果入口 |

## 线程所有权

- `io_context` 承载玩家 accept、可信结果 HTTP、Session、GameService、Room 和 Observer 回调；RoomActor 的命令由 strand 串行执行。
- Session 只经 `BusinessService` 提交业务命令；GameService 只经 notifier 推送协议事件，网络层与领域层不直接互相操作容器。
- MySQL/Redis 的阻塞调用进入容量受限的 `StorageExecutor`。队列满时明确失败，不无限堆积。
- ObserverStore 在写入时复制/规整展示数据，HTTP 请求读取不可变 JSON 快照；慢消费者通过事件序号缺口识别丢失。
- 停机顺序是停止接入、停止观察入口、排空 I/O 和房间、停止存储执行器，Stop 可重复调用。

## 关键取舍

- 模块化单体优先于微服务：v1 的目标是可复现闭环和线程/一致性证据，而不是部署拓扑复杂度。
- 回调/strand 优先于 C++20 协程：语言标准锁定 C++17，并由编译契约测试阻止误升 C++20。
- MySQL 是事实源，Redis 是缓存：允许缓存短时过期，但不允许“Redis 成功、MySQL 未提交”的半结算。
- 系统包薄适配器优先于 configure 时临时下载第三方包装库：降低 fresh-clone 网络漂移。

详细决定见 `docs/adr/0001`—`0005`，线程契约见 `docs/specs/thread-ownership-v1.md`。

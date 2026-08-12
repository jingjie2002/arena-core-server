# 竞域 C++17 最终项目总规划（2026-08-11 修订）

## 1. 项目结论

已确认中文展示名称：

> `竞域｜C++17 游戏匹配、房间、排行榜与结算服务`

已确认仓库名：`arena-core-server`。中文展示名与英文仓库slug分离，避免中文路径影响CMake、脚本、CI和跨平台构建。

项目采用C++17/Linux模块化单体，不制作游戏、不做图形客户端、不照搬Go版微服务，也不从多个GitHub项目拼装代码。它用现有CoreRank已经验证的业务规则作为需求基线，重点补足C++异步网络、Session生命周期、房间状态、并发、数据一致性、Lua、Linux排障、可视化观察和游戏服务端QA。

主开发、构建和验收环境已确认为WSL2 Ubuntu。Web观察台和本地可点击Demo已确认为必做，不再作为可选增强。

## 2. 现有基线审计

### Go CoreRank

本地仓库：`G:\ai编程\简历项目\CoreRank`

- 46个Go/Proto源文件，约13048行，13个测试文件。
- 已有gateway、rank-service、match-service和最小TCP roomserver。
- 已有多模式匹配、Redis Lua ticket CAS、房服注册/心跳/容量预留、join token、原子结算、MySQL旁路持久化、Prometheus/Grafana、smoke/soak和GitHub Actions。
- 当前roomserver协议为TCP JSON-line，房间只覆盖join/ready/leave，Session、重连和C++并发证据不足。
- Go完整回归在本轮因系统Go缓存/模块缓存目录无写权限未跑完；repository、roomserver和config等不依赖缺失模块的测试已通过。正式实现前需隔离`GOCACHE`与`GOMODCACHE`重新跑基线。

### 旧C++项目

本地仓库：`G:\ai编程\简历项目\cpp-rank-match-service`

- C++17命令行项目，SQLite持久化、可选Redis榜单缓存。
- 已有匹配、排行、赛季结算和奖励领取业务，但没有网络、Session、协议、自动测试体系和观察台。
- 结论：不在旧仓库上继续扩建。最终项目新建仓库；旧项目后续退出简历主项目位置。

### GitHub参考边界

- Boost.Asio官方示例：只参考C++17异步accept/read/write、timer、strand和handler生命周期。
- `anarthal/servertech-chat`：只参考完整服务端工程、Redis/MySQL、Docker与测试组织；不采用它的C++20协程和单线程聊天业务。
- `sniper00/moon`：只参考C++核心与Lua规则的职责边界；不采用整套Actor框架。
- `DyXel/Multirole`：只参考Lobby/Room、回放和异常隔离思路；不复制AGPL代码和EDOPro专属逻辑。

最终仓库不fork上述项目、不复制其目录、不组合其源码；每个外部参考只能对应一个明确设计问题，并在ADR中记录采用或拒绝的理由。

## 3. 从CoreRank复用与重新设计的边界

### 复用业务规则，不复制语言结构

- MatchTicket状态：Queued / Matched / Cancelled / Expired。
- 按模式隔离的匹配池与随等待时间扩大的分差窗口。
- 同一玩家不能重复排队。
- Room容量预留与释放。
- join token校验玩家、match、room与成员资格。
- 结算请求指纹和重复结算幂等。
- 排行榜、健康检查、指标和smoke/soak思路。

### 不照搬

- 不照搬gateway/rank/match三服务拆分。
- 不同时保留HTTP gateway、内部gRPC和TCP JSON-line三套复杂入口。
- 不继续使用Redis成功后MySQL旁路写入的原有一致性语义。
- 不复制Go包目录和goroutine并发模型。

### C++版重新设计

- 单进程模块化单体：降低部署和调试复杂度，让C++核心能力集中。
- TCP二进制长度帧+Protobuf协议。
- 明确Session、MatchCoordinator、Room和Storage的执行器/线程归属。
- MySQL作为最终结算事实源，Redis作为匹配/房间临时状态与排行榜缓存。
- 增加轻量Web观察台、可点击Demo场景和完整QA/复测体系。

## 4. 总体架构

```text
arena-core-robot / QA Runner
            │ TCP + Protobuf
            ▼
┌─────────────────── arena-core-server ────────────────────┐
│ NetAcceptor → SessionManager → CommandRouter             │
│                         │                                │
│       MatchCoordinator → RoomManager → SettlementService │
│                         │                  │             │
│       Lua RuleEngine    │          RankService           │
│                         │                  │             │
│       ObserverStore ← EventBus / Snapshots                │
│              │                                           │
│       Beast HTTP Observer API                             │
└──────────────┬───────────────────────┬────────────────────┘
               │                       │
            Redis                  MySQL
               │
         Prometheus metrics

Browser Dashboard ──HTTP──> Observer API
```

运行组件只有：

1. `arena-core-server`：C++主程序。
2. `arena-core-robot`：真实TCP/Protobuf机器人与场景Runner。
3. 浏览器观察台：静态HTML/CSS/JS。
4. Redis、MySQL、Prometheus/Grafana：Docker Compose依赖。

## 5. 技术选型

| 项目 | 选型 | 原因 |
|---|---|---|
| 标准 | C++17 | 与当前学习课程一致；完整支持RAII、智能指针、`std::thread`、`std::filesystem`、`std::optional`等项目所需能力 |
| 构建 | CMake Presets + vcpkg manifest | Windows/WSL/Linux可重复依赖管理 |
| 网络 | Boost.Asio | 目标JD高度相关，支持异步连接、定时器和strand |
| HTTP观察 | Boost.Beast | 复用Boost，不引入完整Web框架 |
| 协议 | Protobuf + 4字节大端长度帧 | 比JSON-line更像网络服务端，易做异常协议测试 |
| Redis | redis-plus-plus + hiredis | 支持ZSet、Lua和连接池，封装清晰 |
| MySQL | MySQL Connector/C++ | 正式持久化与事务/索引证据 |
| Lua | Lua 5.4 + sol2 | 控制C++/Lua集成复杂度，保留规则扩展能力 |
| 日志 | spdlog | 结构化日志、trace/session/room关联 |
| 测试 | GoogleTest + CTest | 单元、协议和组件测试标准化 |
| JSON | nlohmann/json | 配置、观察接口和QA报告，不用于主TCP协议 |
| 指标 | prometheus-cpp | 连接、匹配、房间、错误和延迟指标 |
| CI | GitHub Actions Ubuntu GCC/Clang | Linux主环境、双编译器和Sanitizer检查 |

如果某个依赖在Windows/WSL构建不稳定，优先保证Ubuntu/WSL主线，不为Windows原生兼容牺牲Linux服务端设计。

### 环境分层

| 环境 | 固定职责 | 是否决定项目完成 |
|---|---|---|
| WSL2 Ubuntu | 主开发、全量构建、测试、Sanitizer、本地Compose和性能基线 | 是 |
| GitHub Actions Ubuntu | GCC/Clang双编译器、单元/集成检查和发布门禁 | 是 |
| GitHub fresh clone | 在全新目录验证克隆、启动、smoke、测试和观察台，无本机缓存依赖 | 是 |

当前项目不使用云服务器。所有简历声明必须能在WSL2 Ubuntu、GitHub Actions和全新Git克隆环境中复现。

### C++17实施约束

- 不使用C++20协程、`std::jthread`、`std::stop_token`、`std::span`、concepts或ranges。
- Asio异步流程采用回调/完成处理器和小型私有步骤函数，不写多层匿名回调嵌套。
- Session通过`std::enable_shared_from_this`维持异步操作期间的生命周期；定时器和跨模块通知优先捕获`weak_ptr`，避免循环引用。
- 线程池使用`std::vector<std::thread>`；通过`boost::asio::signal_set`、原子停止状态、`executor_work_guard`和显式`join()`完成优雅停机。
- 网络帧头使用`std::array<std::uint8_t, 4>`，消息体使用有上限的`std::vector<std::uint8_t>`，通过`boost::asio::buffer`读写，不依赖`std::span`。
- 所有目标必须显式以`-std=c++17`或MSVC`/std:c++17`构建；CI禁止因为编译器默认标准不同而产生隐式升级。

## 6. 线程与并发模型

### 网络层

- 一个`io_context`，运行在可配置网络线程池中。
- 异步读取拆成`ReadHeader → ValidateLength → ReadBody → Dispatch → ReadHeader`步骤；异步写入使用每Session发送队列，任意时刻只允许一个`async_write`在途。
- 每个Session使用strand串行处理自己的读写、心跳和关闭。
- `shared_ptr<Session>`维持异步操作期间生命周期；定时器/回调避免形成循环引用。
- 所有关闭路径统一进入`Session::Stop(reason)`，保证socket、timer、buffer和注册表只清理一次。

### 业务层

- `MatchCoordinator`由单独strand拥有，所有排队、取消、超时和匹配决策串行化。
- 每个Room拥有自己的strand；房间内操作串行，多个Room可以并行。
- `RoomRegistry`只保存弱引用/受控句柄，禁止观察台直接修改业务对象。

### 数据层

- MySQL阻塞调用进入有界`StorageExecutor`线程池。
- Redis操作通过连接池执行；需要回到业务状态时把完成结果post回原strand。
- 线程池满时返回明确背压错误，不无限创建线程或积压任务。

### 观察层

- 服务只向`ObserverStore`发布不可变快照和事件。
- Web观察API读取快照，不跨线程直接遍历Session/Room可变容器。

### C++17优雅停机顺序

1. `signal_set`接收SIGINT/SIGTERM，只允许第一个停止请求进入关闭流程。
2. 停止Acceptor，不再接收新连接，并将服务状态切换为Draining。
3. 在各自strand中停止Session、MatchCoordinator和Room定时器，拒绝新业务请求。
4. 等待有界时间内的结算/存储任务完成；超时任务记录原因并按一致性规则终止。
5. 释放`executor_work_guard`，调用`io_context.stop()`作为最终保险。
6. 对所有`std::thread`执行显式`join()`，最后关闭Redis/MySQL连接池和日志。

该顺序必须由组件测试和发布回归验证，不能只依赖析构函数“碰巧退出”。

这套模型的面试重点是“谁拥有数据、在哪个执行器修改、异步对象何时销毁”，不是堆锁。

## 7. 协议设计

### 帧格式

```text
4-byte big-endian payload length
Protobuf Envelope {
  protocol_version
  message_type
  request_id
  session_token
  payload
}
```

约束：

- 单帧最大64KiB。
- 读取长度后再读取payload，覆盖半包和粘包。
- 未知版本/消息、超长包和解析失败返回协议错误并关闭连接。
- `request_id`用于响应关联和幂等/重复消息检测。

### V1消息

- `Hello / HelloAck`
- `Heartbeat / HeartbeatAck`
- `QueueMatch / MatchQueued`
- `CancelMatch / MatchCancelled`
- `MatchFound`
- `JoinRoom / RoomJoined`
- `ReadyRoom / RoomStateChanged`
- `LeaveRoom / RoomLeft`
- `GetRank / RankResult`
- `ErrorResponse`

普通客户端不能直接提交最终比赛结果。Room结束通过内部`IRoomResultSource`扩展点触发；本地Demo/QA模式使用确定性的模拟结果源，避免把客户端伪造结算设计进正式协议。

## 8. 状态机

### Session

```text
Connected → Active → Reconnecting → Closed
```

### MatchTicket

```text
Queued → Matched
Queued → Cancelled
Queued → Expired
```

### Room

```text
Created → Waiting → Ready → Running → Settling → Closed
                   ↘ Aborted
```

每个转换都明确：允许的消息、前置条件、持久化动作、失败回滚、指标与日志。非法转换必须返回稳定错误码并被测试覆盖。

## 9. 匹配、房间与结算规则

### 匹配

- 1v1为必做模式；数据结构支持未来扩展，不实现5v5。
- 按`match_mode`隔离Redis ZSet。
- 初始MMR窗口固定配置；随等待时间线性扩大，上限可配置。
- Redis Lua原子完成玩家防重复入队、取消和候选摘取/CAS。
- 分配失败时ticket回到可匹配状态或明确失败，不能静默丢失。

### 房间

- join token包含match/room/player/过期时间和签名或安全随机值。
- 只有匹配成员且token有效才能加入。
- 全员ready后进入Running；Demo模式按可配置计时自动产生确定性结果。
- 断线保留短暂重连窗口；超时后Room按规则继续/中止，V1只支持双人房间。

### 结算

- MySQL事务插入match settlement并更新玩家积分。
- `match_id`唯一约束+请求指纹保证相同重放幂等，不同结果冲突。
- 事务提交后更新Redis排行榜；Redis失败则记录dirty玩家/重建任务，不回滚已经提交的MySQL事实。
- 提供排行榜缓存重建工具和一致性检查用例。

## 10. 数据模型

### MySQL表

- `players`：玩家与当前MMR/rank score。
- `match_tickets`：最终ticket记录与状态。
- `matches`：match、room、mode、状态与时间。
- `match_players`：参赛成员与结算分数。
- `settlements`：match唯一结算、payload hash和提交时间。
- `rank_cache_dirty`：Redis排行榜待修复玩家/任务。

关键索引：玩家+状态、match ID、room ID、leaderboard+score、dirty状态。每个索引必须有查询场景和`EXPLAIN`证据，不为“看起来专业”无目的加索引。

### Redis key

```text
match:queue:{mode}             ZSet
match:ticket:{ticket_id}       Hash
match:player_ticket:{player}   String
room:snapshot:{room_id}        Hash/TTL
rank:{leaderboard}             ZSet
session:reconnect:{token}      String/TTL
```

## 11. Lua规则

两类Lua分开：

1. Redis Lua：保证排队、取消、匹配摘取的原子边界。
2. 嵌入式Lua：`match_rule.lua`和`score_rule.lua`，负责匹配窗口/积分变化等纯规则。

规则脚本必须：

- 有输入输出schema和范围校验。
- 有版本号和启动时加载校验。
- 脚本异常返回明确错误，不静默使用未知结果。
- 有正常、边界、错误和版本回滚测试。

不实现通用热更新平台；V1只支持受控重新加载。

## 12. Web观察台与Demo

### 页面

1. 总览：在线、队列、房间、错误、Redis/MySQL健康。
2. Session/匹配：连接、心跳、ticket、等待时间和trace。
3. Room：成员、ready、状态迁移和拒绝原因。
4. 排行/结算：TopN、最近结算、幂等拦截和缓存一致性。
5. QA：场景运行、通过/失败、Bug ID和复测报告。

### 接口

- `GET /api/overview`
- `GET /api/sessions`
- `GET /api/tickets`
- `GET /api/rooms`
- `GET /api/rankings`
- `GET /api/events?trace_id=...`
- `GET /api/qa/runs`

默认绑定localhost，只读。Demo控制接口只有`--demo-mode`启用：

- `POST /api/demo/scenarios/{scenario_id}`

前端使用原生HTML/CSS/JS轮询，不引入React/Vue。README提供截图/GIF和90秒演示路径。

## 13. 测试与游戏QA计划

### 测试层

1. 单元：状态机、匹配窗口、积分、Lua、幂等。
2. 协议：半包、粘包、超长包、未知消息、重复request ID。
3. 组件：Session关闭、timer取消、Room并发、StorageExecutor背压。
4. 集成：Redis Lua、MySQL事务/索引、缓存重建。
5. E2E：Robot执行连接→匹配→入房→准备→结束→结算→排行。
6. 故障/弱网：断线重连、Redis超时、MySQL失败、重启恢复、重复结算。
7. 并发/稳定性：多连接、多Room、长时间运行、优雅停机和资源释放。
8. 发布回归：smoke、定向复测、影响范围和全套报告。

### 必做QA场景

- 正常匹配闭环。
- 重复排队与取消。
- 匹配超时和分配失败回滚。
- outsider/错误join token入房。
- 未全员ready不能开始。
- 心跳超时、断线和重连。
- 重复request ID。
- 相同结算重放幂等、不同结果冲突。
- Redis排行榜更新失败后的修复。
- MySQL事务失败不产生半结算。
- 服务优雅停机与重启。

### 可复现设施

- FakeClock、固定随机种子、trace/session/room/match ID。
- 场景JSON/YAML和Robot操作日志。
- 可重放一条场景的`arena-core-robot --replay`。
- Redis/MySQL repository故障注入开关只在测试构建启用。
- 构建版本、配置、初始数据、日志和测试结果归档。

### 性能验收

- 首个稳定性目标：同一参考环境下500个Robot连接、250个双人Room、持续10分钟正常场景，无崩溃、无ASan/UBSan错误、正常流程业务错误为0。
- 记录P50/P95/P99、吞吐、错误率、CPU、内存和测试环境；不在实测前承诺延迟数字。
- TSan用于缩小后的并发测试，不强求在完整500连接场景运行。

## 14. 仓库结构

```text
apps/
  server/
  robot/
include/arena_core/
src/
  net/
  protocol/
  session/
  match/
  room/
  rank/
  settlement/
  storage/
  rules/
  observer/
  metrics/
proto/
rules/
  match_rule.lua
  score_rule.lua
web/
  index.html
  app.js
  styles.css
tests/
  unit/
  protocol/
  component/
  integration/
  e2e/
  fault/
qa/
  cases/
  bugs/
  reports/
tools/
  replay/
config/
migrations/
scripts/
docker/
docs/
  adr/
  handoffs/
  quality/
CMakeLists.txt
CMakePresets.json
vcpkg.json
docker-compose.yml
```

## 15. 实施里程碑

### M0 基线与规格（2—3个工程日）

- 隔离Go缓存重新验证CoreRank基线。
- 固定协议、状态机、错误码、数据schema和需求-用例矩阵。
- 产出ADR：模块化单体、线程模型和事实源选择。

验收：规格无未决核心语义，测试矩阵能追踪每项需求。

### M1 工程骨架（2—4日）

- CMake/vcpkg/presets、日志、配置、错误类型、CI。
- GoogleTest/CTest和最小server/robot可执行文件。
- GCC、Clang和MSVC目标显式锁定C++17，加入禁止C++20语法的编译验证。

验收：Ubuntu GCC/Clang构建，空骨架测试通过。

### M2 领域模型与状态机（3—5日）

- Session/MatchTicket/Room状态、错误码和纯内存服务。
- FakeClock和单元测试。

验收：所有合法/非法转换和边界用例通过。

### M3 网络、协议与Session（6—9日）

- Asio回调式accept/read/write、长度帧、Protobuf、发送队列、心跳、超时、关闭和重连。
- 协议异常和生命周期测试。

验收：Robot能稳定连接/心跳，半包/粘包/非法包和关闭路径通过。

### M4 匹配与房间（4—6日）

- MatchCoordinator、Lua原子队列、join token、Room strand和Demo结果源。

验收：正常/取消/超时/回滚/错误token/ready/结束闭环通过。

### M5 MySQL、排行与Lua（5—8日）

- 数据迁移、事务结算、Redis榜单、dirty修复、规则引擎。

验收：幂等、冲突、MySQL失败、Redis失败和重建测试通过；有EXPLAIN证据。

### M6 观察台与Demo（5—9日）

- Beast观察API、原生网页、QA场景按钮、trace时间线。

验收：浏览器可完整观察一次8人匹配/4房间演示和重复结算复测。

### M7 QA、故障与稳定性（7—12日）

- Robot场景、重放、故障注入、Sanitizer、并发/soak和发布回归。

验收：必做QA场景全部通过；500连接/250房间/10分钟参考场景有真实报告。

### M8 文档与发布（3—5日）

- README、架构、协议、数据、测试、故障复盘、演示GIF、已知限制和v1.0 release。
- 增加Docker/Compose演示入口、原生CMake开发入口、fresh-clone验收脚本和GitHub Actions发布门禁。

验收：一台仅预装Git与Docker/Compose的干净WSL2 Ubuntu无需修改源码或配置，即可启动依赖、运行smoke并打开观察台；原生开发入口可完成构建和测试；所有声明均可由公开命令复现。

### 总工期

- 功能闭环：约2—3周工程实现。
- 简历质量完整版本：约4—6周。
- 如果依赖、Windows/WSL、CI或并发问题反复，预留到6—8周。

这是工程实现时间，不包含用户在其他工作区的学习课程时间。不能负责任地承诺一周内完成并达到上述质量线。

### C++17专项风险

- 回调嵌套：通过`ReadHeader/ReadBody/Dispatch/DoWrite`等命名步骤函数控制，不在一个函数中堆叠多层lambda。
- 生命周期：异步回调必须明确捕获`shared_ptr`还是`weak_ptr`，并测试Session关闭后回调到达的路径。
- 重复关闭：socket、timer、注册表和发送队列只能清理一次，`Stop()`必须幂等。
- 线程退出：不得依赖进程结束自动回收；网络线程和存储线程全部显式`join()`。
- 错误传播：每次异步操作必须处理`error_code`并映射为日志、关闭原因或业务错误，禁止静默return。

这些风险不会削减功能，但会提高M3和M7的测试密度，也是C++17版本最重要的面试证据。

## 16. 工程质量与个人掌握度双重验收

项目从现在起维护两条互不替代的进度线：

1. **工程完成度**：代码是否正确、可测试、可复现、可维护。
2. **个人掌握度**：用户是否能运行、解释、判断、修改和复测。

工程完成不等于用户已经掌握；用户暂时看不懂也不等于工程可以不做质量验证。只有两条线都达到门槛，项目才允许作为简历第一项目。

### 每个里程碑的工程质量门

- 需求门：功能必须对应需求ID、状态约束和明确非目标。
- 设计门：关键取舍写入ADR，说明备选方案、选择原因和失败边界。
- 构建门：C++17双编译器、警告策略和依赖锁定通过。
- 正确性门：正常、边界、非法、重复和失败路径均有自动测试。
- 安全性门：生命周期、并发归属、背压、幂等和数据一致性有专门检查。
- 可观测门：日志、指标、trace和观察台能解释一次真实流程。
- 回归门：修复Bug后保存失败用例、定向复测和影响范围回归。
- 诚实门：性能数字、限制和未验证项必须与真实证据一致。

### 每个里程碑给用户的交接包

`docs/handoffs/Mx-*`固定包含：

- 本阶段解决了什么问题，以及明确没解决什么。
- 一张模块/状态/数据流图。
- 不超过5个关键入口文件及其职责。
- 一条最短运行命令和预期现象。
- 一条正常场景、一条失败场景及对应日志/指标。
- 一项小修改任务和一项Bug复现任务。
- 当前已知限制、下一阶段依赖和可回滚点。

这不是另做一套课程，而是工程交接材料；后续课程工作区可以直接引用它。

### 个人掌握度四级门槛

| 等级 | 可验证表现 | 是否可写成本人主项目 |
|---:|---|---|
| 1 | 能启动并看到结果，但不能解释主要链路 | 否 |
| 2 | 能在提示下解释模块和测试结果 | 否 |
| 3 | 能独立解释主链、完成一个小修改并做回归 | 有条件 |
| 4 | 能定位故障、说明取舍、现场修改并验证 | 是 |

核心模块Session、Match/Room、Settlement/Rank、QA/故障复测均需达到至少3级；至少两个模块达到4级。最终还必须通过一次10分钟无稿演示和一次未提前给答案的小修改。未达标时，仓库可以保持工程完成，但简历只标记为学习/参考状态，不包装为本人完全主导。

### 各里程碑的个人验证任务

| 里程碑 | 用户需要亲自完成的最小证据 |
|---|---|
| M0 | 用自己的话解释协议帧、三个状态机和MySQL/Redis职责边界 |
| M1 | 独立执行构建/测试命令，制造并定位一个编译或配置错误 |
| M2 | 给Room增加一个非法迁移用例，并说明为什么应当拒绝 |
| M3 | 修改心跳超时或最大帧长，运行协议/Session回归并解释日志 |
| M4 | 修改一项MMR窗口或ready规则，证明匹配和房间测试同步变化 |
| M5 | 修改Lua积分规则，验证MySQL结算、Redis排行和幂等结果 |
| M6 | 给观察台增加一个筛选/字段，并说明数据从哪个快照产生 |
| M7 | 从失败场景复现一个Bug，提出假设、定位、修复并完成定向复测 |
| M8 | 完成10分钟无稿演示，回答边界问题并现场完成一项小修改 |

这些任务不要求一开始独立写出整个模块；它们用于逐步把“AI生成的工程”转化成“本人能够判断和维护的项目”。

### 用户不需要一开始判断整套项目

判断顺序固定为：

1. 先看可见行为是否符合需求。
2. 再看测试能否主动打破错误实现。
3. 再追踪一个请求经过哪些模块和数据。
4. 再完成一个局部规则修改。
5. 最后才判断架构取舍和性能边界。

这样把“我不知道项目好不好”拆成一组可回答的小问题，而不是要求用户直接审查整套C++服务端。

## 17. GitHub克隆即用与发布计划

公开仓库目标为`arena-core-server`。这里的“拉取即可使用”不是零依赖，而是把前置条件收敛为Git和Docker Engine + Docker Compose，并保证用户无需修改源码、Compose文件或配置文件。不能把开发机已有依赖、编译缓存、数据库数据或私有文件当成项目能够运行的证据。

### 路径A：简历演示模式（主入口）

v1.0发布后的目标流程为：

```bash
git clone https://github.com/jingjie2002/arena-core-server.git
cd arena-core-server
./scripts/doctor.sh
./scripts/demo-up.sh
./scripts/smoke.sh
```

- `doctor.sh`只检查WSL/Linux、Git、Docker和Compose版本，缺失时给出明确安装指引，不静默修改系统。
- `demo-up.sh`在Git忽略的`.runtime/`生成本次运行配置，启动Server、Redis、MySQL、观察台和最小Robot场景，自动执行数据库迁移与确定性种子数据，并等待健康检查通过。
- `smoke.sh`执行连接、匹配、入房、准备、确定性结算、排行更新和幂等复测，失败时返回非零退出码并指向日志。
- README明确给出观察台本地地址、预期画面、完整演示步骤和常见故障处理。
- `demo-down.sh`默认只停止服务、不删除数据卷；需要清空数据时使用单独的显式重置命令，避免误删。

Docker镜像必须使用固定基础镜像版本，依赖版本由vcpkg manifest/lock信息固定。仓库不得提交真实密钥、数据库数据、构建产物和运行日志；`.env.example`只记录可公开的默认值，实际运行值由脚本写入`.runtime/`。

### 路径B：C++原生开发模式（工程入口）

面试官或开发者不使用容器编译源码时，目标流程为：

```bash
./scripts/doctor.sh --native
cmake --preset linux-debug
cmake --build --preset linux-debug -j
ctest --preset linux-debug --output-on-failure
```

- `CMakePresets.json`固定编译器模式、构建目录和vcpkg toolchain，不要求用户手写长参数。
- GCC与Clang均以`-std=c++17`构建；Debug测试路径启用严格警告，Sanitizer使用独立preset。
- 原生模式不要求本机预装Redis/MySQL，集成测试依赖仍可由Compose按需启动。

### fresh-clone硬验收

M8发布前必须在全新目录或一次全新的CI checkout完成以下验证，不能复用项目原目录的`build/`、`.runtime/`、vcpkg缓存或数据库卷：

1. `doctor.sh`能准确报告前置条件。
2. `docker compose config`通过，所有必需变量都有安全默认值或由脚本生成。
3. `demo-up.sh`从零构建或拉取固定版本镜像，迁移、种子和健康检查自动完成。
4. `smoke.sh`通过，浏览器观察台能看到一次完整业务时间线。
5. 原生CMake路径能完成GCC/Clang构建、CTest和目标Sanitizer测试。
6. 停止后再次启动，幂等迁移、数据恢复和健康检查仍通过。
7. README中的所有复制命令由CI或发布检查实际执行，不保留“仅作者机器可用”的隐藏步骤。

### GitHub展示与发布材料

- 首页README：60秒项目定位、架构图、快速启动、演示流程、测试命令、实测结果和明确限制。
- `docs/`：协议、状态机、数据一致性、线程归属、测试策略、故障复盘和个人交接材料。
- 截图/GIF：观察台完整流程、QA场景和故障恢复；不以剪辑视频替代可复现命令。
- GitHub Actions：GCC/Clang、单元/集成、格式/静态检查、ASan/UBSan及fresh-clone smoke门禁。
- v1.0 release：固定源码标签、变更说明、已知限制；可提供固定标签的GHCR镜像加速演示，但必须保留从源码构建的Compose回退路径。
- 许可证：公开发布前默认建议MIT等宽松许可证，最终在发布M8时确认，不复制参考仓库实现。

## 18. 最终验收清单

项目只有同时满足以下条件才算“完整最终项目”：

- Ubuntu/WSL可重复构建，GCC/Clang以C++17模式CI通过。
- TCP/Protobuf、Session、心跳、关闭与重连完整。
- 匹配、Room、结算、排行闭环完整。
- Redis/MySQL/Lua有真实集成和失败路径。
- 相同结算幂等，不同结果冲突，无半结算。
- Web观察台和本地可点击Demo可用。
- 单元、协议、集成、E2E、故障和发布回归通过。
- ASan/UBSan通过，TSan目标测试通过或有明确限制记录。
- 有可复现的性能与稳定性报告。
- 有至少两个真实Bug复现/修复/复测记录。
- README、架构、协议、数据库、测试和限制与实际一致。
- 不包含密钥、伪造性能、生产级高可用或商业上线承诺。

## 19. 简历最终呈现（实现后）

项目只保留4条核心证据，避免写成技术说明书：

1. C++17/Boost.Asio/Linux异步网络、Protobuf与Session生命周期。
2. 匹配、Room、断线重连、MySQL事务结算和Redis排行榜闭环。
3. strand/有界存储线程池、Lua规则、幂等和Redis失败修复设计。
4. Robot自动化、故障复测、Sanitizer、真实压测和Web观察台。

性能数字、通过率和Bug数量只能使用最终实测结果，不在规划阶段预写。

## 20. 当前明确不做

- 不制作游戏、图形客户端、战斗/技能/地图。
- 不做完整账号、支付、商城、公会、邮件、聊天、反作弊。
- 不做帧同步、AOI、MMO世界和5v5。
- 不做Kubernetes、服务发现、多机高可用、Redis Cluster或跨地域容灾。
- 不做云服务器部署、公网演示、域名/TLS和线上运维；当前交付只面向WSL2、本地Docker和GitHub。
- 不把项目拆成多个微服务。
- 不修改课程或选择第二项目。
- 不覆盖/删除现有Go CoreRank和旧C++仓库。

## 21. 已锁定规格与交付边界

已锁定：

1. 中文展示名：`竞域`。
2. 仓库名：`arena-core-server`。
3. 语言标准：C++17。
4. 主环境：WSL2 Ubuntu。
5. Web观察台和可点击Demo：必做。
6. 工程质量与个人掌握度：双重验收，未达到个人门槛不写成简历第一项目。
7. GitHub fresh clone：在干净WSL2环境中无需修改源码或配置即可启动、smoke、查看观察台并运行测试。
8. 云服务器：明确排除，不作为实现、验收或简历表述的一部分。

当前规格已经足以进入M0设计；本轮不创建仓库、不实现代码，后续只有用户明确要求开始实施时才进入构建阶段。

# GitHub 游戏服务端参考项目调研

- 日期：2026-08-12
- 用途：为“竞域”v1.1 产品定位、功能边界和技术路线提供设计证据
- 结论性质：只借鉴公开架构与工程思路，不复制第三方代码

## 1. 调研结论

竞域不应该扩张成完整 MMO 引擎，也不应该仿照成熟游戏后端平台把账号、社交、聊天、商城、战斗同步全部塞进一个版本。结合当前已经完成的 C++17 后台能力，最合适的方向是：

> 做成一个面向小型 1v1 竞技游戏的独立对局编排与结算服务，补齐可信结果接入、开发者接入说明和可参数化的本地 QA 实验台。

参考项目带来的核心修正有四点：

1. 玩家协议、可信服务协议、只读观察接口、测试控制接口必须分开。
2. 测试页面应创建一次真实、可追踪、可重放的 `Run`，而不是直接伪造后台数据。
3. 匹配条件、故障条件、预期断言和性能阈值应成为显式配置。
4. 产品要保持窄边界；大型分布式能力只有在真实需求出现后再增加。

## 2. 参考项目对照

| 参考项目 | 值得借鉴 | 调整到竞域后的做法 | 明确不采用 |
|---|---|---|---|
| [Open Match](https://github.com/googleforgames/open-match) | Ticket、MatchProfile、Director、Assignment 将匹配输入、匹配规则和分配结果分开 | 保留现有 `MatchTicket`；为 Lab 场景增加有类型的 Profile/参数快照；用单体内的 MatchCoordinator 完成编排 | Go、Kubernetes、分布式 Matchmaker、专用游戏服务器调度 |
| [Nakama](https://github.com/heroiclabs/nakama) | 把多人游戏、匹配、排行、开发控制台和指标当成不同产品能力；权威比赛与普通客户端权限分离 | 将玩家 TCP、可信结果入口、只读观察台和本地 Lab Controller 分成四个边界 | 账号、好友、群组、聊天、商城、锦标赛、完整 BaaS 平台 |
| [Moon](https://github.com/sniper00/moon) | Actor/Service 有明确线程所有权，通过消息传递协作；Lua 只承担合适的逻辑层职责 | 继续使用 Session/Match/Room strand；Lua 只计算版本化积分规则 | C++23、Lua 协程框架、全量 Actor 重写、多协议游戏引擎 |
| [BoostServerTech Chat](https://github.com/anarthal/servertech-chat) | C++ 网络服务、MySQL、Redis、Web 客户端、Compose、集成测试和文档组成完整可运行工程 | 延续现有 Boost/MySQL/Redis/Compose，并补齐第三方接入最小示例和端到端用例 | C++20 协程、Next.js、把聊天业务带入项目 |
| [CppServer](https://github.com/chronoxor/CppServer) | 显式启动/停止生命周期、协议示例、性能测试与跨平台构建证据 | 保留现有 CMake、GCC/Clang、优雅停机、Robot 和阈值化性能报告 | 替换 Boost.Asio、同时支持 TCP/UDP/SSL/WebSocket 等无需求协议 |
| [Toxiproxy](https://github.com/Shopify/toxiproxy) | 通过独立 TCP 代理和 HTTP API，确定性注入延迟、断开、超时、连接重置和丢包 | 仅在 `lab` Compose profile 中作为可选依赖；服务端测试连接经代理访问 Redis/MySQL；运行结束必须自动复原 | 在正式服务里加入任意故障开关，或让网页直接停数据库进程 |
| [k6](https://github.com/grafana/k6) | 场景、阶段、虚拟用户、断言阈值、结果输出是压测的基本模型 | 把这些概念实现进现有 C++ Robot/QA Run；固定小规模交互测试和专用压力预设分开 | 引入 JavaScript 测试主线、替换现有 Robot、把通用 HTTP 压测当成游戏协议压测 |
| [KBEngine](https://github.com/kbengine/kbengine) | C++ 底层与可扩展游戏逻辑分层，客户端插件和压力测试属于正式交付的一部分 | 增加可信比赛结果适配边界、最小客户端示例、稳定性证据 | Python 游戏逻辑、多进程动态负载均衡、MMO 引擎和客户端插件矩阵 |
| [Boost.Asio](https://github.com/boostorg/asio) | 官方示例、测试和文档与核心实现共同维护 | 继续使用当前 callback + strand 路线，并对异步对象生命周期做专项回归 | 为追逐新语法切换 C++20/23 协程主线 |

## 3. 为什么不直接采用一个现成框架

### 3.1 Open Match 和 Nakama 的范围过大

Open Match解决的是可扩展匹配基础设施，Nakama 是完整游戏后端平台。竞域当前要证明的是 C++ 异步网络、状态机、事务一致性、缓存修复、真实协议 QA 和工程交付。如果直接引入这些平台，关键能力会被第三方系统替代，项目反而失去自身技术证据。

### 3.2 Moon 和 KBEngine 会改变项目性质

它们属于游戏服务端框架或 MMO 引擎。整体迁移会引入新的运行模型、脚本主线和分布式复杂度，并让已经验证的 Session/Match/Room/Settlement 实现失去意义。竞域只借鉴职责分层，不迁移框架。

### 3.3 k6 不适合替代当前 Robot

k6 的场景、阶段和阈值模型值得采用，但竞域的主协议是自定义 TCP + Protobuf。现有 Robot 已经直接走真实协议，扩展它比新增 JavaScript 测试栈更准确，也更符合个人 C++ 技术路线。

## 4. 新依赖判断

v1.1 推荐只增加一个可选运行依赖：Toxiproxy 固定版本容器，并且只随 `lab` Compose profile 启动。

理由：

- 它测试的是真实 TCP 依赖故障，不是服务内伪造的布尔开关。
- 通过 HTTP API 可以由 C++ Lab Controller 精确启用和清理故障。
- 它不改变 C++ 主技术栈，也不进入正式业务二进制。
- Redis 与 MySQL 均可复用同一套故障代理方法。

限制：

- 默认 Compose 和正式运行不经过 Toxiproxy。
- 每次 Run 启动前检查代理干净状态，结束、取消或超时后执行统一 reset。
- Controller 不执行 Docker、Shell 或任意系统命令。
- Toxiproxy 不可用时，只禁用依赖故障场景，不影响正常对局场景。

## 5. 对竞域技术路线的最终约束

### 保留

- C++17
- Boost.Asio / Boost.Beast callback 与 strand
- Protobuf 玩家协议
- MySQL 事实事务
- Redis 排行缓存与 dirty 修复
- Lua C API 版本化积分规则
- CMake、GCC、Clang、GTest/CTest、Sanitizer
- Docker Compose、原生 HTML/CSS/JavaScript
- 模块化单体和独立可执行程序

### 新增但不换栈

- `arena-core-lab`：C++17 + Beast 的本地场景控制程序
- `arena_core_qa` 可复用场景运行库：Robot 和 Lab 共用
- 受信任比赛结果 HTTP/JSON 入口：复用 Beast 与 nlohmann/json，独立于玩家 TCP
- 版本化 Scenario Schema、Run 状态机、事件和 JSON 报告
- 可选 Toxiproxy Compose profile

### 不新增

- React、Vue、Next.js、Node.js 构建链
- Go/Java/Python 常驻业务服务
- gRPC、Kafka、Kubernetes、服务网格
- C++20/23 协程主线
- 账号、社交、聊天、商城、战斗同步、地图和技能系统

## 6. 调研转化原则

第三方项目只提供设计证据，不成为竞域需求来源。每个功能必须同时满足以下条件才进入 v1.1：

1. 直接帮助第三方接入对局后台，或帮助 QA 验证真实服务端行为。
2. 能复用当前 C++17 技术栈与已完成模块。
3. 有明确自动验收和失败清理方式。
4. 不把项目扩成通用游戏后端平台或 MMO 引擎。

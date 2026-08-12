# 项目地图

## 2026-08-12 v1.1 正式设计新增结构

- `docs/specs/v1.1-product-design-proposal.md`：当前待用户确认的推荐设计，覆盖产品定位、可信结果入口、玩家结果通知、QA Lab、P0 场景、Run 模型、技术边界、M9—M12 和验收标准。
- `docs/research/github-game-server-reference-review-2026-08-12.md`：Open Match、Nakama、Moon、BoostServerTech Chat、CppServer、Toxiproxy、k6、KBEngine 和 Boost.Asio 的设计借鉴/拒绝矩阵。
- `docs/specs/project-positioning-and-v1.1-requirements-draft.md`：保留为第一轮讨论记录，不再作为当前推荐方案。

本轮只新增设计资料；`apps/`、`src/`、`include/`、`proto/`、`tests/` 和 Compose 尚未发生 v1.1 代码变化。

---

## 2026-08-12 v1.1 定位设计新增结构

- `docs/specs/project-positioning-and-v1.1-requirements-draft.md`：待确认的项目定位、目标用户、主要目标、v1.1 分层需求、运行控制台职责、备选方案、非目标和发布门槛。
- 该文件当前是设计草案，不覆盖已完成的 v1.0 规格；用户确认后才成为下一阶段范围基线。

---

## 2026-08-12 M8 最终交付结构

- `README.md`、`LICENSE`：非技术入口、复现命令、证据索引和 MIT 许可。
- `Dockerfile`、`compose.yaml`、`deploy/haproxy.cfg`：从源码构建的 C++ 镜像、MySQL 8、Redis 7、幂等迁移和仅宿主机 loopback 暴露的独立网关。
- `scripts/doctor.sh`、`scripts/demo-up.sh`、`scripts/smoke.sh`、`scripts/demo-down.sh`：公开演示生命周期。
- `scripts/release/m8-clean-copy-check.sh`：排除 Git、构建、运行缓存和旧数据卷的隔离副本验收。
- `scripts/release/m8-local-clone-check.sh`：只使用已提交 Git 内容的真实 Linux 克隆验收，覆盖脚本权限、原生构建与独立 Compose。
- `docs/architecture.md`、`protocol-and-flow.md`、`data-consistency.md`、`testing.md`、`incident-reviews.md`、`known-limitations.md`：公开技术说明。
- `docs/release-notes-v1.0.0.md`、`docs/operations/M8-final-verification-2026-08-12.md`、`docs/handoffs/M8-release.md`：最终版本、操作证据与非技术交接。
- `.github/workflows/ci.yml`：GCC/Clang、Sanitizer、manifest、发布契约和 clean-copy Compose 门禁。

本地 Git `main` 保存完成历史，`v1.0.0` 标签固定最终基线；远端 GitHub 不属于当前本地文件结构。

---

## 2026-08-12 M7 后新增结构

- `include/arena_core/qa/`、`src/qa/`：可复用 Robot 场景模型、真实 TCP 执行、结构化报告、分位数和重放核对。
- `tests/qa/`：场景解析、报告与重放契约测试；`tests/game/`、`tests/data/` 同步增加停机和重复故障序列覆盖。
- `scenarios/qa/`：完整流程、8 人回归、活跃停机、500 连接预检和 10 分钟正式稳定性场景。
- `scripts/dev/m7-e2e.sh`、`scripts/dev/m7-soak.sh`：重启重放、SIGTERM 截止、资源采样与 500/250 长时验证入口。
- `config/arena-core.m7.json`、`config/arena-core.soak.json`：隔离端口的短回归和长时测试配置。
- `docs/quality/M7-stability-report-2026-08-12.md`：Sanitizer、真实依赖、停机、500/250/600 秒数据与边界。
- `docs/operations/M7-automatic-verification-2026-08-12.md`、`docs/handoffs/M7-qa-stability.md`：自动操作记录和非技术交接。

构建产物与稳定性原始采样仍位于被忽略的 `out/`，不会提交进源码仓库；正式结论已保存在 `docs/`。

---

## 2026-08-12 M6 后新增结构

- `include/arena_core/observer/`、`src/observer/`：线程安全 ObserverStore、有限事件日志和 Boost.Beast HTTP 服务。
- `web/observer/`：无外部网络依赖的原生观察台，覆盖总览、房间、排行、QA 和 trace 时间线。
- `tests/observer/`：快照隔离、慢消费者缺口、HTTP 只读边界、Demo 门禁与 trace 顺序测试。
- `config/arena-core.demo.json`：只绑定 loopback、数据依赖关闭的本地可见 Demo 配置。
- `scripts/dev/m6-observer-smoke.sh`：启动—API—场景—停止的自动 smoke。
- `scripts/dev/m6-demo.sh`：人工验收用 start/status/stop 生命周期入口。
- `docs/operations/M6-manual-verification-2026-08-12.md`：第二次计划内人工介入的操作与反馈记录。

---

## 2026-08-12 M5 后新增结构

- `include/arena_core/storage/`、`src/storage/`：有界 StorageExecutor 与 MySQL 结算事实仓库。
- `include/arena_core/settlement/`、`src/settlement/`：Room 到持久化结算的异步边界、dirty 修复和重建编排。
- `include/arena_core/rank/`、`src/rank/`：内存/Redis 排行缓存、稳定并列和邻近查询。
- `include/arena_core/rules/`、`src/rules/`、`rules/`：内建与 Lua 规则引擎、版本契约和指令预算。
- `migrations/001_m5_settlement.sql`：玩家、比赛、票据、结算、参赛记录和缓存 dirty 表。
- `tests/data/`、`tests/integration/data_integration_test.cpp`：事务、幂等、回滚、缓存故障与真实依赖测试。
- `scripts/dev/m5-data-integration.sh`：隔离端口启动 MySQL/Redis、迁移、测试、生产启动/停止和 EXPLAIN 的单命令入口。
- `docs/adr/0004-system-data-client-adapters.md`：系统包适配器选型及其 fresh-clone 取舍。

---

## 2026-08-11 M4 后新增结构

- `include/arena_core/game/`、`src/game/`：GameService/MatchCoordinator 与 RoomActor。
- `include/arena_core/session/business_service.hpp`：网络层和业务层的异步边界。
- `tests/game/game_flow_test.cpp`：真实 TCP 的匹配、房间、超时、回滚和重连测试。
- `rules/redis/match_queue.lua`：Redis 队列原子操作 v1。
- `scripts/dev/m4-redis-lua-smoke.sh`：无持久化临时 Redis 验证入口。

---

## 2026-08-11 M3 后新增结构

- `proto/arena_core.proto`：v1 Envelope、会话、匹配、Room 和排行榜消息契约。
- `include/arena_core/protocol/`、`src/protocol/`：长度帧、解析、载荷和请求指纹。
- `include/arena_core/session/`、`src/session/`：令牌、Registry、Session 和 TcpServer。
- `tests/protocol/`、`tests/component/session_server_test.cpp`：协议及真实 loopback 网络测试。
- `scripts/dev/m3-smoke.sh`、`config/arena-core.smoke.json`：真实 Server/Robot smoke 入口。

---

## 2026-08-11 M2 后新增结构

- `include/arena_core/domain/`、`src/domain/`：Clock、ID、Session、MatchTicket 和 Room 领域模型。
- `include/arena_core/core/result.hpp`：无异常业务边界使用的 C++17 `Result<T>`。
- `tests/unit/domain_test.cpp`：状态机合法/非法转换与边界测试。

---

## 2026-08-11 M1 后新增结构

- `CMakeLists.txt`、`CMakePresets.json`、`cmake/`：C++17 构建、编译器策略和语言 contract。
- `vcpkg.json`：固定依赖基线及 data/rules/observer 可选 feature。
- `apps/server/`、`apps/robot/`：最小可执行入口。
- `include/arena_core/core/`、`src/core/`：版本、C++17 守卫、错误、配置和日志基础。
- `tests/unit/`、`tests/compile/`：M1 单元和语言契约测试。
- `config/arena-core.example.json`：无秘密的公开配置样例。
- `scripts/dev/build-wsl.sh`：GCC/Clang 构建测试入口。
- `.github/workflows/ci.yml`：Ubuntu 双编译器 CI 定义。

构建产物统一在被忽略的 `out/` 下，不属于源码地图。

---

## 2026-08-11 M0 后地图

- `docs/specs/`：M0 锁定的需求、协议、状态机、错误、数据和并发规格。
- `docs/adr/`：模块化单体、执行器所有权、MySQL 事实源三项架构决定。
- `docs/quality/requirements-test-matrix.md`：26 项 v1 需求与后续验证入口。
- `docs/handoffs/M0-baseline-and-specs.md`：M0 非技术交接。
- `G:\ai编程\简历项目\CoreRank`：只读 Go 业务参考，不属于本项目交付物；记录提交为 `5d6638ec8090adc7a5fda39bb9786c12676e7452`。

M0 没有创建业务代码。M1 将创建 `src/`、`include/`、`tests/`、`config/`、`cmake/` 和构建/CI 基础；后续再按里程碑加入 `proto/`、数据库迁移、Web、Robot 与运行脚本。

---

以下为 M0 前地图，仅供追溯。

## 当前目录总览

- `AGENTS.md`：项目内AI协作、范围锁定、验证和报告的唯一总入口。
- `plans/arena-core-server-master-plan-2026-08-11.md`：竞域完整范围、架构、里程碑、测试、学习交接和发布验收的唯一执行基线。
- `docs/jingyu-cpp17-project-brief-2026-08-11.md`：项目定位、技术路线、完成标准与GitHub拉取即用要求的简要介绍。
- `.codex/context/`：当前状态、执行方案和项目地图，供新窗口恢复上下文。
- `.codex/reports/`：每轮真实工作报告。
- `.codex/rules/`与`.codex/hooks/`：范围、安全、验证和Harness自动检查。
- `.agents/skills/`：Harness初始化与模式切换工作流。

## 当前实际状态

- 工作区目前只有规划、摘要和Codex/Harness协作文件，没有业务源码、测试、构建配置或运行产物。
- 当前目录没有`.git`元数据，尚未初始化为`arena-core-server`仓库。
- 这一状态与`full/design`阶段一致；版本管理和工程骨架应在用户批准实施后按M0/M1边界建立。

## 尚未创建的实施目录

- `src/`、`include/`：服务端与公共接口实现。
- `tests/`：单元、协议、组件、集成、E2E和故障测试。
- `proto/`：Protobuf协议源文件。
- `scripts/`：环境检查、Demo启动、smoke、停止和fresh-clone验收脚本。
- `config/`：公开配置模板，不保存密钥。
- `docs/handoffs/`：M0—M8工程交接与个人掌握材料。

这些目录只有进入对应里程碑时才创建，不提前生成空架构。

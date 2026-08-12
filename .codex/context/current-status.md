# 当前状态

## 2026-08-12 进入项目定位与 v1.1 设计阶段

- 用户明确指出现有观察台更像教学页面，无法证明后台能力，并要求先重新明确项目属于哪类游戏服务端、主要目标和需求。
- Harness 已切换为 `full/design`；本轮只做定位和需求设计，不修改业务代码、不上传 GitHub。
- 推荐定位已形成草案：`竞域｜C++17 1v1 竞技游戏对局编排与结算服务端`，负责战斗之外的会话、匹配、房间、权威结算和排行恢复。
- 草案明确区分运行时用户、仓库读者、核心目标、v1.1 需求、真实运行控制台、备选定位、非目标与 GitHub 发布门槛。
- 当前工程状态重新表述为“后台工程基线完成，产品定位与公开展示尚未完成”；设计确认前不进入实现。
- 正式草案：`docs/specs/project-positioning-and-v1.1-requirements-draft.md`；本轮报告：`.codex/reports/第18次-项目定位与v1.1需求设计-2026-08-12-1236.md`。

---

## 2026-08-12 v1.0.1 观察台按钮修复完成

- 用户在重启后的 `49120` 观察台发现三个按钮全部显示服务器无法完成；真实复现确认三个 POST 均被 HTTP 403 `demo_mode_disabled` 拒绝，不是用户操作问题。
- 根因是 Compose 核心容器需要监听 Docker 私有网络 `0.0.0.0`，但启动脚本同时关闭了 Demo；页面仍显示按钮，导致前后端能力不一致。
- 已增加显式受信任 loopback 代理配置：本机 Compose 可启用 Demo，普通非 loopback 配置仍拒绝；HAProxy 仍只发布宿主机 `127.0.0.1`。
- smoke 已加入 Demo 入口探针；GCC/Clang 各 82/82、Docker 镜像 82/82、主 Compose 真实业务与隔离 clean-copy 全部通过。
- 真实浏览器三项依次显示“完整比赛演示通过”“重复提交保护通过”“排行榜故障已经自动恢复”，错误日志为 0；本机服务以 `1.0.1` 健康运行。
- 正式记录：`docs/operations/v1.0.1-observer-demo-fix-2026-08-12.md`；发布说明：`docs/release-notes-v1.0.1.md`；审计报告：`.codex/reports/第17次-v1.0.1观察台按钮修复-2026-08-12-1222.md`。

---

## 2026-08-12 M8 与本地 v1.0 已完成

- 用户在重启后要求项目一次性推进到完成；M0—M8 的工程实现、验证、文档和本地版本历史现已闭环。
- M8 补齐 README、架构/协议/数据/测试/故障/限制/运维文档、Compose 演示、截图、MIT 许可证、Release Notes 和 GitHub Actions fresh-clone 门禁。
- 重启恢复时发现并修复网关共用网络命名空间、正式镜像默认强制 Demo 模式、干净 Linux `.runtime` 写权限、Dockerfile 隐性 syntax 下载和 Git Shell 可执行位五类发布问题。
- 最终 GCC/Clang 普通回归各 80/80；clean-copy 原生 GCC 80/80 与独立 Compose 通过；真实 Git `c0338e1` 克隆同样完成 GCC 80/80 和完整 Robot 数据链。
- Compose 停止但保留数据后重新启动通过，MySQL settlement 计数从 4 增至 5，证明旧事实保留且新业务仍可写入；当前本地 Demo 继续运行在 `http://127.0.0.1:49120/`。
- 本地 `main` 已建立可追溯提交，最终记录提交后建立 `v1.0.0` 标签；源码未上传公开 GitHub，也未创建远端 Release。
- 正式操作记录：`docs/operations/M8-final-verification-2026-08-12.md`；非技术交接：`docs/handoffs/M8-release.md`；审计报告：`.codex/reports/第16次-M8发布与最终验收-2026-08-12-1159.md`。

---

## 2026-08-12 M7 已完成，进入 M8 发布准备

- Robot 已覆盖真实 TCP 的连接、匹配、进房、准备、确定性结算和查榜，并支持 JSON 场景、结构化报告与服务重启后的结果重放。
- GCC/Clang 普通回归各 80/80；真实 MySQL/Redis 集成各 12/12；Clang ASan+UBSan 80/80 并通过 E2E，GCC TSan 缩小目标集 50/50。
- 正式稳定性测试达到 500 在线连接、250 个双人房间、600 秒、299500 次心跳、业务错误 0；服务峰值 RSS 22.5 MiB，完整闭环 P99 370.716 ms。
- M7 排除了 Robot 事件句柄浪费、零时长不退出、并发配对假设、场景身份复用、观察字段误读和 TSan 警告门禁等问题，所有最终实现均完成对应回归。
- M7 自动阶段不需要用户补测；正式证据见 `docs/quality/M7-stability-report-2026-08-12.md` 和 `.codex/reports/第15次-M7 QA故障与稳定性-2026-08-12-0942.md`。
- 当前自动进入 M8：补齐公开仓库文档、演示脚本与 fresh-clone 验收。下一次也是第 3/3 次计划内人工介入，只在干净环境复现和公开发布决定确实需要用户时触发。

---

## 2026-08-12 M6 人工可见性验收通过，进入 M7

- 用户确认新版页面“现在看得懂了”，并明确授权按原计划继续推进。
- 用户仍不了解的后台功能属于最终个人掌握与交接范围，不要求在开发中途学习或判断，也不阻塞工程推进。
- M6 第二次计划内人工介入已经结束；下一次计划内人工介入是 M8 fresh-clone 与公开发布决定。
- 当前进入 M7：Robot 全链路、场景重放、故障注入、Sanitizer、500 连接/250 房间/10 分钟稳定性和发布回归。

---

## 2026-08-12 M6 非技术讲解改造完成，等待第二次人工复验

- 页面默认入口已明确说明“这不是游戏画面”，三个按钮改为编号白话任务；按钮下方新增统一大结果卡，直接说明刚才做了什么、结果是否正确、这证明了什么。
- Demo 启动时不再自动预填第 1 项，页面初始显示“等待开始”，避免用户尚未点击就看到来源不明的结果。
- “连接→匹配→房间→比赛→保存成绩→更新排行”改为六步中文流程；Session、内部成绩编号、QA 和 Trace 收入默认折叠的“开发者技术明细”。
- GCC/Clang Observer 定向测试各 6/6；两套真实 HTTP smoke 均通过；真实浏览器三项点击分别显示完整比赛、重复提交保护和排行榜恢复的中文结论，console 日志为 0。
- 新版未完成新的 390×844 真实浏览器复测：浏览器安全策略不允许创建自定义窄屏容器；旧版该尺寸已通过，本次保留并加强响应式断点，后续补测。
- 真实 HTTP 测试前旧 Demo PID 1070 关闭端口后未在 5 秒内退出，核对命令后已仅强制结束该进程；优雅退出超时留给 M7 稳定性排查。后续 PID 4457 已能正常优雅停止，最终 Demo 以 PID 4584 在干净的“等待开始”状态运行。
- 当前仍是第 2/3 次计划内人工介入点。用户只需刷新页面、点击第 1 项并判断大结果卡是否看得懂；复验通过前不进入 M7。

---

## 2026-08-12 M6 已完成自动验收，暂停在第二次人工介入点

- Boost.Beast localhost 观察 API、不可变 ObserverStore、原生 HTML/CSS/JS 页面、trace 时间线和 `--demo-mode` 场景控制已实现。
- 页面覆盖总览、Session/匹配、Room、排行/结算、QA 与事件；实际 Session、GameService、Room 和结算会发布快照，不跨线程遍历可变业务容器。
- GCC/Clang 完整回归各 74/74 通过；观察层各 6/6，真实 HTTP smoke、旧 TCP/Robot smoke 和真实 MySQL/Redis 数据集成都通过。
- 浏览器自动验收已点击正常、重复结算和缓存故障场景：8 个 Session、4 个房间、4 个结算可见；幂等标记、dirty 修复时间线和 trace 筛选正确；浏览器无错误日志，390px 无横向溢出。
- Demo 服务已留在 `http://127.0.0.1:39120/`，供用户完成第二次计划内人工验收；此节点只判断页面是否看得懂、展示是否符合预期。
- 操作记录：`docs/operations/M6-manual-verification-2026-08-12.md`；交接：`docs/handoffs/M6-observer-demo.md`。
- 未得到用户验收反馈前不进入 M7。用户回复“通过，继续”后，记录结果并开始 Robot、故障注入、Sanitizer 与稳定性验证。

---

## 2026-08-12 M5 已完成，下一执行点为 M6

- MySQL 8 事实事务、Redis 7 排行榜与 dirty 修复、嵌入式 Lua 规则和有界 StorageExecutor 已实现。
- RoomActor 已改为异步结算：存储失败时保持 `Settling`，重试成功后才进入 `Closed`，不会把未落库结果误报为完成。
- GCC 与 Clang 完整回归各 67/67 通过；两套真实 MySQL/Redis 数据集成各 11/11 通过，并验证事务回滚、幂等冲突、Redis 失败修复、稳定并列和生产配置启动/停止。
- M5 中发现 `TIMESTAMP` 与 `UTC_TIMESTAMP()` 在 `+08:00` 会话下比较错位，已改为 `CURRENT_TIMESTAMP(6)` 并用真实 dirty 修复用例回归。
- 正式交接：`docs/handoffs/M5-data-consistency.md`；正式报告：`.codex/reports/第12次-M5数据一致性与Lua-2026-08-12-0000.md`。
- 本轮没有进入 M6；下一步是观察台、快照和可点击 Demo。M6 完成后才是第二次计划内人工介入点。

---

## 2026-08-11 已在 M4 完成后安全暂停

- 用户要求离开电脑前先完成文件落盘并暂停；当前没有进入 M5，也没有继续修改业务代码。
- 恢复点明确为 M5 起点：MySQL 事实事务、Redis 排行与 dirty 修复、嵌入式 Lua 计分规则。
- M4 的源码、测试、交接文档和正式报告均已保存；本次暂停只补充状态与操作记录。
- 临时 Redis 测试端口 `6391` 已确认无法连接，说明临时实例已停止。
- 系统中另有一个来源未确认的 `redis-server` 进程；它不是本次临时端口实例，因此保持不动，避免影响用户已有环境。
- 等用户说“开始”或“继续”后，再从 M5 开始推进。

---

## 2026-08-11 M4 已完成，自动进入 M5

- MatchCoordinator strand、扩窗候选、超时/取消/回滚、每房间 strand、加入凭证摘要和确定性 Demo 结果已实现。
- 双编译器游戏标签 7/7 通过；完整双编译器回归在新增最终重连用例前为 53/53，新增用例随后也分别通过。
- Redis 7 原子队列 Lua 已在独立 6391 临时实例真实验证，持久化关闭，测试后实例已停止。
- 正式交接：`docs/handoffs/M4-match-room.md`。
- 当前进入 M5：把内存 settlement 占位替换为 MySQL 事实事务，接入 Redis 排行/dirty 修复和嵌入式 Lua 计分规则。

---

## 2026-08-11 M3 已完成，自动进入 M4

- Protobuf v1、长度帧、Asio TCP Server、Session strand、定时器、发送队列、请求幂等和重连 Registry 已实现。
- GCC/Clang 各 47/47 测试通过；两种编译器的服务端与 Robot 真实 TCP smoke 均通过。
- 断线恢复会轮换两个 token，并用 generation 阻止旧回调破坏新连接；致命协议错误发送稳定错误后关闭。
- 正式交接：`docs/handoffs/M3-network-session.md`。
- 当前进入 M4：在既有 Session handler 上接入确定性 1v1 匹配、加入凭证、Room strand 和失败回滚，不提前实现 MySQL 结算。

---

## 2026-08-11 M2 已完成，自动进入 M3

- 新增 `domain/`：Clock/FakeClock、ID 校验与生成、SessionStateMachine、MatchTicket、Room。
- 合法、非法和精确超时边界在 GCC/Clang 下各 31/31 测试通过。
- 状态转换均为同步纯领域逻辑，不包含 socket、数据库或 Redis；M3 将把 Session 语义接到真实 Asio/Protobuf 传输。
- 正式交接：`docs/handoffs/M2-domain-state-machines.md`。

---

## 2026-08-11 M1 已完成，自动进入 M2

- 已初始化本地 `main` Git 仓库；尚未提交、推送或创建远端。
- 已建立 CMake/CMake Presets/vcpkg manifest、GCC/Clang/MSVC C++17 策略、配置、日志、错误类型、GoogleTest/CTest、最小 server/robot 和 Ubuntu CI 定义。
- WSL2 安装了项目锁定的 M1 开发包：Clang、Boost.System/Thread、Protobuf、spdlog、nlohmann/json、GoogleTest。
- GCC 13.3 与 Clang 18.1 均配置、构建成功，各运行 11/11 测试通过；C++20 拒绝 contract test 通过。
- 示例配置校验、版本输出、安装目录及安装后 self-check 均通过。
- 正式交接：`docs/handoffs/M1-engineering-skeleton.md`。
- 当前进入 M2：只实现领域值对象、FakeClock/ID 抽象和 Session/MatchTicket/Room/Settlement 纯内存状态机，不提前写 TCP 或数据库。

---

## 2026-08-11 M0 已完成，准备进入 M1

- 项目：`竞域`，目标仓库名 `arena-core-server`。
- 任务模式：按已确认范围执行；复杂任务。
- Harness：M0 使用 `full/design`，进入 M1 时切换为 `full/build`。
- 权威范围：`plans/arena-core-server-master-plan-2026-08-11.md`。
- M0 已锁定需求、协议、状态机、错误目录、数据模型、线程所有权、三项 ADR 和需求—测试矩阵。
- M0 正式入口：`docs/specs/m0-v1-requirements.md`；非技术交接：`docs/handoffs/M0-baseline-and-specs.md`。
- CoreRank 参考基线已只读复核：提交 `5d6638ec8090adc7a5fda39bb9786c12676e7452` 上 `go test ./...`、`go vet ./...` 和五个命令入口构建通过。
- CoreRank Redis 集成测试实际连接本机 Redis 后通过；MySQL 因没有 `CORERANK_TEST_MYSQL_DSN` 跳过 6 项，明确记为未验证。
- CoreRank 原仓库未被修改；原有未跟踪 `.gocache-plan/` 保持不动。
- 当前工作区仍没有 C++ 源码、CMake、Compose 或 CI；这是 M0 只冻结规格的预期结果。

### 已锁定的核心语义

- TCP：4 字节大端长度 + Protobuf Envelope，最大 64 KiB。
- Session：Hello 5 秒、心跳 10 秒、无活动 30 秒、重连 15 秒、请求结果幂等缓存。
- Match：v1 只支持 duel 1v1；固定候选顺序和随等待时间扩大的 MMR 窗口。
- Room：加入凭证绑定 match/room/server/player；双方 ready 后由服务端内部生成确定性结果。
- Settlement：MySQL 是事实源；事务内写结算、分数和 dirty 记录，Redis 排行榜失败后可修复。
- Concurrency：Session/Match/Room 使用独立 strand；阻塞存储进入有界 StorageExecutor。

### 用户介入约定

- 三个计划节点按整个开发过程计算：开始授权（已完成）、M6 可视化人工测试、M8 fresh-clone 与公开发布决定。
- M0—M5 不逐里程碑暂停；AI 自动实现、验证、记录并继续。
- 仅权限、凭证、系统安装、不可逆操作或必须改变锁定范围的真实阻塞可例外请求介入。

### 下一步

- 切换为 `full/build` 并实施 M1：工程骨架、依赖清单、构建、配置、日志、基础 CI 和测试入口。
- 之后自动推进 M2—M5；第一次计划内暂停点是 M6 可视化面板完成后的人工验收。

### 当前未验证项

- C++ 工程尚未创建，构建、测试、Demo、Sanitizer 和 fresh-clone 均尚未运行。
- CoreRank 的 MySQL 集成基线未验证，但不会作为 C++ 实现通过与否的替代证据。

---

以下为 M0 开始前的历史状态，仅供追溯。

## 2026-08-11 竞域项目方向已核对

- 工作区用途：只负责C++简历项目`竞域`（仓库名`arena-core-server`）的设计、实现、学习交接、测试和GitHub发布。
- 当前阶段：`full/design`，已完成工作区盘点、范围与技术路线核对，尚未进入M0实施。
- 权威完整计划：`plans/arena-core-server-master-plan-2026-08-11.md`。
- 项目简要介绍：`docs/jingyu-cpp17-project-brief-2026-08-11.md`。
- 当前文件职责无冲突：`plans/`保存唯一执行基线，`docs/`保存对外摘要，`.codex/`只保存协作状态与报告。
- 已核对历史任务`019fb34b-a68f-72d0-ad56-a711b17ad3e1`中从项目选型、范围修正、C++17调整、规格锁定、移除云端到工作区移交的关键决策；当前总规划准确反映最终用户确认。
- 当前目录不是Git仓库，且没有`src/`、`include/`、`tests/`、`proto/`、Compose、CMake或CI文件；这与“尚未实施”的状态一致。
- 已锁定：C++17、WSL2 Ubuntu、模块化单体、Boost.Asio/Protobuf/Redis/MySQL/Lua、Web观察台、Robot与QA、工程质量和个人掌握度双重验收。
- GitHub交付：干净WSL2环境预装Git与Docker/Compose后，无需修改源码或配置即可启动Demo、运行smoke、打开观察台并执行测试。
- 明确排除：云服务器、公网部署、游戏画面/战斗/地图/技能、完整MMO/帧同步、Kubernetes、多机高可用和微服务拆分。
- 原始规划仍保留在BOSS岗位查找工作区作为历史来源；今后的竞域实现与学习工作只在本工作区继续。
- 历史中的早期提案（Go逐行/一比一翻译、C++20、制作游戏、云服务器部署、双层课程工程）均已被后续确认覆盖，不得重新作为当前范围。
- 开发期用户参与方式已纠正为“最少打扰”：M1—M5和M7默认由AI独立实现并自动验证，不要求用户逐阶段读代码或手工测试；用户只在环境权限、可见效果、范围变化和最终发布等必要节点介入。

## 下一步

- 用户明确要求开始实施后进入M0：重新验证CoreRank需求基线，并锁定协议、状态机、错误码、数据模型、线程归属、ADR和需求—测试矩阵。
- M0验收通过后才进入M1，由M1创建C++17工程骨架、构建/测试入口和版本管理基础。
- 未获得开始实施指令前，不创建源码或擅自扩大功能范围。
- 现有范围已由历史任务锁定，用户发出开始指令后AI可直接完成M0技术规格；只有出现无法从现有计划推断的用户可见行为取舍时才询问。
- 计划中的个人掌握任务属于项目完成后的学习/简历门槛，不等于开发过程中每阶段都需要用户参与。

## 未验证项

- 当前只验证了导入文档与来源文件哈希一致。
- 本轮已验证工作区文件清单、规划/摘要/状态的一致性以及当前无Git元数据、无业务源码的事实。
- 项目尚未实现，因此构建、测试、Demo和fresh-clone流程均未运行，不能声称项目已经可用。

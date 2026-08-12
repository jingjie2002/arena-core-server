# 当前方案

## 当前任务

- 任务名称：竞域 v1.1 M9 可信比赛结果接入。
- 任务模式：按已确认范围执行；复杂任务；`full/build`。
- 当前结论：M9 实现、自动验证、文档和分支提交均已完成；没有合并 `main` 或上传 GitHub。
- 明确未做：M10 场景/Run 引擎、Lab Web、Toxiproxy、M11/M12、公开发布。

## M9 完成清单

- [x] 独立可信结果 HTTP listener、Bearer token 与网络边界。
- [x] Room `AwaitingResult` / `ResultTimedOut` 状态与迟到结果拒绝。
- [x] result/match/room/成员/规则/时间校验以及幂等和冲突语义。
- [x] MySQL 已提交 receipt 查询、双玩家推送和参与者只读查询。
- [x] DemoBattleAdapter、Robot 场景、配置与 Compose 接线。
- [x] GCC/Clang 90/90、M9 E2E、真实 MySQL/Redis 12/12 和重启查询。
- [x] 规格、ADR、架构、协议、交接与操作记录。

## 下一步

- 下一里程碑是 M10：参数化场景模型、Run 状态机和 C++ `arena-core-lab` 编排核心。
- M10 可以在既定 v1.1 范围内自动继续；只有出现范围变化、权限/环境阻塞或最终可见效果验收时才需要用户介入。

---

## v1.1 设计基线（历史）

- 任务名称：竞域 v1.1 产品设计与可接入化
- 任务模式：从概念开始；产品设计提案已完成，等待用户确认
- 方案更新时间：2026-08-12（GitHub 调研与正式设计提案完成后）
- Harness：`full/design`；用户确认定位前不进入实现
- 当前分支：`codex/v1.1-scenario-lab`；所有 v1.1 实验改动与 `main` 隔离，确认后才考虑合并

## 当前设计结论（等待用户确认）

- 推荐定位：`竞域｜C++17 1v1 竞技游戏对局编排与结算服务端`。
- 业务边界：负责战斗之外的会话、重连、匹配、房间、可信结果接入、MySQL 结算、Redis 排行与修复；不实现移动、技能和战斗。
- 项目形态：独立服务端 + 玩家协议 + 可信结果 API + 真实 TCP Robot/示例适配器 + 本地开发/QA 控制台 + MySQL/Redis Compose，不是通用框架或普通玩家产品。
- v1.1 核心：Room 在 Ready 后等待可信结果；Demo Battle Adapter 与未来战斗服走同一结果入口；已提交结果通知玩家并可查询。
- 控制台形态：从固定按钮升级为可变参数场景实验台；本机使用者能组合安全输入并重放，独立 Lab Controller 驱动真实 Robot 和受控故障，Observer 仍只读。
- 安全边界：不允许页面直接热改业务对象、数据库、任意 Lua/SQL/Shell 或正式服务配置；远程多人使用需另行增加认证、权限和审计，不纳入当前本机 v1.1。
- 技术路线：继续 C++17、Boost callback/strand、Protobuf、MySQL、Redis、Lua、原生 Web；只增加 C++ `arena-core-lab` 和可选 Toxiproxy lab profile。
- P0 场景：完整对局、断线恢复、重复玩家请求、重复权威结果、Redis 故障恢复；MySQL 故障页面化、Run 对比等留给 v1.2。
- 发布边界：真实演示、README 定位和个人讲解未完成前，不上传 GitHub。
- 推荐设计：`docs/specs/v1.1-product-design-proposal.md`；参考调研：`docs/research/github-game-server-reference-review-2026-08-12.md`；用户确认后切换 `full/build` 并按 M9—M12 实施。

## v1.1 推荐实施阶段（尚未授权实施）

- M9：可信结果入口、`AwaitingResult`、结果通知/查询和接入示例。
- M10：可复用 C++ QA 场景库、Scenario Schema、Run 状态机与四个无代理 P0 场景。
- M11：`arena-core-lab`、四页开发/QA 控制台、Toxiproxy 和 Redis 故障场景。
- M12：双编译器、真实依赖、浏览器 E2E、Sanitizer、稳定性、fresh-clone、文档和最终人工可见性验收。

## v1.0.1 修复结论

- [x] 复现三个按钮统一失败，并确认服务器返回 `demo_mode_disabled`。
- [x] 保留普通非 loopback Demo 拒绝策略，为受宿主机 loopback HAProxy 保护的 Compose 私有网络增加显式例外。
- [x] 新增两项配置测试和无副作用 Compose Demo 探针，避免同类前后端能力错位回归。
- [x] GCC/Clang 各 82/82，主 Docker、真实 Robot/MySQL/Redis、隔离 clean-copy 全部通过。
- [x] 真实浏览器三按钮和页面日志验收通过，当前服务版本 `1.0.1`。
- [x] 保留 `v1.0.0` 历史基线，新增 `v1.0.1` 修复版本，不执行未经授权的公网发布。

## M8 完成结论

- 当前状态：README、架构/协议/数据/测试/故障/运维文档、Compose 演示、截图、许可证和 v1.0 Release Notes 已完成。
- fresh-clone：clean-copy 与真实本地 Git clone 均完成原生 GCC、80 项 CTest、独立 Compose、Robot、MySQL、Redis 和观察台验收。
- 用户介入：用户在重启后明确要求一次性推进到完成，因此原第 3/3 次人工复现改由 Codex 在本机全新 Linux 目录自动完成并留下证据。
- 发布边界：本地 `main` 与 `v1.0.0` 标签属于项目完成基线；上传公开 GitHub 和创建远端 Release 未获外部发布授权，因此保持未执行。

## 当前进度

- [x] M0：CoreRank 只读基线、v1 需求、协议、状态机、错误、数据、线程所有权、ADR、测试矩阵。
- [x] M1：C++17 骨架、依赖、构建、日志、配置、CI、基础测试。
- [x] M2：领域模型、FakeClock/ID 抽象和状态机。
- [x] M3：TCP/Protobuf/Session/心跳/关闭/重连。
- [x] M4：匹配、加入凭证、Room 和失败回滚。
- [x] M5：MySQL 结算、Redis 排行、Lua 与缓存修复。
- [x] M6：观察台、非技术讲解、自动验证和人工可见性复验均通过。
- [x] M7：Robot、故障注入、Sanitizer、故障/停机和 500/250/600 秒稳定性验证。
- [x] M8：文档、Compose、clean-copy、真实 Git clone、MIT 许可证、本地 Git 历史与 v1.0 收口。

## 目标

- 实现一条可解释、可接入、可测试的 C++17 游戏服务端闭环：TCP/Protobuf/Session/心跳/重连→匹配→Room→等待可信结果→MySQL 结算→Redis 排行→玩家结果通知。
- 通过Web观察台、Robot、故障注入、Sanitizer和真实测试报告形成简历证据。
- 通过GitHub fresh-clone硬验收，消除作者机器缓存、私有配置和已有数据库依赖。

## 当前采用方案

- v1.0 历史范围与里程碑以`plans/arena-core-server-master-plan-2026-08-11.md`为已完成执行基线；v1.1 尚未进入实现。
- v1.1 产品范围以`docs/specs/v1.1-product-design-proposal.md`为待确认推荐方案，确认后再生成实施级任务分解。
- 简要定位以`docs/jingyu-cpp17-project-brief-2026-08-11.md`为快速入口。
- 已读取历史任务`019fb34b-a68f-72d0-ad56-a711b17ad3e1`并完成交叉核对；历史最终确认与当前总规划一致。
- 实施顺序固定为M0规格→M1骨架→M2状态机→M3网络/Session→M4匹配/Room→M5数据/Lua→M6观察台→M7 QA/稳定性→M8文档与发布。
- 每个里程碑同时交付工程证据和个人掌握任务；未达到掌握门槛时，不把项目写成简历第一项目。

## 历史决策优先级

- 最终采用：继承Go CoreRank业务规则，但以C++游戏服务端方式独立重设计；保留Go CoreRank和旧C++项目。
- 最终采用：C++17、WSL2 Ubuntu、Web观察台与可点击Demo、Robot与完整QA、GitHub fresh-clone验收。
- 最终采用：工程完成度与个人掌握度双重验收，但不在本工作区建设独立课程工程或双仓学习体系。
- 已废弃：逐行/一比一翻译Go代码、C++20协程主线、双人回合制玩法或完整游戏、云服务器部署、公网演示、课程编排。
- 发生歧义时，以当前工作区完整总规划和用户后续明确确认优先于历史中的早期建议。

## 开发过程中的用户介入（最少打扰）

- 默认原则：用户不需要读懂项目代码，也不需要在M1—M5、M7逐阶段手工测试；AI负责设计、实现、构建、自动测试、故障测试和报告。
- 开始节点：用户只需明确“开始M0”。现有产品方向已经锁定，M0的协议、状态机、错误码、数据模型和测试矩阵由AI按权威计划完成。
- 环境节点（条件触发）：只有WSL2/Docker安装、管理员权限、系统重启、交互式密码或本机专属操作无法由AI完成时，才请用户协助操作或批准权限。
- 可见效果节点（M6）：观察台和Demo首次完整可用时，请用户看一次页面、点击正常/失败场景，并只回答“是否看得懂、是否符合想要的展示效果”；技术正确性仍由AI测试。
- 发布节点（M8）：请用户在自己的干净WSL2环境执行fresh-clone入口，确认README可照做；创建/公开GitHub仓库、发布Release和采用简历表述前必须获得用户明确授权。
- 异常节点（条件触发）：若发现必须扩大功能、改变已锁定技术路线、新增重要依赖、执行不可逆操作、接触凭证或出现多个用户可见方案且无法可靠代选，AI才暂停并请求决定，同时给出推荐选项。

## AI无需询问即可决定

- 既定范围内的目录、类、接口和文件拆分。
- CMake/vcpkg/CI、编译器、日志、配置和测试组织细节。
- Asio回调、生命周期、strand、线程池、缓冲区和优雅停机实现。
- Protobuf字段细节、Redis Lua、MySQL事务/索引、缓存修复和故障注入实现。
- 单元、协议、组件、集成、E2E、Sanitizer、稳定性和性能测试设计。
- 观察台API、快照、HTML/CSS/JS内部实现，只要不改变已确认的展示目标。

## 与最终个人掌握度的区别

- 开发期可以由AI独立完成上述技术工作，不要求用户中途学习或审批。
- 项目完成后是否作为“本人主项目”是另一项任务；届时再依据交接包、演示、小修改和复测评估用户掌握度，不阻塞当前工程开发。

## 影响范围

- 后续预计新增：`src/`、`include/`、`tests/`、`proto/`、`scripts/`、`config/`、Compose、CMake Presets、CI和项目文档。
- 保留并维护：`.codex/`与`.agents/`协作管理区。
- 不在本工作区进行：BOSS岗位检索、简历投递、云服务器操作、现有Go CoreRank重写或删除。

## 具体执行路径

1. 新窗口读取`AGENTS.md`、本状态文件、Harness状态、本方案和完整计划。
2. 用户确认开始实施后，先完成M0基线复核、规格文档、ADR与验收矩阵，不直接创建完整工程或跳到网络代码。
3. 开发期每个里程碑按“AI实现→自动验证→必要时展示可见结果→报告”推进；个人解释/修改任务只记录到交接包，不要求用户中途完成。
4. M8在全新目录或干净CI checkout完成GitHub克隆即用验收。

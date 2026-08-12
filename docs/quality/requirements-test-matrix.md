# v1 需求—测试矩阵

- 状态：已更新至 v1.1 M9 可信结果接入证据
- 说明：`计划` 表示测试入口尚待对应里程碑实现；实现后必须补充命令和证据链接。

| 需求 | 主要验证 | 层级 | 里程碑 | 当前 |
|---|---|---|---|---|
| SYS-001 | 配置解析、依赖失败、健康状态测试 | unit/integration | M1/M5 | 通过：配置/自检、MySQL 健康检查、data-enabled 生产启动/停止及受信任 loopback 代理 Demo 校验 |
| NET-001 | 分片、合帧、零长、边界与超长帧测试 | unit/integration | M3 | 通过：M3 frame + 真实 TCP 分片/合帧/上限 |
| NET-002 | 版本、类型、畸形 Protobuf、关闭策略测试 | integration | M3 | 通过：M3 协议错误响应与断开测试 |
| SES-001 | Hello 截止、令牌签发、非法 player 测试 | integration | M3 | 通过：M2/M3 领域与 TCP 集成 |
| SES-002 | FakeClock 心跳与超时测试 | unit/integration | M3 | 通过：FakeClock + 真实 timer/Heartbeat/idle |
| SES-003 | 正常恢复、过期、轮换、旧连接竞态测试 | integration | M3 | 通过：token 双轮换与 stale generation 测试 |
| SES-004 | 多来源 Stop、Sanitizer 与压力测试 | unit/stress | M3/M7 | 通过：幂等 Stop；ASan+UBSan 80/80、TSan 目标集 50/50；500 连接/250 房间/600 秒稳定性通过 |
| MAT-001 | 入队、票据字段及正常配对测试 | unit/integration | M4 | 通过：M4 真实 TCP 双玩家流程 |
| MAT-002 | 并发重复入队 Redis Lua 测试 | integration | M4 | 通过：业务重复拒绝 + Redis Lua duplicate guard |
| MAT-003 | 取消、超时、终态迟到命令测试 | unit/integration | M4 | 通过：领域与真实 Session 推送 |
| MAT-004 | 固定时钟候选顺序、扩窗、模式隔离测试 | unit | M4 | 通过：duel 隔离/非法模式/125 分差扩窗 |
| MAT-005 | 容量耗尽与分配失败回滚测试 | integration/fault | M4/M7 | 通过：容量为零时两票据保持 Queued |
| ROM-001 | token 伪造、错绑、过期、重放测试 | unit/integration | M4 | 通过：摘要校验、outsider、篡改、过期与幂等请求 |
| ROM-002 | join/ready 幂等与单次状态推进测试 | unit/integration | M4/M5/M9 | 通过：Room strand；Ready 后只进入 AwaitingResult；结算失败保持 Settling，重试成功才 Closed |
| ROM-003 | 房间内掉线、恢复和恢复过期测试 | integration | M4 | 通过：成员跨 Session 恢复；过期/abort 由 M2/M3 覆盖 |
| SET-001 | 玩家协议无结果写入口、可信来源校验 | contract/unit/e2e | M3/M5/M9 | 通过：玩家协议仍无写入口；独立 HTTP listener 要求 Bearer token，Room 只接收匹配的可信结果 |
| SET-004 | 可信结果等待、通知、查询与超时 | unit/integration/e2e | M9 | 通过：AwaitingResult、双玩家 committed 推送、参与者查询、非参与者拒绝、超时无积分变化 |
| SET-005 | result/match 幂等与冲突 | unit/integration/e2e | M9 | 通过：相同指纹重放、result 内容冲突、match 多结果冲突及跨 match 复用 result_id 均有稳定语义 |
| SET-002 | MySQL 提交/回滚及双玩家锁定测试 | integration | M5 | 通过：真实 MySQL 8 提交、异常 JSON 回滚、玩家行锁及无半状态 |
| SET-003 | 相同指纹重放和不同指纹冲突测试 | integration | M5 | 通过：真实 MySQL 相同指纹幂等、不同指纹稳定冲突 |
| RNK-001 | 排序、并列、邻近查询测试 | unit/integration | M5 | 通过：内存与真实 Redis 排序、player_id 并列稳定序、邻近查询 |
| RNK-002 | Redis 失败、dirty 保留、修复重放测试 | integration/fault | M5/M7 | 通过：真实 Redis 失败保留 dirty，恢复后重放/全量重建；20 轮依赖故障序列无半结算 |
| LUA-001 | golden、版本校验、非法脚本和确定性测试 | unit | M4/M5 | 通过：Redis 原子脚本；嵌入规则版本、边界、确定性、能力隔离和指令预算 |
| OBS-001 | 快照隔离、事件顺序和慢消费者测试 | unit/integration | M6 | 通过：不可变 JSON 快照、有限事件缺口、trace 顺序、只读 HTTP、真实 TCP→Room→Observer 与 Compose Demo 入口探针 |
| QA-001 | Robot 完整双玩家链路 | e2e | M7/M9 | 通过：真实 TCP 完成连接、匹配、进房和准备；DemoBattleAdapter 通过可信 HTTP 提交后完成结算、结果推送与查榜 |
| QA-002 | 固定种子、依赖故障、回放一致性 | e2e/fault | M7 | 通过：固定场景报告可在重启后回放核对；依赖故障与恢复序列通过 |
| OPS-001 | 各状态下 SIGTERM 排空与截止测试 | integration | M7 | 通过：覆盖排队、已入房、活跃连接、已完成和真实数据依赖；E2E 最慢退出 105 ms |
| REL-001 | 空目录 fresh clone 按 README 全量执行 | manual/release | M8 | 通过：本地 Git `c0338e1` 克隆到临时 Linux 目录；直接执行脚本、GCC 80/80、全新 Compose/数据卷与完整 Robot smoke 通过 |

## M0 已有证据

- Go CoreRank 参考基线：`go test ./...`、`go vet ./...` 和五个入口构建通过。
- CoreRank Redis 集成路径已验证；MySQL 因缺少测试 DSN 跳过 6 项，已明确记录为未验证。
- 上述证据只证明参考实现当前基线，不替代 C++ 项目测试。

## M5 实现证据

- 依赖版本：MySQL 8.0.46、Redis 7、MySQL Connector/C++ 1.1.12、hiredis 1.2.0、Lua 5.4.6。
- 双编译器完整回归：`cmake --build --preset linux-gcc && ctest --test-dir out/build/linux-gcc --output-on-failure`，Clang 将 preset/path 改为 `linux-clang`；两边各 67/67。
- 真实数据集成：`./scripts/dev/m5-data-integration.sh out/build/linux-gcc`，Clang 对应 `out/build/linux-clang`；两边各 11/11。
- 集成入口会启动隔离 MySQL/Redis、执行迁移、验证真实事务/回滚/幂等/dirty 修复/排行并列、启动 data-enabled 服务端、发送 SIGTERM 并执行 EXPLAIN。
- `DataIntegration` 在普通完整回归中没有测试环境变量时会按设计跳过；上面的 M5 集成入口注入真实依赖后已实际执行通过，跳过记录没有被删除或冒充通过。

## M6 实现证据

- 双编译器完整回归：GCC/Clang 各 74/74；普通全回归中的真实数据集成仍按设计跳过，随后由 M5 隔离入口在两套构建中各 11/11 实际执行。
- 观察层定向测试：`ctest --test-dir out/build/linux-gcc -L observer --output-on-failure`，Clang 同理；各 6/6。
- 真实 HTTP smoke：`./scripts/dev/m6-observer-smoke.sh out/build/linux-gcc`，Clang 同理；覆盖静态页、8 人/4 房间、重复结算与 Redis 修复。
- 真实 TCP/Robot 回归：`./scripts/dev/m3-smoke.sh out/build/linux-gcc`，Clang 同理；新增观察端口没有破坏既有协议入口。
- 浏览器实际点击：正常、幂等和缓存修复三按钮均通过，trace 筛选仅保留目标事件，console error/warning 为 0。旧版 390×844 视口无横向溢出；讲解改造后的版本因浏览器安全策略未重新创建该窄屏容器，已保留并加强响应式断点。
- 安全边界：HTTP 仅绑定 loopback；Demo POST 同时要求 `--demo-mode` 和 `X-Arena-Demo: 1`，并发送 CSP、禁止 frame 和 no-sniff 响应头。

## M7 实现证据

- 双编译器普通回归：GCC/Clang 各 80/80；旧版 Robot 心跳 smoke 与完整 M7 Robot E2E 在两套构建中均通过。
- 真实数据集成：`./scripts/dev/m5-data-integration.sh out/build/linux-gcc`，Clang 对应 `out/build/linux-clang`；两边各 12/12，并执行完整 Robot 的 MySQL 结算和 Redis 查榜。
- 功能、重放和停机：`./scripts/dev/m7-e2e.sh out/build/linux-gcc`；记录与重启后回放一致，8 人形成 4 房间，活跃连接停机与重启回归通过，最慢退出 105 ms。
- Sanitizer：Clang ASan+UBSan 80/80 并通过同构建 E2E；GCC TSan 目标集 `unit|protocol|network|qa` 50/50。Boost.Asio 的 fence 工具链提示仅在 TSan 构建中保留为警告。
- 稳定性：`./scripts/dev/m7-soak.sh out/build/linux-gcc scenarios/qa/soak-500-connections.json`；500 连接、250 房间保持 600 秒，业务错误 0，服务峰值 RSS 22.5 MiB，P99 完整业务闭环约 370.716 ms。
- 完整指标与边界见 `docs/quality/M7-stability-report-2026-08-12.md`。`clang-format` 在当前环境不可用，因此未声称自动格式检查通过；Shell/JSON 语法、尾随空白、GCC/Clang 严格构建均已验证。

## v1.0.1 修复证据

- 根因复现：Compose 页面三个 Demo POST 均返回 HTTP 403 `demo_mode_disabled`，确认是启动配置关闭入口，不是用户误操作或业务服务损坏。
- 双编译器：GCC/Clang 各 82/82；新增“显式受信任代理允许 Demo”和“Demo 关闭时拒绝代理例外”两项配置测试。
- 主 Compose 与镜像：镜像内 GCC 82/82；Robot、MySQL、Redis、观察台和无副作用 Demo 探针通过；服务版本为 `1.0.1`。
- clean-copy：全新临时源码目录 GCC 82/82，独立 Compose/数据卷和完整 smoke 通过。
- 浏览器：三按钮依次显示正确中文结论，页面错误日志为 0。完整操作见 `docs/operations/v1.0.1-observer-demo-fix-2026-08-12.md`。

## 证据更新规则

- 每个 `计划` 在实现后更新为 `通过`、`部分通过` 或 `阻塞`。
- `通过` 必须附可重复命令；集成/E2E 还要注明依赖版本或 Compose 服务。
- 失败和跳过项不得删除，必须记录原因和补偿计划。
- M8 只有在全新 Linux Git clone 或独立 CI checkout 中重复成功后，REL-001 才能改为通过。

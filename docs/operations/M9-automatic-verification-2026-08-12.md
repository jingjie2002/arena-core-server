# M9 自动验证与操作记录

- 日期：2026-08-12
- 分支：`codex/v1.1-scenario-lab`
- 范围：M9 可信比赛结果接入
- 结论：通过；没有要求用户执行手工步骤

## 1. 双编译器回归

GCC：

```bash
cmake --build --preset linux-gcc
ctest --test-dir out/build/linux-gcc --output-on-failure
```

结果：90/90 通过。普通回归中的真实 MySQL 集成项因为没有注入测试环境而按设计跳过，随后由独立真实依赖入口实际执行。

Clang：

```bash
cmake --build --preset linux-clang
ctest --test-dir out/build/linux-clang --output-on-failure
```

结果：90/90 通过；跳过策略与 GCC 相同。

## 2. M9 独立端到端验证

```bash
./scripts/dev/m9-e2e.sh out/build/linux-gcc
```

验证链路：真实 Robot TCP 连接、匹配、Join/Ready、`DemoBattleAdapter` 调用受保护的可信结果 HTTP、MySQL 结算、双玩家 committed 推送、可信查询、幂等重放和冲突拒绝。

关键断言：

- 无凭证请求返回 HTTP 401。
- 首次合法提交完成一场结算。
- 相同结果再次提交返回既有 receipt，`idempotent=true`。
- 相同比赛的冲突结果返回 HTTP 409。
- Robot 收到已提交结果并完成排行核对。

结果：通过。

## 3. 真实 MySQL / Redis 与重启持久性

```bash
./scripts/dev/m5-data-integration.sh out/build/linux-gcc
```

结果：真实数据标签 12/12 通过；完整 Robot 形成并结算真实房间。脚本随后重启服务，通过可信 GET 查询重启前的 result receipt，证明结果不是只存在于内存。MySQL `EXPLAIN` 仍使用预期索引，Redis 故障/dirty 修复原有覆盖保持通过。

## 4. 过程中的环境冲突

第一次尝试复用 M7 默认端口时，发现用户重启前保留的本地服务仍占用 `49110`。没有停止或修改用户正在运行的进程；M9 新建隔离配置，改用 `49410`（玩家 TCP）、`49420`（Observer）和 `49430`（可信结果），随后独立端到端验证通过。

这属于本机端口占用，不是产品代码失败。隔离测试产物保存在被 Git 忽略的 `out/build/linux-gcc/qa/m9-e2e/`。

## 5. 安全与范围检查

- service token 只从 `ARENA_RESULT_SERVICE_TOKEN` 环境变量读取。
- 配置示例、日志、Observer 和错误响应不保存真实凭证。
- 可信结果 listener 默认关闭且默认只允许 loopback。
- 本轮没有实现 Lab 页面、Toxiproxy、M10 场景引擎，没有合并 `main` 或上传 GitHub。

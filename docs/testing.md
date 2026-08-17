# 测试策略

## 分层

| 层级 | 主要内容 | 入口 |
|---|---|---|
| unit / contract | 配置、错误、状态机、C++17 守卫、Lua 规则 | `ctest --preset linux-gcc` |
| protocol / network | 帧边界、粘包/拆包、Hello、心跳、重连、关闭 | CTest 标签 `protocol|network` |
| game / data | 匹配、房间、结算、幂等、dirty 修复 | CTest 标签 `game|data` |
| real integration | 隔离 MySQL 8、Redis 7、迁移与 EXPLAIN | `scripts/dev/m5-data-integration.sh` |
| E2E / replay | 真实 Robot 全链、重启回放、8 人/4 房间、停机 | `scripts/dev/m7-e2e.sh` |
| Scenario Lab | Schema、Run 状态机、真实 TCP/结果、重连、重复请求/结果 | CTest 标签 `lab` + `scripts/lab-smoke.sh` |
| dependency fault | Toxiproxy 真实 Redis 断链、MySQL 事实、dirty 修复和清理 | Lab `redis_failure_recovery` |
| sanitizer | Clang ASan+UBSan 全量；GCC TSan 目标集 | CMake sanitizer presets |
| stability | 500 连接、250 房间、资源采样 | `scripts/dev/m7-soak.sh` |
| release smoke | 默认只读 Compose 与可选 Lab profile | `scripts/demo-up.sh [--lab]` |
| release isolation | 精确 detached commit 的原生构建、默认/Lab Compose | `scripts/release/m8-local-clone-check.sh` |

## 为什么普通 CTest 有一个 Skipped

`DataIntegrationTest` 只有在注入真实 MySQL/Redis 环境变量时才执行。普通 CTest 不伪造依赖，因此把该项标为 Skipped；`m5-data-integration.sh` 启动隔离实例、注入环境变量后会真正执行全部 12 个 data 测试，并额外运行完整 Robot 数据链。

## v1.1 Lab 验收口径

- Scenario 请求只接受版本化白名单字段；host、port、token、路径、SQL/Shell 等必须被拒绝。
- 五种 P0 都要在 Compose Lab 中走真实 TCP 和可信结果入口；Redis 场景必须证明服务确实经过 Toxiproxy。
- 断线恢复同时跑窗口内与窗口外；重复结果同时跑 exact 与 conflicting。
- 重放必须生成新 run_id，保留原 scenario/parameters/seed/timeout；动态 match/room/result ID 不要求相同。
- 取消与超时必须进入对应终态并完成 cleanup；一个活动 Run 不能被第二个覆盖。
- `scripts/lab-smoke.sh` 自动覆盖上述核心链路，并在任何退出路径 reset Toxiproxy。

## Sanitizer 口径

- `linux-clang-asan-ubsan`：最终源码运行动态发现的 119 项 CTest，并补可信结果与 Lab 核心 E2E。
- `linux-gcc-tsan`：执行可稳定覆盖并发的 `unit|protocol|network|game|observer|qa|lab` 目标集。
- GCC 对 Boost.Asio `atomic_thread_fence` 的 TSan 工具链提示沿用既有已知限制；其他编译警告仍按错误处理。
- 历史 `80/80`、`50/50` 只能作为 v1.0 基线，不能代替 v1.1 最终结果；测试数由 CTest 动态发现并写入 M12 记录。

## 稳定性口径

500 Robot 同时建立独立 socket，组成 250 个一次性双人房间，随后保持活跃连接 600 秒并持续心跳。这个场景验证连接生命周期、房间闭环和长时资源稳定，不代表每秒持续创建 250 个房间，也不代表服务极限吞吐。

完整需求映射见 `docs/quality/requirements-test-matrix.md`，v1.1 最终实测数值见 `docs/operations/M12-final-verification-2026-08-17.md`；M7 报告只保留为 v1.0 历史基线。

M12 的 clean-copy 入口会复制源码到临时目录，明确排除 `.git`、`out`、`build`、`.runtime`、vcpkg 缓存和旧数据卷；它只是文件系统预检，不证明 Git 已提交内容和可执行位。

正式隔离门禁使用 `scripts/release/m8-local-clone-check.sh`：源工作区必须干净，脚本精确 detached checkout 当前/指定 commit，再运行 GCC 全量、默认 Compose、五场景 Lab、重放/取消/超时/清理，并确认 Git 可执行位和 clone 状态没有变化。文件名保留 M8 只是兼容旧入口，摘要已改为 M12。

## 浏览器验收

最终 Compose Lab 在桌面与 `390×844` 两种视口检查四页：能力总览、场景参数、Run 详情、接入指南。浏览器必须看到 API 真实空态/真实 Run，console/page error 为 0，无横向溢出；轮询使用语义终态，不依赖固定 sleep。Observer 另行确认没有写按钮、没有旧 Demo POST。

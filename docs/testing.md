# 测试策略

## 分层

| 层级 | 主要内容 | 入口 |
|---|---|---|
| unit / contract | 配置、错误、状态机、C++17 守卫、Lua 规则 | `ctest --preset linux-gcc` |
| protocol / network | 帧边界、粘包/拆包、Hello、心跳、重连、关闭 | CTest 标签 `protocol|network` |
| game / data | 匹配、房间、结算、幂等、dirty 修复 | CTest 标签 `game|data` |
| real integration | 隔离 MySQL 8、Redis 7、迁移与 EXPLAIN | `scripts/dev/m5-data-integration.sh` |
| E2E / replay | 真实 Robot 全链、重启回放、8 人/4 房间、停机 | `scripts/dev/m7-e2e.sh` |
| sanitizer | Clang ASan+UBSan 全量；GCC TSan 目标集 | CMake sanitizer presets |
| stability | 500 连接、250 房间、资源采样 | `scripts/dev/m7-soak.sh` |
| release smoke | Compose 从配置生成到数据库/排行/观察台 | `scripts/demo-up.sh` |
| release isolation | 排除 Git/构建/运行缓存后的原生构建与独立 Compose | `scripts/release/m8-clean-copy-check.sh` |

## 为什么普通 CTest 有一个 Skipped

`DataIntegrationTest` 只有在注入真实 MySQL/Redis 环境变量时才执行。普通 CTest 不伪造依赖，因此把该项标为 Skipped；`m5-data-integration.sh` 启动隔离实例、注入环境变量后会真正执行全部 12 个 data 测试，并额外运行完整 Robot 数据链。

## Sanitizer 口径

- `linux-clang-asan-ubsan`：全量 80/80，并运行真实 E2E。
- `linux-gcc-tsan`：按计划执行 `unit|protocol|network|qa` 目标集 50/50。
- GCC 对 Boost.Asio `atomic_thread_fence` 给出 TSan 工具链警告；只在 TSan 构建中将该条警告保留但不升格为 `-Werror`，其他警告仍按错误处理。

## 稳定性口径

500 Robot 同时建立独立 socket，组成 250 个一次性双人房间，随后保持活跃连接 600 秒并持续心跳。这个场景验证连接生命周期、房间闭环和长时资源稳定，不代表每秒持续创建 250 个房间，也不代表服务极限吞吐。

完整需求映射见 `docs/quality/requirements-test-matrix.md`，实测数值见 `docs/quality/M7-stability-report-2026-08-12.md`。

M8 的 release isolation 会复制源码到临时目录，明确排除 `.git`、`out`、`build`、`.runtime`、vcpkg 缓存和旧数据卷；结束时自动删除临时源码、容器和数据卷，只把摘要写回 `.runtime/m8-clean-copy-summary.json`。

首次本地发布还会运行 `scripts/release/m8-local-clone-check.sh`：它只读取 Git 已提交内容，克隆到临时 Linux 目录后重复 GCC 与 Compose 验收，用来发现未提交文件依赖和 Git 可执行位问题。

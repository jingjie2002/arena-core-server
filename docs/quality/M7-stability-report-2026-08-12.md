# M7 稳定性与 Sanitizer 实测报告

## 非技术结论

M7 通过。自动 Robot 已能完成“连接→匹配→进入房间→双方准备→确定性结算→查询排行”的真实 TCP 全链路，并能在服务重启后重放同一场景、核对业务结果。

正式稳定性测试在本机 WSL2 中保持 500 个在线连接和 250 个双人房间 600 秒，业务错误为 0，服务未崩溃，结束时能够正常停止。

## 参考环境

- 系统：WSL2 Linux 5.15.167.4，x86_64，glibc 2.39。
- CPU 口径：WSL 可见 16 个逻辑处理器；`ps %CPU` 以单个逻辑处理器 100% 为口径。
- 普通编译器：GCC 13.3.0、Clang 18.1.3。
- Sanitizer：Clang 18 ASan+UBSan；GCC 13 TSan 缩小并发集。
- 构建类型：Debug，C++17，项目警告默认按错误处理。

## 正式 500/250/10 分钟结果

| 指标 | 实测 |
|---|---:|
| 成功连接 | 500 |
| 完整双人房间 | 250 |
| Robot 计时 | 600005 ms |
| 满载采样 | 119 次，约 614.5 秒 |
| Robot 心跳 | 299500 |
| 业务错误 | 0 |
| 房间全链路 P50 | 364.978 ms |
| 房间全链路 P95 | 369.676 ms |
| 房间全链路 P99 | 370.716 ms |
| 最大房间全链路延迟 | 371.409 ms |
| 服务峰值 CPU | 22.4% |
| 服务峰值 RSS | 22.5 MiB |
| Robot 峰值 RSS | 44.9 MiB |

这里的延迟是一次完整业务闭环的端到端时间，不是单个网络包延迟；吞吐数字包含 10 分钟连接保持期，因此不能当作服务极限吞吐。

## 功能、故障和停机证据

- 场景记录与服务重启后的 `--replay` 结果一致，计时差异不会误判为业务漂移。
- 8 个并发 Robot 完成 4 个房间，保持期间持续心跳，业务错误为 0。
- 服务分别在已完成房间、活跃连接和重启回归状态接收 SIGTERM，最慢退出 105 ms。
- 真实 MySQL/Redis 隔离测试在 GCC、Clang 两套构建中各 12/12 通过；完整 Robot 确实经过 MySQL 结算和 Redis 排行查询。
- 20 轮依赖故障序列覆盖 MySQL 失败不写排行、Redis 写失败保留 dirty、恢复后修复重放，最终无半结算。

## Sanitizer 证据

- Clang ASan+UBSan：80/80 全量回归通过；同一构建额外通过 Robot 记录、重放、8 人流程、活跃停机与重启回归。
- GCC TSan：按总计划使用缩小并发集，50/50 通过。
- GCC 会对 Boost.Asio 的 `atomic_thread_fence` 输出 TSan 工具链能力提示；只在 TSan 构建中将这一条从错误降为可见警告，其他警告仍按错误处理。

## 预检中发现并修正的问题

第一次 500 连接预检只建立了 255 个连接，Robot 报 `Too many open files`。原因不是服务容量，而是每个 Robot socket 都额外创建了一个 Asio `io_context`，在 WSL 的 1024 文件句柄上限下浪费事件句柄。

修正为 500 个独立 socket 共享一个 Robot I/O 上下文后，没有调高系统限制，5 秒预检和正式 10 分钟测试都达到 500 连接、250 房间。该最终实现随后重新经过 ASan+UBSan、Sanitizer E2E 和 TSan 回归。

## 可复现入口

```bash
# 功能、回放、停机和重启
./scripts/dev/m7-e2e.sh out/build/linux-gcc

# 500 连接预检
./scripts/dev/m7-soak.sh out/build/linux-gcc scenarios/qa/preflight-500-connections.json

# 正式 10 分钟测试
./scripts/dev/m7-soak.sh out/build/linux-gcc scenarios/qa/soak-500-connections.json

# Sanitizer
cmake --preset linux-clang-asan-ubsan
cmake --build --preset linux-clang-asan-ubsan
ctest --preset linux-clang-asan-ubsan

cmake --preset linux-gcc-tsan
cmake --build --preset linux-gcc-tsan
TSAN_OPTIONS=halt_on_error=1 ctest --preset linux-gcc-tsan -L 'unit|protocol|network|qa'
```

`out/build/linux-gcc/qa/m7-soak/` 中的原始 JSON、CSV 和 time 输出属于可再生构建产物；本文件保存适合版本管理的结论与关键数值。

## 边界

- 本轮没有把 250 个房间做成长期反复开关的极限吞吐测试；验收目标是 250 个正常房间完成后保持 500 个活跃连接 10 分钟。
- TSan 按计划只运行缩小并发集，不在完整 500 连接场景运行。
- 未安装 `clang-format`，因此没有自动格式检查结论；Shell/JSON 语法、尾随空白、GCC/Clang `-Werror` 构建和测试均已通过。

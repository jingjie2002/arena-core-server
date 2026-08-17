# v1.1 M10—M12 实施基线

- 状态：已确认产品范围下的实施级基线
- 日期：2026-08-13
- 上位设计：`docs/specs/v1.1-product-design-proposal.md`
- 前置：M9 可信结果边界已完成

## 1. 实施原则

- 用户场景参数与运行环境严格分离。页面不能指定 host、port、token、文件路径或环境变量名。
- `arena-core-lab` 是独立 loopback 本地程序，不进入正式 `arena-core-server` 进程。
- Lab 只编排真实 TCP Robot、可信结果 API 和可选依赖代理，不修改服务端业务内存或数据库。
- 保留旧 Robot 压力入口；交互 Run 使用独立的版本化场景/报告模型。
- 一个 Lab 实例同一时刻只允许一个活动 Run。

## 2. M10 场景与 Run 模型

首批场景：

- `complete_match`：2–32 名偶数玩家，支持统一/阶梯 MMR、seed、连接与 Ready 延迟。
- `reconnect_recovery`：目标阶段、目标玩家、0–25000 ms 重连延迟，验证窗口内/外语义。
- `duplicate_player_request`：安全消息枚举与 1–10 次相同请求重放。
- `duplicate_trusted_result`：exact/conflicting 与 1–10 次提交。
- `redis_failure_recovery`：M11 增加，只允许预定义 Toxiproxy 故障类型与有限持续时间。

Run 终态保留真实结束原因：

```text
Validating -> Starting -> Running -> Verifying -> Cleaning -> Passed/Failed
Running -> Cancelling -> Cleaning -> Cancelled
Running -> TimingOut -> Cleaning -> TimedOut
```

报告包含 run_id、replay_of、不可变输入、最终 seed、版本/commit/配置摘要、时间、事件、断言、首个失败、业务证据和清理证据，不包含任何 token。

## 3. M11 Lab API 与页面

API 固定为：

- `GET /api/lab/v1/capabilities`
- `GET /api/lab/v1/scenarios/{name}/schema`
- `POST /api/lab/v1/runs`
- `GET /api/lab/v1/runs/{run_id}`
- `GET /api/lab/v1/runs/{run_id}/events?after_seq=`
- `POST /api/lab/v1/runs/{run_id}/cancel`
- `POST /api/lab/v1/runs/{run_id}/replay`

页面使用原生 HTML/CSS/JS，明确分为服务概览、场景实验台、运行详情、接入指南四个页面。短轮询读取状态，不引入 WebSocket/SSE/Node。

## 4. Toxiproxy 边界

- 固定可选容器版本 `ghcr.io/shopify/toxiproxy:2.12.0`，只属于 Compose `lab` profile。
- 默认 Compose 的 arena-core 直连 Redis；Lab profile 的 arena-core 只通过预配置 Redis proxy 连接。
- Lab 只能调用固定 Toxiproxy API、固定 proxy 名和预定义 toxic；不能执行 Docker/Shell。
- 每次 Run 前后调用 reset，取消、超时和失败也必须复原代理。
- Toxiproxy API 与 Redis 代理端口不发布到宿主机。

## 5. M12 验收

- GCC、Clang 全量 CTest。
- 真实 MySQL/Redis 与五个 P0 场景 E2E。
- Lab HTTP 契约与真实浏览器桌面/窄屏 E2E。
- Clang ASan+UBSan 和 GCC TSan 适用目标。
- 500 连接、250 房间、600 秒稳定性复验。
- 从当前实验分支已提交 commit 执行真实 Git clone、原生构建和隔离 Compose Lab 验收。
- README、架构、协议、接入、测试、运维、限制、Release Notes 和非技术交接同步到 v1.1。

## 6. 明确不做

- 不做 P1/P2：MySQL 故障页面化、Run 对比、非法帧 UI、压力 UI、2v2、远程账号/RBAC。
- 不允许任意 SQL/Shell/Lua、路径、地址或原始 Protobuf 输入。
- 不增加 WebSocket、SSE、大型前端框架、微服务或 C++20/23。
- 不合并 `main`、不创建 v1.1 标签、不上传 GitHub，直到用户最终确认。

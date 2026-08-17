# M12 v1.1 最终验证与操作记录

## 结论

`codex/v1.1-scenario-lab` 已达到本地 v1.1.0 候选版本的完成标准：核心服务、可信结果入口、可变参数 Scenario Lab、五类真实场景、只读 Observer、故障恢复、双编译器、Sanitizer、600 秒稳定性和隔离发布流程均已验证。

这不等于公开发布。`main`、既有标签和 GitHub 远端未修改；是否合并、打 `v1.1.0` 标签或上传仍由项目所有者决定。

## 验证环境

- Windows + WSL2 Ubuntu 24.04，Linux 5.15 WSL2 内核。
- GCC 13.3.0、Clang 18.1.3、CMake/Ninja、C++17 严格警告即错误。
- Docker Engine 28.5.1、Compose v2。
- MySQL 8.0.46、Redis 7.2.15、Toxiproxy 2.12.0。

## 1. 静态与发布契约

- `git diff --check`：通过。
- 所有受 Git 跟踪的 Shell 脚本 `bash -n`：通过。
- Lab/Observer JavaScript 语法、JSON 文件解析：通过。
- 默认与 `lab` profile 的 `docker compose config --quiet`：通过。
- CMake、`version.hpp` 与文档版本统一为 `1.1.0`。
- 发布脚本不再固定克隆 `main`，而是对源工作树要求 clean，并 detached checkout 精确提交。

## 2. 双编译器与真实依赖

| 门禁 | 结果 |
|---|---|
| GCC 全量 CTest | 119/119；真实 DataIntegration 在普通环境按设计标记 Skipped |
| Clang 全量 CTest | 119/119；跳过策略同 GCC |
| 真实 MySQL/Redis | 12/12，包含事务、冲突、dirty、修复、重建与 EXPLAIN |
| M9 可信结果 E2E | 通过：适配器 → Bearer 入口 → 结算 → 双玩家通知 → 查询/幂等/冲突 |
| M7 Robot E2E | 通过：记录、语义重放、8 人 4 房间、SIGTERM 与重启；最慢退出 103 ms |

普通 CTest 的唯一 Skipped 不是被忽略的失败；`scripts/dev/m5-data-integration.sh` 随后启动隔离真实依赖并实际执行了该项。

## 3. Sanitizer 与并发

- Clang ASan+UBSan：119/119，通过同构建 M9 E2E；另用同构建 Lab 二进制完成五场景核心 smoke，日志中无 AddressSanitizer、UndefinedBehaviorSanitizer 或 runtime error。
- GCC TSan：`unit|protocol|network|game|observer|qa|lab` 目标集 103/103，无竞态报告。
- GCC/Boost.Asio 在 TSan 构建仍输出已知的 `atomic_thread_fence` 工具链能力提示；该提示没有被隐藏，完整 500 连接场景由普通严格构建承担。

## 4. Scenario Lab 与 Compose

`./scripts/demo-up.sh --lab` 使用最终源码构建镜像，镜像内 GCC CTest 119/119，并通过：

- 默认 TCP → 匹配 → Room → MySQL → Redis → Observer smoke；
- 完整对局；
- 恢复窗口内与窗口外断线恢复；
- 同一玩家 Envelope 原样重发；
- 可信结果 exact 幂等与 conflicting 拒绝；
- Toxiproxy Redis 故障的 MySQL committed → cache stale/dirty → Redis recovered；
- replay 生成新 `run_id` 且保留场景、参数、seed、timeout；
- cancel、timeout、单活动 Run 和所有退出路径 cleanup/reset。

默认 profile 不启动 Lab/Toxiproxy；宿主机只以 loopback 暴露玩家 TCP、Observer 和可选 Lab 页面。MySQL、Redis、可信结果与 Toxiproxy 控制端口不发布。

## 5. 浏览器与非技术可见性

- 桌面与 `390×844` 两种视口检查 Lab 四页，无横向溢出。
- 完整对局把 4 名玩家通过真实 TCP 形成 2 个房间、2 个结算和 4 个 fresh 排行记录。
- replay 生成不同 `run_id`，保留同一参数与 seed，并再次通过。
- Run 页面能看到状态、参数、事件、断言、业务证据与清理；页面不内置假成功。
- Observer 没有表单或写按钮，旧 Demo POST 在默认 Compose 中关闭。
- 最终页面使用同源白名单 SVG favicon，在不放宽 CSP 的前提下保持 console/page error 0。

截图：

- `docs/assets/v1.1-lab-run-desktop.png`
- `docs/assets/v1.1-lab-run-mobile.png`

## 6. 600 秒稳定性

命令：

```bash
./scripts/dev/m7-soak.sh out/build/linux-gcc scenarios/qa/soak-500-connections.json
```

实测：

- 成功连接 500/500，峰值在线连接 500；
- 完成并保持 250/250 个双人房间；
- 持续 600.004 秒，心跳 299,500 次；
- 业务错误 0；服务峰值 RSS 24,604 KiB（约 24.0 MiB）；
- 完整业务闭环延迟 P50 386.598 ms、P95 430.494 ms、P99 433.156 ms、最大 434.010 ms。

v1.1 延迟高于 v1.0 历史基线，原因是结果已改走真实可信 HTTP 边界，并对测试工具的结果提交并发做有界控制。该场景证明生命周期与资源稳定，不代表服务极限吞吐。

## 7. 精确提交 fresh-clone

最终源码提交完成后执行：

```bash
ARENA_RELEASE_COMMIT="$(git rev-parse HEAD)" ./scripts/release/m8-local-clone-check.sh
```

脚本要求源工作树 clean，使用 `git clone --no-checkout --no-local` 后 detached checkout 精确提交，验证脚本可执行位、原生 GCC 全量、默认 Compose、Lab Compose、五场景与清理，再确认 clone 工作树仍 clean。机器可读结果写入被忽略的 `.runtime/m12-local-clone-summary.json`；最终 SHA 同时在任务交付消息中记录。

## 8. 过程中发现并处理的问题

- Robot 旧重放比较动态 match/room ID：改为比较业务语义，仍拒绝积分等真实漂移。
- 500 连接建立期间早期连接因等待全员就绪超过 idle：等待门闩期间发送心跳。
- Robot 同时保有 500 TCP socket 并发起 250 个可信 HTTP 提交时触发自身 FD 上限：对测试工具提交端做有界串行，服务端仍承受完整 500 个并发玩家连接。
- ASan 下取消用例可能在 Hello 前抢先取消：用显式同步信号等待 Hello 已到达后再触发取消。
- 浏览器自动请求 favicon 造成无意义 404；拒绝放宽 CSP，改为同源 SVG 与静态白名单，并增加 MIME/路由测试。

## 剩余边界

- 本版本是本机/私网开发与 QA 工具，不包含 TLS、账号、RBAC、审计平台或公网部署能力。
- 当前环境没有 `clang-format`，因此未声称自动格式门禁通过；严格编译、diff、Shell/JSON 与尾随空白门禁已覆盖。
- Docker Compose 会提示 `COMPOSE_BAKE=false` 将来弃用；当前 Docker 28.5.1 路径可用，后续升级 Docker 时再移除兼容开关。

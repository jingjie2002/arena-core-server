# 本地运行与排障手册

## 正常演示

```bash
./scripts/doctor.sh
./scripts/demo-up.sh
```

打开 <http://127.0.0.1:49120/>。页面顶部应显示服务器正常，`.runtime/smoke-report.json` 的 `summary.passed` 应为 `true`。

Observer 是只读页面。需要修改场景条件时，先停止默认模式，再启动 Lab：

```bash
./scripts/demo-down.sh
./scripts/demo-up.sh --lab
```

打开 <http://127.0.0.1:49140/>。启动脚本会先完成默认真实业务 smoke，再执行五类 P0、重连窗口内外、结果冲突、重放、取消、超时和故障清理门禁。

## 常用检查

```bash
docker compose ps
docker compose logs --no-color arena-core
docker compose logs --no-color mysql redis migrate gateway
docker compose --profile lab logs --no-color arena-lab toxiproxy
./scripts/smoke.sh
./scripts/lab-smoke.sh
curl --fail http://127.0.0.1:49120/api/overview
curl --fail http://127.0.0.1:49140/api/lab/v1/health
```

## Docker 在 WSL 中不可用

如果 doctor 提示 Docker 引擎不可用：

1. 启动 Docker Desktop。
2. 打开 `Settings > Resources > WSL Integration`。
3. 为当前 Ubuntu 发行版启用集成并应用设置。
4. 回到 WSL 运行 `docker info` 和 `./scripts/doctor.sh`。

## 端口占用

默认使用宿主机 `49110`（TCP）、`49120`（观察台）和 Lab 模式的 `49140`。可临时改端口：

```bash
ARENA_DEMO_TCP_PORT=50110 ARENA_DEMO_OBSERVER_PORT=50120 ARENA_LAB_PORT=50140 ./scripts/demo-up.sh --lab
```

同一组变量也要用于后续 `smoke.sh` 和访问地址。

## Lab Run 卡住、失败或不能创建

1. 在“运行详情”查看当前状态、首个失败和 cleanup；同一时间只允许一个活动 Run。
2. 若仍在 Running，可点击取消；协议连接会在取消信号或操作 deadline 到达时退出。
3. 查看容器与报告：

   ```bash
   docker compose --profile lab ps
   docker compose --profile lab logs --no-color arena-lab arena-core toxiproxy
   ls -la .runtime/lab-runs
   ```

4. API 显示 terminal 但新 Run 短暂冲突属于错误；当前实现会在报告保存前保持 Cleaning。若仍复现，保留对应 `<run_id>.json` 和日志。
5. 强制停止时使用 `./scripts/demo-down.sh --lab`，它会先 reset Toxiproxy；不要直接删除 `.runtime` 或数据卷。

## Redis 故障后没有恢复

```bash
docker compose --profile lab exec -T toxiproxy toxiproxy-cli reset
docker compose --profile lab logs --no-color toxiproxy arena-core
./scripts/lab-smoke.sh
```

确认 `arena-core` 的运行配置中 Redis 指向 `toxiproxy:26379`。仅启动 Toxiproxy、但核心仍直连 `redis:6379`，不构成真实故障验证。默认 profile 不启动 Toxiproxy，这是正常安全边界。

## Lab 页面不可达但 Observer 正常

- 确认使用 `./scripts/demo-up.sh --lab`，而不是默认 `demo-up.sh`。
- `docker compose --profile lab ps` 中 `arena-lab` 应健康；其健康检查读取 `.runtime/arena-lab.json` 并请求真实 `/api/lab/v1/health`。
- 页面与 API 必须保持同源；不要把静态 HTML 单独用另一个端口打开，否则 CSP 会拒绝请求。

## 构建失败

- 查看 `docker compose build --no-cache arena-core` 的首个编译错误。
- 原生路径先运行 `./scripts/doctor.sh --native`，再运行 `cmake --preset linux-gcc`。
- 不要把当前项目的 `out/` 复制进 fresh-clone 验收目录；构建缓存不属于源码证据。

## MySQL/Redis 就绪失败

```bash
docker compose ps
docker compose logs --no-color mysql redis migrate
```

迁移服务应以退出码 0 完成。普通重启会幂等重复迁移。如果确实需要从空数据重试，先明确执行 `./scripts/demo-down.sh --reset`，再 `./scripts/demo-up.sh`；该操作会删除演示数据库和排行数据。

## 停止与恢复

```bash
./scripts/demo-down.sh
./scripts/demo-up.sh
```

普通停止保留数据卷。重启后再次运行 smoke 时会创建新的运行 ID，不会与此前 MySQL 主键冲突。

Lab 模式使用：

```bash
./scripts/demo-down.sh --lab
./scripts/demo-up.sh --lab
```

只有明确需要清空本项目的演示事实与排行时才加 `--reset`。该操作不可恢复，但不删除源码与已提交的文档证据。

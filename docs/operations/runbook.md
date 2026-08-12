# 本地运行与排障手册

## 正常演示

```bash
./scripts/doctor.sh
./scripts/demo-up.sh
```

打开 <http://127.0.0.1:49120/>。页面顶部应显示服务器正常，`.runtime/smoke-report.json` 的 `summary.passed` 应为 `true`。

## 常用检查

```bash
docker compose ps
docker compose logs --no-color arena-core
docker compose logs --no-color mysql redis migrate gateway
./scripts/smoke.sh
curl --fail http://127.0.0.1:49120/api/overview
```

## Docker 在 WSL 中不可用

如果 doctor 提示 Docker 引擎不可用：

1. 启动 Docker Desktop。
2. 打开 `Settings > Resources > WSL Integration`。
3. 为当前 Ubuntu 发行版启用集成并应用设置。
4. 回到 WSL 运行 `docker info` 和 `./scripts/doctor.sh`。

## 端口占用

默认使用宿主机 `49110`（TCP）和 `49120`（观察台）。可临时改端口：

```bash
ARENA_DEMO_TCP_PORT=50110 ARENA_DEMO_OBSERVER_PORT=50120 ./scripts/demo-up.sh
```

同一组变量也要用于后续 `smoke.sh` 和访问地址。

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

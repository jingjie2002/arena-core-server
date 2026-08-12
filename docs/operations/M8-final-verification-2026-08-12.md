# M8 最终验证操作记录（2026-08-12）

## 范围

本记录覆盖电脑重启后的环境恢复、Compose 发布修复、clean-copy、双编译器、停止/恢复、敏感信息检查、本地 Git 建档和真实 Git clone。没有上传源码、修改 GitHub 仓库或创建远端 Release。

## 环境

- Windows + WSL2 `Ubuntu-F`，GCC 13.3、Clang 18.1、CMake、Ninja。
- Docker Engine 28.5.1。
- Compose 依赖：MySQL 8.0.46、Redis 7.2.15-bookworm、HAProxy 3.0.25-alpine。

## 实际过程与结果

1. 重启后启动 Docker Desktop，保留的 MySQL/Redis/Core 容器恢复；旧网关退出。检查后把网关从 `network_mode: service:arena-core` 改为独立容器，通过 Compose DNS 连接后端，只由网关发布宿主机 `127.0.0.1:49110/49120`。
2. 首次重建遇到 Docker Hub syntax frontend 匿名令牌 `EOF`；删除非必要的远程 Dockerfile syntax 指令，使用 BuildKit 自带语法，消除该隐性下载。
3. 独立网关要求核心服务绑定容器内网。首次启动暴露镜像默认 `--demo-mode` 与真实数据配置冲突；删除正式镜像的强制 Demo 参数后，容器内 80 项 CTest 与完整 smoke 通过。
4. 仅重启 `arena-core`，网关保持运行；后端恢复后 smoke 再次通过，完整闭环 179 ms。
5. 第一次 clean-copy 已完成原生 GCC 80/80 和独立 Compose 健康，但 Robot 无法写全新 Linux `.runtime`；启动脚本显式准备容器用户可写目录后重跑通过，闭环 169 ms。临时源码、容器和数据卷由 trap 清理。
6. 最终当前工作区回归：GCC 80/80，Clang 80/80；普通 CTest 唯一真实依赖项按设计 Skipped，已由 Compose 的 MySQL/Redis 真跑补偿。
7. `demo-down.sh` 保留数据后重新 `demo-up.sh`：MySQL settlements 从 4 增至 5，旧事实未丢，新 Robot smoke 170 ms，四个常驻服务健康。
8. 敏感信息扫描未发现私钥、GitHub token 或 API key；命中的 password 仅为环境变量名称、测试值和公开的本机隔离 Demo 默认值。
9. 首次 Git 建档前确认并删除一个电脑重启遗留的 0 字节 `.git/index.lock`；当时没有 Git 进程，源码和历史未受影响。194 个文件进入首个提交，未包含 `.runtime`、`out`、`.env`、build 或 vcpkg 缓存。
10. 首个提交后发现 Windows 文件系统没有自动给 Shell 脚本记录 Git 可执行位；把 14 个脚本设为 `100755` 并增加真实克隆验收入口。
11. `m8-local-clone-check.sh` 从本地 `main` 克隆提交 `c0338e114a7fcfc8b70725317017e6d4392ec86c` 到 `/tmp`：Git 状态干净，脚本可直接执行，原生 GCC 80/80，全新 Compose/数据卷、Robot、MySQL、Redis、观察台全部通过，闭环 201 ms。临时克隆和测试卷随后清理。

## 机器可读摘要

- `.runtime/m8-clean-copy-summary.json`
- `.runtime/m8-local-clone-summary.json`
- `.runtime/smoke-report.json`

`.runtime` 被 Git 忽略；正式可长期复核的结论保存在本记录、M7 稳定性报告和需求—测试矩阵中。

## 最终结论

REL-001 已满足：项目可从只包含 Git 已提交内容的全新 Linux 目录执行 README 入口，不依赖原工作区构建产物、运行配置或数据库卷。M8 工程门禁通过。

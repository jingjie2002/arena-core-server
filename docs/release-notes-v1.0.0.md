# 竞域 1.0.0 发布说明

## 完成内容

- C++17、Boost.Asio、Protobuf 的 TCP 会话、心跳、重连与请求幂等。
- 1v1 匹配、一次性 Join Token、RoomActor 与服务端确定性结果。
- MySQL 事务事实、Redis 排行缓存、Lua 规则与 dirty 修复。
- 本地 Web 观察台、真实 Robot 场景、重启重放和故障验证。
- Docker Compose、环境 doctor、幂等迁移、业务 smoke 与安全停止入口。
- README、架构/协议/数据/测试/故障/限制/运维文档与 MIT 许可证。

## 质量证据

- GCC 13 与 Clang 18 普通回归各 80/80。
- 真实 MySQL 8 / Redis 7 数据集成、Clang ASan+UBSan、GCC TSan 目标集、E2E 与停机验证通过。
- 500 在线连接、250 个房间、600 秒、299500 次心跳，业务错误 0。
- 排除 `.git`、`out`、`build`、`.runtime`、缓存和旧数据卷后的 clean-copy 验收通过。
- Compose 停止保留数据后重新启动，结算事实仍保留且新 smoke 可继续写入。

## v1.0 收口时修复

- 网关不再与核心容器共用网络命名空间，避免 Docker Desktop 重启时网关抢跑退出；宿主机仍只暴露 `127.0.0.1:49110/49120`。
- 正式镜像不再默认强制 `--demo-mode`，真实 MySQL/Redis Compose 配置可以绑定容器内网地址。
- Demo 启动脚本显式准备容器用户可写的 `.runtime`，干净 Linux 目录可生成 Robot 报告。
- Dockerfile 不再依赖远程 Dockerfile syntax frontend，降低 Docker Hub 瞬时网络失败面。

## 发布边界

本地仓库可固定为 `v1.0.0`。上传公开 GitHub、创建远端 Release、配置仓库可见性和账号权限不属于源码构建动作，需由项目所有者后续明确执行。项目仍是单机模块化服务端工程，不应描述为生产级、高可用或公网游戏平台。

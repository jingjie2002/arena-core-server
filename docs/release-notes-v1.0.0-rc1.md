# 竞域 1.0.0-rc1 发布候选说明

## 已完成

- C++17/Boost.Asio/Protobuf TCP 会话、心跳、重连与幂等请求。
- 1v1 匹配、一次性 Join Token、RoomActor、服务端确定性结果。
- MySQL 事务事实、Redis 排行、Lua 规则、dirty 修复。
- 本地 Web 观察台、可点击讲解 Demo、真实 Robot 场景与重放。
- GCC/Clang、ASan+UBSan、目标 TSan、真实数据集成、停机和 500/250/600 秒稳定性证据。
- Docker Compose、环境 doctor、自动迁移、smoke 与安全停止入口。

## 发布前剩余门禁

- 在不复用当前 `out/`、`.runtime/` 和数据卷的干净副本完成 Compose 与原生构建复现。
- 由项目所有者确认 README 实际可照做。
- 决定许可证、公开仓库地址、`v1.0.0` 标签和 GitHub Release；未经授权不公开。

## 已知限制

见 `docs/known-limitations.md`。候选版本不能描述为生产级、高可用或已公开发布。

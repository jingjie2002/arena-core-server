# M0 交接：基线与规格冻结

- 日期：2026-08-11
- 结论：M0 工程规格已冻结，可以直接进入 M1。

## 非技术摘要

这个项目将做成一个“看得见完整流程、也经得住追问”的 C++ 联机服务端。玩家连接后会被匹配成两人房间，双方准备，服务器自己产生确定性比赛结果，将最终分数可靠写入 MySQL，再把排行榜更新到 Redis。即使 Redis 临时失败，结果也不会丢，后台能从 MySQL 修复排行榜。

开发采用单进程模块化架构，重点证明网络并发、状态机、幂等、事务和故障恢复。不会加入游戏画面、战斗或微服务等范围外内容。

## 已确定的关键选择

- TCP 使用 4 字节大端长度 + Protobuf，最大 64 KiB。
- Hello 5 秒、心跳 10 秒、无活动超时 30 秒、重连窗口 15 秒。
- v1 只做 `duel` 1v1；MMR 初始允许差 100，每等待 1 秒扩大 25，最大 1000。
- 房间加入凭证绑定 match/room/server/player，30 秒或开局时失效。
- 客户端不能提交结果；结果由服务器内部规则生成。
- MySQL 是结算事实源，Redis 是队列和排行榜缓存。
- Session/Match/Room 分别拥有串行执行域，阻塞存储进入有界线程池。

## 文档入口

- 需求基线：`docs/specs/m0-v1-requirements.md`
- 协议：`docs/specs/protocol-v1.md`
- 状态机：`docs/specs/state-machines-v1.md`
- 错误码：`docs/specs/error-catalog-v1.md`
- 数据模型：`docs/specs/data-model-v1.md`
- 并发所有权：`docs/specs/thread-ownership-v1.md`
- 决策记录：`docs/adr/0001-modular-monolith.md` 至 `0003-mysql-source-of-truth.md`
- 需求—测试矩阵：`docs/quality/requirements-test-matrix.md`

## 本阶段没有做

- 没有创建 C++ 源码、CMake、数据库迁移或运行环境。
- 没有修改 Go CoreRank。
- 没有把缺少 DSN 而跳过的 MySQL 测试声称为通过。

## 用户介入

本节点不需要用户操作。按已确认的三节点规则，开发将自动推进，下一次计划内人工介入发生在 M6 可视化面板完成后；届时只需要按步骤看页面并点击验证。

# 已知限制

- v1 是单机模块化单体，不包含多进程分片、集群选主、跨机房容灾、Redis Cluster 或 Kubernetes。
- 不包含图形客户端、战斗同步、AOI、账号/支付/聊天/公会/反作弊；观察台不是游戏画面。
- Observer 与 Lab 只设计为本地 loopback。没有 TLS、账号鉴权、RBAC 或公网部署方案；不能直接改成远程多人共用测试平台。
- 默认 Compose 关闭旧 Demo 写接口并保持 Observer 只读；只有独立 Lab profile 可以创建受控 Run。不得把核心、可信结果、Toxiproxy 或数据依赖端口直接发布到公网。
- Compose 中的演示密码是公开的本机默认值，只适用于 Docker 私有网络；不能用于生产。
- MySQL 是事实源、Redis 是可修缓存，因此故障窗口内排行可以暂时陈旧；dirty 修复保证最终追上，不提供跨依赖的分布式强一致事务。
- 进程重启后 Session、匹配队列和活跃房间不恢复；已经提交的 MySQL 成绩和 Redis/dirty 修复数据可恢复。
- 运行 ID 使用启动时间与系统熵作为顺序种子，目标是避免本地重启时与持久化主键碰撞，不是跨多进程的全局 ID 服务。
- 500/250/600 秒是单机参考稳定性场景，不是容量上限、持续房间吞吐或公网延迟承诺。
- Lab 同一时间只运行一个场景，不提供并行 Run、跨 Run 对比、排队调度或多租户隔离。
- Lab 参数是安全业务白名单，不是任意服务热配置；不能改 host/port/token、直接编辑数据库、运行 SQL/Shell/Lua 或上传 Protobuf。
- Redis 故障页面化只覆盖单代理路径；MySQL 故障页面化、进程 kill、网络分区矩阵和多依赖组合故障留给后续版本。
- Toxiproxy 场景证明的是本地 Compose 中的网络失败语义，不等价于云网络、Redis Cluster 或跨机房故障演练。
- GCC TSan 跑缩小并发目标集，并受 Boost.Asio fence 工具链提示限制；完整 500 连接场景由普通严格构建验证。
- 当前环境没有 `clang-format`，因此没有声称自动格式门禁通过；GCC/Clang `-Werror`、Shell/JSON 语法与尾随空白检查已经执行。
- Windows MSVC preset 只保留契约入口；项目主完成环境和正式验证环境是 WSL2 Ubuntu。
- Docker 28.5.1 会提示 `COMPOSE_BAKE=false` 将来弃用；当前开关用于规避 Windows 非 ASCII 工作区与 Bake 的兼容问题，升级 Docker 后需重新验证并移除。
- 本地工程保留 `1.0.0` / `1.0.1` 历史基线；`1.1.0` 仍位于实验分支。源码尚未上传公开 GitHub，也没有 v1.1 标签，因此不能表述为“v1.1 已公开发布”。

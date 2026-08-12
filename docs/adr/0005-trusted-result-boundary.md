# ADR-0005：玩家协议与可信比赛结果入口分离

- 状态：Accepted
- 日期：2026-08-12

## 背景

v1.0.1 在 Room Ready 后由进程内确定性结果源结束比赛，适合验证结算，但不能代表第三方游戏的真实接入方式。若把提交胜负加入玩家协议，客户端可以伪造结果；若让 Web Observer 直接调用结算，又会破坏只读和权限边界。

## 决策

1. 玩家继续使用 TCP + Protobuf，只能连接、匹配、进入房间、Ready、接收或查询结果。
2. 战斗服/玩法适配器使用独立 HTTP/JSON 可信结果入口。
3. 可信入口使用单独 listener、Bearer service token 和网络边界，默认关闭。
4. Room Ready 后进入 `AwaitingResult`，本地演示也必须通过 Demo Battle Adapter 调用可信入口。
5. Observer 保持只读；Lab Controller 在后续 M10/M11 通过适配器而不是内部状态修改完成场景。

## 采用 HTTP/JSON 的原因

- 现有项目已经使用 Boost.Beast 和 nlohmann/json，不增加依赖。
- 可信结果是低频服务间命令，不需要玩家 TCP 的长连接与推送模型。
- 独立端口更容易实施网络隔离、凭证和未来反向代理 TLS。
- 不引入 gRPC、服务发现或微服务框架，符合 C++17 模块化单体边界。

## 后果

- 增加一个可选 HTTP listener、配置块和内部 API 测试。
- Room 状态机、Robot E2E 和结算测试需要改为显式提交结果。
- 本地/Compose 演示必须提供 service token 环境变量，但不能把真实凭证提交到仓库。
- 公网暴露、远程 RBAC 和 mTLS 不属于 v1.1；非 loopback 只能在明确可信的私有代理边界启用。

# v1.1.0 本地候选版本说明

发布日期状态：未公开发布。当前代码仅位于 `codex/v1.1-scenario-lab`，尚未合并 `main`、创建 `v1.1.0` 标签或推送 GitHub。

## 版本定位

v1.1 把项目从“后台能力 + 教学式三按钮页面”收口为两个明确产品面：

- 可被游戏客户端与可信战斗模块接入的 1v1 对局编排、结算和排行服务；
- 只在本机启动、能改变安全参数并驱动真实协议链的 Scenario Lab。

## 新增与调整

- Room 在双方 Ready 后等待可信战斗结果，普通客户端不能声明胜负。
- 独立 Bearer HTTP 支持结果提交/查询、完全相同结果幂等和冲突拒绝。
- 玩家收到已提交结果 push，并可在断线后按 match 查询 MySQL 持久事实。
- 新增 `arena-core-lab`、版本化 Scenario Schema、单活动 Run 状态机、事件/断言/业务/清理报告、重放、取消和超时。
- 新增完整对局、断线恢复、重复玩家请求、重复可信结果、Redis 故障恢复五类可调 P0 场景。
- Redis 故障通过固定版本 Toxiproxy 作用于核心服务真实依赖路径；成功/失败/取消/超时均执行 reset。
- 新增四页本机 Lab UI；所有运行结果只来自 API，不内置假成功。
- 默认 Observer 移除旧三按钮并关闭 Demo 写模式，保持只读。
- Compose 增加可选 `lab` profile；宿主机仍只暴露 loopback 页面/TCP，数据、可信结果和故障控制端口不发布。
- 发布脚本改为精确 detached commit、动态 CTest 数量、默认与 Lab 两种隔离验收。

## 兼容性

- 玩家 Protobuf 协议仍为 v1，M9 新增的可信结果通知/查询保持向后兼容。
- `scripts/release/m8-*.sh` 文件名为兼容旧入口保留，但内部报告和门禁口径已升级为 M12/v1.1。
- 默认 `./scripts/demo-up.sh` 不再启用 Observer Demo 写接口；需要可变测试必须显式使用 `--lab`。

## 明确未包含

- 图形游戏、战斗同步、AOI、账号/支付/聊天/公会/反作弊；
- 远程多人 Lab、TLS、登录、RBAC、审计平台；
- MySQL 故障页面化、并行 Run/队列、跨 Run 对比；
- 集群、高可用、Kubernetes 或公网部署。

## 验证状态

v1.1 候选版本已完成双编译器各 119 项、真实 MySQL/Redis 12 项、可信结果与 Robot E2E、ASan+UBSan 119 项、TSan 103 项目标集、五场景 Compose Lab、桌面/390px 浏览器和 500 连接/250 房间/600 秒稳定性验证。最终源码提交还会由精确 detached Git clone 重建并验证默认/Lab 两种 Compose；完整口径见 `docs/operations/M12-final-verification-2026-08-17.md`。

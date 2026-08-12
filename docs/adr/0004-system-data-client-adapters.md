# ADR-0004：系统包主线使用薄适配器封装数据客户端

- 状态：Accepted
- 日期：2026-08-11

## 背景

M1 的 WSL/CI 原生构建主线使用 Ubuntu 24.04 系统包。该环境直接提供 MySQL Connector/C++、hiredis 和 Lua 5.4，但不提供 redis-plus-plus 与 sol2 的稳定二进制开发包。如果为了两个包装层在原生构建阶段临时下载或提交第三方源码，会增加 fresh-clone 的网络漂移、许可证材料和构建复杂度。

## 决策

- MySQL 继续使用计划锁定的 MySQL Connector/C++。
- Redis 使用 hiredis，并在 `HiredisRankCache` 内封装连接、RESP 结果检查、原子 Lua 更新和稳定排序。
- 嵌入式 Lua 使用 Lua 5.4 C API，并在 `LuaRuleEngine` 内封装 RAII、版本探针、无标准库沙箱和指令预算。
- vcpkg 的默认 M5 feature 与实际链接库保持一致；上层 `RankCache`、`RuleEngine` 接口不暴露第三方 API。

## 后果

- 原生 WSL/CI 只依赖 Ubuntu 可重复安装的包，避免配置阶段联网抓取未锁定源码。
- 业务层不感知 hiredis 或 Lua C API，未来若切换包装库只替换 adapter。
- 项目承担少量薄适配代码，但这些代码已由故障、边界和真实集成测试覆盖。

## 未采用方案

- 在 CMake configure 时 FetchContent redis-plus-plus/sol2：构建依赖网络状态，不符合当前 fresh-clone 收敛目标。
- 直接把第三方源码复制进仓库：增加供应链与许可证维护面。

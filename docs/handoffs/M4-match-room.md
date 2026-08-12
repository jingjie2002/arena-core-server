# M4 交接：1v1 匹配与 Room

- 日期：2026-08-11
- 结论：两名真实 TCP 客户端已能完成排队、匹配、凭证入房、准备、确定性结果和房间关闭。

## 已实现

- 单一 MatchCoordinator strand 管理票据索引、候选选择、超时、取消和房间分配提交。
- `duel` 初始 MMR 窗口 100，每秒扩大 25，最大 1000；旧票据优先，候选按分差和 ticket id 稳定选择。
- 重复排队拒绝；票据取消幂等；超时会主动推送 Expired。
- 容量为零或房间分配不可用时，两张票据保持 Queued，不出现一半 Matched。
- 每个 RoomActor 有独立 strand；加入凭证绑定 match/room/server/player，保存摘要而非明文，过期或开局后失效。
- outsider、窃取/篡改/过期 token、未入房 ready 均被拒绝。
- 双方 ready 后用 `match_id + 玩家集合 + demo-result-v1` 生成跨运行稳定的种子、胜负和结果指纹。
- 房间成员关系独立于 socket；玩家断线恢复后可继续 ready 并完成房间。
- Redis Lua v1 原子脚本覆盖 enqueue、duplicate guard、cancel 和双票据 commit_match。

## 自动验证

- M4 游戏标签 7/7：GCC、Clang 均通过。
- M4 完整回归基线：两种编译器各 55 项（M4 最终新增重连用例后）测试入口；此前 53/53 全套通过，新增 2 项游戏验证也在双编译器通过。
- `scripts/dev/m4-redis-lua-smoke.sh` 在临时 Redis 7、端口 6391、禁用持久化下通过；测试后端口确认关闭。

## 当前边界

- MatchCoordinator 当前使用内存队列作为运行时实现；Redis Lua 原子契约已真实执行，M5 将接入 C++ Redis adapter 和恢复策略。
- 房间结束当前由内存 settlement 占位立即提交；M5 会把相同冻结结果接入 MySQL 事务，只有事务成功才 Closed。
- RoomActor 只生成确定性演示结果，不是战斗系统。

## 用户介入

不需要。继续自动进入 M5。

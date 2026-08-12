# M2 交接：领域模型与状态机

- 日期：2026-08-11
- 结论：Session、MatchTicket 和 Room 的纯内存状态机及边界测试已完成。

## 已实现

- 可注入 `Clock`、不需真实等待的 `FakeClock` 和确定性顺序 ID 生成器。
- 玩家/资源 ID 的字符集与长度校验。
- Session：Hello 截止、Active 活动刷新、30 秒 idle、断线、15 秒恢复、generation 轮换和幂等关闭。
- MatchTicket：duel/MMR/等待范围校验，Queued 到 Matched/Cancelled/Expired 的不可逆转换。
- Room：两名指定玩家 join/ready、单次 start、结果冻结、结算关闭和开局前 abort。
- 通用 `Result<T>` 和状态名称输出，供后续网络、观测和测试复用。

## 验证结果

- GCC 13.3：31/31 通过。
- Clang 18.1：31/31 通过。
- 其中 20 个测试覆盖 M2 新增的 ID、FakeClock、Session、票据和 Room 行为。
- 精确边界已覆盖：Hello 第 5 秒、重连第 15 秒、idle 第 30 秒、票据到期时刻、MMR 0/10000、等待 1 秒/10 分钟。

## 当前边界

- 本阶段只验证同步领域语义；strand、socket、timer 和真实并发属于 M3/M4。
- 加入凭证、匹配候选算法和服务端结果源属于 M4。
- MySQL 幂等结算与 Redis dirty 修复属于 M5。

## 用户介入

不需要。继续自动进入 M3。

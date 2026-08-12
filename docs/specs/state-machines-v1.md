# 竞域 v1 状态机

- 状态：已锁定
- 原则：转换由唯一所有者串行执行；终态转换幂等；非法转换返回稳定错误且不产生副作用。

## 1. Session

```text
Connected --Hello成功--> Active
Active --非主动断线--> Reconnecting
Reconnecting --有效恢复Hello--> Active
Connected --Hello超时/协议错误--> Closed
Active --主动退出/致命错误/服务关闭--> Closed
Reconnecting --恢复窗口过期/服务关闭--> Closed
```

- Connected 只允许 Hello。
- Active 更新最后活动时间并接受业务请求。
- Reconnecting 保留玩家、票据和房间关系，但不接受旧连接命令。
- Closed 是终态；`Stop()`、超时回调和 socket 错误可并发触发，但清理只执行一次。

## 2. MatchTicket

```text
Queued --> Matched
Queued --> Cancelled
Queued --> Expired
```

- 新票据只能从不存在进入 Queued。
- 同一玩家同一模式只允许一个 Queued 票据。
- 房间分配失败不先提交 Matched；票据仍有效时保持或重新进入原有确定顺序。
- Matched、Cancelled、Expired 都是终态，迟到操作返回当前状态而不逆转。

## 3. Room

```text
Created --> Waiting --> Ready --> Running --> Settling --> Closed
Created/Waiting/Ready --> Aborted
```

- Created：分配已建立，等待成员加入。
- Waiting：至少一名成员已加入，但尚未双方 ready。
- Ready：两名成员均已加入并 ready，马上由所有者推进。
- Running：服务端执行确定性比赛规则；客户端不能写入结果。
- Settling：内部结果已冻结，正在提交 MySQL 事务。
- Closed：事实已提交；Redis 排行榜可能已更新，或存在可修复的 dirty 记录。
- Aborted：只允许在事实提交前进入，释放容量并按票据策略回队或结束。

Settling 后不得因客户端离线回到前态。MySQL 暂时失败时保持可重试的冻结结果，不重新生成结果。

## 4. Settlement

```text
Pending --> Committed --> CacheSynced
                      \-> CacheDirty --> CacheSynced
Pending --事务失败--> Pending
Pending --指纹冲突--> Conflict
```

- `match_id` 唯一，结果指纹覆盖规则版本、玩家集合、胜负与分数变化。
- 同 match、同指纹重复提交读取已提交结果，视为幂等成功。
- 同 match、不同指纹进入 Conflict，禁止覆盖。
- MySQL 事务失败不写 Redis；事务成功后 Redis 失败仅产生 CacheDirty。

## 5. 服务生命周期

```text
Starting --> Running --> Draining --> Stopped
Starting --关键依赖失败--> Failed
Running --不可恢复故障--> Failed
```

Draining 顺序固定：停止新连接、停止新排队与撮合、通知并排空或终止房间、完成在途结算、停止修复任务、关闭 Redis/MySQL、停止 io_context。

## 6. 状态不变量

- 一个活动 player 只绑定一个可写 Session。
- 一个 Queued ticket 只属于一个 mode 队列。
- 一个 match 恰有两个不同 player，且每个 player 在该 match 只有一条成员记录。
- Room 的 player 集合与 match 成员集合一致。
- 每个 match 最多一条已提交 settlement。
- Closed room 的冻结结果不会变化。
- Redis 排行值最终可由 MySQL 恢复，不作为判定结算是否成功的依据。

# 竞域 v1 并发与线程所有权

- 状态：已锁定
- 目标：通过显式所有权避免业务对象上的粗粒度全局锁。

## 1. 执行器

| 执行域 | 默认规模 | 所有权与限制 |
|---|---:|---|
| Network `io_context` | `max(2, min(4, hardware_concurrency))`，未知时 2 | 只执行 socket、timer 和短回调。 |
| Session strand | 每个 Session 一个 | 串行处理该连接的读写、超时、请求幂等和 Stop。 |
| MatchCoordinator strand | 全局一个 | 串行修改匹配票据索引、候选选择和房间分配提交。 |
| Room strand | 每个 Room 一个 | 串行处理 join、ready、断线、结果冻结和状态推进。 |
| StorageExecutor | 4 线程、队列上限 512 | 执行阻塞 MySQL/Redis 客户端调用；队满返回 SERVER_BUSY。 |
| Observer strand | 一个 | 合并业务事件并发布不可变快照。 |

线程数和队列上限可配置，但默认值用于测试与演示。

## 2. 跨域调用规则

- 调用方提交包含不可变输入的任务，禁止把业务对象裸指针交给存储线程。
- 阻塞操作完成后，将值对象结果 post 回原 Session、Match 或 Room strand，再修改状态。
- Match 创建 Room 时先预留容量；只有 RoomRegistry 确认后才原子提交 ticket 状态。
- Room 结算期间保存冻结结果和操作代号；迟到回调必须校验代号及当前状态。
- Observer 只接收事件副本/快照，不反向取得可变业务引用。

## 3. 生命周期规则

- 异步对象以 `shared_ptr` 持有自身到回调结束；回调不得捕获悬空引用。
- timer、socket error、主动关闭和服务排空最终都调用幂等 `Stop(reason)`。
- Stop 首次执行时取消计时器、关闭 socket、撤销会话索引并通知所属业务域；重复调用只返回既有结果。
- 回调必须检查 generation/operation id，旧连接和旧 timer 不得影响恢复后的新连接。

## 4. 背压

- 每个 Session 限制待发送字节数和消息数；超限先拒绝新业务，持续超限则关闭慢客户端。
- StorageExecutor 队列有界，不允许无限堆积。
- Match 每轮处理有预算，处理后主动让出执行权，避免阻塞网络循环。
- 观测客户端只能消费采样/合并事件，不得拖慢业务生产者。

## 5. 关闭顺序

1. 接收退出信号并将 Server 状态改为 Draining。
2. 停止 accept，新请求返回 SERVER_SHUTTING_DOWN。
3. MatchCoordinator 停止创建新配对并收敛活动票据。
4. Room 完成已冻结结算或在提交前安全 abort。
5. 等待 StorageExecutor 在途任务到有界截止时间。
6. 停止缓存修复器和 Observer，关闭连接池。
7. 停止 io_context，join 所有线程。

超时退出必须记录仍未收敛的操作标识，不能无期限等待。

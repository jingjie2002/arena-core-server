# ADR-0002：strand 所有权与有界存储执行器

- 状态：Accepted
- 日期：2026-08-11

## 背景

TCP 回调、超时、断线、撮合和数据库完成回调可能并发触达同一对象。全局互斥锁易造成锁顺序问题；在网络线程执行同步 MySQL/Redis 又会放大延迟。

## 决策

- 每个 Session 和 Room 拥有独立 Asio strand；MatchCoordinator 使用单一 strand。
- 所有状态转换只在其所有者 strand 发生。
- 同步数据库和缓存调用进入 4 线程、512 有界队列的 StorageExecutor；结果以值对象 post 回来源 strand。
- 生命周期使用 `shared_ptr`、generation/operation id 和幂等 Stop 管理。
- Observer 只消费不可变快照。

## 后果

- 优点：常规状态转换无需对象级锁；并发边界可测试；慢存储不会直接阻塞 socket。
- 代价：跨 strand 操作变成异步状态机，需要防止迟到回调。
- 缓解：冻结输入、操作代号、有界队列、明确的关闭顺序和竞态测试。

## 未采用方案

- 全局 mutex：容易形成性能瓶颈和隐含锁顺序。
- 每对象独立 OS 线程：资源成本高，缺乏必要性。
- 完整 actor 框架：引入额外依赖和认知负担，超出 v1。

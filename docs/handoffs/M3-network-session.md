# M3 交接：TCP、Protobuf 与 Session

- 日期：2026-08-11
- 结论：真实 TCP Server/Robot 已完成 Hello、心跳、超时、幂等请求和断线重连闭环。

## 已实现

- 4 字节大端长度帧，64 KiB 上限，Protobuf v1 Envelope 和完整 v1 消息 schema。
- Asio callback 异步 accept/read/write；每个 Session 有 strand、发送队列和 deadline timer。
- 5 秒 Hello、30 秒无活动、15 秒重连默认值，测试可缩短但语义相同。
- 生产令牌由 128 位 `random_device` 输入编码；测试使用确定性生成器，日志不输出令牌。
- 最近 256 个请求、60 秒结果缓存；同 ID 同内容重放原响应，不同内容返回冲突并断开。
- 恢复时 session/reconnect token 都轮换，generation 防止旧连接回调撤销新会话。
- Server 处理 SIGINT/SIGTERM、停止 accept，并幂等关闭活动 Session。
- Robot 可真实连接、Hello 并发送指定次数心跳。

## 自动验证

- GCC 13.3：47/47 测试通过。
- Clang 18.1：47/47 测试通过。
- 其中协议 5 项、网络/Session 11 项；覆盖半包、粘包、超长、畸形、版本错误、Hello 超时、idle、重复请求、冲突、恢复、过期、旧 generation 和重复 Stop。
- GCC 与 Clang 产物分别执行 `scripts/dev/m3-smoke.sh`，真实 Robot Hello + 3 次心跳通过。

## 运行 smoke

```bash
bash scripts/dev/m3-smoke.sh out/build/linux-gcc
```

## 当前边界

- QueueMatch、Room 和 Rank 消息已在 schema 中固定，但业务 handler 在对应里程碑前返回可重试错误。
- SessionRegistry 的短临界区使用 mutex 保护跨 strand 索引；每个 Session 的 socket/timer/队列状态仍只由自身 strand 修改。
- 完整优雅停机还需 M4/M5 的匹配、房间和存储排空步骤，并在 M7 做 Sanitizer/压力验证。

## 用户介入

不需要。继续自动进入 M4。

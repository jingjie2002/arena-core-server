# 竞域 v1 错误目录

- 状态：已锁定
- 传输：`ErrorResponse { code, message, retryable, request_id }`
- 规则：`message` 可供人阅读但不作为程序判断依据；不得暴露密钥、SQL、堆栈或内部路径。

| code | retryable | 关闭连接 | 含义 |
|---|---:|---:|---|
| CONFIG_FILE_NOT_FOUND | 否 | 否 | 指定的公开配置文件不存在。 |
| CONFIG_PARSE_FAILED | 否 | 否 | 配置 JSON 无法安全解析。 |
| CONFIG_VALIDATION_FAILED | 否 | 否 | 配置值违反范围或组合约束。 |
| PROTOCOL_UNSUPPORTED_VERSION | 否 | 是 | 协议版本不受支持。 |
| PROTOCOL_UNKNOWN_MESSAGE | 否 | 是 | 消息类型未知。 |
| PROTOCOL_FRAME_TOO_LARGE | 否 | 是 | 帧长度为零或超过上限。 |
| PROTOCOL_DECODE_FAILED | 否 | 是 | Envelope 或载荷无法解析。 |
| PROTOCOL_REQUEST_ID_CONFLICT | 否 | 是 | 同一 request id 使用了不同内容。 |
| SESSION_HELLO_REQUIRED | 否 | 是 | Hello 前发送了其他消息。 |
| SESSION_PLAYER_ID_INVALID | 否 | 否 | 玩家标识格式非法。 |
| SESSION_TOKEN_INVALID | 否 | 是 | 会话令牌缺失或无效。 |
| SESSION_RECONNECT_EXPIRED | 是 | 否 | 重连令牌过期，客户端可重新登录。 |
| SESSION_NOT_ACTIVE | 是 | 否 | 会话当前不能执行该业务。 |
| MATCH_MMR_INVALID | 否 | 否 | MMR 超出 0 至 10000。 |
| MATCH_MODE_UNSUPPORTED | 否 | 否 | v1 只支持 duel。 |
| MATCH_WAIT_INVALID | 否 | 否 | 等待时长不在允许区间。 |
| MATCH_ALREADY_QUEUED | 否 | 否 | 玩家已有排队票据。 |
| MATCH_TICKET_NOT_FOUND | 否 | 否 | 票据不存在或不属于当前玩家。 |
| MATCH_TICKET_NOT_QUEUED | 否 | 否 | 票据已进入终态。 |
| MATCH_TICKET_EXPIRED | 是 | 否 | 票据已过期，可重新排队。 |
| MATCH_CAPACITY_UNAVAILABLE | 是 | 否 | 房间暂时没有容量。 |
| ROOM_NOT_FOUND | 否 | 否 | 房间不存在或不可见。 |
| ROOM_JOIN_TOKEN_INVALID | 否 | 否 | 加入凭证无效、过期或绑定不一致。 |
| ROOM_PLAYER_NOT_ASSIGNED | 否 | 否 | 玩家不属于该房间。 |
| ROOM_ALREADY_STARTED | 否 | 否 | 房间已进入 AwaitingResult 或之后状态。 |
| ROOM_NOT_JOINED | 否 | 否 | 玩家尚未成功加入。 |
| ROOM_INVALID_STATE | 否 | 否 | 当前状态不允许该操作。 |
| SETTLEMENT_NOT_ALLOWED | 否 | 否 | 外部来源或错误房间状态试图结算。 |
| SETTLEMENT_CONFLICT | 否 | 否 | 同一 match 出现不同结果指纹。 |
| SETTLEMENT_NOT_FOUND | 否 | 否 | result 与活动/已提交 match 均不存在。 |
| SETTLEMENT_ACCESS_DENIED | 否 | 否 | 非比赛参与者试图查询该比赛结果。 |
| SETTLEMENT_PLAYER_SET_INVALID | 否 | 否 | 结算玩家与比赛成员不一致。 |
| SETTLEMENT_STORAGE_FAILED | 是 | 否 | 结算事务未提交，可安全重试。 |
| RANK_PLAYER_NOT_FOUND | 否 | 否 | 玩家尚无可查询分数。 |
| RANK_QUERY_INVALID | 否 | 否 | 榜单查询参数非法。 |
| RANK_CACHE_UNAVAILABLE | 是 | 否 | 排行缓存暂时不可用。 |
| RULE_LOAD_FAILED | 否 | 否 | 规则脚本加载或校验失败。 |
| RULE_EXECUTION_FAILED | 是 | 否 | 规则执行失败且未产生副作用。 |
| RULE_VERSION_MISMATCH | 否 | 否 | 请求或已冻结结果引用了错误规则版本。 |
| DEPENDENCY_REDIS_UNAVAILABLE | 是 | 否 | Redis 暂时不可用。 |
| DEPENDENCY_MYSQL_UNAVAILABLE | 是 | 否 | MySQL 暂时不可用。 |
| SERVER_BUSY | 是 | 否 | 有界工作队列已满，服务主动背压。 |
| SERVER_SHUTTING_DOWN | 是 | 否 | 服务正在排空，不接受新工作。 |
| INTERNAL_ERROR | 是 | 否 | 未分类内部错误，详情只进入脱敏日志。 |

## 连接策略

- 协议边界已失去同步或会话令牌遭破坏时关闭连接。
- 合法业务拒绝默认保持连接，便于客户端纠正或重试。
- 依赖故障不自动断开所有客户端；由具体请求返回 retryable 错误，并由健康状态暴露故障。

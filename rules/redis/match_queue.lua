-- arena-core-server match queue atomic contract, version 1.
-- KEYS[1] queue ZSET, KEYS[2] player-active key, KEYS[3] ticket HASH.
-- ARGV[1] action: enqueue | cancel | commit_match
-- Remaining arguments are action-specific and all keys must share one Redis Cluster hash tag.

local action = ARGV[1]

if action == "enqueue" then
  if redis.call("EXISTS", KEYS[2]) == 1 then
    return {err = "MATCH_ALREADY_QUEUED"}
  end
  local ticket_id = ARGV[2]
  local enqueue_ms = ARGV[3]
  local ttl_ms = ARGV[4]
  redis.call("HSET", KEYS[3], "state", "Queued", "ticket_id", ticket_id, "enqueue_ms", enqueue_ms)
  redis.call("PEXPIRE", KEYS[3], ttl_ms)
  redis.call("PSETEX", KEYS[2], ttl_ms, ticket_id)
  redis.call("ZADD", KEYS[1], enqueue_ms, ticket_id)
  return {"Queued", ticket_id}
end

if action == "cancel" then
  local ticket_id = ARGV[2]
  if redis.call("HGET", KEYS[3], "state") ~= "Queued" then
    return {err = "MATCH_TICKET_NOT_QUEUED"}
  end
  redis.call("HSET", KEYS[3], "state", "Cancelled")
  redis.call("ZREM", KEYS[1], ticket_id)
  if redis.call("GET", KEYS[2]) == ticket_id then
    redis.call("DEL", KEYS[2])
  end
  return {"Cancelled", ticket_id}
end

if action == "commit_match" then
  local first_ticket = ARGV[2]
  local second_ticket = ARGV[3]
  local first_state_key = KEYS[3]
  local second_state_key = KEYS[4]
  if redis.call("HGET", first_state_key, "state") ~= "Queued" or
     redis.call("HGET", second_state_key, "state") ~= "Queued" then
    return {err = "MATCH_TICKET_NOT_QUEUED"}
  end
  redis.call("HSET", first_state_key, "state", "Matched", "match_id", ARGV[4])
  redis.call("HSET", second_state_key, "state", "Matched", "match_id", ARGV[4])
  redis.call("ZREM", KEYS[1], first_ticket, second_ticket)
  redis.call("DEL", KEYS[2], KEYS[5])
  return {"Matched", ARGV[4]}
end

return {err = "UNSUPPORTED_ACTION"}

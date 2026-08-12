#!/usr/bin/env bash
set -euo pipefail

port="${ARENA_REDIS_TEST_PORT:-6391}"
pid_file="/tmp/arena-core-redis-${port}.pid"
log_file="/tmp/arena-core-redis-${port}.log"

redis-server \
  --bind 127.0.0.1 \
  --port "${port}" \
  --daemonize yes \
  --save "" \
  --appendonly no \
  --dir /tmp \
  --pidfile "${pid_file}" \
  --logfile "${log_file}"

cleanup() {
  redis-cli -p "${port}" shutdown nosave >/dev/null 2>&1 || true
}
trap cleanup EXIT

for attempt in $(seq 1 30); do
  if redis-cli -p "${port}" ping 2>/dev/null | grep -q PONG; then
    break
  fi
  sleep 0.1
done

redis_cli=(redis-cli --raw -p "${port}")
script="rules/redis/match_queue.lua"
queue="arena:test:{duel}:queue"

"${redis_cli[@]}" FLUSHDB >/dev/null

enqueue_one=$("${redis_cli[@]}" --eval "${script}" "${queue}" \
  "arena:test:{duel}:player:p1" "arena:test:{duel}:ticket:t1" , enqueue t1 1000 30000)
grep -q "Queued" <<<"${enqueue_one}"

duplicate=$("${redis_cli[@]}" --eval "${script}" "${queue}" \
  "arena:test:{duel}:player:p1" "arena:test:{duel}:ticket:t1b" , enqueue t1b 1001 30000 2>&1 || true)
grep -q "MATCH_ALREADY_QUEUED" <<<"${duplicate}"

cancelled=$("${redis_cli[@]}" --eval "${script}" "${queue}" \
  "arena:test:{duel}:player:p1" "arena:test:{duel}:ticket:t1" , cancel t1)
grep -q "Cancelled" <<<"${cancelled}"

"${redis_cli[@]}" --eval "${script}" "${queue}" \
  "arena:test:{duel}:player:p1" "arena:test:{duel}:ticket:t1" , enqueue t1 2000 30000 >/dev/null
"${redis_cli[@]}" --eval "${script}" "${queue}" \
  "arena:test:{duel}:player:p2" "arena:test:{duel}:ticket:t2" , enqueue t2 2001 30000 >/dev/null

matched=$("${redis_cli[@]}" --eval "${script}" "${queue}" \
  "arena:test:{duel}:player:p1" "arena:test:{duel}:ticket:t1" \
  "arena:test:{duel}:ticket:t2" "arena:test:{duel}:player:p2" , commit_match t1 t2 match-1)
grep -q "Matched" <<<"${matched}"
test "$("${redis_cli[@]}" ZCARD "${queue}")" = "0"
test "$("${redis_cli[@]}" HGET "arena:test:{duel}:ticket:t1" state)" = "Matched"
test "$("${redis_cli[@]}" HGET "arena:test:{duel}:ticket:t2" state)" = "Matched"

echo "Redis Lua queue smoke passed on port ${port}"

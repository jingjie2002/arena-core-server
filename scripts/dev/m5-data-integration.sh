#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="${1:-${project_root}/out/build/linux-gcc}"
report_dir="${build_dir}/qa/m7-data"
mysql_port="${ARENA_M5_MYSQL_PORT:-33307}"
redis_port="${ARENA_M5_REDIS_PORT:-6392}"
runtime_dir="$(mktemp -d -t arena-m5-data-XXXXXX)"
mysql_dir="${runtime_dir}/mysql"
mysql_socket="${mysql_dir}/mysql.sock"
mysql_log="${mysql_dir}/mysql.log"
redis_pid="${runtime_dir}/redis.pid"
server_pid=""

mkdir -p "${report_dir}"

cleanup() {
  if [[ -n "${server_pid}" ]] && kill -0 "${server_pid}" >/dev/null 2>&1; then
    kill -TERM "${server_pid}" >/dev/null 2>&1 || true
    wait "${server_pid}" >/dev/null 2>&1 || true
  fi
  if [[ -S "${mysql_socket}" ]]; then
    mysqladmin --no-defaults --socket="${mysql_socket}" -uroot shutdown >/dev/null 2>&1 || true
  fi
  redis-cli -h 127.0.0.1 -p "${redis_port}" shutdown nosave >/dev/null 2>&1 || true
  rm -rf -- "${runtime_dir}"
}
trap cleanup EXIT INT TERM

mkdir -p "${mysql_dir}"
mysql_user_args=()
if [[ "$(id -u)" -eq 0 ]]; then
  chmod 0755 "${runtime_dir}"
  chown mysql:mysql "${mysql_dir}"
  mysql_user_args=(--user=mysql)
fi
mysqld --no-defaults --initialize-insecure "${mysql_user_args[@]}" --datadir="${mysql_dir}" --log-error="${mysql_log}"
mysqld --no-defaults \
  "${mysql_user_args[@]}" \
  --datadir="${mysql_dir}" \
  --socket="${mysql_socket}" \
  --port="${mysql_port}" \
  --bind-address=127.0.0.1 \
  --pid-file="${mysql_dir}/mysql.pid" \
  --log-error="${mysql_log}" \
  --skip-log-bin \
  --mysqlx=0 \
  --daemonize

for _ in $(seq 1 60); do
  if mysqladmin --no-defaults --socket="${mysql_socket}" -uroot ping --silent; then
    break
  fi
  sleep 0.25
done
mysqladmin --no-defaults --socket="${mysql_socket}" -uroot ping --silent

mysql --no-defaults --socket="${mysql_socket}" -uroot <<'SQL'
CREATE DATABASE arena_m5 CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci;
CREATE USER 'arena_test'@'127.0.0.1' IDENTIFIED BY 'arena_test';
GRANT ALL PRIVILEGES ON arena_m5.* TO 'arena_test'@'127.0.0.1';
FLUSH PRIVILEGES;
SQL
mysql --no-defaults --socket="${mysql_socket}" -uroot arena_m5 < "${project_root}/migrations/001_m5_settlement.sql"

redis-server \
  --bind 127.0.0.1 \
  --port "${redis_port}" \
  --save "" \
  --appendonly no \
  --daemonize yes \
  --pidfile "${redis_pid}" \
  --dir "${runtime_dir}"
redis-cli -h 127.0.0.1 -p "${redis_port}" ping | grep -qx PONG

export ARENA_M5_MYSQL_URI="tcp://127.0.0.1:${mysql_port}"
export ARENA_M5_MYSQL_USER="arena_test"
export ARENA_M5_MYSQL_PASSWORD="arena_test"
export ARENA_M5_MYSQL_SCHEMA="arena_m5"
export ARENA_M5_REDIS_HOST="127.0.0.1"
export ARENA_M5_REDIS_PORT="${redis_port}"

set +e
ctest --test-dir "${build_dir}" -L data --output-on-failure
test_status=$?
set -e
if [[ "${test_status}" -ne 0 ]]; then
  mysql --no-defaults --socket="${mysql_socket}" -uroot arena_m5 -e \
    "SELECT leaderboard,player_id,target_score,settlement_id,status,attempts,next_retry_at FROM rank_cache_dirty ORDER BY player_id;"
  exit "${test_status}"
fi

server_config="${runtime_dir}/server.json"
server_log="${runtime_dir}/server.log"
server_port="${ARENA_M5_SERVER_PORT:-49310}"
cat > "${server_config}" <<JSON
{
  "environment": "m5-integration",
  "network": {"bind_address": "127.0.0.1", "tcp_port": ${server_port}, "observer_port": 49320},
  "execution": {"io_threads": 2, "storage_threads": 2, "storage_queue_capacity": 32},
  "logging": {"level": "info", "file": ""},
  "data": {
    "enabled": true,
    "mysql": {"uri": "tcp://127.0.0.1:${mysql_port}", "user": "arena_test", "password_env": "ARENA_MYSQL_PASSWORD", "schema": "arena_m5"},
    "redis": {"host": "127.0.0.1", "port": ${redis_port}, "database": 15, "password_env": "ARENA_REDIS_PASSWORD", "key_prefix": "arena:m5-server:"},
    "rules": {"match_script": "${project_root}/rules/match_rule.lua", "score_script": "${project_root}/rules/score_rule.lua"},
    "repair_interval_ms": 200,
    "repair_batch_size": 16
  }
}
JSON
ARENA_MYSQL_PASSWORD=arena_test "${build_dir}/arena-core-server" --config "${server_config}" >"${server_log}" 2>&1 &
server_pid=$!
for _ in $(seq 1 40); do
  if grep -q "TCP server listening" "${server_log}"; then
    break
  fi
  if ! kill -0 "${server_pid}" >/dev/null 2>&1; then
    cat "${server_log}"
    exit 1
  fi
  sleep 0.1
done
grep -q "TCP server listening" "${server_log}"
"${build_dir}/arena-core-robot" \
  --scenario "${project_root}/scenarios/qa/full-flow-replay.json" \
  --port "${server_port}" \
  --output "${report_dir}/real-data-flow.json"
kill -TERM "${server_pid}"
wait "${server_pid}"
server_pid=""
grep -q "TCP server stopped" "${server_log}"

sample_match_id="$(mysql --no-defaults --socket="${mysql_socket}" -uroot arena_m5 -Nse \
  "SELECT match_id FROM settlements ORDER BY committed_at LIMIT 1;")"
mysql --no-defaults --socket="${mysql_socket}" -uroot arena_m5 -e \
  "EXPLAIN SELECT settlement_id,fingerprint,rule_version FROM settlements WHERE match_id='${sample_match_id}';"
mysql --no-defaults --socket="${mysql_socket}" -uroot arena_m5 -e \
  "EXPLAIN SELECT leaderboard,player_id,target_score,settlement_id,attempts FROM rank_cache_dirty WHERE status='pending' AND next_retry_at<=CURRENT_TIMESTAMP(6) ORDER BY next_retry_at,leaderboard,player_id LIMIT 64;"

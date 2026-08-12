#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
runtime_dir="${project_root}/.runtime"
config_path="${runtime_dir}/arena-core.json"
report_path="${runtime_dir}/smoke-report.json"
tcp_port="${ARENA_DEMO_TCP_PORT:-49110}"
observer_port="${ARENA_DEMO_OBSERVER_PORT:-49120}"

cd "${project_root}"
./scripts/doctor.sh
mkdir -p "${runtime_dir}"
chmod 0777 "${runtime_dir}"

cat > "${config_path}" <<JSON
{
  "environment": "compose-demo",
  "network": {
    "bind_address": "0.0.0.0",
    "tcp_port": 9100,
    "observer_port": 8080,
    "max_frame_bytes": 65536
  },
  "execution": {
    "io_threads": 2,
    "storage_threads": 2,
    "storage_queue_capacity": 128
  },
  "logging": {"level": "info", "file": ""},
  "session": {
    "hello_timeout_ms": 5000,
    "idle_timeout_ms": 30000,
    "reconnect_window_ms": 15000
  },
  "observer": {
    "enabled": true,
    "demo_mode": true,
    "allow_non_loopback_demo": true,
    "static_root": "web/observer",
    "max_events": 2048
  },
  "data": {
    "enabled": true,
    "mysql": {
      "uri": "tcp://mysql:3306",
      "user": "arena",
      "password_env": "ARENA_MYSQL_PASSWORD",
      "schema": "arena"
    },
    "redis": {
      "host": "redis",
      "port": 6379,
      "database": 0,
      "password_env": "ARENA_REDIS_PASSWORD",
      "key_prefix": "arena:demo:",
      "connect_timeout_ms": 1000
    },
    "rules": {
      "match_script": "rules/match_rule.lua",
      "score_script": "rules/score_rule.lua"
    },
    "repair_interval_ms": 500,
    "repair_batch_size": 32
  }
}
JSON

rm -f -- "${report_path}"
docker compose config --quiet
docker compose up --build --detach

ready=0
for _ in $(seq 1 90); do
  if curl --fail --silent "http://127.0.0.1:${observer_port}/api/overview" >/dev/null 2>&1; then
    ready=1
    break
  fi
  if ! docker compose ps --status running --services | grep -qx arena-core; then
    docker compose logs --no-color arena-core gateway mysql redis migrate >&2
    exit 1
  fi
  sleep 1
done
if [[ "${ready}" -ne 1 ]]; then
  docker compose logs --no-color arena-core gateway mysql redis migrate >&2
  echo "观察台在 90 秒内没有就绪" >&2
  exit 1
fi
./scripts/smoke.sh

printf '\n竞域演示已就绪：\n'
printf '  观察台：http://127.0.0.1:%s/\n' "${observer_port}"
printf '  TCP 端口：127.0.0.1:%s\n' "${tcp_port}"
printf '  smoke 报告：%s\n' "${report_path}"
printf '停止但保留数据：./scripts/demo-down.sh\n'

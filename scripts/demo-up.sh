#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
runtime_dir="${project_root}/.runtime"
config_path="${runtime_dir}/arena-core.json"
lab_config_path="${runtime_dir}/arena-lab.json"
toxiproxy_config_path="${runtime_dir}/toxiproxy.json"
report_path="${runtime_dir}/smoke-report.json"
tcp_port="${ARENA_DEMO_TCP_PORT:-49110}"
observer_port="${ARENA_DEMO_OBSERVER_PORT:-49120}"
lab_port="${ARENA_LAB_PORT:-49140}"
lab_mode=0

if [[ "${1:-}" == "--lab" && $# -eq 1 ]]; then
  lab_mode=1
elif [[ $# -ne 0 ]]; then
  echo "usage: ./scripts/demo-up.sh [--lab]" >&2
  exit 2
fi

cd "${project_root}"

# Docker Desktop 4.50 enables Compose Bake delegation by default. Its file
# sharing session header cannot represent every non-ASCII Windows workspace
# path, while the regular Compose builder handles this repository correctly.
# Keep the launch reproducible for the project's Chinese parent directory.
export COMPOSE_BAKE=false

reset_lab_faults() {
  if docker compose --profile lab ps --status running --services 2>/dev/null | grep -qx toxiproxy; then
    if docker compose --profile lab ps --status running --services 2>/dev/null | grep -qx arena-lab; then
      docker compose --profile lab exec -T arena-lab \
        /opt/arena-core/bin/arena-core-lab --config /opt/arena-core/.runtime/arena-lab.json \
        --reset-faults >/dev/null 2>&1 && return
    fi
    # Fall back to recreating the distroless proxy when no Lab control client
    # is available (for example while upgrading an older local image).
    docker compose --profile lab restart toxiproxy >/dev/null 2>&1 || true
  fi
}
on_error() {
  local status=$?
  reset_lab_faults
  exit "${status}"
}
trap on_error ERR INT TERM

./scripts/doctor.sh
mkdir -p "${runtime_dir}"
chmod 0777 "${runtime_dir}"
if [[ "${lab_mode}" -eq 1 ]]; then
  mkdir -p "${runtime_dir}/lab-runs"
  chmod 0777 "${runtime_dir}/lab-runs"
fi

redis_host="redis"
redis_port=6379
observer_demo_mode=false
trusted_result_bind="127.0.0.1"
trusted_result_allow_non_loopback=false
if [[ "${lab_mode}" -eq 1 ]]; then
  redis_host="toxiproxy"
  redis_port=26379
  trusted_result_bind="0.0.0.0"
  trusted_result_allow_non_loopback=true
fi

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
    "demo_mode": ${observer_demo_mode},
    "allow_non_loopback_demo": ${observer_demo_mode},
    "static_root": "web/observer",
    "max_events": 2048
  },
  "trusted_result": {
    "enabled": true,
    "bind_address": "${trusted_result_bind}",
    "port": 9200,
    "allow_non_loopback": ${trusted_result_allow_non_loopback},
    "service_token_env": "ARENA_RESULT_SERVICE_TOKEN",
    "result_wait_timeout_ms": 30000
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
      "host": "${redis_host}",
      "port": ${redis_port},
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

if [[ "${lab_mode}" -eq 1 ]]; then
  git_commit="$(git rev-parse --short=12 HEAD 2>/dev/null || printf unknown)"
  service_config_digest="$(sha256sum "${config_path}" | awk '{print $1}')"
  cat > "${lab_config_path}" <<JSON
{
  "environment": "compose-lab",
  "git_commit": "${git_commit}",
  "service_config_digest": "${service_config_digest}",
  "http": {
    "bind_address": "0.0.0.0",
    "port": 8081,
    "allow_non_loopback": true,
    "static_root": "web/lab",
    "maximum_request_body_bytes": 65536
  },
  "reports": {"directory": ".runtime/lab-runs"},
  "player": {"host": "arena-core", "port": 9100},
  "trusted_result": {
    "host": "arena-core",
    "port": 9200,
    "service_token_env": "ARENA_RESULT_SERVICE_TOKEN"
  },
  "observer": {"host": "arena-core", "port": 8080},
  "session": {"reconnect_window_ms": 15000},
  "toxiproxy": {
    "enabled": true,
    "host": "toxiproxy",
    "port": 8474,
    "proxy_name": "arena_lab_redis",
    "listen": "0.0.0.0:26379",
    "upstream": "redis:6379"
  }
}
JSON
  cat > "${toxiproxy_config_path}" <<JSON
[
  {
    "name": "arena_lab_redis",
    "listen": "0.0.0.0:26379",
    "upstream": "redis:6379",
    "enabled": true,
    "toxics": []
  }
]
JSON
fi

rm -f -- "${report_path}"
# A profile switch changes the core service's Redis and trusted-result bind
# addresses. Recreate the project so an already-running container cannot keep
# the previous in-memory configuration while reading a newly written file.
reset_lab_faults
docker compose --profile lab down --remove-orphans
if [[ "${lab_mode}" -eq 1 ]]; then
  docker compose --profile lab config --quiet
  docker compose --profile lab up --build --detach
else
  docker compose config --quiet
  docker compose up --build --detach
fi

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

if [[ "${lab_mode}" -eq 1 ]]; then
  ./scripts/smoke.sh
  ./scripts/lab-smoke.sh
else
  ./scripts/smoke.sh
fi

trap - ERR INT TERM
printf '\n竞技演示已就绪：\n'
printf '  观察台：http://127.0.0.1:%s/\n' "${observer_port}"
printf '  TCP 端口：127.0.0.1:%s\n' "${tcp_port}"
printf '  smoke 报告：%s\n' "${report_path}"
if [[ "${lab_mode}" -eq 1 ]]; then
  printf '  场景实验台：http://127.0.0.1:%s/\n' "${lab_port}"
  printf '  Lab Run 报告：%s/lab-runs/\n' "${runtime_dir}"
  printf '停止并清除故障注入（保留数据）：./scripts/demo-down.sh --lab\n'
else
  printf '停止但保留数据：./scripts/demo-down.sh\n'
fi

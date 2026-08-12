#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="${1:-${project_root}/out/build/linux-gcc}"
runtime_dir="$(mktemp -d -t arena-m7-e2e-XXXXXX)"
report_dir="${build_dir}/qa/m7-e2e"
server_log="${report_dir}/server.log"
server_pid=""
robot_pid=""
export ARENA_RESULT_SERVICE_TOKEN="${ARENA_RESULT_SERVICE_TOKEN:-arena-local-m9-token}"

mkdir -p "${report_dir}"

cleanup() {
  if [[ -n "${robot_pid}" ]] && kill -0 "${robot_pid}" >/dev/null 2>&1; then
    kill -TERM "${robot_pid}" >/dev/null 2>&1 || true
    wait "${robot_pid}" >/dev/null 2>&1 || true
  fi
  if [[ -n "${server_pid}" ]] && kill -0 "${server_pid}" >/dev/null 2>&1; then
    kill -TERM "${server_pid}" >/dev/null 2>&1 || true
    for _ in $(seq 1 50); do
      kill -0 "${server_pid}" >/dev/null 2>&1 || break
      sleep 0.1
    done
    if kill -0 "${server_pid}" >/dev/null 2>&1; then
      kill -KILL "${server_pid}" >/dev/null 2>&1 || true
    fi
    wait "${server_pid}" >/dev/null 2>&1 || true
  fi
  rm -rf -- "${runtime_dir}"
}
trap cleanup EXIT INT TERM

start_server() {
  : >"${server_log}"
  "${build_dir}/arena-core-server" --config "${project_root}/config/arena-core.m7.json" \
    >"${server_log}" 2>&1 &
  server_pid=$!
  for _ in $(seq 1 60); do
    if ! kill -0 "${server_pid}" >/dev/null 2>&1; then
      cat "${server_log}" >&2
      return 1
    fi
    if "${build_dir}/arena-core-robot" --port 49110 --player m7-readiness --heartbeats 1 \
        >/dev/null 2>&1; then
      if kill -0 "${server_pid}" >/dev/null 2>&1; then
        return 0
      fi
      cat "${server_log}" >&2
      return 1
    fi
    sleep 0.1
  done
  echo "M7 server readiness timed out" >&2
  return 1
}

stop_server() {
  local label="$1"
  local started
  started="$(date +%s%3N)"
  kill -TERM "${server_pid}"
  for _ in $(seq 1 50); do
    if ! kill -0 "${server_pid}" >/dev/null 2>&1; then
      wait "${server_pid}"
      server_pid=""
      printf '%s,%s\n' "${label}" "$(( $(date +%s%3N) - started ))" \
        >>"${report_dir}/shutdown-latency-ms.csv"
      return 0
    fi
    sleep 0.1
  done
  echo "server did not stop within 5 seconds during ${label}" >&2
  return 1
}

: >"${report_dir}/shutdown-latency-ms.csv"
start_server
timeout 30s "${build_dir}/arena-core-robot" \
  --scenario "${project_root}/scenarios/qa/full-flow-replay.json" \
  --output "${report_dir}/record.json"
stop_server "recorded-state"

start_server
timeout 30s "${build_dir}/arena-core-robot" \
  --replay "${report_dir}/record.json" \
  --output "${report_dir}/replay.json"
timeout 30s "${build_dir}/arena-core-robot" \
  --scenario "${project_root}/scenarios/qa/regression-8-players.json" \
  --output "${report_dir}/regression.json"
stop_server "completed-rooms"

start_server
set +e
timeout 75s "${build_dir}/arena-core-robot" \
  --scenario "${project_root}/scenarios/qa/graceful-stop-active.json" \
  --output "${runtime_dir}/active-stop.json" >"${report_dir}/active-robot.log" 2>&1 &
robot_pid=$!
set -e
for _ in $(seq 1 80); do
  active="$(curl -fsS http://127.0.0.1:49120/api/overview 2>/dev/null | \
    python3 -c 'import json,sys; print(json.load(sys.stdin)["counts"]["sessions_online"])' 2>/dev/null || echo 0)"
  if [[ "${active}" -ge 8 ]]; then
    break
  fi
  sleep 0.1
done
if [[ "${active:-0}" -lt 8 ]]; then
  echo "active-session shutdown precondition was not reached" >&2
  exit 1
fi
stop_server "active-sessions"
set +e
wait "${robot_pid}"
robot_status=$?
set -e
robot_pid=""
if [[ "${robot_status}" -eq 0 ]]; then
  echo "active robot unexpectedly survived server shutdown" >&2
  exit 1
fi

start_server
timeout 30s "${build_dir}/arena-core-robot" \
  --scenario "${project_root}/scenarios/qa/regression-8-players.json" \
  --output "${report_dir}/restart-regression.json"
stop_server "restart-regression"

grep -q "TCP server stopped" "${server_log}"
python3 - "${report_dir}" <<'PY'
import json
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
for name in ("record.json", "replay.json", "regression.json", "restart-regression.json"):
    report = json.loads((root / name).read_text(encoding="utf-8"))
    assert report["summary"]["passed"] is True, name
assert json.loads((root / "replay.json").read_text(encoding="utf-8"))["summary"]["replay_verified"] is True
latencies = [int(line.split(",")[1]) for line in (root / "shutdown-latency-ms.csv").read_text().splitlines()]
assert len(latencies) == 4
assert max(latencies) <= 5000
print(f"M7 E2E passed: replay verified, 8-player regression passed, shutdown max={max(latencies)} ms")
PY

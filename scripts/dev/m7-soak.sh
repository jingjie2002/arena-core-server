#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="${1:-${project_root}/out/build/linux-gcc}"
scenario_path="${2:-${project_root}/scenarios/qa/soak-500-connections.json}"
report_dir="${build_dir}/qa/m7-soak"
server_log="${report_dir}/server.log"
metrics_csv="${report_dir}/metrics.csv"
server_pid=""
metrics_pid=""
export ARENA_RESULT_SERVICE_TOKEN="${ARENA_RESULT_SERVICE_TOKEN:-arena-local-m9-token}"

mkdir -p "${report_dir}"

cleanup() {
  if [[ -n "${metrics_pid}" ]] && kill -0 "${metrics_pid}" >/dev/null 2>&1; then
    kill -TERM "${metrics_pid}" >/dev/null 2>&1 || true
    wait "${metrics_pid}" >/dev/null 2>&1 || true
  fi
  if [[ -n "${server_pid}" ]] && kill -0 "${server_pid}" >/dev/null 2>&1; then
    kill -TERM "${server_pid}" >/dev/null 2>&1 || true
    wait "${server_pid}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT INT TERM

: >"${server_log}"
"${build_dir}/arena-core-server" --config "${project_root}/config/arena-core.soak.json" \
  >"${server_log}" 2>&1 &
server_pid=$!
for _ in $(seq 1 100); do
  if ! kill -0 "${server_pid}" >/dev/null 2>&1; then
    cat "${server_log}" >&2
    exit 1
  fi
  if "${build_dir}/arena-core-robot" --port 49210 --player soak-readiness --heartbeats 1 \
      >/dev/null 2>&1; then
    break
  fi
  sleep 0.1
done

printf 'unix_ms,cpu_percent,rss_kb,active_sessions,total_rooms\n' >"${metrics_csv}"
(
  while kill -0 "${server_pid}" >/dev/null 2>&1; do
    read -r cpu rss < <(ps -p "${server_pid}" -o %cpu=,rss=)
    overview="$(curl -fsS http://127.0.0.1:49220/api/overview 2>/dev/null || echo '{}')"
    read -r sessions rooms < <(python3 -c \
      'import json,sys; v=json.load(sys.stdin).get("counts",{}); print(v.get("sessions_online",0),v.get("rooms_total",0))' \
      <<<"${overview}")
    printf '%s,%s,%s,%s,%s\n' "$(date +%s%3N)" "${cpu}" "${rss}" "${sessions}" "${rooms}" \
      >>"${metrics_csv}"
    sleep 5
  done
) &
metrics_pid=$!

/usr/bin/time -v -o "${report_dir}/robot-time.txt" \
  timeout 660s "${build_dir}/arena-core-robot" \
    --scenario "${scenario_path}" \
    --output "${report_dir}/robot-report.json" \
    >"${report_dir}/robot.stdout" 2>"${report_dir}/robot.stderr"

kill -TERM "${metrics_pid}" >/dev/null 2>&1 || true
wait "${metrics_pid}" >/dev/null 2>&1 || true
metrics_pid=""
kill -TERM "${server_pid}"
for _ in $(seq 1 100); do
  kill -0 "${server_pid}" >/dev/null 2>&1 || break
  sleep 0.1
done
if kill -0 "${server_pid}" >/dev/null 2>&1; then
  echo "soak server did not stop within 10 seconds" >&2
  exit 1
fi
wait "${server_pid}"
server_pid=""

python3 - "${report_dir}" <<'PY'
import csv
import json
import pathlib
import platform
import sys

root = pathlib.Path(sys.argv[1])
robot = json.loads((root / "robot-report.json").read_text(encoding="utf-8"))
with (root / "metrics.csv").open(newline="", encoding="utf-8") as handle:
    metrics = list(csv.DictReader(handle))
summary = robot["summary"]
max_connections = max(int(row["active_sessions"]) for row in metrics)
max_rooms = max(int(row["total_rooms"]) for row in metrics)
max_rss_kb = max(int(row["rss_kb"]) for row in metrics)
max_cpu = max(float(row["cpu_percent"]) for row in metrics)
assert summary["passed"] is True
assert summary["business_errors"] == 0
assert summary["successful_connections"] == 500
assert summary["completed_rooms"] == 250
target_duration = int(robot["scenario"]["duration_seconds"])
assert summary["elapsed_ms"] >= max(0, target_duration * 1000 - 1000)
assert max_connections >= 500
assert max_rooms >= 250
result = {
    "result": "passed",
    "environment": {
        "platform": platform.platform(),
        "python": platform.python_version(),
    },
    "target": {"connections": 500, "rooms": 250, "duration_seconds": target_duration},
    "observed": {
        "peak_connections": max_connections,
        "peak_rooms": max_rooms,
        "max_cpu_percent": max_cpu,
        "max_rss_kb": max_rss_kb,
        **summary,
    },
}
(root / "summary.json").write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
latency = summary["latency_ms"]
markdown = f"""# M7 500连接稳定性实测\n\n- 结论：通过\n- 场景：500 个持续 TCP 连接、250 个双人房间、持续 {target_duration} 秒\n- 完整房间：{summary['completed_rooms']}\n- 业务错误：{summary['business_errors']}\n- P50/P95/P99：{latency['p50']:.3f} / {latency['p95']:.3f} / {latency['p99']:.3f} ms\n- 吞吐：{summary['throughput_rounds_per_second']:.3f} 完整房间/秒（含保持期）\n- 峰值 CPU：{max_cpu:.1f}%\n- 峰值 RSS：{max_rss_kb / 1024:.1f} MiB\n- 峰值连接/房间：{max_connections} / {max_rooms}\n- Robot 心跳：{summary['heartbeat_count']}\n"""
(root / "summary.md").write_text(markdown, encoding="utf-8")
print(f"M7 soak passed: connections={max_connections}, rooms={max_rooms}, RSS={max_rss_kb/1024:.1f} MiB")
PY

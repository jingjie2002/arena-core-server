#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
runtime_dir="${project_root}/.runtime"
report_path="${runtime_dir}/smoke-report.json"
observer_port="${ARENA_DEMO_OBSERVER_PORT:-49120}"

cd "${project_root}"
mkdir -p "${runtime_dir}"

reset_lab_faults() {
  if docker compose --profile lab ps --status running --services 2>/dev/null | grep -qx toxiproxy; then
    docker compose --profile lab exec -T toxiproxy toxiproxy-cli reset >/dev/null 2>&1 || true
  fi
}
on_error() {
  local status=$?
  reset_lab_faults
  exit "${status}"
}
trap on_error ERR INT TERM

docker compose ps --status running --services | grep -qx arena-core

docker compose exec -T arena-core /opt/arena-core/bin/arena-core-robot \
  --scenario /opt/arena-core/scenarios/qa/full-flow-replay.json \
  --host 127.0.0.1 \
  --port 9100 \
  --result-host 127.0.0.1 \
  --result-port 9200 \
  --output /opt/arena-core/.runtime/smoke-report.json

python3 - "${report_path}" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as handle:
    report = json.load(handle)
summary = report["summary"]
if not summary["passed"]:
    raise SystemExit("Robot report did not pass")
if summary["successful_connections"] != 2 or summary["completed_rooms"] != 1:
    raise SystemExit(f"unexpected Robot summary: {summary}")
PY

overview="$(curl --fail --silent --show-error "http://127.0.0.1:${observer_port}/api/overview")"
python3 - "${overview}" <<'PY'
import json
import sys

overview = json.loads(sys.argv[1])
if overview.get("service") != "arena-core-server":
    raise SystemExit(f"unexpected observer response: {overview}")
PY

demo_probe_status="$(
  curl --silent --show-error \
    --output /dev/null \
    --write-out '%{http_code}' \
    --request POST \
    --header 'X-Arena-Demo: 1' \
    "http://127.0.0.1:${observer_port}/api/demo/scenarios/__smoke_probe__"
)"
expected_demo_status=403
if [[ "${demo_probe_status}" != "${expected_demo_status}" ]]; then
  echo "observer write-boundary probe: expected ${expected_demo_status}, got ${demo_probe_status}" >&2
  exit 1
fi

docker compose exec -T -e "MYSQL_PWD=${ARENA_DEMO_MYSQL_PASSWORD:-arena_demo_local}" mysql mysql \
  -uarena \
  -Nse "SELECT COUNT(*) FROM settlements;" arena | grep -Eq '^[1-9][0-9]*$'

docker compose exec -T redis redis-cli EXISTS arena:demo:rank:duel:season-1 | grep -qx 1

printf 'smoke passed: TCP -> match -> room -> MySQL settlement -> Redis rank -> observer boundary\n'
trap - ERR INT TERM

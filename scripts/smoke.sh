#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
runtime_dir="${project_root}/.runtime"
report_path="${runtime_dir}/smoke-report.json"
observer_port="${ARENA_DEMO_OBSERVER_PORT:-49120}"

cd "${project_root}"
mkdir -p "${runtime_dir}"

docker compose ps --status running --services | grep -qx arena-core

docker compose exec -T arena-core /opt/arena-core/bin/arena-core-robot \
  --scenario /opt/arena-core/scenarios/qa/full-flow-replay.json \
  --host 127.0.0.1 \
  --port 9100 \
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

docker compose exec -T -e "MYSQL_PWD=${ARENA_DEMO_MYSQL_PASSWORD:-arena_demo_local}" mysql mysql \
  -uarena \
  -Nse "SELECT COUNT(*) FROM settlements;" arena | grep -Eq '^[1-9][0-9]*$'

docker compose exec -T redis redis-cli EXISTS arena:demo:rank:duel:season-1 | grep -qx 1

printf 'smoke passed: TCP -> match -> room -> MySQL settlement -> Redis rank -> observer\n'

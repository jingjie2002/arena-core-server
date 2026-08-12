#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="${1:-${project_root}/out/build/linux-gcc}"
report_dir="${build_dir}/qa/m9-e2e"
server_log="${report_dir}/server.log"
server_pid=""
export ARENA_RESULT_SERVICE_TOKEN="${ARENA_RESULT_SERVICE_TOKEN:-arena-local-m9-token}"

mkdir -p "${report_dir}"

cleanup() {
  if [[ -n "${server_pid}" ]] && kill -0 "${server_pid}" >/dev/null 2>&1; then
    kill -TERM "${server_pid}" >/dev/null 2>&1 || true
    wait "${server_pid}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT INT TERM

: >"${server_log}"
"${build_dir}/arena-core-server" --config "${project_root}/config/arena-core.m9.json" \
  >"${server_log}" 2>&1 &
server_pid=$!
for _ in $(seq 1 80); do
  if ! kill -0 "${server_pid}" >/dev/null 2>&1; then
    cat "${server_log}" >&2
    exit 1
  fi
  if "${build_dir}/arena-core-robot" --port 49410 --player m9-readiness --heartbeats 1 \
      >/dev/null 2>&1; then
    break
  fi
  sleep 0.1
done

timeout 30s "${build_dir}/arena-core-robot" \
  --scenario "${project_root}/scenarios/qa/m9-trusted-result.json" \
  --output "${report_dir}/robot-report.json"

result_id="$(python3 -c \
  'import json,sys; print(json.load(open(sys.argv[1], encoding="utf-8"))["operations"][0]["match_id"])' \
  "${report_dir}/robot-report.json")"
result_id="result-${result_id}"

unauthorized="$(curl --silent --output /dev/null --write-out '%{http_code}' \
  "http://127.0.0.1:49430/internal/v1/match-results/${result_id}")"
test "${unauthorized}" = "401"
curl --fail --silent --show-error \
  --header "Authorization: Bearer ${ARENA_RESULT_SERVICE_TOKEN}" \
  "http://127.0.0.1:49430/internal/v1/match-results/${result_id}" \
  >"${report_dir}/queried-result.json"

python3 - "${report_dir}" <<'PY'
import json
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
result = json.loads((root / "queried-result.json").read_text(encoding="utf-8"))
submission = {key: result[key] for key in (
    "result_id", "match_id", "room_id", "winner_id", "loser_id", "rule_version", "finished_at"
)}
(root / "duplicate-submission.json").write_text(json.dumps(submission), encoding="utf-8")
submission["winner_id"], submission["loser_id"] = submission["loser_id"], submission["winner_id"]
(root / "conflicting-submission.json").write_text(json.dumps(submission), encoding="utf-8")
PY

duplicate_status="$(curl --silent --show-error --output "${report_dir}/duplicate-result.json" \
  --write-out '%{http_code}' --request POST --header 'Content-Type: application/json' \
  --header "Authorization: Bearer ${ARENA_RESULT_SERVICE_TOKEN}" \
  --data-binary "@${report_dir}/duplicate-submission.json" \
  http://127.0.0.1:49430/internal/v1/match-results)"
test "${duplicate_status}" = "200"
conflict_status="$(curl --silent --show-error --output "${report_dir}/conflict-result.json" \
  --write-out '%{http_code}' --request POST --header 'Content-Type: application/json' \
  --header "Authorization: Bearer ${ARENA_RESULT_SERVICE_TOKEN}" \
  --data-binary "@${report_dir}/conflicting-submission.json" \
  http://127.0.0.1:49430/internal/v1/match-results)"
test "${conflict_status}" = "409"

python3 - "${report_dir}" <<'PY'
import json
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
robot = json.loads((root / "robot-report.json").read_text(encoding="utf-8"))
queried = json.loads((root / "queried-result.json").read_text(encoding="utf-8"))
duplicate = json.loads((root / "duplicate-result.json").read_text(encoding="utf-8"))
assert robot["summary"]["passed"] is True
assert robot["summary"]["completed_rooms"] == 1
operation = robot["operations"][0]
assert queried["result_id"] == "result-" + operation["match_id"]
assert queried["winner_id"] == operation["winner_id"]
assert queried["loser_id"] == operation["loser_id"]
assert len(queried["players"]) == 2
assert duplicate["idempotent"] is True
print("M9 E2E passed: adapter -> ingress -> settlement -> player push -> query/idempotency/conflict")
PY

kill -TERM "${server_pid}"
wait "${server_pid}"
server_pid=""
grep -q "TCP server stopped" "${server_log}"

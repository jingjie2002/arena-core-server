#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
lab_port="${ARENA_LAB_PORT:-49140}"
base_url="http://127.0.0.1:${lab_port}/api/lab/v1"

cd "${project_root}"

reset_lab_faults() {
  if docker compose --profile lab ps --status running --services 2>/dev/null | grep -qx arena-lab; then
    docker compose --profile lab exec -T arena-lab \
      /opt/arena-core/bin/arena-core-lab --config /opt/arena-core/.runtime/arena-lab.json \
      --reset-faults >/dev/null 2>&1 || true
  fi
}
trap reset_lab_faults EXIT INT TERM
reset_lab_faults

health=""
for _ in $(seq 1 60); do
  if health="$(curl --fail --silent --show-error "${base_url}/health" 2>/dev/null)"; then
    break
  fi
  if ! docker compose --profile lab ps --status running --services | grep -qx arena-lab; then
    docker compose --profile lab logs --no-color arena-lab toxiproxy arena-core >&2
    exit 1
  fi
  sleep 1
done
if [[ -z "${health}" ]]; then
  docker compose --profile lab logs --no-color arena-lab toxiproxy arena-core >&2
  echo "Lab API did not become ready within 60 seconds" >&2
  exit 1
fi
python3 - "${health}" <<'PY'
import json
import sys

document = json.loads(sys.argv[1])
if document.get("status") != "ok" or document.get("schema_version") != 1:
    raise SystemExit(f"unexpected Lab health response: {document}")
PY

capabilities="$(curl --fail --silent --show-error "${base_url}/capabilities")"
python3 - "${capabilities}" <<'PY'
import json
import sys

document = json.loads(sys.argv[1])
if document.get("schema_version") != 1:
    raise SystemExit(f"unexpected capabilities schema: {document}")
if document.get("service", {}).get("name") != "arena-core-lab":
    raise SystemExit(f"unexpected Lab service: {document}")
names = {item.get("name") for item in document.get("scenarios", [])}
required = {
    "complete_match",
    "reconnect_recovery",
    "duplicate_player_request",
    "duplicate_trusted_result",
    "redis_failure_recovery",
}
if names != required:
    raise SystemExit(f"unexpected Lab scenarios: {sorted(names)}")
PY

schema="$(curl --fail --silent --show-error "${base_url}/scenarios/complete_match/schema")"
python3 - "${schema}" <<'PY'
import json
import sys

document = json.loads(sys.argv[1])
if document.get("schema_version") != 1 or document.get("name") != "complete_match":
    raise SystemExit(f"unexpected complete_match schema: {document}")
fields = {field.get("name") for field in document.get("fields", [])}
if "player_count" not in fields or "ready_delay_ms" not in fields:
    raise SystemExit(f"complete_match fields are incomplete: {sorted(fields)}")
PY

run_id_from_response() {
  python3 - "$1" <<'PY'
import json
import sys

document = json.loads(sys.argv[1])
run_id = document.get("run_id")
if not isinstance(run_id, str) or not run_id:
    raise SystemExit(f"Lab did not return a run_id: {document}")
print(run_id)
PY
}

wait_for_run() {
  local run_id="$1"
  local expected_status="$2"
  local report=""
  local status=""
  for _ in $(seq 1 210); do
    report="$(curl --fail --silent --show-error "${base_url}/runs/${run_id}")"
    status="$(python3 - "${report}" <<'PY'
import json
import sys
print(json.loads(sys.argv[1]).get("status", ""))
PY
    )"
    case "${status}" in
      Passed|Failed|Cancelled|TimedOut|Interrupted) break ;;
    esac
    sleep 1
  done

  python3 - "${report}" "${expected_status}" <<'PY'
import json
import sys

document = json.loads(sys.argv[1])
expected_status = sys.argv[2]
if document.get("status") != expected_status:
    raise SystemExit(
        f"Run {document.get('run_id')} expected {expected_status}, got {document.get('status')}: {document}"
    )
assertions = document.get("assertions", [])
if expected_status == "Passed":
    if not assertions or not all(item.get("passed") is True for item in assertions):
        raise SystemExit(f"Lab assertions did not all pass: {assertions}")
    if not isinstance(document.get("evidence"), dict) or not document["evidence"]:
        raise SystemExit("passed Lab Run did not retain business evidence")
cleanup = document.get("cleanup", {})
if cleanup.get("completed") is not True:
    raise SystemExit(f"Lab cleanup did not complete: {cleanup}")
PY

  last_run_id="${run_id}"
  last_report="${report}"
}

create_run() {
  local request="$1"
  local expected_status="$2"
  local created=""
  created="$(
    curl --fail --silent --show-error \
      --request POST \
      --header 'Content-Type: application/json' \
      --header 'X-Arena-Lab: 1' \
      --data "${request}" \
      "${base_url}/runs"
  )"
  wait_for_run "$(run_id_from_response "${created}")" "${expected_status}"
}

last_run_id=""
last_report=""

create_run '{"schema_version":1,"scenario":"complete_match","seed":1101,"timeout_ms":60000,"parameters":{"player_count":4,"mmr_mode":"stair","mmr_base":1000,"mmr_stride":100,"connect_interval_ms":10,"ready_delay_ms":10}}' Passed
complete_run_id="${last_run_id}"
complete_report="${last_report}"

create_run '{"schema_version":1,"scenario":"reconnect_recovery","seed":1102,"timeout_ms":30000,"parameters":{"player_index":0,"disconnect_stage":"after_join","reconnect_delay_ms":500}}' Passed
create_run '{"schema_version":1,"scenario":"reconnect_recovery","seed":1103,"timeout_ms":30000,"parameters":{"player_index":1,"disconnect_stage":"after_join","reconnect_delay_ms":16000}}' Passed
create_run '{"schema_version":1,"scenario":"duplicate_player_request","seed":1104,"timeout_ms":30000,"parameters":{"message":"ready_room","repeat_count":3}}' Passed
create_run '{"schema_version":1,"scenario":"duplicate_trusted_result","seed":1105,"timeout_ms":30000,"parameters":{"mode":"exact","repeat_count":3}}' Passed
create_run '{"schema_version":1,"scenario":"duplicate_trusted_result","seed":1106,"timeout_ms":30000,"parameters":{"mode":"conflicting","repeat_count":3}}' Passed
create_run '{"schema_version":1,"scenario":"redis_failure_recovery","seed":1107,"timeout_ms":60000,"parameters":{"fault_kind":"disconnect","fault_value_ms":0,"fault_duration_ms":1000}}' Passed

replayed="$(
  curl --fail --silent --show-error \
    --request POST \
    --header 'Content-Type: application/json' \
    --header 'X-Arena-Lab: 1' \
    --data '{}' \
    "${base_url}/runs/${complete_run_id}/replay"
)"
wait_for_run "$(run_id_from_response "${replayed}")" Passed
python3 - "${complete_report}" "${last_report}" <<'PY'
import json
import sys

original = json.loads(sys.argv[1])
replay = json.loads(sys.argv[2])
if replay.get("run_id") == original.get("run_id"):
    raise SystemExit("replay reused the original run_id")
if replay.get("replay_of") != original.get("run_id"):
    raise SystemExit(f"replay_of is incorrect: {replay}")
for field in ("scenario", "parameters", "seed", "timeout_ms"):
    if replay.get(field) != original.get(field):
        raise SystemExit(f"replay changed immutable field {field}")
PY

created_cancel="$(
  curl --fail --silent --show-error \
    --request POST \
    --header 'Content-Type: application/json' \
    --header 'X-Arena-Lab: 1' \
    --data '{"schema_version":1,"scenario":"reconnect_recovery","seed":1108,"timeout_ms":30000,"parameters":{"player_index":0,"disconnect_stage":"after_join","reconnect_delay_ms":10000}}' \
    "${base_url}/runs"
)"
cancel_run_id="$(run_id_from_response "${created_cancel}")"
curl --fail --silent --show-error \
  --request POST \
  --header 'Content-Type: application/json' \
  --header 'X-Arena-Lab: 1' \
  --data '{}' \
  "${base_url}/runs/${cancel_run_id}/cancel" >/dev/null
wait_for_run "${cancel_run_id}" Cancelled

create_run '{"schema_version":1,"scenario":"reconnect_recovery","seed":1109,"timeout_ms":1000,"parameters":{"player_index":0,"disconnect_stage":"after_join","reconnect_delay_ms":5000}}' TimedOut

events="$(curl --fail --silent --show-error "${base_url}/runs/${complete_run_id}/events?after_seq=0")"
python3 - "${events}" <<'PY'
import json
import sys

document = json.loads(sys.argv[1])
if not document.get("items") or document.get("next_seq", 0) <= 0:
    raise SystemExit(f"Lab event stream is empty: {document}")
PY

printf 'lab smoke passed: five P0 scenarios + reconnect boundaries + result conflict + replay + cancel + timeout + cleanup\n'

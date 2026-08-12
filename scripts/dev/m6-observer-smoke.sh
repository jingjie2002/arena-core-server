#!/usr/bin/env bash
set -euo pipefail

build_dir="${1:-out/build/linux-gcc}"
server="${build_dir}/arena-core-server"
base_url="http://127.0.0.1:39120"
log_file="${TMPDIR:-/tmp}/arena-core-m6-observer.log"
server_pid=""

cleanup() {
  if [[ -n "${server_pid}" ]] && kill -0 "${server_pid}" 2>/dev/null; then
    kill -TERM "${server_pid}" 2>/dev/null || true
    wait "${server_pid}" 2>/dev/null || true
  fi
}
trap cleanup EXIT

if [[ ! -x "${server}" ]]; then
  echo "server executable not found: ${server}" >&2
  exit 1
fi

"${server}" --config config/arena-core.demo.json --demo-mode >"${log_file}" 2>&1 &
server_pid=$!

for _ in $(seq 1 80); do
  if curl --fail --silent "${base_url}/api/overview" >/dev/null; then
    break
  fi
  if ! kill -0 "${server_pid}" 2>/dev/null; then
    cat "${log_file}" >&2
    exit 1
  fi
  sleep 0.1
done

page=$(curl --fail --silent "${base_url}/")
grep -q "这不是游戏画面" <<<"${page}"
grep -q "查看开发者技术明细" <<<"${page}"
curl --fail --silent "${base_url}/app.js" | grep -q "重复提交保护通过"
curl --fail --silent --request POST --header "X-Arena-Demo: 1" "${base_url}/api/demo/scenarios/eight-player-flow" >/dev/null
overview=$(curl --fail --silent "${base_url}/api/overview")
python3 -c 'import json,sys; d=json.load(sys.stdin); c=d["counts"]; assert (c["sessions_online"],c["rooms_total"],c["settlements"]) == (8,4,4)' <<<"${overview}"

curl --fail --silent --request POST --header "X-Arena-Demo: 1" "${base_url}/api/demo/scenarios/duplicate-settlement" >/dev/null
rankings=$(curl --fail --silent "${base_url}/api/rankings")
python3 -c 'import json,sys; d=json.load(sys.stdin); assert any(x["idempotent_replay"] for x in d["settlements"])' <<<"${rankings}"

curl --fail --silent --request POST --header "X-Arena-Demo: 1" "${base_url}/api/demo/scenarios/redis-repair" >/dev/null
events=$(curl --fail --silent "${base_url}/api/events?trace_id=trace-demo-repair&limit=100")
python3 -c 'import json,sys; d=json.load(sys.stdin); t=[x["type"] for x in d["items"]]; assert "rank.cache_stale" in t and "rank.repaired" in t' <<<"${events}"

echo "M6 observer smoke passed: 8 sessions, 4 rooms, idempotent replay, Redis repair"

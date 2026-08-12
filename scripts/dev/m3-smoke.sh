#!/usr/bin/env bash
set -euo pipefail

build_dir="${1:-out/build/linux-gcc}"
server_log="${build_dir}/m3-server.log"

"${build_dir}/arena-core-server" --config config/arena-core.smoke.json >"${server_log}" 2>&1 &
server_pid=$!

cleanup() {
  if kill -0 "${server_pid}" 2>/dev/null; then
    kill -TERM "${server_pid}"
    wait "${server_pid}"
  fi
}
trap cleanup EXIT

for attempt in $(seq 1 30); do
  if ! kill -0 "${server_pid}" 2>/dev/null; then
    echo "M3 server exited before smoke; server log: ${server_log}" >&2
    exit 1
  fi
  if "${build_dir}/arena-core-robot" --port 19100 --player smoke-robot --heartbeats 3 2>/dev/null; then
    exit 0
  fi
  sleep 0.1
done

echo "M3 smoke failed; server log: ${server_log}" >&2
exit 1

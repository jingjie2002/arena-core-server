#!/usr/bin/env bash
set -euo pipefail

action="${1:-status}"
build_dir="${2:-out/build/linux-gcc}"
server="${build_dir}/arena-core-server"
base_url="http://127.0.0.1:39120"
pid_file="${TMPDIR:-/tmp}/arena-core-m6-demo.pid"
log_file="${TMPDIR:-/tmp}/arena-core-m6-demo.log"

read_pid() {
  if [[ -f "${pid_file}" ]]; then
    tr -d '[:space:]' <"${pid_file}"
  fi
}

is_ours() {
  local pid="$1"
  [[ -n "${pid}" ]] && kill -0 "${pid}" 2>/dev/null &&
    tr '\0' ' ' <"/proc/${pid}/cmdline" |
      grep -Fq "arena-core-server --config config/arena-core.demo.json --demo-mode"
}

case "${action}" in
  start)
    existing_pid="$(read_pid)"
    if is_ours "${existing_pid}"; then
      echo "M6 demo already running: pid=${existing_pid} url=${base_url}"
      exit 0
    fi
    if ss -H -ltn '( sport = :39110 or sport = :39120 )' | grep -q .; then
      echo "M6 demo ports 39110/39120 are already in use; nothing was started" >&2
      exit 1
    fi
    if [[ ! -x "${server}" ]]; then
      echo "server executable not found: ${server}" >&2
      exit 1
    fi
    nohup "${server}" --config config/arena-core.demo.json --demo-mode \
      >"${log_file}" 2>&1 </dev/null &
    demo_pid=$!
    printf '%s\n' "${demo_pid}" >"${pid_file}"
    for _ in $(seq 1 80); do
      if curl --fail --silent "${base_url}/api/overview" >/dev/null; then
        echo "M6 demo started: pid=${demo_pid} url=${base_url} log=${log_file}"
        exit 0
      fi
      if ! is_ours "${demo_pid}"; then
        cat "${log_file}" >&2
        exit 1
      fi
      sleep 0.1
    done
    echo "M6 demo did not become healthy" >&2
    exit 1
    ;;
  stop)
    demo_pid="$(read_pid)"
    if ! is_ours "${demo_pid}"; then
      echo "M6 demo is not running; no process was stopped"
      rm -f -- "${pid_file}"
      exit 0
    fi
    kill -TERM "${demo_pid}"
    for _ in $(seq 1 50); do
      if ! kill -0 "${demo_pid}" 2>/dev/null; then
        rm -f -- "${pid_file}"
        echo "M6 demo stopped: pid=${demo_pid}"
        exit 0
      fi
      sleep 0.1
    done
    echo "M6 demo did not stop within 5 seconds" >&2
    exit 1
    ;;
  status)
    demo_pid="$(read_pid)"
    if is_ours "${demo_pid}"; then
      echo "M6 demo running: pid=${demo_pid} url=${base_url} log=${log_file}"
    else
      echo "M6 demo stopped"
    fi
    ;;
  *)
    echo "usage: $0 {start|stop|status} [build-dir]" >&2
    exit 2
    ;;
esac

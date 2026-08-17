#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${project_root}"

lab_mode=0
reset_data=0
for argument in "$@"; do
  case "${argument}" in
    --lab) lab_mode=1 ;;
    --reset) reset_data=1 ;;
    *)
      echo "usage: ./scripts/demo-down.sh [--lab] [--reset]" >&2
      exit 2
      ;;
  esac
done

reset_lab_faults() {
  if docker compose --profile lab ps --status running --services 2>/dev/null | grep -qx toxiproxy; then
    if docker compose --profile lab ps --status running --services 2>/dev/null | grep -qx arena-lab; then
      docker compose --profile lab exec -T arena-lab \
        /opt/arena-core/bin/arena-core-lab --config /opt/arena-core/.runtime/arena-lab.json \
        --reset-faults >/dev/null 2>&1 && return
    fi
    docker compose --profile lab restart toxiproxy >/dev/null 2>&1 || true
  fi
}
on_error() {
  local status=$?
  reset_lab_faults
  exit "${status}"
}
trap on_error ERR INT TERM

reset_lab_faults
compose_args=(--profile lab)

if [[ "${reset_data}" -eq 1 ]]; then
  printf '即将停止服务，并删除仅属于 arena-core-demo 的 MySQL/Redis 数据卷。\n'
  docker compose "${compose_args[@]}" down --volumes --remove-orphans
  printf '演示容器和 MySQL/Redis 数据卷已删除；源码与 .runtime 中的 Run 报告不受影响。\n'
else
  docker compose "${compose_args[@]}" down --remove-orphans
  printf '演示服务已停止；MySQL/Redis 数据卷和 .runtime 中的 Run 报告已保留。\n'
fi

if [[ "${lab_mode}" -eq 1 ]]; then
  printf 'Lab/Toxiproxy 容器已停止，停止前已尽力清除全部故障注入。\n'
fi
trap - ERR INT TERM

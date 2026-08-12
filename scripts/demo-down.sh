#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${project_root}"

if [[ "${1:-}" == "--reset" ]]; then
  printf '即将停止服务并删除仅属于 arena-core-demo 的 MySQL/Redis 演示卷。\n'
  docker compose down --volumes --remove-orphans
  printf '演示服务和演示数据卷已删除；源码不受影响。\n'
elif [[ $# -eq 0 ]]; then
  docker compose down --remove-orphans
  printf '演示服务已停止；MySQL/Redis 演示卷已保留。\n'
else
  echo "usage: ./scripts/demo-down.sh [--reset]" >&2
  exit 2
fi

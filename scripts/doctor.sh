#!/usr/bin/env bash
set -euo pipefail

mode="demo"
if [[ "${1:-}" == "--native" ]]; then
  mode="native"
elif [[ $# -ne 0 ]]; then
  echo "usage: ./scripts/doctor.sh [--native]" >&2
  exit 2
fi

failures=0

pass() {
  printf '[ok] %s\n' "$1"
}

fail() {
  printf '[missing] %s\n' "$1" >&2
  failures=$((failures + 1))
}

require_command() {
  local command="$1"
  local guidance="$2"
  if command -v "${command}" >/dev/null 2>&1; then
    pass "${command}: $(command -v "${command}")"
  else
    fail "${command}: ${guidance}"
  fi
}

case "$(uname -s)" in
  Linux) pass "Linux environment detected" ;;
  *) fail "请在 WSL2 Ubuntu 或 Linux 中运行；当前为 $(uname -s)" ;;
esac

require_command git "请安装 Git"

if [[ "${mode}" == "demo" ]]; then
  require_command curl "请安装 curl（Ubuntu: sudo apt-get install curl）"
  require_command python3 "请安装 Python 3（Ubuntu: sudo apt-get install python3）"
  require_command docker "请安装 Docker Desktop/Engine，并为当前 WSL 发行版启用 Docker integration"
  if command -v docker >/dev/null 2>&1; then
    if ! docker info >/dev/null 2>&1; then
      fail "Docker 引擎不可用；请启动 Docker Desktop，并在 Settings > Resources > WSL Integration 启用当前发行版"
    else
      pass "Docker engine is reachable"
    fi
    if docker compose version >/dev/null 2>&1; then
      pass "Docker Compose v2 is available"
    else
      fail "需要 docker compose v2 插件"
    fi
  fi
else
  for command in cmake ninja g++ protoc pkg-config; do
    require_command "${command}" "请按 README 的 Ubuntu 24.04 原生依赖命令安装"
  done
  if command -v cmake >/dev/null 2>&1; then
    cmake_major="$(cmake --version | awk 'NR == 1 {split($3, parts, "."); print parts[1]}')"
    cmake_minor="$(cmake --version | awk 'NR == 1 {split($3, parts, "."); print parts[2]}')"
    if (( cmake_major < 3 || (cmake_major == 3 && cmake_minor < 25) )); then
      fail "CMake 需要 >= 3.25"
    fi
  fi
fi

if (( failures > 0 )); then
  printf '\n环境检查失败：%d 项需要处理。上面已经给出对应位置。\n' "${failures}" >&2
  exit 1
fi

printf '\n环境检查通过（%s 模式）。\n' "${mode}"

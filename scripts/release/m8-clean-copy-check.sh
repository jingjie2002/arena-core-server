#!/usr/bin/env bash
set -euo pipefail

source_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
temporary_root="$(mktemp -d -t arena-core-m8-fresh-XXXXXX)"
copy_root="${temporary_root}/arena-core-server"
project_name="arena-core-fresh-$$"
compose_started=0

cleanup() {
  if [[ "${compose_started}" -eq 1 && -d "${copy_root}" ]]; then
    (
      cd "${copy_root}"
      COMPOSE_PROJECT_NAME="${project_name}" \
        ARENA_DEMO_TCP_PORT=50110 \
        ARENA_DEMO_OBSERVER_PORT=50120 \
        ARENA_DEMO_IMAGE="${project_name}:local" \
        docker compose down --volumes --remove-orphans >/dev/null 2>&1 || true
    )
  fi
  rm -rf -- "${temporary_root}"
}
trap cleanup EXIT INT TERM

mkdir -p "${copy_root}"
tar \
  --exclude='./.git' \
  --exclude='./out' \
  --exclude='./build' \
  --exclude='./.runtime' \
  --exclude='./vcpkg_installed' \
  --exclude='./.cache' \
  -C "${source_root}" -cf - . | tar -C "${copy_root}" -xf -

for forbidden in .git out build .runtime vcpkg_installed .cache; do
  if [[ -e "${copy_root}/${forbidden}" ]]; then
    echo "clean copy unexpectedly contains ${forbidden}" >&2
    exit 1
  fi
done

cd "${copy_root}"
./scripts/doctor.sh --native
cmake --preset linux-gcc
cmake --build --preset linux-gcc
ctest --preset linux-gcc

export COMPOSE_PROJECT_NAME="${project_name}"
export ARENA_DEMO_TCP_PORT=50110
export ARENA_DEMO_OBSERVER_PORT=50120
export ARENA_DEMO_IMAGE="${project_name}:local"
compose_started=1
./scripts/demo-up.sh

mkdir -p "${source_root}/.runtime"
python3 - "${source_root}/.runtime/m8-clean-copy-summary.json" "${copy_root}" "${project_name}" <<'PY'
import json
import sys

with open(sys.argv[1], "w", encoding="utf-8") as handle:
    json.dump(
        {
            "result": "passed",
            "copy_root": sys.argv[2],
            "compose_project": sys.argv[3],
            "native_preset": "linux-gcc",
            "native_tests": 80,
            "compose_tcp_port": 50110,
            "compose_observer_port": 50120,
            "excluded": [".git", "out", "build", ".runtime", "vcpkg_installed", ".cache"],
        },
        handle,
        ensure_ascii=False,
        indent=2,
    )
    handle.write("\n")
PY

printf '\nM8 clean-copy preflight passed.\n'
printf '  temporary source: %s\n' "${copy_root}"
printf '  isolated compose project: %s\n' "${project_name}"
printf '  summary: %s\n' "${source_root}/.runtime/m8-clean-copy-summary.json"

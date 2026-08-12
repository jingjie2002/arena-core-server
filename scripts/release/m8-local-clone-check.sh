#!/usr/bin/env bash
set -euo pipefail

source_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
temporary_root="$(mktemp -d -t arena-core-m8-clone-XXXXXX)"
clone_root="${temporary_root}/arena-core-server"
project_name="arena-core-clone-$$"
compose_started=0

cleanup() {
  if [[ "${compose_started}" -eq 1 && -d "${clone_root}" ]]; then
    (
      cd "${clone_root}"
      COMPOSE_PROJECT_NAME="${project_name}" \
        ARENA_DEMO_TCP_PORT=51110 \
        ARENA_DEMO_OBSERVER_PORT=51120 \
        ARENA_DEMO_IMAGE="${project_name}:local" \
        docker compose down --volumes --remove-orphans >/dev/null 2>&1 || true
    )
  fi
  rm -rf -- "${temporary_root}"
}
trap cleanup EXIT INT TERM

git clone --no-local --branch main --single-branch "${source_root}" "${clone_root}"
cd "${clone_root}"

if [[ -n "$(git status --short)" ]]; then
  echo "fresh clone is not clean" >&2
  exit 1
fi

commit="$(git rev-parse HEAD)"
./scripts/doctor.sh --native
cmake --preset linux-gcc
cmake --build --preset linux-gcc
ctest --preset linux-gcc

export COMPOSE_PROJECT_NAME="${project_name}"
export ARENA_DEMO_TCP_PORT=51110
export ARENA_DEMO_OBSERVER_PORT=51120
export ARENA_DEMO_IMAGE="${project_name}:local"
compose_started=1
./scripts/demo-up.sh

mkdir -p "${source_root}/.runtime"
python3 - "${source_root}/.runtime/m8-local-clone-summary.json" "${commit}" "${project_name}" <<'PY'
import json
import sys

with open(sys.argv[1], "w", encoding="utf-8") as handle:
    json.dump(
        {
            "result": "passed",
            "commit": sys.argv[2],
            "git_worktree": "clean",
            "compose_project": sys.argv[3],
            "native_preset": "linux-gcc",
            "native_tests": 80,
            "compose_tcp_port": 51110,
            "compose_observer_port": 51120,
        },
        handle,
        ensure_ascii=False,
        indent=2,
    )
    handle.write("\n")
PY

printf '\nM8 local Git clone verification passed.\n'
printf '  commit: %s\n' "${commit}"
printf '  clone: %s\n' "${clone_root}"
printf '  summary: %s\n' "${source_root}/.runtime/m8-local-clone-summary.json"

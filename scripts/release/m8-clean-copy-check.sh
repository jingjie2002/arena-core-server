#!/usr/bin/env bash
set -euo pipefail

# The M8 filename is retained as a public entrypoint. This is an M12 filesystem
# preflight only; scripts/release/m8-local-clone-check.sh is the Git acceptance gate.
source_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
temporary_root="$(mktemp -d -t arena-core-m12-copy-XXXXXX)"
copy_root="${temporary_root}/arena-core-server"
project_name="arena-core-m12-copy-$$"
summary_path="${source_root}/.runtime/m12-clean-copy-summary.json"
compose_touched=0

cleanup() {
  local status=$?
  trap - EXIT INT TERM

  if [[ "${compose_touched}" -eq 1 && -d "${copy_root}" ]]; then
    (
      cd "${copy_root}"
      docker compose --profile lab exec -T toxiproxy toxiproxy-cli reset \
        >/dev/null 2>&1 || true
      docker compose --profile lab down --volumes --remove-orphans \
        >/dev/null 2>&1 || true
    )
  fi

  case "${temporary_root}" in
    "${TMPDIR:-/tmp}"/arena-core-m12-copy-*)
      rm -rf -- "${temporary_root}"
      ;;
    *)
      echo "refusing to remove unexpected temporary path: ${temporary_root}" >&2
      status=1
      ;;
  esac
  exit "${status}"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

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
for entrypoint in \
  scripts/doctor.sh \
  scripts/demo-up.sh \
  scripts/smoke.sh \
  scripts/lab-smoke.sh \
  scripts/demo-down.sh \
  scripts/release/m8-local-clone-check.sh \
  scripts/release/m8-clean-copy-check.sh; do
  if [[ ! -x "${copy_root}/${entrypoint}" ]]; then
    echo "release entrypoint is not executable: ${entrypoint}" >&2
    exit 1
  fi
done

cd "${copy_root}"
./scripts/doctor.sh --native
cmake --preset linux-gcc
cmake --build --preset linux-gcc
ctest --preset linux-gcc
native_tests="$(
  ctest --preset linux-gcc --show-only=json-v1 |
    python3 -c 'import json,sys; print(len(json.load(sys.stdin).get("tests", [])))'
)"
if [[ ! "${native_tests}" =~ ^[1-9][0-9]*$ ]]; then
  echo "CTest did not discover any native tests" >&2
  exit 1
fi

export COMPOSE_PROJECT_NAME="${project_name}"
export ARENA_DEMO_TCP_PORT=50110
export ARENA_DEMO_OBSERVER_PORT=50120
export ARENA_LAB_PORT=50140
export ARENA_DEMO_IMAGE="${project_name}:local"
compose_touched=1

./scripts/demo-up.sh
./scripts/demo-down.sh
./scripts/demo-up.sh --lab
./scripts/demo-down.sh --lab --reset
compose_touched=0

mkdir -p "${source_root}/.runtime"
python3 - \
  "${summary_path}" \
  "${copy_root}" \
  "${project_name}" \
  "${native_tests}" <<'PY'
import json
import sys

summary = {
    "schema_version": 1,
    "gate": "m12-clean-copy-preflight",
    "result": "passed",
    "verification_kind": "filesystem-copy-preflight",
    "git_acceptance": False,
    "git_acceptance_entrypoint": "scripts/release/m8-local-clone-check.sh",
    "temporary_source": sys.argv[2],
    "excluded": [".git", "out", "build", ".runtime", "vcpkg_installed", ".cache"],
    "release_entrypoints": "executable",
    "native": {
        "compiler": "gcc",
        "preset": "linux-gcc",
        "discovered_tests": int(sys.argv[4]),
        "passed_tests": int(sys.argv[4]),
    },
    "compose": {
        "project": sys.argv[3],
        "default": {
            "smoke": "passed",
            "tcp_port": 50110,
            "observer_port": 50120,
        },
        "lab": {
            "smoke": "passed",
            "tcp_port": 50110,
            "observer_port": 50120,
            "lab_port": 50140,
            "faults_reset": True,
        },
        "volumes_removed": True,
    },
}

with open(sys.argv[1], "w", encoding="utf-8") as handle:
    json.dump(summary, handle, ensure_ascii=False, indent=2)
    handle.write("\n")
PY

printf '\nM12 clean-copy preflight passed (this is not Git acceptance).\n'
printf '  native tests: %s/%s\n' "${native_tests}" "${native_tests}"
printf '  default Compose smoke: passed\n'
printf '  Lab Compose smoke: passed (faults reset, volumes removed)\n'
printf '  Git acceptance entrypoint: scripts/release/m8-local-clone-check.sh\n'
printf '  summary: %s\n' "${summary_path}"

#!/usr/bin/env bash
set -euo pipefail

# The M8 filename is a public compatibility entry. The checks below are the M12 gate.
source_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
temporary_root="$(mktemp -d -t arena-core-m12-clone-XXXXXX)"
clone_root="${temporary_root}/arena-core-server"
project_name="arena-core-m12-clone-$$"
summary_path="${source_root}/.runtime/m12-local-clone-summary.json"
compose_touched=0

cleanup() {
  local status=$?
  trap - EXIT INT TERM

  if [[ "${compose_touched}" -eq 1 && -d "${clone_root}" ]]; then
    (
      cd "${clone_root}"
      docker compose --profile lab exec -T toxiproxy toxiproxy-cli reset \
        >/dev/null 2>&1 || true
      docker compose --profile lab down --volumes --remove-orphans \
        >/dev/null 2>&1 || true
    )
  fi

  case "${temporary_root}" in
    "${TMPDIR:-/tmp}"/arena-core-m12-clone-*)
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

if [[ -n "$(git -C "${source_root}" status --porcelain --untracked-files=all)" ]]; then
  echo "M12 release verification requires a clean source worktree" >&2
  exit 1
fi

requested_commit="${ARENA_RELEASE_COMMIT:-$(git -C "${source_root}" rev-parse HEAD)}"
if ! target_commit="$(git -C "${source_root}" rev-parse --verify "${requested_commit}^{commit}")"; then
  echo "ARENA_RELEASE_COMMIT does not identify a commit: ${requested_commit}" >&2
  exit 1
fi

git clone --no-checkout --no-local "${source_root}" "${clone_root}"
git -C "${clone_root}" checkout --detach "${target_commit}"

clone_commit="$(git -C "${clone_root}" rev-parse HEAD)"
if [[ "${clone_commit}" != "${target_commit}" ]]; then
  echo "clone HEAD does not match the requested release commit" >&2
  exit 1
fi
if [[ -n "$(git -C "${clone_root}" status --porcelain --untracked-files=all)" ]]; then
  echo "M12 detached clone is not clean" >&2
  exit 1
fi
if git -C "${clone_root}" symbolic-ref --quiet HEAD >/dev/null 2>&1; then
  echo "M12 release checkout must use a detached HEAD" >&2
  exit 1
fi

for entrypoint in \
  scripts/doctor.sh \
  scripts/demo-up.sh \
  scripts/smoke.sh \
  scripts/lab-smoke.sh \
  scripts/demo-down.sh \
  scripts/release/m8-local-clone-check.sh \
  scripts/release/m8-clean-copy-check.sh; do
  if [[ ! -x "${clone_root}/${entrypoint}" ]]; then
    echo "release entrypoint is not executable: ${entrypoint}" >&2
    exit 1
  fi
done

cd "${clone_root}"
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
export ARENA_DEMO_TCP_PORT=51110
export ARENA_DEMO_OBSERVER_PORT=51120
export ARENA_LAB_PORT=51140
export ARENA_DEMO_IMAGE="${project_name}:local"
compose_touched=1

./scripts/demo-up.sh
./scripts/demo-down.sh
./scripts/demo-up.sh --lab
./scripts/demo-down.sh --lab --reset
compose_touched=0

if [[ "$(git rev-parse HEAD)" != "${target_commit}" ]] ||
   [[ -n "$(git status --porcelain --untracked-files=all)" ]]; then
  echo "M12 verification changed the detached release checkout" >&2
  exit 1
fi

mkdir -p "${source_root}/.runtime"
python3 - \
  "${summary_path}" \
  "${requested_commit}" \
  "${target_commit}" \
  "${clone_commit}" \
  "${project_name}" \
  "${native_tests}" <<'PY'
import json
import sys

summary = {
    "schema_version": 1,
    "gate": "m12-release-isolation",
    "result": "passed",
    "verification_kind": "git-clone",
    "source_worktree": "clean",
    "requested_commit": sys.argv[2],
    "verified_commit": sys.argv[3],
    "clone_head": sys.argv[4],
    "clone_worktree": "clean",
    "release_entrypoints": "executable",
    "native": {
        "compiler": "gcc",
        "preset": "linux-gcc",
        "discovered_tests": int(sys.argv[6]),
        "passed_tests": int(sys.argv[6]),
    },
    "compose": {
        "project": sys.argv[5],
        "default": {
            "smoke": "passed",
            "tcp_port": 51110,
            "observer_port": 51120,
        },
        "lab": {
            "smoke": "passed",
            "tcp_port": 51110,
            "observer_port": 51120,
            "lab_port": 51140,
            "faults_reset": True,
        },
        "volumes_removed": True,
    },
}

with open(sys.argv[1], "w", encoding="utf-8") as handle:
    json.dump(summary, handle, ensure_ascii=False, indent=2)
    handle.write("\n")
PY

printf '\nM12 isolated Git clone verification passed.\n'
printf '  requested commit: %s\n' "${requested_commit}"
printf '  verified commit: %s\n' "${target_commit}"
printf '  native tests: %s/%s\n' "${native_tests}" "${native_tests}"
printf '  default Compose smoke: passed\n'
printf '  Lab Compose smoke: passed (faults reset, volumes removed)\n'
printf '  summary: %s\n' "${summary_path}"

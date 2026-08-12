#!/usr/bin/env bash
set -euo pipefail

preset="${1:-linux-gcc}"

case "${preset}" in
  linux-gcc|linux-clang|linux-clang-asan-ubsan|linux-gcc-tsan|linux-gcc-vcpkg) ;;
  *)
    echo "unsupported preset: ${preset}" >&2
    exit 2
    ;;
esac

cmake --preset "${preset}"
cmake --build --preset "${preset}"
ctest --preset "${preset}"

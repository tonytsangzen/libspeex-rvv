#!/usr/bin/env bash
# Build helper.
#   ./scripts/build.sh              # native host build (dev sanity checks)
#   ./scripts/build.sh riscv64      # cross build with RVV (needs toolchain)
# Extra args are forwarded to cmake configure, e.g.:
#   ./scripts/build.sh native -DFIXED_POINT=OFF
set -euo pipefail
cd "$(dirname "$0")/.."

preset=${1:-native}
if [ $# -gt 0 ]; then shift; fi

cmake --preset "$preset" "$@"
cmake --build "build/$preset" -j

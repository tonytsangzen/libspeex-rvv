#!/usr/bin/env bash
# A/B bit-exactness harness — the regression gate for this project.
#
# Builds the same tools twice with identical options — once against the
# ported tree (A) and once against the pristine baseline (B) — then
# compares encoded bitstreams and decoded PCM byte-for-byte, for every
# mode (nb/wb/uwb) and quality. Cross-decode (A's stream into B's decoder
# and vice versa) is checked too, so bitstream-format compatibility is
# covered, not just encoder determinism.
#
# Native:
#   ./test/run_ab.sh
# Cross (toolchain + qemu or a remote runner):
#   CMAKE_ARGS="-DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-riscv64-linux.cmake -D_USE_RVV=ON" \
#   RUNNER="qemu-riscv64 -L /usr/riscv64-unknown-linux-gnu" ./test/run_ab.sh
#
# Env overrides: A_DIR (default src), B_DIR (default baseline),
# QUALITIES (default "2 8"), BUILD_A, BUILD_B, DATA.
set -euo pipefail
cd "$(dirname "$0")/.."

A_DIR=${A_DIR:-src}
B_DIR=${B_DIR:-baseline}
BUILD_A=${BUILD_A:-}
BUILD_B=${BUILD_B:-}
CMAKE_ARGS=${CMAKE_ARGS:-}
RUNNER=${RUNNER:-}
QUALITIES=${QUALITIES:-"2 8"}
DATA=${DATA:-}

# Cross builds (CMAKE_ARGS given, e.g. toolchain file) must not reuse the
# native build dirs — CMake caches the compiler and would reject reconfig.
if [ -n "$CMAKE_ARGS" ]; then
   BUILD_A=${BUILD_A:-build/ab-x-a}
   BUILD_B=${BUILD_B:-build/ab-x-b}
   DATA=${DATA:-build/ab-x-data}
else
   BUILD_A=${BUILD_A:-build/ab-a}
   BUILD_B=${BUILD_B:-build/ab-b}
   DATA=${DATA:-build/ab-data}
fi

configure_build() {
   local dir=$1 builddir=$2
   cmake -S . -B "$builddir" -DCMAKE_BUILD_TYPE=Release \
         -DSPEEX_SRC_DIR="$PWD/$dir" $CMAKE_ARGS
   cmake --build "$builddir" -j
}

# Word splitting of RUNNER/CMAKE_ARGS is intentional.
run() { $RUNNER "$@"; }

configure_build "$A_DIR" "$BUILD_A"
configure_build "$B_DIR" "$BUILD_B"
mkdir -p "$DATA"

fail=0
for mode in nb wb uwb; do
   case $mode in
      nb)  rate=8000 ;;
      wb)  rate=16000 ;;
      uwb) rate=32000 ;;
   esac
   in=$DATA/in-$mode.raw
   run "$BUILD_A/test/speex_ab" gen "$rate" 2 "$in"
   for q in $QUALITIES; do
      a=$DATA/a-$mode-q$q.spx
      b=$DATA/b-$mode-q$q.spx
      run "$BUILD_A/test/speex_ab" encode "$mode" "$q" "$in" "$a"
      run "$BUILD_B/test/speex_ab" encode "$mode" "$q" "$in" "$b"
      if ! cmp -s "$a" "$b"; then
         echo "FAIL($mode q$q): bitstreams differ (A=$A_DIR vs B=$B_DIR)"
         fail=1
         continue
      fi
      run "$BUILD_A/test/speex_ab" decode "$mode" "$a" "$DATA/dec-aa-$mode-q$q.raw"
      run "$BUILD_B/test/speex_ab" decode "$mode" "$b" "$DATA/dec-bb-$mode-q$q.raw"
      run "$BUILD_A/test/speex_ab" decode "$mode" "$b" "$DATA/dec-ab-$mode-q$q.raw"
      run "$BUILD_B/test/speex_ab" decode "$mode" "$a" "$DATA/dec-ba-$mode-q$q.raw"
      if ! cmp -s "$DATA/dec-aa-$mode-q$q.raw" "$DATA/dec-bb-$mode-q$q.raw"; then
         echo "FAIL($mode q$q): round-trip PCM differs"
         fail=1
         continue
      fi
      if ! cmp -s "$DATA/dec-aa-$mode-q$q.raw" "$DATA/dec-ab-$mode-q$q.raw"; then
         echo "FAIL($mode q$q): cross-decode PCM differs (A decodes B)"
         fail=1
         continue
      fi
      if ! cmp -s "$DATA/dec-bb-$mode-q$q.raw" "$DATA/dec-ba-$mode-q$q.raw"; then
         echo "FAIL($mode q$q): cross-decode PCM differs (B decodes A)"
         fail=1
         continue
      fi
      size=$(stat -f%z "$a" 2>/dev/null || stat -c%s "$a")
      echo "PASS $mode q$q (bitstream $size bytes)"
   done
done

if [ "$fail" -eq 0 ]; then
   echo "== A/B bit-exactness: ALL PASS (A=$A_DIR, B=$B_DIR) =="
else
   echo "== A/B bit-exactness: FAILED =="
   exit 1
fi

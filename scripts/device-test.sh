#!/usr/bin/env bash
# Build (cross, static) both libspeex trees and run tests/benchmarks on the
# RISC-V test board over adb (SpacemiT X60, Bianbu Linux).
#
#   ./scripts/device-test.sh build    # docker cross-build, static binaries
#   ./scripts/device-test.sh deploy   # adb push binaries to the board
#   ./scripts/device-test.sh ab       # on-device A/B bit-exactness (vs baseline)
#   ./scripts/device-test.sh bench    # on-device bench_e2e, tree A (src) and B (baseline)
#   ./scripts/device-test.sh          # all of the above
#
# Env overrides: SERIAL (adb serial), REMOTE (device work dir), BUILD_A/BUILD_B.
# The A tree is the port (src/), B is the pristine reference (baseline/).
# Binaries are static (-static) so glibc on the board does not matter.
set -euo pipefail
cd "$(dirname "$0")/.."

SERIAL=${SERIAL:-BPMIK102080640200}
adb_s() { adb -s "$SERIAL" "$@"; }
REMOTE=${REMOTE:-/tmp/speex-rvv}
BUILDENV_IMAGE=${BUILDENV_IMAGE:-speex-rvv-buildenv}
BUILD_A=${BUILD_A:-build/device-a}
BUILD_B=${BUILD_B:-build/device-b}
BENCH_SECONDS=${BENCH_SECONDS:-4}
BENCH_REPS=${BENCH_REPS:-5}

ensure_image() {
   if ! docker image inspect "$BUILDENV_IMAGE" >/dev/null 2>&1; then
      echo "== building docker buildenv image (one-time) =="
      docker build -q -t "$BUILDENV_IMAGE" -f scripts/Dockerfile.buildenv scripts/
   fi
}

do_build() {
   ensure_image
   docker run --rm -v "$PWD":/work -w /work \
      -e BUILD_A="$BUILD_A" -e BUILD_B="$BUILD_B" -e RVV_MARCH=${RVV_MARCH:-rv64gcv} \
      -e RVV_KERNELS=${RVV_KERNELS:-ON} -e SPEEX_PROF=${SPEEX_PROF:-OFF} \
      -e SPEEX_OPT_FLAGS="${SPEEX_OPT_FLAGS:-}" \
      "$BUILDENV_IMAGE" bash -euc '
      set -e
      common=(-DCMAKE_BUILD_TYPE=Release
              -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-riscv64-linux.cmake
              -D_USE_RVV=ON -DRVV_KERNELS=${RVV_KERNELS:-ON} -DRVV_MARCH=$RVV_MARCH
              -DSPEEX_PROF=${SPEEX_PROF:-OFF} -DSPEEX_OPT_FLAGS="${SPEEX_OPT_FLAGS}"
              -DCMAKE_EXE_LINKER_FLAGS=-static)
      cmake -S . -B "$BUILD_A" "${common[@]}"
      cmake --build "$BUILD_A" -j"$(nproc)"
      cmake -S . -B "$BUILD_B" "${common[@]}" -DSPEEX_SRC_DIR=/work/baseline
      # B (baseline) has no RVV symbols: build only the artifacts the A/B
      # and bench steps consume, not the kernel diff tests.
      cmake --build "$BUILD_B" -j"$(nproc)" --target speex speex_ab bench_e2e testenc testenc_wb testenc_uwb
      for b in "$BUILD_A/test/speex_ab" "$BUILD_A/benchmark/bench_e2e"; do
         if readelf -l "$b" | grep -q INTERP; then
            echo "ERROR: $b is not static"; exit 1
         fi
      done
   '
   echo "== build done =="
}

do_deploy() {
   adb_s shell "mkdir -p $REMOTE"
   adb_s push "$BUILD_A/test/speex_ab"        "$REMOTE/speex_ab_a" > /dev/null
   adb_s push "$BUILD_B/test/speex_ab"        "$REMOTE/speex_ab_b" > /dev/null
   adb_s push "$BUILD_A/benchmark/bench_e2e"  "$REMOTE/bench_a"      > /dev/null
   adb_s push "$BUILD_B/benchmark/bench_e2e"  "$REMOTE/bench_b"      > /dev/null
   echo "== deploy done ($REMOTE) =="
}

prepare_bench_env() {
   # Best effort: pin frequency for stable numbers; works when adbd is root.
   adb_s shell "if [ \"\$(id -u)\" = 0 ]; then for f in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo performance > \$f 2>/dev/null; done; fi; true"
   adb_s shell "cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq 2>/dev/null; grep -m1 -o 'rv64[a-z0-9_]*' /proc/cpuinfo" || true
}

hash_one() { # $1=file
   adb_s shell "cd $REMOTE && sha256sum $1" | tr -d '\r' | awk '{print $1}'
}

do_ab() {
   do_build    # freshness: never test binaries from an older source state
   do_deploy
   prepare_bench_env
   adb_s shell "cd $REMOTE && ./speex_ab_a gen 8000 2 in-nb.raw && ./speex_ab_a gen 16000 2 in-wb.raw && ./speex_ab_a gen 32000 2 in-uwb.raw"
   fail=0
   for mode in nb wb uwb; do
      for q in 2 8; do
         adb_s shell "cd $REMOTE && \
            ./speex_ab_a encode $mode $q in-$mode.raw a-$mode-q$q.spx && \
            ./speex_ab_b encode $mode $q in-$mode.raw b-$mode-q$q.spx && \
            ./speex_ab_a decode $mode a-$mode-q$q.spx dec-aa-$mode-q$q.raw && \
            ./speex_ab_b decode $mode b-$mode-q$q.spx dec-bb-$mode-q$q.raw && \
            ./speex_ab_a decode $mode b-$mode-q$q.spx dec-ab-$mode-q$q.raw"
         ha=$(hash_one "a-$mode-q$q.spx")
         hb=$(hash_one "b-$mode-q$q.spx")
         haa=$(hash_one "dec-aa-$mode-q$q.raw")
         hbb=$(hash_one "dec-bb-$mode-q$q.raw")
         hab=$(hash_one "dec-ab-$mode-q$q.raw")
         if [ -n "$ha" ] && [ "$ha" = "$hb" ] && [ "$haa" = "$hbb" ] && [ "$haa" = "$hab" ]; then
            echo "PASS $mode q$q (device A/B bit-exact)"
         else
            echo "FAIL $mode q$q (device A/B mismatch)"
            fail=1
         fi
      done
   done
   [ "$fail" -eq 0 ] && echo "== on-device A/B: ALL PASS ==" || { echo "== on-device A/B: FAILED =="; exit 1; }
}

do_benchp() {
   # Paired A/B benchmark (review R10/M3): alternate A and B for
   # BENCH_PAIRS rounds per mode/quality; report the median B/A time ratio
   # with min..max, which is robust against session drift.
   do_build
   do_deploy
   pairs=${BENCH_PAIRS:-5}
   adb_s shell "cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor" > /dev/null
   for mode in nb wb uwb; do
      for q in 8 2; do
         rm -f /tmp/pairs.txt
         for p in $(seq 1 $pairs); do
            for side in a b; do
               if [ $side = a ]; then bin=bench_a; else bin=bench_b; fi
               out=$(adb_s shell "cd $REMOTE && ./$bin $mode $q 2 3 2>/dev/null")
               e=$(echo "$out" | grep -o "encode: *[0-9.]*" | grep -o "[0-9.]*" | head -1)
               d=$(echo "$out" | grep -o "decode: *[0-9.]*" | grep -o "[0-9.]*" | head -1)
               echo "$e $d" >> /tmp/pairs.txt
            done
         done
         cp /tmp/pairs.txt build/pairs-$mode-q$q.txt 2>/dev/null || true
         awk -v mode=$mode -v q=$q -v pairs=$pairs '
            { e[NR]=$1; d[NR]=$2 }
            END {
              for (i = 1; i <= pairs; i++) { k = 2*i - 1; er[i] = e[k+1]/e[k]; dr[i] = d[k+1]/d[k] }
              # insertion sort (n=5)
              for (i = 1; i <= pairs; i++) { a=er[i]; for (j=i-1;j>=1&&er[j]>a;j--) er[j+1]=er[j]; er[j+1]=a }
              for (i = 1; i <= pairs; i++) { a=dr[i]; for (j=i-1;j>=1&&dr[j]>a;j--) dr[j+1]=dr[j]; dr[j+1]=a }
              m = int(pairs/2) + 1
              printf("%s q%d B/A: enc median %.3f [%.3f..%.3f]  dec median %.3f [%.3f..%.3f]\n",
                     mode, q, er[m], er[1], er[pairs], dr[m], dr[1], dr[pairs]);
            }' /tmp/pairs.txt
      done
   done
}

do_bench() {
   do_build    # freshness (review R10)
   do_deploy
   prepare_bench_env
   for mode in nb wb uwb; do
      for q in 2 8; do
         echo "--- A(src) $mode q$q ---"
         adb_s shell "cd $REMOTE && ./bench_a $mode $q $BENCH_SECONDS $BENCH_REPS"
         echo "--- B(baseline) $mode q$q ---"
         adb_s shell "cd $REMOTE && ./bench_b $mode $q $BENCH_SECONDS $BENCH_REPS"
      done
   done
}

if [ $# -eq 0 ]; then set -- all; fi
for cmd in "$@"; do
   case "$cmd" in
      build)  do_build ;;
      deploy) do_deploy ;;
      ab)     do_ab ;;
      bench)  do_bench ;;
      benchp) do_benchp ;;
      all)    do_build; do_deploy; do_ab; do_bench ;;
      *) echo "usage: $0 [build|deploy|ab|bench|all]"; exit 1 ;;
   esac
done

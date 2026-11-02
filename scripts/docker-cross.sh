#!/usr/bin/env bash
# Cross-build and run the full A/B suite inside Docker — no local RISC-V
# toolchain required. Uses Ubuntu's riscv64 cross-compiler plus qemu-user
# to execute the binaries. Results land in build/ (mounted from the repo).
#
#   ./scripts/docker-cross.sh
#
# Note: apt + qemu run under emulation can take a few minutes on the
# first run; nothing is cached between runs.
set -euo pipefail
cd "$(dirname "$0")/.."

docker run --rm -v "$PWD":/work -w /work ubuntu:24.04 bash -euc '
   export DEBIAN_FRONTEND=noninteractive
   apt-get update -qq
   apt-get install -y -qq build-essential cmake gcc-riscv64-linux-gnu qemu-user > /dev/null

   export CMAKE_ARGS="-DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-riscv64-linux.cmake -D_USE_RVV=ON -DRVV_MARCH=rv64gcv"
   export RUNNER="qemu-riscv64 -L /usr/riscv64-linux-gnu"

   cmake -S . -B build/riscv64-docker -DCMAKE_BUILD_TYPE=Release \
         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-riscv64-linux.cmake \
         -D_USE_RVV=ON -DRVV_MARCH=rv64gcv
   cmake --build build/riscv64-docker -j"$(nproc)"

   ./test/run_ab.sh
   $RUNNER build/riscv64-docker/test/testenc \
      build/ab-x-data/in-nb.raw build/riscv64-docker/testenc.out.raw
   $RUNNER build/riscv64-docker/benchmark/bench_e2e wb 8 2
'

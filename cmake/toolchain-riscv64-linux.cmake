# RISC-V 64 Linux (glibc) cross toolchain.
#
# Works out of the box with Debian/Ubuntu's cross compiler:
#   apt install gcc-riscv64-unknown-linux-gnu
# Override the prefix if your toolchain names its binaries differently:
#   cmake ... -DRISCV64_PREFIX=riscv64-linux-gnu-
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR riscv64)

set(RISCV64_PREFIX "riscv64-unknown-linux-gnu-" CACHE STRING "RISC-V cross toolchain prefix")
find_program(RISCV64_C_COMPILER NAMES "${RISCV64_PREFIX}gcc" "riscv64-linux-gnu-gcc" REQUIRED)
set(CMAKE_C_COMPILER "${RISCV64_C_COMPILER}")

# Cross toolchains name their sysroot differently per distro:
#   Debian gcc-riscv64-unknown-linux-gnu -> /usr/riscv64-unknown-linux-gnu
#   Ubuntu gcc-riscv64-linux-gnu         -> /usr/riscv64-linux-gnu
# qemu-user needs the same path at run time: qemu-riscv64 -L <sysroot> ...
if(EXISTS /usr/riscv64-unknown-linux-gnu)
  set(CMAKE_FIND_ROOT_PATH /usr/riscv64-unknown-linux-gnu)
elseif(EXISTS /usr/riscv64-linux-gnu)
  set(CMAKE_FIND_ROOT_PATH /usr/riscv64-linux-gnu)
endif()
if(DEFINED CMAKE_FIND_ROOT_PATH)
  set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
  set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
  set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
endif()

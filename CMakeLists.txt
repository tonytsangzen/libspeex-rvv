cmake_minimum_required(VERSION 3.20)
project(libspeex-rvv C)

# Which libspeex tree to build. Defaults to the ported working copy (src/);
# point it at baseline/ to build the untouched reference for A/B testing
# (test/run_ab.sh does exactly that).
set(SPEEX_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src" CACHE PATH
    "libspeex tree to build (must contain libspeex/ and include/speex/)")

option(FIXED_POINT "Fixed-point build (the porting target for embedded RISC-V)" ON)
option(_USE_RVV    "Build for a RISC-V V-extension target (-march=$(RVV_MARCH))" OFF)
option(RVV_KERNELS "Enable the RVV kernel overrides under src/libspeex/rvv/ (requires _USE_RVV). OFF = compiler autovec only, for baselines" ON)
set(RVV_MARCH "rv64gcv" CACHE STRING "-march value used when _USE_RVV is ON")

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_C_STANDARD 99)

set(CONF_DIR "${CMAKE_CURRENT_SOURCE_DIR}/cmake/conf")
configure_file("${CONF_DIR}/config.h.in" "${CMAKE_CURRENT_BINARY_DIR}/config.h" @ONLY)

# Explicit source list, identical to the upstream meson default set with the
# KISS FFT backend. Deliberately excludes testenc*/smallft.c/vorbis_psy.c so
# both this build and the baseline A/B build stay deterministic.
set(SPEEX_SOURCES
  ${SPEEX_SRC_DIR}/libspeex/bits.c
  ${SPEEX_SRC_DIR}/libspeex/cb_search.c
  ${SPEEX_SRC_DIR}/libspeex/exc_10_16_table.c
  ${SPEEX_SRC_DIR}/libspeex/exc_10_32_table.c
  ${SPEEX_SRC_DIR}/libspeex/exc_20_32_table.c
  ${SPEEX_SRC_DIR}/libspeex/exc_5_256_table.c
  ${SPEEX_SRC_DIR}/libspeex/exc_5_64_table.c
  ${SPEEX_SRC_DIR}/libspeex/exc_8_128_table.c
  ${SPEEX_SRC_DIR}/libspeex/filters.c
  ${SPEEX_SRC_DIR}/libspeex/gain_table.c
  ${SPEEX_SRC_DIR}/libspeex/gain_table_lbr.c
  ${SPEEX_SRC_DIR}/libspeex/hexc_10_32_table.c
  ${SPEEX_SRC_DIR}/libspeex/hexc_table.c
  ${SPEEX_SRC_DIR}/libspeex/high_lsp_tables.c
  ${SPEEX_SRC_DIR}/libspeex/kiss_fft.c
  ${SPEEX_SRC_DIR}/libspeex/kiss_fftr.c
  ${SPEEX_SRC_DIR}/libspeex/lpc.c
  ${SPEEX_SRC_DIR}/libspeex/lsp.c
  ${SPEEX_SRC_DIR}/libspeex/lsp_tables_nb.c
  ${SPEEX_SRC_DIR}/libspeex/ltp.c
  ${SPEEX_SRC_DIR}/libspeex/modes.c
  ${SPEEX_SRC_DIR}/libspeex/modes_wb.c
  ${SPEEX_SRC_DIR}/libspeex/nb_celp.c
  ${SPEEX_SRC_DIR}/libspeex/quant_lsp.c
  ${SPEEX_SRC_DIR}/libspeex/sb_celp.c
  ${SPEEX_SRC_DIR}/libspeex/speex.c
  ${SPEEX_SRC_DIR}/libspeex/speex_callbacks.c
  ${SPEEX_SRC_DIR}/libspeex/speex_header.c
  ${SPEEX_SRC_DIR}/libspeex/stereo.c
  ${SPEEX_SRC_DIR}/libspeex/vbr.c
  ${SPEEX_SRC_DIR}/libspeex/vq.c
  ${SPEEX_SRC_DIR}/libspeex/window.c
)

add_library(speex ${SPEEX_SOURCES})
target_include_directories(speex
  PUBLIC
    "${SPEEX_SRC_DIR}/include"
    "${CONF_DIR}"
  PRIVATE
    "${SPEEX_SRC_DIR}/libspeex"
    "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_definitions(speex PRIVATE HAVE_CONFIG_H)

if(FIXED_POINT)
  target_compile_definitions(speex PRIVATE FIXED_POINT)
else()
  target_compile_definitions(speex PRIVATE FLOATING_POINT)
endif()
if(_USE_RVV)
  target_compile_options(speex PRIVATE -march=${RVV_MARCH})
  if(RVV_KERNELS)
    target_compile_definitions(speex PRIVATE _USE_RVV)
  endif()
endif()
if(UNIX)
  target_link_libraries(speex PUBLIC m)
endif()

add_subdirectory(test)
add_subdirectory(benchmark)

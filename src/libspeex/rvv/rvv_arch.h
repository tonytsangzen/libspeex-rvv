/* Shared prelude for the RVV kernels in this directory.
 * Include this before <riscv_vector.h> usage in every *_rvv.h. */
#ifndef SPEEX_RVV_ARCH_H
#define SPEEX_RVV_ARCH_H

#if !defined(__riscv) || !defined(__riscv_vector)
#error "RVV kernels need a RISC-V target built with the V extension (-march=rv64gcv or later)"
#endif

#include <riscv_vector.h>

/* This project targets the ratified RVV 1.0 intrinsics ABI
 * (gcc >= 12, clang >= 15). T-Head C9xx cores (C910/C920/C906) ship the
 * draft RVV 0.7.1 with a different intrinsics surface; if such a core is
 * the target, put its compatibility shims here behind SPEEX_RVV_THEAD
 * and keep the helper names below stable so kernels don't care. */
#define SPEEX_RVV_1P0 1

/* Kernels must stay length-agnostic: always size loops with vsetvl*,
 * never with a compile-time VLEN. Add shared helpers here as they
 * emerge (e.g. fixed-point saturating MAC helpers that replicate the
 * exact rounding of arch.h macros). */

#endif /* SPEEX_RVV_ARCH_H */

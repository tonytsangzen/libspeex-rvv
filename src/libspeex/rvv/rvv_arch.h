/* Shared prelude for the RVV kernels in this directory.
 * Include this before <riscv_vector.h> usage in every *_rvv.h. */
#ifndef SPEEX_RVV_ARCH_H
#define SPEEX_RVV_ARCH_H

#if !defined(__riscv) || !defined(__riscv_vector)
#error "RVV kernels need a RISC-V target built with the V extension (-march=rv64gcv or later)"
#endif

#include <riscv_vector.h>

/* ISA vs intrinsics API are different version axes (review section 2):
 * the ISA here is RVV 1.0 (__riscv_v == 1000000), while gcc 13.3's
 * intrinsics surface is the v0.11 C API (__riscv_v_intrinsic == 11000) —
 * do not read the ISA macro as an API guarantee. Everything this project
 * uses has been verified against gcc 13.3's actual behaviour, including
 * the policy suffixes (_tu) and the assembly operand order of vmerge;
 * segment tuple types (vint16m1x4_t and the vlseg intrinsics) are NOT
 * available in gcc 13. T-Head C9xx cores ship the draft RVV 0.7.1 with a
 * different intrinsics surface; if such a core is the target, put its
 * compatibility shims here behind SPEEX_RVV_THEAD and keep the helper
 * names below stable so kernels don't care. */
#define SPEEX_RVV_1P0 1

/* Segment tuple types (vlseg4e16 / vint16m1x4_t) need the v1.0
 * intrinsics API (gcc-14+, __riscv_v_intrinsic >= 12000). When absent
 * (gcc-13 = v0.11 API) the dot kernels fall back to per-field strided
 * loads with identical values.
 *
 * Adoption verdict (round 24, X60 paired measurement, fresh builds only):
 * all four dot kernels ship segment loads by default. An earlier
 * "-2..-27% encode regression" for the combined build turned out to be a
 * stale-object artifact (incremental rebuild kept an old nb_celp.o after
 * the kernel-gating macros changed; scripts/device-test.sh now wipes the
 * build dirs). Fresh paired runs: pitch_xcorr-only SEG +4.3..+9.1% encode
 * vs all-strided; full SEG ties that on encode and adds +2.8..+6.6%
 * decode, so SEG-everywhere is the default. Per-kernel ablation switches
 * (R24_NO_IP / R24_NO_B5 / R24_NO_SUMSQ / R24_NO_PX) remain available. */
#if defined(__riscv_v_intrinsic) && __riscv_v_intrinsic >= 12000
#define SPEEX_RVV_SEGV4 1
#define SPEEX_RVV_SEG_DOT_ALL 1
#endif
/* Turn SPEEX_RVV_SEG_DOT_ALL off to keep inner_prod/batch5/sumsq on the
 * strided path (segment loads stay bit-exact either way). */

/* Cross-TU RVV helpers shared between module overrides (definitions live
 * in the module headers; prototypes here so any override can call them). */
#if defined(FIXED_POINT)
void pitch_xcorr(const spx_word16_t *_x, const spx_word16_t *_y, spx_word32_t *corr, int len, int nb_pitch, char *stack);
void rvv_inner_prod_batch5(const spx_word16_t *a, const spx_word16_t *b, const spx_word16_t *x,
                           int len, spx_word32_t *out);
void rvv_gain_corr9(const spx_word16_t *x0, const spx_word16_t *x1, const spx_word16_t *x2,
                    const spx_word16_t *nt, int len, spx_word32_t *c, spx_word32_t *a);
#endif

/* Kernels must stay length-agnostic: always size loops with vsetvl*,
 * never with a compile-time VLEN. Add shared helpers here as they
 * emerge (e.g. fixed-point saturating MAC helpers that replicate the
 * exact rounding of arch.h macros). */

#endif /* SPEEX_RVV_ARCH_H */

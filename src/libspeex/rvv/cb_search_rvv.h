/* RVV implementation of compute_weighted_codebook (cb_search.c).
 *
 * Entry-parallel like vq_rvv.h: one lane per codebook entry, j-major loop.
 * Per entry the scalar reference computes a triangular convolution
 *   resj = sum_{k<=j} shape[k]*r[j-k]            (int32 wrap adds)
 *   res[j] = (int16)(resj >> 13)                 (EXTRACT16 is a plain cast)
 *   E[i]  += res[j]*res[j]                       (int32 wrap adds)
 * All adds are two's-complement so lane-wise (per-entry) accumulation in
 * the same k/j order is bit-exact. The shape columns (signed char) are
 * loaded once per group with strided loads and sign-extended.
 *
 * Requires the VAR_ARRAYS stack_alloc mode (uses VLAs of vector types).
 * Only valid for the fixed-point build; otherwise cb_search.c falls back
 * to the C reference. */
#ifndef SPEEX_CB_SEARCH_RVV_H
#define SPEEX_CB_SEARCH_RVV_H

#include "rvv_arch.h"

#if defined(FIXED_POINT)

#define OVERRIDE_COMPUTE_WEIGHTED_CODEBOOK 1

static void compute_weighted_codebook(const signed char *shape_cb, const spx_word16_t *r, spx_word16_t *resp, spx_word16_t *resp2, spx_word32_t *E, int shape_cb_size, int subvect_size, char *stack)
{
   size_t vlmax = __riscv_vsetvlmax_e32m2();
   size_t off = 0;
   int j, k;
   (void)resp2;
   (void)stack;

   while (off < (size_t)shape_cb_size)
   {
      size_t vl = __riscv_vsetvl_e16m1((size_t)shape_cb_size - off);
      const signed char *scb = shape_cb + off * (size_t)subvect_size;
      ptrdiff_t sstride = (ptrdiff_t)subvect_size; /* int8 elements: bytes == elems */
      ptrdiff_t rstride = (ptrdiff_t)(2 * subvect_size);
      vint32m2_t energy = __riscv_vmv_v_x_i32m2(0, vlmax);
      /* Shape columns parked in a scalar buffer: gcc does not allow arrays
       * of RVV vector types, and reloading from here is a cheap unit-stride
       * load per (j,k). */
      spx_word16_t sbuf[subvect_size * vlmax];

      for (k = 0; k < subvect_size; k++)
      {
         vint8mf2_t raw = __riscv_vlse8_v_i8mf2(scb + k, sstride, vl);
         __riscv_vse16_v_i16m1(sbuf + k * vlmax, __riscv_vsext_vf2_i16m1(raw, vl), vl);
      }

      for (j = 0; j < subvect_size; j++)
      {
         vint32m2_t part = __riscv_vmv_v_x_i32m2(0, vlmax);
         vint16m1_t res16;
         for (k = 0; k <= j; k++)
         {
            vint16m1_t scol = __riscv_vle16_v_i16m1(sbuf + k * vlmax, vl);
            part = __riscv_vwmacc_vx_i32m2(part, r[j - k], scol, vl);
         }
         part = __riscv_vsra_vx_i32m2(part, 13, vl);
         res16 = __riscv_vncvt_x_x_w_i16m1(part, vl);
         energy = __riscv_vwmacc_vv_i32m2(energy, res16, res16, vl);
         __riscv_vsse16_v_i16m1(resp + off * (size_t)subvect_size + j, rstride, res16, vl);
      }
      __riscv_vse32_v_i32m2(E + off, energy, vl);
      off += vl;
   }
}

#endif /* FIXED_POINT */

#if defined(FIXED_POINT) && !defined(DISABLE_ENCODER) && defined(OVERRIDE_TARGET_UPDATE)

/* t[n] = SUB16(t[n], PSHR32(MULT16_16(g, r[n]), 13)), element-wise.
 * PSHR32 = (a + (1<<(shift-1))) >> shift (arithmetic); SUB16 is a
 * truncating 16-bit subtract (wrap). All exact with vector ops. */
#define OVERRIDE_TARGET_UPDATE 1
static inline void target_update(spx_word16_t *t, spx_word16_t g, spx_word16_t *r, int len)
{
   size_t vlmax = __riscv_vsetvlmax_e32m2();
   size_t off = 0, n = (size_t)len;
   while (n > 0)
   {
      size_t vl = __riscv_vsetvl_e16m1(n);
      vint16m1_t rv = __riscv_vle16_v_i16m1(r + off, vl);
      vint16m1_t tv = __riscv_vle16_v_i16m1(t + off, vl);
      vint32m2_t prod = __riscv_vmv_v_x_i32m2(0, vlmax);
      prod = __riscv_vwmacc_vx_i32m2(prod, g, rv, vl);
      prod = __riscv_vadd_vx_i32m2(prod, 1 << 12, vl);
      prod = __riscv_vsra_vx_i32m2(prod, 13, vl);
      tv = __riscv_vsub_vv_i16m1(tv, __riscv_vncvt_x_x_w_i16m1(prod, vl), vl);
      __riscv_vse16_v_i16m1(t + off, tv, vl);
      off += vl;
      n -= vl;
   }
}

#endif /* FIXED_POINT && !DISABLE_ENCODER */

#endif /* SPEEX_CB_SEARCH_RVV_H */

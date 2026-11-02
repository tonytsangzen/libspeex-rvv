/* RVV implementation of inner_prod (ltp.c).
 *
 * Bit-exactness argument: the scalar reference processes len>>2 blocks of
 * four samples; per block it accumulates the exact int16*int16 products
 * into an int32 (wrap-around), shifts that block sum right by 6
 * (arithmetic, truncating) and adds the shifted value into the running
 * total (wrap-around). Vector lane j below covers block j exactly:
 * strided loads (stride 8 bytes) feed one block per lane, the four
 * widen-MACs reproduce the block sum in the same order, vsra reproduces
 * the per-block shift, and the final reduction sums shifted blocks —
 * all adds are two's-complement, so order does not matter modulo 2^32.
 * Truncating len to a multiple of 4 is replicated by looping over
 * len>>2 blocks only.
 *
 * NB: gcc 13's reduction intrinsics take (vector, scalar-carrier); the
 * RVV C-API document lists (carrier, vector). See rvv_arch.h notes. */
#ifndef SPEEX_MATH_RVV_H
#define SPEEX_MATH_RVV_H

#include "rvv_arch.h"

#if defined(FIXED_POINT)

#define OVERRIDE_INNER_PROD 1

static inline spx_word32_t rvv_inner_prod(const spx_word16_t *x, const spx_word16_t *y, int len)
{
   size_t vlmax = __riscv_vsetvlmax_e32m2();
   vint32m2_t acc = __riscv_vmv_v_x_i32m2(0, vlmax);
   vint32m1_t zero = __riscv_vmv_v_x_i32m1(0, 1);
   const int16_t *px = (const int16_t *)x;
   const int16_t *py = (const int16_t *)y;
   size_t vl, n = (size_t)len >> 2; /* scalar reference truncates len to blocks of 4 */
   while (n > 0)
   {
      vl = __riscv_vsetvl_e16m1(n);
      vint16m1_t x0 = __riscv_vlse16_v_i16m1(px + 0, 4 * sizeof(spx_word16_t), vl);
      vint16m1_t x1 = __riscv_vlse16_v_i16m1(px + 1, 4 * sizeof(spx_word16_t), vl);
      vint16m1_t x2 = __riscv_vlse16_v_i16m1(px + 2, 4 * sizeof(spx_word16_t), vl);
      vint16m1_t x3 = __riscv_vlse16_v_i16m1(px + 3, 4 * sizeof(spx_word16_t), vl);
      vint16m1_t y0 = __riscv_vlse16_v_i16m1(py + 0, 4 * sizeof(spx_word16_t), vl);
      vint16m1_t y1 = __riscv_vlse16_v_i16m1(py + 1, 4 * sizeof(spx_word16_t), vl);
      vint16m1_t y2 = __riscv_vlse16_v_i16m1(py + 2, 4 * sizeof(spx_word16_t), vl);
      vint16m1_t y3 = __riscv_vlse16_v_i16m1(py + 3, 4 * sizeof(spx_word16_t), vl);
      vint32m2_t part = __riscv_vmv_v_x_i32m2(0, vlmax);
      part = __riscv_vwmacc_vv_i32m2(part, x0, y0, vl);
      part = __riscv_vwmacc_vv_i32m2(part, x1, y1, vl);
      part = __riscv_vwmacc_vv_i32m2(part, x2, y2, vl);
      part = __riscv_vwmacc_vv_i32m2(part, x3, y3, vl);
      part = __riscv_vsra_vx_i32m2(part, 6, vl);
      acc = __riscv_vadd_vv_i32m2(acc, part, vl);
      px += 4 * vl;
      py += 4 * vl;
      n -= vl;
   }
   return (spx_word32_t)__riscv_vmv_x_s_i32m1_i32(
      __riscv_vredsum_vs_i32m2_i32m1(acc, zero, vlmax));
}

spx_word32_t inner_prod(const spx_word16_t *x, const spx_word16_t *y, int len)
{
   return rvv_inner_prod(x, y, len);
}

/* Batched pitch_xcorr: compute 4 lags per pass with independent
 * accumulators so the (serial-latency) reduction happens once per 4 lags
 * instead of once per lag, and the x[] strided loads are shared. Each lag
 * uses exactly the rvv_inner_prod blocked-shift scheme, so results are
 * bit-exact vs the scalar pitch_xcorr/inner_prod pair. Leftover lags
 * (nb_pitch not a multiple of 4) fall back to rvv_inner_prod. */
#define OVERRIDE_PITCH_XCORR 1
static void pitch_xcorr(const spx_word16_t *_x, const spx_word16_t *_y, spx_word32_t *corr, int len, int nb_pitch, char *stack)
{
   int blocks = len >> 2;
   size_t vlmax = __riscv_vsetvlmax_e32m2();
   vint32m1_t zero = __riscv_vmv_v_x_i32m1(0, 1);
   int i = 0;
   int lag;
   (void)stack;

   while (i + 4 <= nb_pitch)
   {
      vint32m2_t acc0 = __riscv_vmv_v_x_i32m2(0, vlmax);
      vint32m2_t acc1 = __riscv_vmv_v_x_i32m2(0, vlmax);
      vint32m2_t acc2 = __riscv_vmv_v_x_i32m2(0, vlmax);
      vint32m2_t acc3 = __riscv_vmv_v_x_i32m2(0, vlmax);
      const int16_t *px = (const int16_t *)_x;
      const int16_t *py0 = (const int16_t *)_y + i;
      const int16_t *py1 = py0 + 1;
      const int16_t *py2 = py0 + 2;
      const int16_t *py3 = py0 + 3;
      size_t vl, n = (size_t)blocks;
      while (n > 0)
      {
         vl = __riscv_vsetvl_e16m1(n);
         vint16m1_t x0 = __riscv_vlse16_v_i16m1(px + 0, 8, vl);
         vint16m1_t x1 = __riscv_vlse16_v_i16m1(px + 1, 8, vl);
         vint16m1_t x2 = __riscv_vlse16_v_i16m1(px + 2, 8, vl);
         vint16m1_t x3 = __riscv_vlse16_v_i16m1(px + 3, 8, vl);
         vint16m1_t ya0 = __riscv_vlse16_v_i16m1(py0 + 0, 8, vl);
         vint16m1_t ya1 = __riscv_vlse16_v_i16m1(py0 + 1, 8, vl);
         vint16m1_t ya2 = __riscv_vlse16_v_i16m1(py0 + 2, 8, vl);
         vint16m1_t ya3 = __riscv_vlse16_v_i16m1(py0 + 3, 8, vl);
         vint16m1_t yb0 = __riscv_vlse16_v_i16m1(py1 + 0, 8, vl);
         vint16m1_t yb1 = __riscv_vlse16_v_i16m1(py1 + 1, 8, vl);
         vint16m1_t yb2 = __riscv_vlse16_v_i16m1(py1 + 2, 8, vl);
         vint16m1_t yb3 = __riscv_vlse16_v_i16m1(py1 + 3, 8, vl);
         vint16m1_t yc0 = __riscv_vlse16_v_i16m1(py2 + 0, 8, vl);
         vint16m1_t yc1 = __riscv_vlse16_v_i16m1(py2 + 1, 8, vl);
         vint16m1_t yc2 = __riscv_vlse16_v_i16m1(py2 + 2, 8, vl);
         vint16m1_t yc3 = __riscv_vlse16_v_i16m1(py2 + 3, 8, vl);
         vint16m1_t yd0 = __riscv_vlse16_v_i16m1(py3 + 0, 8, vl);
         vint16m1_t yd1 = __riscv_vlse16_v_i16m1(py3 + 1, 8, vl);
         vint16m1_t yd2 = __riscv_vlse16_v_i16m1(py3 + 2, 8, vl);
         vint16m1_t yd3 = __riscv_vlse16_v_i16m1(py3 + 3, 8, vl);
         vint32m2_t part;

         part = __riscv_vmv_v_x_i32m2(0, vlmax);
         part = __riscv_vwmacc_vv_i32m2(part, x0, ya0, vl);
         part = __riscv_vwmacc_vv_i32m2(part, x1, ya1, vl);
         part = __riscv_vwmacc_vv_i32m2(part, x2, ya2, vl);
         part = __riscv_vwmacc_vv_i32m2(part, x3, ya3, vl);
         acc0 = __riscv_vadd_vv_i32m2(acc0, __riscv_vsra_vx_i32m2(part, 6, vl), vl);

         part = __riscv_vmv_v_x_i32m2(0, vlmax);
         part = __riscv_vwmacc_vv_i32m2(part, x0, yb0, vl);
         part = __riscv_vwmacc_vv_i32m2(part, x1, yb1, vl);
         part = __riscv_vwmacc_vv_i32m2(part, x2, yb2, vl);
         part = __riscv_vwmacc_vv_i32m2(part, x3, yb3, vl);
         acc1 = __riscv_vadd_vv_i32m2(acc1, __riscv_vsra_vx_i32m2(part, 6, vl), vl);

         part = __riscv_vmv_v_x_i32m2(0, vlmax);
         part = __riscv_vwmacc_vv_i32m2(part, x0, yc0, vl);
         part = __riscv_vwmacc_vv_i32m2(part, x1, yc1, vl);
         part = __riscv_vwmacc_vv_i32m2(part, x2, yc2, vl);
         part = __riscv_vwmacc_vv_i32m2(part, x3, yc3, vl);
         acc2 = __riscv_vadd_vv_i32m2(acc2, __riscv_vsra_vx_i32m2(part, 6, vl), vl);

         part = __riscv_vmv_v_x_i32m2(0, vlmax);
         part = __riscv_vwmacc_vv_i32m2(part, x0, yd0, vl);
         part = __riscv_vwmacc_vv_i32m2(part, x1, yd1, vl);
         part = __riscv_vwmacc_vv_i32m2(part, x2, yd2, vl);
         part = __riscv_vwmacc_vv_i32m2(part, x3, yd3, vl);
         acc3 = __riscv_vadd_vv_i32m2(acc3, __riscv_vsra_vx_i32m2(part, 6, vl), vl);

         px += 4 * vl;
         py0 += 4 * vl;
         py1 += 4 * vl;
         py2 += 4 * vl;
         py3 += 4 * vl;
         n -= vl;
      }
      corr[nb_pitch - 1 - i] = (spx_word32_t)__riscv_vmv_x_s_i32m1_i32(
         __riscv_vredsum_vs_i32m2_i32m1(acc0, zero, vlmax));
      corr[nb_pitch - 1 - (i + 1)] = (spx_word32_t)__riscv_vmv_x_s_i32m1_i32(
         __riscv_vredsum_vs_i32m2_i32m1(acc1, zero, vlmax));
      corr[nb_pitch - 1 - (i + 2)] = (spx_word32_t)__riscv_vmv_x_s_i32m1_i32(
         __riscv_vredsum_vs_i32m2_i32m1(acc2, zero, vlmax));
      corr[nb_pitch - 1 - (i + 3)] = (spx_word32_t)__riscv_vmv_x_s_i32m1_i32(
         __riscv_vredsum_vs_i32m2_i32m1(acc3, zero, vlmax));
      i += 4;
   }
   for (; i < nb_pitch; i++)
      corr[nb_pitch - 1 - i] = rvv_inner_prod(_x, _y + i, len);
}

#endif /* FIXED_POINT */

#endif /* SPEEX_MATH_RVV_H */

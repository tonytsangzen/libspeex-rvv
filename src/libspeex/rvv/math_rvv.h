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

/* Round 16: five blocked-shift dots in one sweep, sharing the three input
 * streams' deinterleaved loads (multicomb's iexc0_mag/iexc1_mag/exc_mag/
 * corr0/corr1). Per-dot accumulation is exactly rvv_inner_prod's (block of
 * 4 exact int32 products, arithmetic >>6, wrap add into the running sum —
 * order-free modulo 2^32), so each output is bit-identical to five
 * separate inner_prod calls.
 * out[0]=a.a  out[1]=b.b  out[2]=x.x  out[3]=a.x  out[4]=b.x */
void rvv_inner_prod_batch5(const spx_word16_t *a, const spx_word16_t *b, const spx_word16_t *x,
                           int len, spx_word32_t *out)
{
   size_t vlmax = __riscv_vsetvlmax_e32m2();
   vint32m2_t s0 = __riscv_vmv_v_x_i32m2(0, vlmax);
   vint32m2_t s1 = s0, s2 = s0, s3 = s0, s4 = s0;
   vint32m1_t zero = __riscv_vmv_v_x_i32m1(0, 1);
   const int16_t *pa = (const int16_t *)a;
   const int16_t *pb = (const int16_t *)b;
   const int16_t *px = (const int16_t *)x;
   size_t vl, n = (size_t)len >> 2;

   while (n > 0)
   {
      vl = __riscv_vsetvl_e16m1(n);
      {
         vint16m1_t a0 = __riscv_vlse16_v_i16m1(pa + 0, 8, vl);
         vint16m1_t a1 = __riscv_vlse16_v_i16m1(pa + 1, 8, vl);
         vint16m1_t a2 = __riscv_vlse16_v_i16m1(pa + 2, 8, vl);
         vint16m1_t a3 = __riscv_vlse16_v_i16m1(pa + 3, 8, vl);
         vint16m1_t b0 = __riscv_vlse16_v_i16m1(pb + 0, 8, vl);
         vint16m1_t b1 = __riscv_vlse16_v_i16m1(pb + 1, 8, vl);
         vint16m1_t b2 = __riscv_vlse16_v_i16m1(pb + 2, 8, vl);
         vint16m1_t b3 = __riscv_vlse16_v_i16m1(pb + 3, 8, vl);
         vint16m1_t x0 = __riscv_vlse16_v_i16m1(px + 0, 8, vl);
         vint16m1_t x1 = __riscv_vlse16_v_i16m1(px + 1, 8, vl);
         vint16m1_t x2 = __riscv_vlse16_v_i16m1(px + 2, 8, vl);
         vint16m1_t x3 = __riscv_vlse16_v_i16m1(px + 3, 8, vl);
         vint32m2_t p;
         p = __riscv_vmv_v_x_i32m2(0, vlmax);
         p = __riscv_vwmacc_vv_i32m2(p, a0, a0, vl);
         p = __riscv_vwmacc_vv_i32m2(p, a1, a1, vl);
         p = __riscv_vwmacc_vv_i32m2(p, a2, a2, vl);
         p = __riscv_vwmacc_vv_i32m2(p, a3, a3, vl);
         s0 = __riscv_vadd_vv_i32m2(s0, __riscv_vsra_vx_i32m2(p, 6, vl), vl);
         p = __riscv_vmv_v_x_i32m2(0, vlmax);
         p = __riscv_vwmacc_vv_i32m2(p, b0, b0, vl);
         p = __riscv_vwmacc_vv_i32m2(p, b1, b1, vl);
         p = __riscv_vwmacc_vv_i32m2(p, b2, b2, vl);
         p = __riscv_vwmacc_vv_i32m2(p, b3, b3, vl);
         s1 = __riscv_vadd_vv_i32m2(s1, __riscv_vsra_vx_i32m2(p, 6, vl), vl);
         p = __riscv_vmv_v_x_i32m2(0, vlmax);
         p = __riscv_vwmacc_vv_i32m2(p, x0, x0, vl);
         p = __riscv_vwmacc_vv_i32m2(p, x1, x1, vl);
         p = __riscv_vwmacc_vv_i32m2(p, x2, x2, vl);
         p = __riscv_vwmacc_vv_i32m2(p, x3, x3, vl);
         s2 = __riscv_vadd_vv_i32m2(s2, __riscv_vsra_vx_i32m2(p, 6, vl), vl);
         p = __riscv_vmv_v_x_i32m2(0, vlmax);
         p = __riscv_vwmacc_vv_i32m2(p, a0, x0, vl);
         p = __riscv_vwmacc_vv_i32m2(p, a1, x1, vl);
         p = __riscv_vwmacc_vv_i32m2(p, a2, x2, vl);
         p = __riscv_vwmacc_vv_i32m2(p, a3, x3, vl);
         s3 = __riscv_vadd_vv_i32m2(s3, __riscv_vsra_vx_i32m2(p, 6, vl), vl);
         p = __riscv_vmv_v_x_i32m2(0, vlmax);
         p = __riscv_vwmacc_vv_i32m2(p, b0, x0, vl);
         p = __riscv_vwmacc_vv_i32m2(p, b1, x1, vl);
         p = __riscv_vwmacc_vv_i32m2(p, b2, x2, vl);
         p = __riscv_vwmacc_vv_i32m2(p, b3, x3, vl);
         s4 = __riscv_vadd_vv_i32m2(s4, __riscv_vsra_vx_i32m2(p, 6, vl), vl);
      }
      pa += 4 * vl;
      pb += 4 * vl;
      px += 4 * vl;
      n -= vl;
   }
   out[0] = (spx_word32_t)__riscv_vmv_x_s_i32m1_i32(__riscv_vredsum_vs_i32m2_i32m1(s0, zero, vlmax));
   out[1] = (spx_word32_t)__riscv_vmv_x_s_i32m1_i32(__riscv_vredsum_vs_i32m2_i32m1(s1, zero, vlmax));
   out[2] = (spx_word32_t)__riscv_vmv_x_s_i32m1_i32(__riscv_vredsum_vs_i32m2_i32m1(s2, zero, vlmax));
   out[3] = (spx_word32_t)__riscv_vmv_x_s_i32m1_i32(__riscv_vredsum_vs_i32m2_i32m1(s3, zero, vlmax));
   out[4] = (spx_word32_t)__riscv_vmv_x_s_i32m1_i32(__riscv_vredsum_vs_i32m2_i32m1(s4, zero, vlmax));
}

/* pitch_gain_search_3tap_vq: entry-parallel evaluation of the 9-term
 * quadratic form over the gain codebook (lanes = codebook entries),
 * followed by a scalar argmin that replicates the reference's selection
 * rule exactly (strict >, first-wins, gain_sum <= max_gain).
 *
 * Bit-exactness: the scalar interleaves MULT16_16_16 (int16*int16 -> int,
 * then truncated to int16 by the enclosing MULT16_16 cast) with exact
 * widening MACs into int32. Each term below reproduces that: vwmul for
 * the truncated product, vncvt (low 16 bits), then a widening MAC with
 * the C coefficient (negated where the reference SUBs). Wrap int32 adds
 * are order-free. g[k] = ADD16(ptr[k], 32) wrapping in int16. */
#define OVERRIDE_PITCH_GAIN_SEARCH_3TAP_VQ 1
int pitch_gain_search_3tap_vq(
  const signed char *gain_cdbk,
  int                gain_cdbk_size,
  spx_word16_t      *C16,
  spx_word16_t       max_gain
)
{
   size_t vlmax = __riscv_vsetvlmax_e32m2();
   vint32m2_t zero32 = __riscv_vmv_v_x_i32m2(0, vlmax);
   vint16m1_t zero16 = __riscv_vmv_v_x_i16m1(0, vlmax);
   spx_word32_t sums[128];
   size_t off = 0, n = (size_t)gain_cdbk_size;
   int best_cdbk = 0;
   spx_word32_t best_sum = -VERY_LARGE32;
   int i;

   while (n > 0)
   {
      size_t vl = __riscv_vsetvl_e16m1(n);
      size_t base4 = off << 2;
      vint16m1_t g0 = __riscv_vadd_vx_i16m1(
            __riscv_vwcvt_x_x_v_i16m1(__riscv_vlse8_v_i8mf2(gain_cdbk + base4 + 0, 4, vl), vl), 32, vl);
      vint16m1_t g1 = __riscv_vadd_vx_i16m1(
            __riscv_vwcvt_x_x_v_i16m1(__riscv_vlse8_v_i8mf2(gain_cdbk + base4 + 1, 4, vl), vl), 32, vl);
      vint16m1_t g2 = __riscv_vadd_vx_i16m1(
            __riscv_vwcvt_x_x_v_i16m1(__riscv_vlse8_v_i8mf2(gain_cdbk + base4 + 2, 4, vl), vl), 32, vl);
      vint32m2_t sum = zero32;
      vint16m1_t tmp;

      /* sum += (g[k]*64)*C[k] for k=0..2 : tmp = trunc16(g*64) */
      tmp = __riscv_vncvt_x_x_w_i16m1(__riscv_vwmul_vx_i32m2(g0, 64, vl), vl);
      sum = __riscv_vwmacc_vx_i32m2(sum, C16[0], tmp, vl);
      tmp = __riscv_vncvt_x_x_w_i16m1(__riscv_vwmul_vx_i32m2(g1, 64, vl), vl);
      sum = __riscv_vwmacc_vx_i32m2(sum, C16[1], tmp, vl);
      tmp = __riscv_vncvt_x_x_w_i16m1(__riscv_vwmul_vx_i32m2(g2, 64, vl), vl);
      sum = __riscv_vwmacc_vx_i32m2(sum, C16[2], tmp, vl);
      /* sum -= (ga*gb)*C[k]: negate the PRODUCT vector, not the
         coefficient — (int16_t)(-C16[k]) wraps for C16[k] == -32768 and
         flips the term's sign (latent round-13 bug, unreachable from the
         codec since normalize16 caps |C16| <= 32767, but wrong for the
         full test range). tmp = trunc16(product) is exact: g in
         [-96,160] keeps |ga*gb| <= 25600 < 32768, so the wrap negate is
         exact too, and int16*int16 -> int32 stays exact for |C16| <=
         32768. */
      tmp = __riscv_vncvt_x_x_w_i16m1(__riscv_vwmul_vv_i32m2(g0, g1, vl), vl);
      sum = __riscv_vwmacc_vx_i32m2(sum, C16[3], __riscv_vrsub_vx_i16m1(tmp, 0, vl), vl);
      tmp = __riscv_vncvt_x_x_w_i16m1(__riscv_vwmul_vv_i32m2(g2, g1, vl), vl);
      sum = __riscv_vwmacc_vx_i32m2(sum, C16[4], __riscv_vrsub_vx_i16m1(tmp, 0, vl), vl);
      tmp = __riscv_vncvt_x_x_w_i16m1(__riscv_vwmul_vv_i32m2(g2, g0, vl), vl);
      sum = __riscv_vwmacc_vx_i32m2(sum, C16[5], __riscv_vrsub_vx_i16m1(tmp, 0, vl), vl);
      /* sum -= (g0*g0)*C[6], (g1*g1)*C[7], (g2*g2)*C[8] */
      tmp = __riscv_vncvt_x_x_w_i16m1(__riscv_vwmul_vv_i32m2(g0, g0, vl), vl);
      sum = __riscv_vwmacc_vx_i32m2(sum, C16[6], __riscv_vrsub_vx_i16m1(tmp, 0, vl), vl);
      tmp = __riscv_vncvt_x_x_w_i16m1(__riscv_vwmul_vv_i32m2(g1, g1, vl), vl);
      sum = __riscv_vwmacc_vx_i32m2(sum, C16[7], __riscv_vrsub_vx_i16m1(tmp, 0, vl), vl);
      tmp = __riscv_vncvt_x_x_w_i16m1(__riscv_vwmul_vv_i32m2(g2, g2, vl), vl);
      sum = __riscv_vwmacc_vx_i32m2(sum, C16[8], __riscv_vrsub_vx_i16m1(tmp, 0, vl), vl);

      __riscv_vse32_v_i32m2(sums + off, sum, vl);
      off += vl;
      n -= vl;
   }

   /* scalar argmin with the reference's exact selection rule */
   for (i = 0; i < gain_cdbk_size; i++)
   {
      spx_word16_t gain_sum = (spx_word16_t)gain_cdbk[4*i+3];
      if (sums[i] > best_sum && gain_sum <= max_gain)
      {
         best_sum = sums[i];
         best_cdbk = i;
      }
   }
   return best_cdbk;
}

/* Batched pitch_xcorr: compute 4 lags per pass with independent
 * accumulators so the (serial-latency) reduction happens once per 4 lags
 * instead of once per lag, and the x[] strided loads are shared. Each lag
 * uses exactly the rvv_inner_prod blocked-shift scheme, so results are
 * bit-exact vs the scalar pitch_xcorr/inner_prod pair. Leftover lags
 * (nb_pitch not a multiple of 4) fall back to rvv_inner_prod. */
#define OVERRIDE_PITCH_XCORR 1
/* non-static since round 16: filters.c's interp_pitch batch-uses it */
void pitch_xcorr(const spx_word16_t *_x, const spx_word16_t *_y, spx_word32_t *corr, int len, int nb_pitch, char *stack)
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

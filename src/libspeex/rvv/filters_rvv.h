/* RVV implementation of the transposed-direct-form filters (filters.c).
 *
 * The scalar body keeps an int32 state array updated per sample:
 *   mem[j] = mem[j+1] + coef[j]*x_i   (j < ord-1),  mem[ord-1] = coef[ord-1]*x_i
 * i.e. a one-lane shift of mem plus a coefficient product — no reduction,
 * so it vectorizes as one lane per tap:
 *   vstate = vrgather(vstate, vidx+1) + vwmacc(zero, x_i, vcoef)
 * with vrgather's spec-guaranteed zero for out-of-range indices providing
 * the "mem[ord] = 0" the scalar code implies.
 *
 * All vector ops run at full VLMAX (no tail-agnostic lanes anywhere), and
 * the invariant "vstate lanes >= ord are 0" holds by construction: the
 * coefficient vector is zero-padded, the shifted vector slides the zero
 * block down one lane, and lane VLMAX-1 is vrgather-zeroed. The per-sample
 * scalar fixups (yi = EXTRACT16(SATURATE(x + PSHR32(mem[0], LPC_SHIFT),
 * 32767))) replicate arch.h/fixed_generic.h macros verbatim; mem[0] is
 * read back through memory each sample (one 40-byte store, keeps the
 * caller's mem coherent).
 *
 * ord must be <= VLMAX (10 in all libspeex code paths, VLMAX=16 at
 * VLEN=256); scalar copies of the C reference cover anything larger.
 * Only valid for the fixed-point build; otherwise filters.c falls back. */
#ifndef SPEEX_FILTERS_RVV_H
#define SPEEX_FILTERS_RVV_H

#include "rvv_arch.h"

#if defined(FIXED_POINT)

/* Scalar fallbacks: verbatim copies of the filters.c reference for
 * ord > VLMAX (never hit by libspeex, kept for safety). */
static void iir_mem16_c(const spx_word16_t *x, const spx_coef_t *den, spx_word16_t *y, int N, int ord, spx_mem_t *mem, char *stack)
{
   int i,j;
   spx_word16_t yi,nyi;
   for (i=0;i<N;i++)
   {
      yi = EXTRACT16(SATURATE(ADD32(EXTEND32(x[i]),PSHR32(mem[0],LPC_SHIFT)),32767));
      nyi = NEG16(yi);
      for (j=0;j<ord-1;j++)
         mem[j] = MAC16_16(mem[j+1],den[j],nyi);
      mem[ord-1] = MULT16_16(den[ord-1],nyi);
      y[i] = yi;
   }
}

static void fir_mem16_c(const spx_word16_t *x, const spx_coef_t *num, spx_word16_t *y, int N, int ord, spx_mem_t *mem, char *stack)
{
   int i,j;
   spx_word16_t xi,yi;
   for (i=0;i<N;i++)
   {
      xi=x[i];
      yi = EXTRACT16(SATURATE(ADD32(EXTEND32(x[i]),PSHR32(mem[0],LPC_SHIFT)),32767));
      for (j=0;j<ord-1;j++)
         mem[j] = MAC16_16(mem[j+1], num[j],xi);
      mem[ord-1] = MULT16_16(num[ord-1],xi);
      y[i] = yi;
   }
}

static void filter_mem16_c(const spx_word16_t *x, const spx_coef_t *num, const spx_coef_t *den, spx_word16_t *y, int N, int ord, spx_mem_t *mem, char *stack)
{
   int i,j;
   spx_word16_t xi,yi,nyi;
   for (i=0;i<N;i++)
   {
      xi= x[i];
      yi = EXTRACT16(SATURATE(ADD32(EXTEND32(x[i]),PSHR32(mem[0],LPC_SHIFT)),32767));
      nyi = NEG16(yi);
      for (j=0;j<ord-1;j++)
         mem[j] = MAC16_16(MAC16_16(mem[j+1], num[j],xi), den[j],nyi);
      mem[ord-1] = ADD32(MULT16_16(num[ord-1],xi), MULT16_16(den[ord-1],nyi));
      y[i] = yi;
   }
}

#define OVERRIDE_IIR_MEM16 1
void iir_mem16(const spx_word16_t *x, const spx_coef_t *den, spx_word16_t *y, int N, int ord, spx_mem_t *mem, char *stack)
{
   size_t vlmax = __riscv_vsetvlmax_e32m2();
   size_t vl_ord = (size_t)ord;
   int i;
   if (vl_ord > vlmax || ord < 1)
   {
      iir_mem16_c(x, den, y, N, ord, mem, stack);
      return;
   }
   {
      vint32m2_t zero32 = __riscv_vmv_v_x_i32m2(0, vlmax);
      vint16m1_t zero16 = __riscv_vmv_v_x_i16m1(0, vlmax);
      vint16m1_t vden = __riscv_vslideup_vx_i16m1(zero16,
            __riscv_vle16_v_i16m1(den, vl_ord), 0, vl_ord);
      vuint32m2_t vidx = __riscv_vadd_vx_u32m2(__riscv_vid_v_u32m2(vlmax), 1, vlmax);
      vint32m2_t vstate = __riscv_vslideup_vx_i32m2(zero32,
            __riscv_vle32_v_i32m2(mem, vl_ord), 0, vl_ord);

      for (i = 0; i < N; i++)
      {
         spx_word16_t yi, nyi;
         vint32m2_t prod, shifted;
         __riscv_vse32_v_i32m2(mem, vstate, vl_ord);
         yi = EXTRACT16(SATURATE(ADD32(EXTEND32(x[i]), PSHR32(mem[0], LPC_SHIFT)), 32767));
         nyi = NEG16(yi);
         prod = __riscv_vwmacc_vx_i32m2(zero32, (int16_t)nyi, vden, vlmax);
         shifted = __riscv_vrgather_vv_i32m2(vstate, vidx, vlmax);
         vstate = __riscv_vadd_vv_i32m2(shifted, prod, vlmax);
         y[i] = yi;
      }
      __riscv_vse32_v_i32m2(mem, vstate, vl_ord);
   }
}

#if !defined(DISABLE_ENCODER)
#define OVERRIDE_FIR_MEM16 1
void fir_mem16(const spx_word16_t *x, const spx_coef_t *num, spx_word16_t *y, int N, int ord, spx_mem_t *mem, char *stack)
{
   size_t vlmax = __riscv_vsetvlmax_e32m2();
   size_t vl_ord = (size_t)ord;
   int i;
   if (vl_ord > vlmax || ord < 1)
   {
      fir_mem16_c(x, num, y, N, ord, mem, stack);
      return;
   }
   {
      vint32m2_t zero32 = __riscv_vmv_v_x_i32m2(0, vlmax);
      vint16m1_t zero16 = __riscv_vmv_v_x_i16m1(0, vlmax);
      vint16m1_t vnum = __riscv_vslideup_vx_i16m1(zero16,
            __riscv_vle16_v_i16m1(num, vl_ord), 0, vl_ord);
      vuint32m2_t vidx = __riscv_vadd_vx_u32m2(__riscv_vid_v_u32m2(vlmax), 1, vlmax);
      vint32m2_t vstate = __riscv_vslideup_vx_i32m2(zero32,
            __riscv_vle32_v_i32m2(mem, vl_ord), 0, vl_ord);

      for (i = 0; i < N; i++)
      {
         spx_word16_t yi;
         vint32m2_t prod, shifted;
         __riscv_vse32_v_i32m2(mem, vstate, vl_ord);
         yi = EXTRACT16(SATURATE(ADD32(EXTEND32(x[i]), PSHR32(mem[0], LPC_SHIFT)), 32767));
         prod = __riscv_vwmacc_vx_i32m2(zero32, x[i], vnum, vlmax);
         shifted = __riscv_vrgather_vv_i32m2(vstate, vidx, vlmax);
         vstate = __riscv_vadd_vv_i32m2(shifted, prod, vlmax);
         y[i] = yi;
      }
      __riscv_vse32_v_i32m2(mem, vstate, vl_ord);
   }
}
#endif /* !DISABLE_ENCODER */

#if !defined(DISABLE_ENCODER)
#define OVERRIDE_FILTER_MEM16 1
void filter_mem16(const spx_word16_t *x, const spx_coef_t *num, const spx_coef_t *den, spx_word16_t *y, int N, int ord, spx_mem_t *mem, char *stack)
{
   size_t vlmax = __riscv_vsetvlmax_e32m2();
   size_t vl_ord = (size_t)ord;
   int i;
   if (vl_ord > vlmax || ord < 1)
   {
      filter_mem16_c(x, num, den, y, N, ord, mem, stack);
      return;
   }
   {
      vint32m2_t zero32 = __riscv_vmv_v_x_i32m2(0, vlmax);
      vint16m1_t zero16 = __riscv_vmv_v_x_i16m1(0, vlmax);
      vint16m1_t vnum = __riscv_vslideup_vx_i16m1(zero16,
            __riscv_vle16_v_i16m1(num, vl_ord), 0, vl_ord);
      vint16m1_t vden = __riscv_vslideup_vx_i16m1(zero16,
            __riscv_vle16_v_i16m1(den, vl_ord), 0, vl_ord);
      vuint32m2_t vidx = __riscv_vadd_vx_u32m2(__riscv_vid_v_u32m2(vlmax), 1, vlmax);
      vint32m2_t vstate = __riscv_vslideup_vx_i32m2(zero32,
            __riscv_vle32_v_i32m2(mem, vl_ord), 0, vl_ord);

      for (i = 0; i < N; i++)
      {
         spx_word16_t yi, nyi;
         vint32m2_t prod, shifted;
         __riscv_vse32_v_i32m2(mem, vstate, vl_ord);
         yi = EXTRACT16(SATURATE(ADD32(EXTEND32(x[i]), PSHR32(mem[0], LPC_SHIFT)), 32767));
         nyi = NEG16(yi);
         /* mem[j] = mem[j+1] + num[j]*xi + den[j]*nyi */
         prod = __riscv_vwmacc_vx_i32m2(zero32, x[i], vnum, vlmax);
         prod = __riscv_vwmacc_vx_i32m2(prod, (int16_t)nyi, vden, vlmax);
         shifted = __riscv_vrgather_vv_i32m2(vstate, vidx, vlmax);
         vstate = __riscv_vadd_vv_i32m2(shifted, prod, vlmax);
         y[i] = yi;
      }
      __riscv_vse32_v_i32m2(mem, vstate, vl_ord);
   }
}
#endif /* !DISABLE_ENCODER */

/* --- multicomb tail + compute_rms16 helpers (decode enhancer) --- */

/* new_exc[i] = ADD16(exc[i], EXTRACT16(PSHR32(gain0*iexc0[i] +
 * gain1*iexc1[i], 8))). PSHR32 = (a + (1<<(shift-1))) >> shift
 * (arithmetic); EXTRACT16 and ADD16 are truncating/wrapping 16-bit ops.
 * All exact with widening MACs + shift + narrowing + wrap add. */
static inline void rvv_comb_update(spx_word16_t *new_exc, const spx_word16_t *exc,
                                   const spx_word16_t *iexc0, const spx_word16_t *iexc1,
                                   spx_word16_t gain0, spx_word16_t gain1, int len)
{
   size_t vlmax = __riscv_vsetvlmax_e32m2();
   vint32m2_t zero32 = __riscv_vmv_v_x_i32m2(0, vlmax);
   size_t off = 0, n = (size_t)len;
   while (n > 0)
   {
      size_t vl = __riscv_vsetvl_e16m1(n);
      vint16m1_t e0 = __riscv_vle16_v_i16m1(iexc0 + off, vl);
      vint16m1_t e1 = __riscv_vle16_v_i16m1(iexc1 + off, vl);
      vint32m2_t acc = __riscv_vwmacc_vx_i32m2(zero32, gain0, e0, vl);
      acc = __riscv_vwmacc_vx_i32m2(acc, gain1, e1, vl);
      acc = __riscv_vadd_vx_i32m2(acc, 1 << 7, vl);
      acc = __riscv_vsra_vx_i32m2(acc, 8, vl);
      vint16m1_t p16 = __riscv_vncvt_x_x_w_i16m1(acc, vl);
      vint16m1_t out = __riscv_vadd_vv_i16m1(__riscv_vle16_v_i16m1(exc + off, vl), p16, vl);
      __riscv_vse16_v_i16m1(new_exc + off, out, vl);
      off += vl;
      n -= vl;
   }
}

/* new_exc[i] = MULT16_16_Q14(ngain, new_exc[i]) = (int16)((ngain*x[i]) >> 14) */
static inline void rvv_gain_scale(spx_word16_t *x, spx_word16_t ngain, int len)
{
   size_t vlmax = __riscv_vsetvlmax_e32m2();
   vint32m2_t zero32 = __riscv_vmv_v_x_i32m2(0, vlmax);
   size_t off = 0, n = (size_t)len;
   while (n > 0)
   {
      size_t vl = __riscv_vsetvl_e16m1(n);
      vint32m2_t prod = __riscv_vwmacc_vx_i32m2(zero32, ngain,
            __riscv_vle16_v_i16m1(x + off, vl), vl);
      prod = __riscv_vsra_vx_i32m2(prod, 14, vl);
      __riscv_vse16_v_i16m1(x + off, __riscv_vncvt_x_x_w_i16m1(prod, vl), vl);
      off += vl;
      n -= vl;
   }
}

/* Blocked sum-of-squares shared by compute_rms16: lane = 4-sample block
 * (strided loads, same scheme as rvv_inner_prod), per-block >>6, wrap
 * int32 adds — exact vs the scalar reference. pre_shift replicates
 * SHR16 (up=0, arithmetic >>) / SHL16 (up=1, wrapping <<) exactly.
 * Callers guarantee len % 4 == 0 (all libspeex callers do). */
static inline spx_word32_t rvv_sumsq_blocked(const spx_word16_t *x, int len, int pre_shift, int up)
{
   size_t vlmax = __riscv_vsetvlmax_e32m2();
   vint32m2_t acc = __riscv_vmv_v_x_i32m2(0, vlmax);
   vint32m1_t zero = __riscv_vmv_v_x_i32m1(0, 1);
   size_t vl, n = (size_t)len >> 2;
   size_t off = 0;
   while (n > 0)
   {
      size_t k;
      vl = __riscv_vsetvl_e16m1(n);
      vint32m2_t part = __riscv_vmv_v_x_i32m2(0, vlmax);
      for (k = 0; k < 4; k++)
      {
         vint16m1_t w = __riscv_vlse16_v_i16m1(x + off + k, 4 * sizeof(spx_word16_t), vl);
         vint16m1_t w2 = up ? __riscv_vsll_vx_i16m1(w, (unsigned int)pre_shift, vl)
                            : __riscv_vsra_vx_i16m1(w, (unsigned int)pre_shift, vl);
         part = __riscv_vwmacc_vv_i32m2(part, w2, w2, vl);
      }
      part = __riscv_vsra_vx_i32m2(part, 6, vl);
      acc = __riscv_vadd_vv_i32m2(acc, part, vl);
      off += 4 * vl;
      n -= vl;
   }
   return (spx_word32_t)__riscv_vmv_x_s_i32m1_i32(
      __riscv_vredsum_vs_i32m2_i32m1(acc, zero, vlmax));
}

#endif /* FIXED_POINT */

#endif /* SPEEX_FILTERS_RVV_H */

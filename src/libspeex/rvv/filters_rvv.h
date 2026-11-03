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
      /* _tu: lanes >= vl keep vd (=zero32/zero16), so the zero tail the
       * VLMAX-wide vrgather/MAC reads below is guaranteed, not incidental
       * (review R2). */
      vint16m1_t vden = __riscv_vslideup_vx_i16m1_tu(zero16,
            __riscv_vle16_v_i16m1(den, vl_ord), 0, vl_ord);
      vuint32m2_t vidx = __riscv_vadd_vx_u32m2(__riscv_vid_v_u32m2(vlmax), 1, vlmax);
      vint32m2_t vstate = __riscv_vslideup_vx_i32m2_tu(zero32,
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
      vint16m1_t vnum = __riscv_vslideup_vx_i16m1_tu(zero16,
            __riscv_vle16_v_i16m1(num, vl_ord), 0, vl_ord);
      vuint32m2_t vidx = __riscv_vadd_vx_u32m2(__riscv_vid_v_u32m2(vlmax), 1, vlmax);
      vint32m2_t vstate = __riscv_vslideup_vx_i32m2_tu(zero32,
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
      vint16m1_t vnum = __riscv_vslideup_vx_i16m1_tu(zero16,
            __riscv_vle16_v_i16m1(num, vl_ord), 0, vl_ord);
      /* _tu: lanes >= vl keep vd (=zero32/zero16), so the zero tail the
       * VLMAX-wide vrgather/MAC reads below is guaranteed, not incidental
       * (review R2). */
      vint16m1_t vden = __riscv_vslideup_vx_i16m1_tu(zero16,
            __riscv_vle16_v_i16m1(den, vl_ord), 0, vl_ord);
      vuint32m2_t vidx = __riscv_vadd_vx_u32m2(__riscv_vid_v_u32m2(vlmax), 1, vlmax);
      vint32m2_t vstate = __riscv_vslideup_vx_i32m2_tu(zero32,
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
      /* _tu: keep the zeroed tail of the persistent accumulator — a short
       * VL update is allowed to clobber inactive lanes otherwise (R1). */
      acc = __riscv_vadd_vv_i32m2_tu(acc, acc, part, vl);
      off += 4 * vl;
      n -= vl;
   }
   return (spx_word32_t)__riscv_vmv_x_s_i32m1_i32(
      __riscv_vredsum_vs_i32m2_i32m1(acc, zero, vlmax));
}

/* --- QMF polyphase split/synthesis (wideband band split/recombine) --- */

/* qmf_decomp: lane k = output sample k (input step 2). Tap j reads
 * x[2k+j] and x2[2k-j] (x2[t]=x[t+M-1]) — both stride-4-byte loads.
 * y2's sign alternates with the loop tap parity (even: SUB, odd: ADD),
 * folded into a pre-negated coefficient copy a2[j] = (j&1)? a[j] : -a[j]
 * where a[j] is the REVERSED prototype (a[M-1-t]=aa[t]) — only the first
 * M2 taps take part (polyphase half-band). Tail:
 * EXTRACT16(SATURATE(PSHR32(y,15),32767)). */
static void rvv_qmf_decomp(const spx_word16_t *xx, const spx_word16_t *aa, spx_word16_t *y1, spx_word16_t *y2, int N, int M, spx_word16_t *mem, char *stack)
{
   int i,j,M2;
   VARDECL(spx_word16_t *a);
   VARDECL(spx_word16_t *a2);
   VARDECL(spx_word16_t *x);
   spx_word16_t *x2;
   size_t vlmax = __riscv_vsetvlmax_e32m2();
   vint32m2_t zero32 = __riscv_vmv_v_x_i32m2(0, vlmax);

   ALLOC(a, M, spx_word16_t);
   ALLOC(a2, M, spx_word16_t);
   ALLOC(x, N+M-1, spx_word16_t);
   x2=x+M-1;
   M2=M>>1;
   for (i=0;i<M;i++)
      a[M-i-1]= aa[i];
   for (j=0;j<M2;j++)
      a2[j] = (j&1)? a[j] : -a[j];
   for (i=0;i<M-1;i++)
      x[i]=mem[M-i-2];
   for (i=0;i<N;i++)
      x[i+M-1]=SHR16(xx[i],1);
   for (i=0;i<M-1;i++)
      mem[i]=SHR16(xx[N-i-1],1);

   {
      size_t K = (size_t)N>>1;   /* output samples; N is even */
      size_t off = 0, n = K;
      while (n > 0)
      {
         size_t vl = __riscv_vsetvl_e16m1(n);
         vint32m2_t acc1 = zero32, acc2 = zero32;
         size_t base = off<<1;   /* input index i = 2k */
         for (j=0;j<M2;j++)
         {
            vint16m1_t xl = __riscv_vlse16_v_i16m1(x + base + j, 4, vl);
            vint16m1_t xr = __riscv_vlse16_v_i16m1(x2 + base - j, 4, vl);
            vint16m1_t s = __riscv_vadd_vv_i16m1(xl, xr, vl);
            vint16m1_t d = __riscv_vsub_vv_i16m1(xl, xr, vl);
            acc1 = __riscv_vwmacc_vx_i32m2(acc1, a[j], s, vl);
            acc2 = __riscv_vwmacc_vx_i32m2(acc2, a2[j], d, vl);
         }
         /* EXTRACT16(SATURATE(PSHR32(y,15),32767)) */
         acc1 = __riscv_vadd_vx_i32m2(acc1, 1 << 14, vl);
         acc2 = __riscv_vadd_vx_i32m2(acc2, 1 << 14, vl);
         acc1 = __riscv_vsra_vx_i32m2(acc1, 15, vl);
         acc2 = __riscv_vsra_vx_i32m2(acc2, 15, vl);
         acc1 = __riscv_vmin_vx_i32m2(acc1, 32767, vl);
         acc2 = __riscv_vmin_vx_i32m2(acc2, 32767, vl);
         acc1 = __riscv_vmax_vx_i32m2(acc1, -32767, vl);
         acc2 = __riscv_vmax_vx_i32m2(acc2, -32767, vl);
         __riscv_vse16_v_i16m1(y1 + off, __riscv_vncvt_x_x_w_i16m1(acc1, vl), vl);
         __riscv_vse16_v_i16m1(y2 + off, __riscv_vncvt_x_x_w_i16m1(acc2, vl), vl);
         off += vl;
         n -= vl;
      }
   }
}

/* qmf_synth: lane l = output group i=2l (4 contiguous outputs
 * y[4l..4l+3]). Tap/window pairing from the scalar rolling-register
 * loop, by tap t with pair p=t>>1:
 *   even t: y0 += a[t]*(xx1[N2-1+p-i]-xx2[N2-1+p-i])
 *           y2 += a[t]*(xx1[N2-2+p-i]-xx2[N2-2+p-i])
 *   odd  t: y1 += a[t]*(xx1[N2-1+p-i]+xx2[N2-1+p-i])
 *           y3 += a[t]*(xx1[N2-2+p-i]+xx2[N2-2+p-i])
 * Only the first M2 taps participate (polyphase). The xx1/xx2 arrays are
 * re-laid-out DESCENDING during setup (yyp[q]=xx1[N2+M2-2-q]) so every
 * lane read is an ascending stride-4 load at base M2-1-p / M2-p. The
 * scalar "multiply twice by the same coef" form equals the sum/diff form
 * modulo 2^32 (wrap int32 adds, exact products). Tails:
 * EXTRACT16(SATURATE32(PSHR32(y,15),32767)), stored via four stride-8
 * stores (interleave). N2 must be a multiple of 2. */
/* qmf_synth: schedule-driven entry-parallel version. A scalar pre-pass
 * replays the reference's rolling registers and emits one entry per
 * PRODUCT (xx1/xx2 array, window index, sign, target accumulator); the
 * vector loop executes the schedule mechanically with lanes = output
 * groups (i = 2l). The window index DESCENDS by 2 elements per lane
 * (i grows), hence the negative -4-byte load stride; outputs are
 * interleaved y[4l..4l+3], stored with +8-byte strides. Verified by
 * test/qmf_diff.c (per-tap isolation + codec-real N=320/M=64). */
static void rvv_qmf_synth(const spx_word16_t *x1, const spx_word16_t *x2, const spx_word16_t *a, spx_word16_t *y, int N, int M, spx_word16_t *mem1, spx_word16_t *mem2, char *stack)
{
   int i, t;
   int M2, N2;
   VARDECL(spx_word16_t *xx1);
   VARDECL(spx_word16_t *xx2);
   VARDECL(spx_word16_t *yyp);
   VARDECL(spx_word16_t *yy2p);
   size_t vlmax = __riscv_vsetvlmax_e32m2();
   vint32m2_t zero32 = __riscv_vmv_v_x_i32m2(0, vlmax);

   M2 = M>>1;
   N2 = N>>1;
   if ((M & 3) || (N2 & 1))
      return; /* not reached for codec QMF parameters; caller falls back */

   ALLOC(xx1, M2+N2, spx_word16_t);
   ALLOC(xx2, M2+N2, spx_word16_t);
   ALLOC(yyp, M2+N2, spx_word16_t);
   ALLOC(yy2p, M2+N2, spx_word16_t);

   for (i = 0; i < N2; i++)
      xx1[i] = x1[N2-1-i];
   for (i = 0; i < M2; i++)
      xx1[N2+i] = mem1[2*i+1];
   for (i = 0; i < N2; i++)
      xx2[i] = x2[N2-1-i];
   for (i = 0; i < M2; i++)
      xx2[N2+i] = mem2[2*i+1];
   for (i = 0; i < M2+N2-1; i++)
   {
      yyp[i] = xx1[N2+M2-2-i];
      yy2p[i] = xx2[N2+M2-2-i];
   }
   yyp[M2+N2-1] = 0;
   yy2p[M2+N2-1] = 0;

   {
      /* schedule: 16 products per 4-tap pair (pair j = 2*jj covers taps
       * 4jj..4jj+3); window indices WITHOUT the per-lane -i term */
      int s_arr[4*M], s_idx[4*M], s_sign[4*M], s_acc[4*M], s_cidx[4*M];
      int nw = 0;
      int j, x10i = N2-2, x20i = N2-2, x11i, x21i;
      int term, acc_i;
      for (j = 0; j < M2; j += 2)
      {
         x11i = N2-1+j; x21i = N2-1+j;
         /* substep A: y0 += a[2j]*x11 - a[2j]*x21
                      y2 += a[2j]*x10 - a[2j]*x20
                      y1 += a[2j+1]*x11 + a[2j+1]*x21
                      y3 += a[2j+1]*x10 + a[2j+1]*x20 */
         s_arr[nw]=0; s_idx[nw]=x11i; s_sign[nw]=+1; s_acc[nw]=0; s_cidx[nw]=2*j;   nw++;
         s_arr[nw]=1; s_idx[nw]=x21i; s_sign[nw]=-1; s_acc[nw]=0; s_cidx[nw]=2*j;   nw++;
         s_arr[nw]=0; s_idx[nw]=x10i; s_sign[nw]=+1; s_acc[nw]=2; s_cidx[nw]=2*j;   nw++;
         s_arr[nw]=1; s_idx[nw]=x20i; s_sign[nw]=-1; s_acc[nw]=2; s_cidx[nw]=2*j;   nw++;
         s_arr[nw]=0; s_idx[nw]=x11i; s_sign[nw]=+1; s_acc[nw]=1; s_cidx[nw]=2*j+1; nw++;
         s_arr[nw]=1; s_idx[nw]=x21i; s_sign[nw]=+1; s_acc[nw]=1; s_cidx[nw]=2*j+1; nw++;
         s_arr[nw]=0; s_idx[nw]=x10i; s_sign[nw]=+1; s_acc[nw]=3; s_cidx[nw]=2*j+1; nw++;
         s_arr[nw]=1; s_idx[nw]=x20i; s_sign[nw]=+1; s_acc[nw]=3; s_cidx[nw]=2*j+1; nw++;
         /* substep B: y0 += a[2j+2]*x10' - a[2j+2]*x20'
                      y2 += a[2j+2]*x11 - a[2j+2]*x21
                      y1 += a[2j+3]*x10' + a[2j+3]*x20'
                      y3 += a[2j+3]*x11 + a[2j+3]*x21
            (x10'/x20' = rolled windows xx1[N2+j-i]/xx2[N2+j-i]) */
         s_arr[nw]=0; s_idx[nw]=N2+j; s_sign[nw]=+1; s_acc[nw]=0; s_cidx[nw]=2*j+2; nw++;
         s_arr[nw]=1; s_idx[nw]=N2+j; s_sign[nw]=-1; s_acc[nw]=0; s_cidx[nw]=2*j+2; nw++;
         s_arr[nw]=0; s_idx[nw]=x11i; s_sign[nw]=+1; s_acc[nw]=2; s_cidx[nw]=2*j+2; nw++;
         s_arr[nw]=1; s_idx[nw]=x21i; s_sign[nw]=-1; s_acc[nw]=2; s_cidx[nw]=2*j+2; nw++;
         s_arr[nw]=0; s_idx[nw]=N2+j; s_sign[nw]=+1; s_acc[nw]=1; s_cidx[nw]=2*j+3; nw++;
         s_arr[nw]=1; s_idx[nw]=N2+j; s_sign[nw]=+1; s_acc[nw]=1; s_cidx[nw]=2*j+3; nw++;
         s_arr[nw]=0; s_idx[nw]=x11i; s_sign[nw]=+1; s_acc[nw]=3; s_cidx[nw]=2*j+3; nw++;
         s_arr[nw]=1; s_idx[nw]=x21i; s_sign[nw]=+1; s_acc[nw]=3; s_cidx[nw]=2*j+3; nw++;
         x10i = N2+j; x20i = N2+j;
      }
      (void)x10i; (void)x20i;
      {
         size_t G = (size_t)N2>>1;   /* output groups; N2 multiple of 2 */
         size_t off = 0, n = G;
         while (n > 0)
         {
            size_t vl = __riscv_vsetvl_e16m1(n);
            size_t base = off<<1;   /* i = 2l for lane l */
            vint32m2_t acc0 = zero32, acc1 = zero32, acc2 = zero32, acc3 = zero32;
            for (term = 0; term < nw; term++)
            {
               const spx_word16_t *src = s_arr[term] ? yy2p : yyp;
               /* transform the xx1/xx2 window index into the descending
                * yyp/yy2p layout; the window index still descends by 2
                * per lane: byte stride -4 */
               int q = (N2+M2-2) - s_idx[term] + (int)base;
               vint16m1_t w = __riscv_vlse16_v_i16m1(src + q, 4, vl);
               spx_word16_t c = (spx_word16_t)(s_sign[term] * a[s_cidx[term]]);
               switch (s_acc[term])
               {
               case 0: acc0 = __riscv_vwmacc_vx_i32m2(acc0, c, w, vl); break;
               case 1: acc1 = __riscv_vwmacc_vx_i32m2(acc1, c, w, vl); break;
               case 2: acc2 = __riscv_vwmacc_vx_i32m2(acc2, c, w, vl); break;
               case 3: acc3 = __riscv_vwmacc_vx_i32m2(acc3, c, w, vl); break;
               }
            }
            for (acc_i = 0; acc_i < 4; acc_i++)
            {
               if (acc_i == 0) { acc0 = __riscv_vadd_vx_i32m2(acc0, 1 << 14, vl); acc0 = __riscv_vsra_vx_i32m2(acc0, 15, vl); acc0 = __riscv_vmin_vx_i32m2(acc0, 32767, vl); acc0 = __riscv_vmax_vx_i32m2(acc0, -32767, vl); }
               if (acc_i == 1) { acc1 = __riscv_vadd_vx_i32m2(acc1, 1 << 14, vl); acc1 = __riscv_vsra_vx_i32m2(acc1, 15, vl); acc1 = __riscv_vmin_vx_i32m2(acc1, 32767, vl); acc1 = __riscv_vmax_vx_i32m2(acc1, -32767, vl); }
               if (acc_i == 2) { acc2 = __riscv_vadd_vx_i32m2(acc2, 1 << 14, vl); acc2 = __riscv_vsra_vx_i32m2(acc2, 15, vl); acc2 = __riscv_vmin_vx_i32m2(acc2, 32767, vl); acc2 = __riscv_vmax_vx_i32m2(acc2, -32767, vl); }
               if (acc_i == 3) { acc3 = __riscv_vadd_vx_i32m2(acc3, 1 << 14, vl); acc3 = __riscv_vsra_vx_i32m2(acc3, 15, vl); acc3 = __riscv_vmin_vx_i32m2(acc3, 32767, vl); acc3 = __riscv_vmax_vx_i32m2(acc3, -32767, vl); }
            }
            {
               size_t ybase = base<<1;   /* y[2i] = y[4l] */
               __riscv_vsse16_v_i16m1(y + ybase + 0, 8, __riscv_vncvt_x_x_w_i16m1(acc0, vl), vl);
               __riscv_vsse16_v_i16m1(y + ybase + 1, 8, __riscv_vncvt_x_x_w_i16m1(acc1, vl), vl);
               __riscv_vsse16_v_i16m1(y + ybase + 2, 8, __riscv_vncvt_x_x_w_i16m1(acc2, vl), vl);
               __riscv_vsse16_v_i16m1(y + ybase + 3, 8, __riscv_vncvt_x_x_w_i16m1(acc3, vl), vl);
            }
            off += vl;
            n -= vl;
         }
      }
   }

   for (i = 0; i < M2; i++)
      mem1[2*i+1] = xx1[i];
   for (i = 0; i < M2; i++)
      mem2[2*i+1] = xx2[i];
}


/* normalize16: the max-abs scan is an order-free reduction (NEG32 wrap
 * replicated with a masked negate), the shift-decision loop stays scalar so
 * the sig_shift sequence is bit-identical, and the y[] store truncates via
 * vncvt like EXTRACT16. */
#ifndef R14_NO_NORM16
#define OVERRIDE_NORMALIZE16 1
int normalize16(const spx_sig_t *x, spx_word16_t *y, spx_sig_t max_scale, int len)
{
   int sig_shift = 0;
   spx_sig_t max_val = 1;
   {
      size_t off = 0;
      while (off < (size_t)len)
      {
         size_t vl = __riscv_vsetvl_e32m2((size_t)len - off);
         vint32m2_t v = __riscv_vle32_v_i32m2(x + off, vl);
         vint32m2_t neg = __riscv_vrsub_vx_i32m2(v, 0, vl);
         vint32m2_t ax = __riscv_vmerge_vvm_i32m2(v, neg,
               __riscv_vmslt_vx_i32m2_b16(v, 0, vl), vl);
         vint32m1_t red = __riscv_vredmax_vs_i32m2_i32m1(ax,
               __riscv_vmv_v_x_i32m1(-2147483647-1, 1), vl);
         int m = __riscv_vmv_x_s_i32m1_i32(red);
         if (m > max_val)
            max_val = m;
         off += vl;
      }
   }
   while (max_val>max_scale)
   {
      sig_shift++;
      max_val >>= 1;
   }
   {
      size_t off = 0;
      while (off < (size_t)len)
      {
         size_t vl = __riscv_vsetvl_e32m2((size_t)len - off);
         vint32m2_t v = __riscv_vle32_v_i32m2(x + off, vl);
         v = __riscv_vsra_vx_i32m2(v, sig_shift, vl);
         __riscv_vse16_v_i16m1(y + off, __riscv_vncvt_x_x_w_i16m1(v, vl), vl);
         off += vl;
      }
   }
   return sig_shift;
}
#endif /* R14_NO_NORM16 */


#endif /* FIXED_POINT */

#endif /* SPEEX_FILTERS_RVV_H */

/* RVV implementation of the transposed-direct-form filters (filters.c).
 *
 * The scalar body keeps an int32 state array updated per sample:
 *   mem[j] = mem[j+1] + coef[j]*x_i   (j < ord-1),  mem[ord-1] = coef[ord-1]*x_i
 * i.e. a one-lane shift of mem plus a coefficient product. Round 19
 * (review K1) keeps the state in registers for the whole call:
 *   vstate = vslide1down(vstate, 0, ord) + vwmacc(x_i, vcoef)
 * vslide1down moves every lane down one (lane j = old state[j+1]) and
 * inserts 0 at lane ord-1 — the same result as the old vrgather(vidx+1)
 * over a zero-tailed vector, without the per-sample 40-byte mem store +
 * mem[0] reload (state[0] is read with vmv.x.s).
 *
 * All vector ops run at VL=ord (<= VLMAX; ord is 10 or fewer in every
 * libspeex path), so no tail-agnostic lane is ever read; the initial
 * vslideup is _tu against a zero vector. The per-sample scalar fixups
 * (yi = EXTRACT16(SATURATE(x + PSHR32(state[0], LPC_SHIFT), 32767)))
 * replicate arch.h/fixed_generic.h macros verbatim, and the caller's mem
 * is written back once at the end.
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
      /* Review K1: state stays in registers (no per-sample mem store +
       * reload round trip); all activity runs at VL=ord; state[0] is read
       * with vmv.x.s; vslide1down performs the down-shift with a zero
       * entering at the top (lane j gets state[j+1], lane ord-1 gets 0 —
       * identical to the vrgather(vidx+1) + OOB-zero rule). */
      vint16m1_t vden = __riscv_vle16_v_i16m1(den, vl_ord);
      vint32m2_t vstate = __riscv_vslideup_vx_i32m2_tu(
            __riscv_vmv_v_x_i32m2(0, vlmax),
            __riscv_vle32_v_i32m2(mem, vl_ord), 0, vl_ord);

      for (i = 0; i < N; i++)
      {
         spx_word32_t s0 = __riscv_vmv_x_s_i32m2_i32(vstate);
         spx_word16_t yi, nyi;
         yi = EXTRACT16(SATURATE(ADD32(EXTEND32(x[i]), PSHR32(s0, LPC_SHIFT)), 32767));
         nyi = NEG16(yi);
         vstate = __riscv_vslide1down_vx_i32m2(vstate, 0, vl_ord);
         vstate = __riscv_vwmacc_vx_i32m2(vstate, (int16_t)nyi, vden, vl_ord);
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
      /* Review K1, as iir_mem16. */
      vint16m1_t vnum = __riscv_vle16_v_i16m1(num, vl_ord);
      vint32m2_t vstate = __riscv_vslideup_vx_i32m2_tu(
            __riscv_vmv_v_x_i32m2(0, vlmax),
            __riscv_vle32_v_i32m2(mem, vl_ord), 0, vl_ord);

      for (i = 0; i < N; i++)
      {
         spx_word32_t s0 = __riscv_vmv_x_s_i32m2_i32(vstate);
         spx_word16_t yi;
         yi = EXTRACT16(SATURATE(ADD32(EXTEND32(x[i]), PSHR32(s0, LPC_SHIFT)), 32767));
         vstate = __riscv_vslide1down_vx_i32m2(vstate, 0, vl_ord);
         vstate = __riscv_vwmacc_vx_i32m2(vstate, x[i], vnum, vl_ord);
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
      /* Review K1, as fir_mem16: state in registers, VL=ord, no per-sample
       * mem round trip. x may alias y: x[i] is consumed into the products
       * before y[i] is stored, per sample. */
      vint16m1_t vnum = __riscv_vle16_v_i16m1(num, vl_ord);
      vint16m1_t vden = __riscv_vle16_v_i16m1(den, vl_ord);
      vint32m2_t vstate = __riscv_vslideup_vx_i32m2_tu(
            __riscv_vmv_v_x_i32m2(0, vlmax),
            __riscv_vle32_v_i32m2(mem, vl_ord), 0, vl_ord);

      for (i = 0; i < N; i++)
      {
         spx_word32_t s0 = __riscv_vmv_x_s_i32m2_i32(vstate);
         spx_word16_t yi, nyi;
         yi = EXTRACT16(SATURATE(ADD32(EXTEND32(x[i]), PSHR32(s0, LPC_SHIFT)), 32767));
         nyi = NEG16(yi);
         /* mem[j] = mem[j+1] + num[j]*xi + den[j]*nyi */
         vstate = __riscv_vslide1down_vx_i32m2(vstate, 0, vl_ord);
         vstate = __riscv_vwmacc_vx_i32m2(vstate, x[i], vnum, vl_ord);
         vstate = __riscv_vwmacc_vx_i32m2(vstate, (int16_t)nyi, vden, vl_ord);
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
static inline vint32m2_t sumsq_step(vint32m2_t part, vint16m1_t w, int pre_shift, int up, size_t vl)
{
   vint16m1_t w2 = up ? __riscv_vsll_vx_i16m1(w, (unsigned int)pre_shift, vl)
                      : __riscv_vsra_vx_i16m1(w, (unsigned int)pre_shift, vl);
   return __riscv_vwmacc_vv_i32m2(part, w2, w2, vl);
}

static inline spx_word32_t rvv_sumsq_blocked(const spx_word16_t *x, int len, int pre_shift, int up)
{
   size_t vlmax = __riscv_vsetvlmax_e32m2();
   vint32m2_t acc = __riscv_vmv_v_x_i32m2(0, vlmax);
   vint32m1_t zero = __riscv_vmv_v_x_i32m1(0, 1);
   size_t vl, n = (size_t)len >> 2;
   size_t off = 0;
   while (n > 0)
   {
      vl = __riscv_vsetvl_e16m1(n);
      vint32m2_t part = __riscv_vmv_v_x_i32m2(0, vlmax);
#ifdef SPEEX_RVV_SEGV4
      /* one segment load replaces four strided loads; field k holds
       * exactly the elements the k-th vlse16 loaded, so the shifted
       * operands and the MAC order (k = 0..3) are unchanged */
      vint16m1x4_t ws = __riscv_vlseg4e16_v_i16m1x4(x + off, vl);
      part = sumsq_step(part, __riscv_vget_v_i16m1x4_i16m1(ws, 0), pre_shift, up, vl);
      part = sumsq_step(part, __riscv_vget_v_i16m1x4_i16m1(ws, 1), pre_shift, up, vl);
      part = sumsq_step(part, __riscv_vget_v_i16m1x4_i16m1(ws, 2), pre_shift, up, vl);
      part = sumsq_step(part, __riscv_vget_v_i16m1x4_i16m1(ws, 3), pre_shift, up, vl);
#else
      {
         size_t k;
         for (k = 0; k < 4; k++)
         {
            vint16m1_t w = __riscv_vlse16_v_i16m1(x + off + k, 4 * sizeof(spx_word16_t), vl);
            part = sumsq_step(part, w, pre_shift, up, vl);
         }
      }
#endif
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
static void qmf_synth_c(const spx_word16_t *x1, const spx_word16_t *x2, const spx_word16_t *a, spx_word16_t *y, int N, int M, spx_word16_t *mem1, spx_word16_t *mem2, char *stack);

/* QMF synthesis, fixed-form (review K2): the round-11 schedule interpreter
 * built 5 int[4*M] tables per call and walked them term by term (5 array
 * reads + a switch per vector MAC). The per-pair term pattern is static, so
 * the two substeps are written out directly — 8 window loads and 16
 * widening MACs per tap pair, with the x11/x10 vectors reused between the
 * substeps exactly as the scalar reference reuses x11/x21 and x10/x20.
 *
 * Bit-exactness: per output the accumulation sequence is literally the
 * scalar one (y0: +a0*x11, +(NEG16(a0))*x21, ... with NEG16's wrap for
 * -32768 preserved by the (int16_t) negation), lanes are independent
 * output groups, and the PSHR/SATURATE tail is unchanged. The yyp/yy2p
 * pre-reversal turns the per-lane window index into an ascending +4-byte
 * stride (no negative strides).
 *
 * Unsupported shapes (M,N not multiples of 4 / N2 odd) fall back to a
 * verbatim copy of the C reference instead of silently doing nothing. */
static void rvv_qmf_synth(const spx_word16_t *x1, const spx_word16_t *x2, const spx_word16_t *a, spx_word16_t *y, int N, int M, spx_word16_t *mem1, spx_word16_t *mem2, char *stack)
{
   int i;
   int M2, N2;
   VARDECL(spx_word16_t *xx1);
   VARDECL(spx_word16_t *xx2);
   VARDECL(spx_word16_t *yyp);
   VARDECL(spx_word16_t *yy2p);
   size_t vlmax = __riscv_vsetvlmax_e32m2();
   vint32m2_t zero32 = __riscv_vmv_v_x_i32m2(0, vlmax);

   M2 = M>>1;
   N2 = N>>1;
   if ((M & 3) || (N2 & 1) || M2 < 2)
   {
      qmf_synth_c(x1, x2, a, y, N, M, mem1, mem2, stack);
      return;
   }

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
      size_t G = (size_t)N2>>1;   /* output groups; N2 multiple of 2 */
      size_t off = 0, n = G;
      int j;
      int p = N2+M2-2;            /* yyp = mirror of xx: yyp[p-k] = xx[k] */
      while (n > 0)
      {
         size_t vl = __riscv_vsetvl_e16m1(n);
         size_t base = off<<1;   /* i = 2l for lane l */
         vint32m2_t acc0 = zero32, acc1 = zero32, acc2 = zero32, acc3 = zero32;
         int x10q = p - (N2-2) + (int)base;  /* window idx + per-lane -i term */
         for (j = 0; j < M2; j += 2)
         {
            int q11 = p - (N2-1+j) + (int)base;
            spx_word16_t a0 = a[2*j], a1 = a[2*j+1];
            spx_word16_t a2 = a[2*j+2], a3 = a[2*j+3];
            vint16m1_t w11, w11b, w10, w10b, wB, wBb;
            /* substep A: y0 += a0*x11 - a0*x21 ; y1 += a1*x11 + a1*x21
                          y2 += a0*x10 - a0*x20 ; y3 += a1*x10 + a1*x20 */
            w11  = __riscv_vlse16_v_i16m1(yyp  + q11, 4, vl);
            w11b = __riscv_vlse16_v_i16m1(yy2p + q11, 4, vl);
            w10  = __riscv_vlse16_v_i16m1(yyp  + x10q, 4, vl);
            w10b = __riscv_vlse16_v_i16m1(yy2p + x10q, 4, vl);
            acc0 = __riscv_vwmacc_vx_i32m2(acc0, a0, w11, vl);
            acc0 = __riscv_vwmacc_vx_i32m2(acc0, (int16_t)(-a0), w11b, vl);
            acc1 = __riscv_vwmacc_vx_i32m2(acc1, a1, w11, vl);
            acc1 = __riscv_vwmacc_vx_i32m2(acc1, a1, w11b, vl);
            acc2 = __riscv_vwmacc_vx_i32m2(acc2, a0, w10, vl);
            acc2 = __riscv_vwmacc_vx_i32m2(acc2, (int16_t)(-a0), w10b, vl);
            acc3 = __riscv_vwmacc_vx_i32m2(acc3, a1, w10, vl);
            acc3 = __riscv_vwmacc_vx_i32m2(acc3, a1, w10b, vl);
            /* substep B: rolling window xx[N2+j]; y0/y1 take it, y2/y3
             * reuse x11/x21 — the scalar reference's exact swap */
            wB  = __riscv_vlse16_v_i16m1(yyp  + (p-(N2+j)) + (int)base, 4, vl);
            wBb = __riscv_vlse16_v_i16m1(yy2p + (p-(N2+j)) + (int)base, 4, vl);
            acc0 = __riscv_vwmacc_vx_i32m2(acc0, a2, wB, vl);
            acc0 = __riscv_vwmacc_vx_i32m2(acc0, (int16_t)(-a2), wBb, vl);
            acc1 = __riscv_vwmacc_vx_i32m2(acc1, a3, wB, vl);
            acc1 = __riscv_vwmacc_vx_i32m2(acc1, a3, wBb, vl);
            acc2 = __riscv_vwmacc_vx_i32m2(acc2, a2, w11, vl);
            acc2 = __riscv_vwmacc_vx_i32m2(acc2, (int16_t)(-a2), w11b, vl);
            acc3 = __riscv_vwmacc_vx_i32m2(acc3, a3, w11, vl);
            acc3 = __riscv_vwmacc_vx_i32m2(acc3, a3, w11b, vl);
            x10q = p - (N2+j) + (int)base;
         }
         for (i = 0; i < 4; i++)
         {
            if (i == 0) { acc0 = __riscv_vadd_vx_i32m2(acc0, 1 << 14, vl); acc0 = __riscv_vsra_vx_i32m2(acc0, 15, vl); acc0 = __riscv_vmin_vx_i32m2(acc0, 32767, vl); acc0 = __riscv_vmax_vx_i32m2(acc0, -32767, vl); }
            if (i == 1) { acc1 = __riscv_vadd_vx_i32m2(acc1, 1 << 14, vl); acc1 = __riscv_vsra_vx_i32m2(acc1, 15, vl); acc1 = __riscv_vmin_vx_i32m2(acc1, 32767, vl); acc1 = __riscv_vmax_vx_i32m2(acc1, -32767, vl); }
            if (i == 2) { acc2 = __riscv_vadd_vx_i32m2(acc2, 1 << 14, vl); acc2 = __riscv_vsra_vx_i32m2(acc2, 15, vl); acc2 = __riscv_vmin_vx_i32m2(acc2, 32767, vl); acc2 = __riscv_vmax_vx_i32m2(acc2, -32767, vl); }
            if (i == 3) { acc3 = __riscv_vadd_vx_i32m2(acc3, 1 << 14, vl); acc3 = __riscv_vsra_vx_i32m2(acc3, 15, vl); acc3 = __riscv_vmin_vx_i32m2(acc3, 32767, vl); acc3 = __riscv_vmax_vx_i32m2(acc3, -32767, vl); }
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

   for (i = 0; i < M2; i++)
      mem1[2*i+1] = xx1[i];
   for (i = 0; i < M2; i++)
      mem2[2*i+1] = xx2[i];
}

/* verbatim C reference for shapes the vector form does not support */
static void qmf_synth_c(const spx_word16_t *x1, const spx_word16_t *x2, const spx_word16_t *a, spx_word16_t *y, int N, int M, spx_word16_t *mem1, spx_word16_t *mem2, char *stack)
{
   int i, j;
   int M2, N2;
   VARDECL(spx_word16_t *xx1);
   VARDECL(spx_word16_t *xx2);

   M2 = M>>1;
   N2 = N>>1;
   ALLOC(xx1, M2+N2, spx_word16_t);
   ALLOC(xx2, M2+N2, spx_word16_t);

   for (i = 0; i < N2; i++)
      xx1[i] = x1[N2-1-i];
   for (i = 0; i < M2; i++)
      xx1[N2+i] = mem1[2*i+1];
   for (i = 0; i < N2; i++)
      xx2[i] = x2[N2-1-i];
   for (i = 0; i < M2; i++)
      xx2[N2+i] = mem2[2*i+1];

   for (i = 0; i < N2; i += 2) {
      spx_sig_t y0, y1, y2, y3;
      spx_word16_t x10, x20;

      y0 = y1 = y2 = y3 = 0;
      x10 = xx1[N2-2-i];
      x20 = xx2[N2-2-i];

      for (j = 0; j < M2; j += 2) {
         spx_word16_t x11, x21;
         spx_word16_t a0, a1;

         a0 = a[2*j];
         a1 = a[2*j+1];
         x11 = xx1[N2-1+j-i];
         x21 = xx2[N2-1+j-i];

         /* We multiply twice by the same coef to avoid overflows */
         y0 = MAC16_16(MAC16_16(y0, a0, x11), NEG16(a0), x21);
         y1 = MAC16_16(MAC16_16(y1, a1, x11), a1, x21);
         y2 = MAC16_16(MAC16_16(y2, a0, x10), NEG16(a0), x20);
         y3 = MAC16_16(MAC16_16(y3, a1, x10), a1, x20);

         a0 = a[2*j+2];
         a1 = a[2*j+3];
         x10 = xx1[N2+j-i];
         x20 = xx2[N2+j-i];

         y0 = MAC16_16(MAC16_16(y0, a0, x10), NEG16(a0), x20);
         y1 = MAC16_16(MAC16_16(y1, a1, x10), a1, x20);
         y2 = MAC16_16(MAC16_16(y2, a0, x11), NEG16(a0), x21);
         y3 = MAC16_16(MAC16_16(y3, a1, x11), a1, x21);
      }
      y[2*i] = EXTRACT16(SATURATE32(PSHR32(y0,15),32767));
      y[2*i+1] = EXTRACT16(SATURATE32(PSHR32(y1,15),32767));
      y[2*i+2] = EXTRACT16(SATURATE32(PSHR32(y2,15),32767));
      y[2*i+3] = EXTRACT16(SATURATE32(PSHR32(y3,15),32767));
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

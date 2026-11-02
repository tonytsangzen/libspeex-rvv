/* RVV implementation of the open-loop pitch search (ltp.c) and the
 * element-wise tails of pitch_gain_search_3tap.
 *
 * open_loop_nbest_pitch: the branchy top-N selection loop is replaced by N
 * argmax passes. The C insertion list is provably equal to the top-N
 * cross-multiplied ratios with first-occurrence priority, so each pass keeps
 * a per-lane running (num, den, lag) triple (strict > keeps the earliest
 * chunk inside a lane; chunks advance in ascending lag order) and a final
 * scalar reduction picks max ratio with ties going to the smaller lag —
 * exactly the C result for every N. All compares replicate MULT16_16 on
 * int16-ranged operands, so 32-bit vector multiplies are exact. Masked-out
 * winners get num=-1, which loses to any real candidate (num>=0) and to the
 * C initial state (score=-1, ener=0) the same way the first accept does.
 *
 * The windowed-energy recursion keeps its scalar rolling loop because the C
 * code clamps every intermediate to >=0 (a non-associative min); only the
 * per-lag squares are vectorized. Saturation scan/restore are plain
 * order-free reductions over the same sample range.
 *
 * pitch_gain_search_3tap tails: new_target update and excitation build are
 * pure element-wise MAC/truncate loops (no aliasing: the exc sources sit at
 * strictly negative offsets from the exc window), replicated macro for
 * macro. Only valid for the fixed-point build. */
#ifndef SPEEX_LTP_RVV_H
#define SPEEX_LTP_RVV_H

#include "rvv_arch.h"

#if defined(FIXED_POINT)

#ifndef R14_NO_OPENLOOP
#define OVERRIDE_OPEN_LOOP_NBEST_PITCH 1

void open_loop_nbest_pitch(spx_word16_t *sw, int start, int end, int len, int *pitch, spx_word16_t *gain, int N, char *stack)
{
   int i, j, n;
   VARDECL(spx_word32_t *corr);
   VARDECL(spx_word16_t *corr16);
   VARDECL(spx_word16_t *ener16);
   spx_word32_t *energy;   /* aliases corr, like the C reference */
   VARDECL(spx_word16_t *num);
   VARDECL(spx_word16_t *den);
   spx_word32_t e0;
   int cshift=0, eshift=0;
   int scaledown = 0;
   int nl = end-start+1;
   size_t vlmax = __riscv_vsetvlmax_e16m1(); /* == e32m2 lane count at VLEN=256 */

   ALLOC(corr16, nl, spx_word16_t);
   ALLOC(ener16, nl, spx_word16_t);
   ALLOC(corr, nl, spx_word32_t);
   energy = corr;
   ALLOC(num, nl, spx_word16_t);
   ALLOC(den, nl, spx_word16_t);

   for (i=0;i<N;i++)
      pitch[i]=start;

   /* Saturation scan over sw[-end..len): OR of ABS16(sw[i])>16383, an
      order-free reduction (NEG16 wrap included). */
   {
      const spx_word16_t *p = sw - end;
      int total = end + len;
      int maxabs = -32768;
      size_t off = 0;
      while (off < (size_t)total)
      {
         size_t vl = __riscv_vsetvl_e16m2((size_t)total - off);
         vint16m2_t v = __riscv_vle16_v_i16m2(p + off, vl);
         vint16m2_t neg = __riscv_vrsub_vx_i16m2(v, 0, vl);
         /* gcc-13 vmerge_vvm(op1, op2, m) = m ? op2 : op1 (assembly operand
            order: op1 is the false branch, NOT the spec's vs1-true order) */
         vint16m2_t ax = __riscv_vmerge_vvm_i16m2(v, neg,
               __riscv_vmslt_vx_i16m2_b8(v, 0, vl), vl);
         vint16m1_t red = __riscv_vredmax_vs_i16m2_i16m1(ax,
               __riscv_vmv_v_x_i16m1(-32768, 1), vl);
         int m = __riscv_vmv_x_s_i16m1_i16(red);
         if (m > maxabs)
            maxabs = m;
         off += vl;
      }
      if (maxabs > 16383)
         scaledown = 1;
   }
   if (scaledown)
   {
      spx_word16_t *p = sw - end;
      int total = end + len;
      size_t off = 0;
      while (off < (size_t)total)
      {
         size_t vl = __riscv_vsetvl_e16m2((size_t)total - off);
         vint16m2_t v = __riscv_vle16_v_i16m2(p + off, vl);
         __riscv_vse16_v_i16m2(p + off, __riscv_vsra_vx_i16m2(v, 1, vl), vl);
         off += vl;
      }
   }
   energy[0]=inner_prod(sw-start, sw-start, len);
   e0=inner_prod(sw, sw, len);
   {
      VARDECL(spx_word32_t *sqa);
      VARDECL(spx_word32_t *sqb);
      /* Ascending-address windows for the entering/leaving samples; the
         rolling loop walks both backwards. */
      const spx_word16_t *pa = sw - end;
      const spx_word16_t *pb = sw + (len - end);
      int k;
      ALLOC(sqa, nl-1, spx_word32_t);
      ALLOC(sqb, nl-1, spx_word32_t);
      {
         size_t off = 0;
         while (off < (size_t)(nl-1))
         {
            size_t vl = __riscv_vsetvl_e16m2((size_t)(nl-1) - off);
            vint16m2_t va = __riscv_vle16_v_i16m2(pa + off, vl);
            vint16m2_t vb = __riscv_vle16_v_i16m2(pb + off, vl);
            __riscv_vse32_v_i32m4(sqa + off,
                  __riscv_vsra_vx_i32m4(__riscv_vwmul_vv_i32m4(va, va, vl), 6, vl), vl);
            __riscv_vse32_v_i32m4(sqb + off,
                  __riscv_vsra_vx_i32m4(__riscv_vwmul_vv_i32m4(vb, vb, vl), 6, vl), vl);
            off += vl;
         }
      }
      for (k=0;k<nl-1;k++)
      {
         spx_word32_t e = ADD32(energy[k], sqa[nl-2-k]);
         e = SUB32(e, sqb[nl-2-k]);
         if (e<0)
            e = 0;
         energy[k+1] = e;
      }
   }

#ifdef FIXED_POINT
   eshift = normalize16(energy, ener16, 32766, nl);
#endif
   pitch_xcorr(sw, sw-end, corr, len, nl, stack);

#ifdef FIXED_POINT
   cshift = normalize16(corr, corr16, 180, nl);
   if (scaledown)
   {
      spx_word16_t *p = sw - end;
      int total = end + len;
      size_t off = 0;
      while (off < (size_t)total)
      {
         size_t vl = __riscv_vsetvl_e16m2((size_t)total - off);
         vint16m2_t v = __riscv_vle16_v_i16m2(p + off, vl);
         __riscv_vse16_v_i16m2(p + off, __riscv_vsll_vx_i16m2(v, 1, vl), vl);
         off += vl;
      }
   }
#endif
   /* Top-N selection as N argmax passes (see header comment). */
   {
      size_t off = 0;
      while (off < (size_t)nl)
      {
         size_t vl = __riscv_vsetvl_e16m1((size_t)nl - off);
         vint16m1_t c = __riscv_vle16_v_i16m1(corr16 + off, vl);
         vint16m1_t en = __riscv_vle16_v_i16m1(ener16 + off, vl);
         /* tmp = (spx_word16_t)(corr16*corr16): exact int product truncated
            at the word16 store. */
         vint16m1_t nn = __riscv_vncvt_x_x_w_i16m1(__riscv_vwmul_vv_i32m2(c, c, vl), vl);
         /* den = ADD16(1, ener16): int16 wrap add. */
         vint16m1_t dd = __riscv_vadd_vx_i16m1(en, 1, vl);
         __riscv_vse16_v_i16m1(num + off, nn, vl);
         __riscv_vse16_v_i16m1(den + off, dd, vl);
         off += vl;
      }
      /* C leaves slots [nl, N) at their init value (start) when there are
         fewer lags than N; masked entries would otherwise win a pass. */
      int npasses = (N < nl ? N : nl);
      for (n=0;n<npasses;n++)
      {
         vint32m2_t rn = __riscv_vmv_v_x_i32m2(-1, vlmax);
         vint32m2_t rd = __riscv_vmv_v_x_i32m2(0, vlmax);
         vuint32m2_t ri = __riscv_vmv_v_x_u32m2(0xffffffffu, vlmax);
         spx_word32_t ln[vlmax], ld[vlmax], li[vlmax];
         int bn, bd;
         unsigned bi;
         int l;
         off = 0;
         while (off < (size_t)nl)
         {
            size_t vl = __riscv_vsetvl_e16m1((size_t)nl - off);
            vint32m2_t a = __riscv_vwcvt_x_x_v_i32m2(
                  __riscv_vle16_v_i16m1(num + off, vl), vl);
            vint32m2_t b = __riscv_vwcvt_x_x_v_i32m2(
                  __riscv_vle16_v_i16m1(den + off, vl), vl);
            /* pA = num*run_den, pB = run_num*den; operands stay int16-ranged
               so the 32-bit multiplies are exact like MULT16_16. */
            vint32m2_t pA = __riscv_vmul_vv_i32m2(a, rd, vl);
            vint32m2_t pB = __riscv_vmul_vv_i32m2(rn, b, vl);
            vbool16_t m = __riscv_vmslt_vv_i32m2_b16(pB, pA, vl); /* pA > pB */
            vuint32m2_t ci = __riscv_vadd_vx_u32m2(__riscv_vid_v_u32m2(vl),
                  (unsigned)(start + (int)off), vl);
            rn = __riscv_vmerge_vvm_i32m2(rn, a, m, vl);
            rd = __riscv_vmerge_vvm_i32m2(rd, b, m, vl);
            ri = __riscv_vmerge_vvm_u32m2(ri, ci, m, vl);
            off += vl;
         }
         __riscv_vse32_v_i32m2(ln, rn, vlmax);
         __riscv_vse32_v_i32m2(ld, rd, vlmax);
         __riscv_vse32_v_u32m2(li, ri, vlmax);
         bn = (int)ln[0]; bd = (int)ld[0]; bi = (unsigned)li[0];
         for (l=1;l<(int)vlmax;l++)
         {
            /* Lexicographic: candidate beats the running best when its
               cross-multiplied ratio is higher, or equal with a smaller lag
               (first occurrence in C scan order). Products stay exact: all
               operands are int16-ranged. */
            spx_word32_t pA = ln[l]*bd;
            spx_word32_t pB = bn*ld[l];
            if (pA > pB || (pA == pB && li[l] < bi))
            {
               bn = (int)ln[l]; bd = (int)ld[l]; bi = (unsigned)li[l];
            }
         }
         pitch[n] = (int)bi;
         if (bd != 0) /* real winner (init lanes carry den==0) */
            num[(int)bi - start] = -1;
      }
   }
   if (gain)
   {
      for (j=0;j<N;j++)
      {
         spx_word16_t g;
         i=pitch[j];
         g = DIV32(SHL32(EXTEND32(corr16[i-start]),cshift), 10+SHR32(MULT16_16(spx_sqrt(e0),spx_sqrt(SHL32(EXTEND32(ener16[i-start]),eshift))),6));
         if (g<0)
            g = 0;
         gain[j]=g;
      }
   }
}
#endif /* R14_NO_OPENLOOP */

/* new_target[i] = SUB16(new_target[i],
 *    EXTRACT16(PSHR32((gain0*x2[i] + gain1*x1[i]) + gain2*x0[i], 6)))
 * — element-wise, every macro replicated (wrap adds, rounding shift,
 * truncation). */
static inline void rvv_gain_update_new_target(spx_word16_t *new_target,
   const spx_word16_t *x0, const spx_word16_t *x1, const spx_word16_t *x2,
   spx_word16_t gain0, spx_word16_t gain1, spx_word16_t gain2, int nsf)
{
   size_t off = 0;
   while (off < (size_t)nsf)
   {
      size_t vl = __riscv_vsetvl_e16m1((size_t)nsf - off);
      vint16m1_t a0 = __riscv_vle16_v_i16m1(x0 + off, vl);
      vint16m1_t a1 = __riscv_vle16_v_i16m1(x1 + off, vl);
      vint16m1_t a2 = __riscv_vle16_v_i16m1(x2 + off, vl);
      vint32m2_t acc = __riscv_vwmul_vx_i32m2(a2, gain0, vl);
      acc = __riscv_vwmacc_vx_i32m2(acc, gain1, a1, vl);
      acc = __riscv_vwmacc_vx_i32m2(acc, gain2, a0, vl);
      acc = __riscv_vadd_vx_i32m2(acc, 32, vl);
      acc = __riscv_vsra_vx_i32m2(acc, 6, vl);
      vint16m1_t r = __riscv_vncvt_x_x_w_i16m1(acc, vl);
      vint16m1_t nt = __riscv_vle16_v_i16m1(new_target + off, vl);
      nt = __riscv_vsub_vv_i16m1(nt, r, vl);
      __riscv_vse16_v_i16m1(new_target + off, nt, vl);
      off += vl;
   }
}

/* exc[j] = MAC16_16(exc[j], g7, src[j]) for j in [from,to). The caller has
 * zeroed exc and the source range sits at strictly negative offsets from the
 * exc window, so the read-modify-write cannot overlap between lanes. */
static inline void rvv_gain_build_exc(spx_sig_t *exc, const spx_word16_t *src,
                                      int from, int to, spx_word16_t g7)
{
   size_t off = (size_t)from;
   while (off < (size_t)to)
   {
      size_t vl = __riscv_vsetvl_e16m1((size_t)to - off);
      vint16m1_t e = __riscv_vle16_v_i16m1(src + off, vl);
      vint32m2_t acc = __riscv_vle32_v_i32m2(exc + off, vl);
      acc = __riscv_vwmacc_vx_i32m2(acc, g7, e, vl);
      __riscv_vse32_v_i32m2(exc + off, acc, vl);
      off += vl;
   }
}

#endif /* FIXED_POINT */

#endif /* SPEEX_LTP_RVV_H */

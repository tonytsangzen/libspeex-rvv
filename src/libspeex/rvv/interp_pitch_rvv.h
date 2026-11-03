/* RVV implementation of interp_pitch (filters.c, static helper).
 *
 * The 7-tap correlation search stays scalar but calls inner_prod, which is
 * already the RVV override. The final 80-sample interpolation loop is
 * entry-parallel: lane t interpolates output sample t, and for a fixed tap
 * k every lane reads exc[off + c0 + k + t] — a plain contiguous load with
 * the base moved by k. Per sample:
 *   tmp   = sum_k exc[..] * shift_filt[maxi-1][k]   (exact int16*int16
 *           products, wrap-around int32 adds — order-free)
 *   interp= (int16)((tmp + (1<<14)) >> 15)          (PSHR32 + truncating cast)
 * The maxi==0 branch is SHL32(exc,15) fed through the same PSHR.
 *
 * exc is indexed with negative offsets exactly like the scalar reference
 * (the caller guarantees the history before exc is valid).
 * Only valid for the fixed-point build; otherwise filters.c falls back. */
#ifndef SPEEX_INTERP_PITCH_RVV_H
#define SPEEX_INTERP_PITCH_RVV_H

#include "rvv_arch.h"

#if defined(FIXED_POINT)

static int interp_pitch(
spx_word16_t *exc,          /*decoded excitation*/
spx_word16_t *interp,          /*decoded excitation*/
int pitch,               /*pitch period*/
int len
)
{
   int i,j,k;
   spx_word32_t corr[4][7];
   spx_word32_t maxcorr;
   int maxi, maxj;
   size_t vlmax = __riscv_vsetvlmax_e32m2();
   vint32m2_t zero32 = __riscv_vmv_v_x_i32m2(0, vlmax);

   {
      /* the 7 correlations are consecutive lags of one x stream — exactly
         pitch_xcorr's contract (corr[nb_pitch-1-i] = inner_prod(x, y+i,
         len)), so a single batched sweep replaces 7 inner_prod calls with
         identical per-dot arithmetic. */
      spx_word32_t corr_flat[7];
      pitch_xcorr(exc, exc-pitch-3, corr_flat, len, 7, 0);
      for (i=0;i<7;i++)
         corr[0][i] = corr_flat[6-i];
   }
   for (i=0;i<3;i++)
   {
      for (j=0;j<7;j++)
      {
         int i1, i2;
         spx_word32_t tmp=0;
         i1 = 3-j;
         if (i1<0)
            i1 = 0;
         i2 = 10-j;
         if (i2>7)
            i2 = 7;
         for (k=i1;k<i2;k++)
            tmp += MULT16_32_Q15(shift_filt[i][k],corr[0][j+k-3]);
         corr[i+1][j] = tmp;
      }
   }
   maxi=maxj=0;
   maxcorr = corr[0][0];
   for (i=0;i<4;i++)
   {
      for (j=0;j<7;j++)
      {
         if (corr[i][j] > maxcorr)
         {
            maxcorr = corr[i][j];
            maxi=i;
            maxj=j;
         }
      }
   }
   {
      /* interp[i] reads exc[i + c0 + k] with c0 = maxj - pitch - 6
       * (maxi>0), or exc[i + c0 + 3] (maxi==0). */
      int c0 = maxj - pitch - 6;
      size_t off = 0, n = (size_t)len;
      while (n > 0)
      {
         size_t vl = __riscv_vsetvl_e16m1(n);
         vint32m2_t acc = zero32;
         if (maxi > 0)
         {
            for (k = 0; k < 7; k++)
            {
               vint16m1_t w = __riscv_vle16_v_i16m1(exc + off + c0 + k, vl);
               acc = __riscv_vwmacc_vx_i32m2(acc, shift_filt[maxi-1][k], w, vl);
            }
         }
         else
         {
            vint16m1_t w = __riscv_vle16_v_i16m1(exc + off + c0 + 3, vl);
            acc = __riscv_vwmacc_vx_i32m2(acc, 1, w, vl);
            acc = __riscv_vsll_vx_i32m2(acc, 15, vl);
         }
         acc = __riscv_vadd_vx_i32m2(acc, 1 << 14, vl);
         acc = __riscv_vsra_vx_i32m2(acc, 15, vl);
         __riscv_vse16_v_i16m1(interp + off, __riscv_vncvt_x_x_w_i16m1(acc, vl), vl);
         off += vl;
         n -= vl;
      }
   }
   return pitch-maxj+3;
}

#endif /* FIXED_POINT */

#endif /* SPEEX_INTERP_PITCH_RVV_H */

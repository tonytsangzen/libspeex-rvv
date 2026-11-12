/* RVV implementation of lsp_to_lpc (lsp.c, fixed-point version).
 *
 * The scalar code cascades m second-order sections over an impulse,
 * filling the xp/xq matrices row by row; the per-stage inner loop is a
 * pure element-wise update over contiguous row segments:
 *   xp[i+1][j+2] = (xp[i][j+2] - MULT16_32_Q14(freqn[2i],  xp[i][j+1]))
 *                + xp[i][j]
 * (xq likewise with freqn[2i+1]). Lanes = row position j.
 *
 * MULT16_32_Q14(a,b) splits b into high (b>>14) and low (b&0x3fff) 16-bit
 * halves: a*hi + ((a*lo)>>14), each product an exact int16*int16 widening
 * MAC and the shift arithmetic — all replicated exactly with vector ops.
 * Row adds are int32 wrap (ADD32/SUB32), so element order is free.
 * The scalar prologue (freqn, matrix zero/trivial entries), the trailing
 * column of each stage and the final ak extraction stay scalar.
 *
 * Only valid for the fixed-point build; otherwise lsp.c falls back. */
#ifndef SPEEX_LSP_RVV_H
#define SPEEX_LSP_RVV_H

#include "rvv_arch.h"

#if defined(FIXED_POINT)

void lsp_to_lpc(const spx_lsp_t *freq,spx_coef_t *ak,int lpcrdr, char *stack)
{
    int i,j;
    spx_word32_t xout1,xout2,xin;
    spx_word32_t mult, a;
    vint64m4_t zero64;
    int zero64_init = 0;
    VARDECL(spx_word16_t *freqn);
    VARDECL(spx_word32_t **xp);
    VARDECL(spx_word32_t *xpmem);
    VARDECL(spx_word32_t **xq);
    VARDECL(spx_word32_t *xqmem);
    int m = lpcrdr>>1;
    size_t vlmax = __riscv_vsetvlmax_e32m2();
    vint32m2_t zero32 = __riscv_vmv_v_x_i32m2(0, vlmax);
    vint16m1_t zero16 = __riscv_vmv_v_x_i16m1(0, vlmax);

    ALLOC(xp, (m+1), spx_word32_t*);
    ALLOC(xpmem, (m+1)*(lpcrdr+1+2), spx_word32_t);

    ALLOC(xq, (m+1), spx_word32_t*);
    ALLOC(xqmem, (m+1)*(lpcrdr+1+2), spx_word32_t);

    for(i=0; i<=m; i++) {
      xp[i] = xpmem + i*(lpcrdr+1+2);
      xq[i] = xqmem + i*(lpcrdr+1+2);
    }

    /* work out 2cos terms in Q14 */

    ALLOC(freqn, lpcrdr, spx_word16_t);
    for (i=0;i<lpcrdr;i++)
       freqn[i] = ANGLE2X(freq[i]);

    #define QIMP  21   /* scaling for impulse */

    xin = SHL32(EXTEND32(1), (QIMP-1)); /* 0.5 in QIMP format */

    /* first col and last non-zero values of each row are trivial */

    for(i=0;i<=m;i++) {
     xp[i][1] = 0;
     xp[i][2] = xin;
     xp[i][2+2*i] = xin;
     xq[i][1] = 0;
     xq[i][2] = xin;
     xq[i][2+2*i] = xin;
    }

    /* 2nd row (first output row) is trivial */

    xp[1][3] = -MULT16_32_Q14(freqn[0],xp[0][2]);
    xq[1][3] = -MULT16_32_Q14(freqn[1],xq[0][2]);

    xout1 = xout2 = 0;

    /* now generate remaining rows */

    for(i=1;i<m;i++) {

      int count = 2*(i+1)-2;
      spx_word16_t ap = freqn[2*i], aq = freqn[2*i+1];
      /* constants across chunks: build once per row, not per chunk */
      vint16m1_t ap16 = __riscv_vmv_v_x_i16m1(ap, vlmax);
      vint16m1_t aq16 = __riscv_vmv_v_x_i16m1(aq, vlmax);
      size_t off = 0, n = (size_t)count;
      j = 1;
      while (n > 0)
      {
         size_t vl = __riscv_vsetvl_e16m1(n);
         /* p row */
         {
            /* MULT16_32_Q14(ap, b) via the exact i64 product:
             * ap*b = ap*(hi*2^14 + lo), and arithmetic-shifting the exact
             * i64 sum right by 14 gives ap*hi + (ap*lo)>>14 with no
             * intermediate wrap; truncating to i32 is then congruent mod
             * 2^32 to the C ADD32 form (vnsra = shift+narrow in one op).
             * |ap| <= 2^15, |lo| < 2^14 so ap*lo never wraps i32; |hi|
             * would need |b| >= 2^28 to overflow int16 (xp stays ~2^24). */
            if (!zero64_init)
            {
               zero64 = __riscv_vmv_v_x_i64m4(0, vlmax);
               zero64_init = 1;
            }
            vint64m4_t prod = __riscv_vwmacc_vx_i64m4(zero64, ap,
                  __riscv_vle32_v_i32m2(xp[i] + j + 1, vl), vl);
            vint32m2_t term = __riscv_vnsra_wx_i32m2(prod, 14, vl);
            vint32m2_t out = __riscv_vadd_vv_i32m2(
               __riscv_vsub_vv_i32m2(__riscv_vle32_v_i32m2(xp[i] + j + 2, vl),
                                     term, vl),
               __riscv_vle32_v_i32m2(xp[i] + j, vl), vl);
            __riscv_vse32_v_i32m2(xp[i+1] + j + 2, out, vl);
         }
         /* q row */
         {
            vint64m4_t prod = __riscv_vwmacc_vx_i64m4(zero64, aq,
                  __riscv_vle32_v_i32m2(xq[i] + j + 1, vl), vl);
            vint32m2_t term = __riscv_vnsra_wx_i32m2(prod, 14, vl);
            vint32m2_t out = __riscv_vadd_vv_i32m2(
               __riscv_vsub_vv_i32m2(__riscv_vle32_v_i32m2(xq[i] + j + 2, vl),
                                     term, vl),
               __riscv_vle32_v_i32m2(xq[i] + j, vl), vl);
            __riscv_vse32_v_i32m2(xq[i+1] + j + 2, out, vl);
         }
         off += vl;
         j += vl;
         n -= vl;
      }
      (void)off;

      /* for last col xp[i][j+2] = xq[i][j+2] = 0 */

      mult = MULT16_32_Q14(freqn[2*i],xp[i][j+1]);
      xp[i+1][j+2] = SUB32(xp[i][j], mult);
      mult = MULT16_32_Q14(freqn[2*i+1],xq[i][j+1]);
      xq[i+1][j+2] = SUB32(xq[i][j], mult);
    }

    /* process last row to extra a{k} */

    /* The C loop is a disguised element-wise form: iteration j uses
     * xout1/xout2 carried from iteration j-1, which are exactly
     * xp[m][j+1]/xq[m][j+1]. Substituting, a[j] (j>=2) is
     * ADD32(ADD32(xp[m][j+2], xp[m][j+1]),
     *       SUB32(xq[m][j+2], xq[m][j+1])) -- note the xp pair ADDS and
     * the xq pair SUBTRACTS -- under the same mod-2^32 arithmetic
     * (two's-complement add/sub are associative modulo 2^32), and
     * PSHR32/clamp are element-wise - so the serial chain vectorizes
     * bit-exactly. j=1 is the scalar seed (xout1=xout2=0). */
    {
       int shift = QIMP-13;
       a = PSHR32(xp[m][3] + xq[m][3], shift);
       if (a < -32767) a = -32767;
       if (a > 32767) a = 32767;
       ak[0] = (short)a;
       {
          /* clamp in i32 BEFORE narrowing: the C code clamps the shifted
           * value to +-32767 and only then stores as short; narrowing
           * first would wrap out-of-range values instead of clamping */
          size_t off = 1, n = (size_t)lpcrdr - 1;
          while (n > 0)
          {
             size_t vl = __riscv_vsetvl_e16m1(n);
             /* lane e covers j = off+e+1, so xp[j+2] = xp[m][off+e+3]:
              * cur/prv bases are off+3 / off+2 (off is the ak index minus
              * 1, not j) */
             vint32m2_t xp_cur = __riscv_vle32_v_i32m2(xp[m] + off + 3, vl);
             vint32m2_t xp_prv = __riscv_vle32_v_i32m2(xp[m] + off + 2, vl);
             vint32m2_t xq_cur = __riscv_vle32_v_i32m2(xq[m] + off + 3, vl);
             vint32m2_t xq_prv = __riscv_vle32_v_i32m2(xq[m] + off + 2, vl);
             vint32m2_t sum = __riscv_vadd_vv_i32m2(
                  __riscv_vadd_vv_i32m2(xp_cur, xp_prv, vl),
                  __riscv_vsub_vv_i32m2(xq_cur, xq_prv, vl), vl);
             vint32m2_t shifted = __riscv_vsra_vx_i32m2(
                  __riscv_vadd_vx_i32m2(sum, 1 << (shift - 1), vl),
                  shift, vl);
             vint32m2_t clamped = __riscv_vmin_vv_i32m2(
                  __riscv_vmax_vv_i32m2(shifted,
                     __riscv_vmv_v_x_i32m2(-32767, vl), vl),
                  __riscv_vmv_v_x_i32m2(32767, vl), vl);
             __riscv_vse16_v_i16m1(ak + off,
                  __riscv_vncvt_x_x_w_i16m1(clamped, vl), vl);
             off += vl;
             n -= vl;
          }
       }
    }

}

#endif /* FIXED_POINT */

#endif /* SPEEX_LSP_RVV_H */

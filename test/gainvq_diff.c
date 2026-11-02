/* Differential test for pitch_gain_search_3tap_vq: verbatim scalar
 * reference vs the linked libspeex version (RVV when enabled).
 * Build & run under qemu like test/qmf_diff.c. */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "ltp.h"
#include "arch.h"

#include <stdio.h>
#include <stdlib.h>

#ifndef VERY_LARGE32
#define VERY_LARGE32 ((spx_word32_t)0x7fffffff)
#endif

static inline spx_word32_t compute_pitch_error_ref(spx_word16_t *C, spx_word16_t *g, spx_word16_t pitch_control)
{
   spx_word32_t sum = 0;
   sum = ADD32(sum,MULT16_16(MULT16_16_16(g[0],pitch_control),C[0]));
   sum = ADD32(sum,MULT16_16(MULT16_16_16(g[1],pitch_control),C[1]));
   sum = ADD32(sum,MULT16_16(MULT16_16_16(g[2],pitch_control),C[2]));
   sum = SUB32(sum,MULT16_16(MULT16_16_16(g[0],g[1]),C[3]));
   sum = SUB32(sum,MULT16_16(MULT16_16_16(g[2],g[1]),C[4]));
   sum = SUB32(sum,MULT16_16(MULT16_16_16(g[2],g[0]),C[5]));
   sum = SUB32(sum,MULT16_16(MULT16_16_16(g[0],g[0]),C[6]));
   sum = SUB32(sum,MULT16_16(MULT16_16_16(g[1],g[1]),C[7]));
   sum = SUB32(sum,MULT16_16(MULT16_16_16(g[2],g[2]),C[8]));
   return sum;
}

static int pitch_gain_search_3tap_vq_ref(
  const signed char *gain_cdbk,
  int                gain_cdbk_size,
  spx_word16_t      *C16,
  spx_word16_t       max_gain
)
{
  const signed char *ptr=gain_cdbk;
  int                best_cdbk=0;
  spx_word32_t       best_sum=-VERY_LARGE32;
  spx_word16_t       g[3];
  spx_word16_t       pitch_control=64;
  spx_word16_t       gain_sum;
  int                i;

  for (i=0;i<gain_cdbk_size;i++) {
    ptr = gain_cdbk+4*i;
    g[0]=ADD16((spx_word16_t)ptr[0],32);
    g[1]=ADD16((spx_word16_t)ptr[1],32);
    g[2]=ADD16((spx_word16_t)ptr[2],32);
    gain_sum = (spx_word16_t)ptr[3];

    {
      spx_word32_t sum = 0;
      sum = ADD32(sum,MULT16_16(MULT16_16_16(g[0],pitch_control),C16[0]));
      sum = ADD32(sum,MULT16_16(MULT16_16_16(g[1],pitch_control),C16[1]));
      sum = ADD32(sum,MULT16_16(MULT16_16_16(g[2],pitch_control),C16[2]));
      sum = SUB32(sum,MULT16_16(MULT16_16_16(g[0],g[1]),C16[3]));
      sum = SUB32(sum,MULT16_16(MULT16_16_16(g[2],g[1]),C16[4]));
      sum = SUB32(sum,MULT16_16(MULT16_16_16(g[2],g[0]),C16[5]));
      sum = SUB32(sum,MULT16_16(MULT16_16_16(g[0],g[0]),C16[6]));
      sum = SUB32(sum,MULT16_16(MULT16_16_16(g[1],g[1]),C16[7]));
      sum = SUB32(sum,MULT16_16(MULT16_16_16(g[2],g[2]),C16[8]));
      sum = compute_pitch_error_ref(C16, g, pitch_control);
      if (sum>best_sum && gain_sum<=max_gain) {
        best_sum=sum;
        best_cdbk=i;
      }
    }
  }

  return best_cdbk;
}

/* the lib exposes pitch_gain_search_3tap_vq as static — redeclare via
 * its C++-mangled-free symbol is impossible; instead the test calls the
 * REAL path through a wrapper compiled into the lib: see ltp.h hook.
 * For the diff we access it through the same translation-unit trick:
 * ltp.c's static is not linkable, so lib exports pitch_gain_search_3tap_vq
 * only when OVERRIDE is set — declare it here. */
int pitch_gain_search_3tap_vq(const signed char *gain_cdbk, int gain_cdbk_size, spx_word16_t *C16, spx_word16_t max_gain);

static unsigned rs = 0xfeed1234u;
static int rnd16(void)
{
   rs = rs * 1103515245u + 12345u;
   return (int)((rs >> 8) & 0xffffu) - 32768;
}

int main(void)
{
   static signed char cdbk[4*128];
   static spx_word16_t C16[9];
   static spx_word16_t x1[512], x2[512], ya[1024], yb[1024];
   static spx_word16_t m1a[80], m1b[80], m2a[80], m2b[80];
   static spx_word16_t aa[80];
   static char stack[16384];
   long fails = 0, total = 0;
   int trial, i;

   for (trial = 0; trial < 2000; trial++)
   {
      int size = (trial % 4) * 16 + 16;   /* 16/32/48/64 entries */
      spx_word16_t max_gain = (spx_word16_t)(trial % 3 == 0 ? 128 : (rnd16() & 63) + 31);
      for (i = 0; i < 4*size; i++)
         cdbk[i] = (signed char)rnd16();
      for (i = 0; i < 9; i++)
         C16[i] = (spx_word16_t)rnd16();

      {
         int ra = pitch_gain_search_3tap_vq_ref(cdbk, size, C16, max_gain);
         int rb = pitch_gain_search_3tap_vq(cdbk, size, C16, max_gain);
         total++;
         if (ra != rb)
         {
            if (fails < 3)
            {
               printf("MISMATCH trial=%d size=%d max_gain=%d ref=%d got=%d\n",
                      trial, size, max_gain, ra, rb);
               printf("C16:"); for (i=0;i<9;i++) printf(" %d", C16[i]); printf("\n");
               for (i = 0; i < size; i++)
               {
                  spx_word16_t g[3];
                  g[0]=ADD16((spx_word16_t)cdbk[4*i],32);
                  g[1]=ADD16((spx_word16_t)cdbk[4*i+1],32);
                  g[2]=ADD16((spx_word16_t)cdbk[4*i+2],32);
                  printf("  e%02d: sum=%11ld gain_sum=%d g=(%d,%d,%d)%s\n", i,
                         (long)compute_pitch_error_ref(C16, g, 64),
                         (int)(spx_word16_t)cdbk[4*i+3], g[0], g[1], g[2],
                         (i==ra)?" <=ref":(i==rb)?" <=got":"");
               }
            }
            fails++;
         }
      }
   }

   /* per-tap isolation: single-coefficient sweeps with full dump */
   {
      const int NFRAMES = 160;
      const int MQMF = 24;
      int t, k;
      for (t = 0; t < MQMF; t++)
      {
         int bad = -1;
         for (i = 0; i < MQMF; i++) aa[i] = 0;
         aa[t] = 12345;
         for (i = 0; i < NFRAMES; i++) { x1[i] = (spx_word16_t)rnd16(); x2[i] = (spx_word16_t)rnd16(); }
         for (i = 0; i < MQMF; i++) { m1a[i] = (spx_word16_t)rnd16(); m1b[i] = m1a[i]; m2a[i] = (spx_word16_t)rnd16(); m2b[i] = m2a[i]; }
         qmf_synth_ref(x1, x2, aa, ya, NFRAMES, MQMF, m1a, m2a, stack);
         qmf_synth(x1, x2, aa, yb, NFRAMES, MQMF, m1b, m2b, stack);
         for (i = 0; i < 2*NFRAMES; i++)
            if (ya[i] != yb[i]) { bad = i; break; }
         printf("TAP-ISO t=%d bad=%d\n", t, bad);
         if (bad >= 0 && t < 4)
         {
            printf("  ref y[0..7]:"); for (k = 0; k < 8; k++) printf(" %d", ya[k]); printf("\n");
            printf("  got y[0..7]:"); for (k = 0; k < 8; k++) printf(" %d", yb[k]); printf("\n");
            printf("  x1[0..5]:"); for (k = 0; k < 6; k++) printf(" %d", x1[k]); printf("\n");
            printf("  x2[0..5]:"); for (k = 0; k < 6; k++) printf(" %d", x2[k]); printf("\n");
            printf("  m1[0..3]:"); for (k = 0; k < 4; k++) printf(" %d", m1a[k]); printf("\n");
         }
         if (bad >= 0) fails++;
      }
   }
   printf("%ld/%ld mismatches\n", fails, total);
   return fails != 0;
}

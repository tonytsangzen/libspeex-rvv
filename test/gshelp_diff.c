/* Differential test for the pitch_gain_search_3tap RVV tail helpers
 * (rvv_gain_build_exc / rvv_gain_update_new_target in rvv/ltp_rvv.h):
 * verbatim scalar references vs the static-inline vector helpers, run
 * directly so any codegen-dependent divergence shows up on-device.
 *
 * Build & run like test/inner_prod_diff.c (link libspeex.a for arch.h).
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "arch.h"
#include "ltp.h"
#include "stack_alloc.h"
#include "math_approx.h"
#define R14_NO_OPENLOOP 1   /* skip open_loop body: needs static pitch_xcorr */
#include "rvv/ltp_rvv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long rng_state = 0x5bf03635UL;
static unsigned rnd(void)
{
   rng_state = rng_state*1664525UL + 1013904223UL;
   return (rng_state >> 8) & 0xffff;
}

int main(void)
{
   static const int nsfs[] = {1, 2, 7, 8, 15, 16, 17, 31, 32, 33, 40, 41, 64, 80};
   long total = 0, fails = 0;
   int si, trial;

   for (si=0; si<(int)(sizeof(nsfs)/sizeof(nsfs[0])); si++)
      for (trial=0; trial<300; trial++)
      {
         int nsf = nsfs[si];
         int pitch = 17 + (trial % 100);
         spx_word16_t *x0 = malloc(nsf*sizeof(spx_word16_t));
         spx_word16_t *x1 = malloc(nsf*sizeof(spx_word16_t));
         spx_word16_t *x2 = malloc(nsf*sizeof(spx_word16_t));
         spx_word16_t *nt1 = malloc(nsf*sizeof(spx_word16_t));
         spx_word16_t *nt2 = malloc(nsf*sizeof(spx_word16_t));
         /* exc window with valid history BELOW the pointer: exc2 points at
          * the END of a real history region inside one allocation, so the
          * exc2[j-pp]/[j-pp-pitch] reads land in initialized memory
          * (review R4: they used to run off the front of the block). */
         int hist = pitch + pitch + 8;   /* deepest read is about -(2*pitch+1) */
         spx_sig_t *excb1 = malloc((hist+nsf)*sizeof(spx_sig_t));
         spx_sig_t *excb2 = malloc((hist+nsf)*sizeof(spx_sig_t));
         spx_word16_t *exc2b = malloc(hist*sizeof(spx_word16_t));
         spx_word16_t *exc2 = exc2b + hist;
         spx_sig_t *exc1 = excb1 + hist, *exc2v = excb2 + hist;
         spx_word16_t gain[3];
         int i, g7;

         for (i=0;i<nsf;i++)
         {
            x0[i] = (spx_word16_t)((int)rnd()-32768);
            x1[i] = (spx_word16_t)((int)rnd()-32768);
            x2[i] = (spx_word16_t)((int)rnd()-32768);
            nt1[i] = nt2[i] = (spx_word16_t)((int)rnd()-32768);
         }
         for (i=0;i<hist;i++)
            exc2b[i] = (spx_word16_t)((int)rnd()-32768);   /* exc2[-hist..-1] */
         gain[0] = (spx_word16_t)((int)rnd()%257 - 96);
         gain[1] = (spx_word16_t)((int)rnd()%257 - 96);
         gain[2] = (spx_word16_t)((int)rnd()%257 - 96);

         /* ---- new_target update ---- */
         {
            for (i=0;i<nsf;i++)
            {
               spx_word32_t tmp = ADD32(ADD32(MULT16_16(gain[0],x2[i]),MULT16_16(gain[1],x1[i])),
                                    MULT16_16(gain[2],x0[i]));
               nt1[i] = SUB16(nt1[i], EXTRACT16(PSHR32(tmp,6)));
            }
            rvv_gain_update_new_target(nt2, x0, x1, x2, gain[0], gain[1], gain[2], nsf);
            total++;
            if (memcmp(nt1, nt2, nsf*sizeof(spx_word16_t)) != 0)
            {
               if (fails < 5)
                  for (i=0;i<nsf;i++)
                     if (nt1[i]!=nt2[i]) {
                        printf("NT-MISMATCH nsf=%d trial=%d at [%d]: ref=%d got=%d\n",
                               nsf, trial, i, (int)nt1[i], (int)nt2[i]);
                        break;
                     }
               fails++;
            }
         }

         /* ---- exc build (mirrors the 3-iteration caller incl. ranges) ---- */
         {
            int pp, tmp1, tmp3;
            { int z; for (z=0;z<nsf;z++) { exc1[z]=0; exc2v[z]=0; } }
            for (i=0;i<3;i++)
            {
               pp = pitch+1-i;
               tmp1=nsf; if (tmp1>pp) tmp1=pp;
               tmp3=nsf; if (tmp3>pp+pitch) tmp3=pp+pitch;
               g7 = SHL16(gain[2-i],7);
               /* scalar reference */
               {
                  int j;
                  for (j=0;j<tmp1;j++)
                     exc1[j]=MAC16_16(exc1[j],g7,exc2[j-pp]);
                  for (j=tmp1;j<tmp3;j++)
                     exc1[j]=MAC16_16(exc1[j],g7,exc2[j-pp-pitch]);
               }
               /* vector helper */
               if (tmp1>0)
                  rvv_gain_build_exc(exc2v, exc2-pp, 0, tmp1, g7);
               if (tmp3>tmp1)
                  rvv_gain_build_exc(exc2v, exc2-pp-pitch, tmp1, tmp3, g7);
            }
            total++;
            if (memcmp(exc1, exc2v, nsf*sizeof(spx_sig_t)) != 0)
            {
               if (fails < 5)
                  for (i=0;i<nsf;i++)
                     if (exc1[i]!=exc2v[i]) {
                        printf("EXC-MISMATCH nsf=%d trial=%d at [%d]: ref=%ld got=%ld\n",
                               nsf, trial, i, (long)exc1[i], (long)exc2v[i]);
                        break;
                     }
               fails++;
            }
         }

         free(x0); free(x1); free(x2); free(nt1); free(nt2);
         free(excb1); free(excb2); free(exc2b);
      }

   printf("%ld/%ld mismatches\n", fails, total);
   return fails != 0;
}

/* Differential test for the vq_nbest / vq_nbest_sign N==1 vector tails:
 * verbatim upstream scalar references vs the linked libspeex versions.
 * Stresses first-min tie-breaking, sign handling, wrap-prone raw distances
 * and the N>1 insertion fallback.
 *
 * Round 15 layout: the RVV build's vq_nbest/_sign take the codebook in
 * COLUMN-MAJOR order (codebook[j*entries + e], as produced by the
 * compute_weighted_codebook override); the scalar references keep the
 * upstream entry-major layout, so the driver feeds each side its own copy
 * of the same values.
 *
 * Build & run under qemu like test/inner_prod_diff.c.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "arch.h"
#include "vq.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef VERY_LARGE32
#define VERY_LARGE32 ((spx_word32_t)0x7fffffff)
#endif

/* ---- verbatim upstream references (vq.c) ---- */

static void vq_nbest_ref(spx_word16_t *in, const spx_word16_t *codebook, int len, int entries, spx_word32_t *E, int N, int *nbest, spx_word32_t *best_dist, char *stack)
{
   int i,j,k,used;
   used=0;
   for (i=0;i<entries;i++)
   {
      spx_word32_t dist=0;
      for (j=0;j<len;j++)
         dist = MAC16_16(dist,in[j],*codebook++);
      dist = SUB32(SHR32(E[i],1),dist);
      if (i<N || dist<best_dist[N-1])
      {
         for (k=N-1; (k >= 1) && (k > used || dist < best_dist[k-1]); k--)
         {
            best_dist[k]=best_dist[k-1];
            nbest[k] = nbest[k-1];
         }
         best_dist[k]=dist;
         nbest[k]=i;
         used++;
      }
   }
}

static void vq_nbest_sign_ref(spx_word16_t *in, const spx_word16_t *codebook, int len, int entries, spx_word32_t *E, int N, int *nbest, spx_word32_t *best_dist, char *stack)
{
   int i,j,k, sign, used;
   used=0;
   for (i=0;i<entries;i++)
   {
      spx_word32_t dist=0;
      for (j=0;j<len;j++)
         dist = MAC16_16(dist,in[j],*codebook++);
      if (dist>0)
      {
         sign=0;
         dist=-dist;
      } else
      {
         sign=1;
      }
      dist = ADD32(dist,SHR32(E[i],1));
      if (i<N || dist<best_dist[N-1])
      {
         for (k=N-1; (k >= 1) && (k > used || dist < best_dist[k-1]); k--)
         {
            best_dist[k]=best_dist[k-1];
            nbest[k] = nbest[k-1];
         }
         best_dist[k]=dist;
         nbest[k]=i;
         used++;
         if (sign)
            nbest[k]+=entries;
      }
   }
}

/* ---- driver ---- */

static unsigned long rng_state = 0x9e3779b9UL;
static unsigned rnd(void)
{
   rng_state = rng_state*1664525UL + 1013904223UL;
   return (rng_state >> 8) & 0xffff;
}

int main(void)
{
   static const int lens[] = {1, 2, 3, 5, 8, 10, 20, 40};
   static const int entrys[] = {1, 2, 7, 15, 16, 17, 31, 32, 64, 127, 128, 320};
   static const int Ns[] = {1, 1, 1, 2, 3, 4}; /* N=1 weighted: the hot path */
   long total = 0, fails = 0;
   int li, ei, ni, trial;
   char stack[16384];

   for (li=0;li<8;li++)
      for (ei=0;ei<12;ei++)
         for (ni=0;ni<6;ni++)
            for (trial=0;trial<60;trial++)
            {
               int len = lens[li], entries = entrys[ei], N = Ns[ni];
               int sign_test = trial & 1;
               spx_word16_t in[64];
               spx_word16_t *cb  = malloc((size_t)entries*len*sizeof(spx_word16_t));
               spx_word16_t *cbT = malloc((size_t)entries*len*sizeof(spx_word16_t));
               spx_word32_t *E   = malloc(entries*sizeof(spx_word32_t));
               int *nb1 = malloc(N*sizeof(int)), *nb2 = malloc(N*sizeof(int));
               spx_word32_t *bd1 = malloc(N*sizeof(spx_word32_t));
               spx_word32_t *bd2 = malloc(N*sizeof(spx_word32_t));
               int i, j, mode = trial % 3;
               /* callers init slots the kernel may leave untouched when
                  entries < N (real callers use VERY_LARGE32-style inits) */
               for (i=0;i<N;i++)
               {
                  nb1[i]=nb2[i]=-1;
                  bd1[i]=bd2[i]=VERY_LARGE32;
               }

               for (j=0;j<len;j++)
               {
                  unsigned v = rnd();
                  in[j] = (spx_word16_t)(mode==0 ? (int)(v%2000)-1000 : (int)v-32768);
               }
               for (i=0;i<entries;i++)
                  for (j=0;j<len;j++)
                  {
                     unsigned v = rnd();
                     cb[(size_t)i*len+j] = (spx_word16_t)(mode==0 ? (int)(v%2000)-1000 : (int)v-32768);
                  }
               for (i=0;i<entries;i++)
                  for (j=0;j<len;j++)
                     cbT[(size_t)j*entries+i] = cb[(size_t)i*len+j];
               for (i=0;i<entries;i++)
               {
                  unsigned v = rnd();
                  if (mode == 2)  /* extreme E to stress SHR32 paths */
                     E[i] = (spx_word32_t)((v&1) ? (v<<14) : -(int)(v<<14));
                  else
                     E[i] = (spx_word32_t)(v % 65536);
               }

               if (sign_test)
                  vq_nbest_sign_ref(in, cb, len, entries, E, N, nb1, bd1, stack);
               else
                  vq_nbest_ref(in, cb, len, entries, E, N, nb1, bd1, stack);
               if (sign_test)
                  vq_nbest_sign(in, cbT, len, entries, E, N, nb2, bd2, stack);
               else
                  vq_nbest(in, cbT, len, entries, E, N, nb2, bd2, stack);

               total++;
               for (i=0;i<N;i++)
                  if (nb1[i] != nb2[i] || bd1[i] != bd2[i])
                  {
                     if (fails < 5)
                        printf("MISMATCH sign=%d len=%d entries=%d N=%d trial=%d mode=%d: "
                               "ref[%d]=(%d,%ld) got[%d]=(%d,%ld)\n",
                               sign_test, len, entries, N, trial, mode,
                               i, nb1[i], (long)bd1[i], i, nb2[i], (long)bd2[i]);
                     fails++;
                     break;
                  }
               free(cb); free(cbT); free(E); free(nb1); free(nb2); free(bd1); free(bd2);
            }

   printf("%ld/%ld mismatches\n", fails, total);
   return fails != 0;
}

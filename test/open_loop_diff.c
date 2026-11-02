/* Differential test for the RVV open_loop_nbest_pitch override: verbatim
 * scalar reference (ltp.c, fixed-point) vs the linked libspeex version.
 * Covers the top-N selection argmax passes, the clamped energy recursion,
 * the scaledown scan/restore and the gain tail, across the codec's real
 * (start,end,len) shapes plus odd/short shapes and saturation-edge data.
 *
 * Build & run under qemu like test/inner_prod_diff.c:
 *   riscv64-linux-gnu-gcc -O2 -march=rv64gcv -static -DHAVE_CONFIG_H \
 *     -DFIXED_POINT -I<build> -Isrc/libspeex -Icmake/conf -Isrc/include \
 *     test/open_loop_diff.c <build>/libspeex.a -lm -o /tmp/oipdiff
 *   qemu-riscv64 /tmp/oipdiff
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "arch.h"
#include "ltp.h"
#include "stack_alloc.h"
#include "math_approx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- verbatim scalar references (must stay in sync with ltp.c) ---- */

static spx_word32_t inner_prod_ref(const spx_word16_t *x, const spx_word16_t *y, int len)
{
   spx_word32_t sum=0;
   len >>= 2;
   while(len--)
   {
      spx_word32_t part=0;
      part = MAC16_16(part,*x++,*y++);
      part = MAC16_16(part,*x++,*y++);
      part = MAC16_16(part,*x++,*y++);
      part = MAC16_16(part,*x++,*y++);
      sum = ADD32(sum,SHR32(part,6));
   }
   return sum;
}

static void pitch_xcorr_ref(const spx_word16_t *_x, const spx_word16_t *_y, spx_word32_t *corr, int len, int nb_pitch, char *stack)
{
   int i;
   for (i=0;i<nb_pitch;i++)
   {
      corr[nb_pitch-1-i]=inner_prod_ref(_x, _y+i, len);
   }
}

int normalize16_ref(const spx_sig_t *x, spx_word16_t *y, spx_sig_t max_scale, int len)
{
   int i;
   spx_sig_t max_val=1;
   int sig_shift;

   for (i=0;i<len;i++)
   {
      spx_sig_t tmp = x[i];
      if (tmp<0)
         tmp = NEG32(tmp);
      if (tmp >= max_val)
         max_val = tmp;
   }

   sig_shift=0;
   while (max_val>max_scale)
   {
      sig_shift++;
      max_val >>= 1;
   }

   for (i=0;i<len;i++)
      y[i] = EXTRACT16(SHR32(x[i], sig_shift));

   return sig_shift;
}

static void open_loop_nbest_pitch_ref(spx_word16_t *sw, int start, int end, int len, int *pitch, spx_word16_t *gain, int N, char *stack)
{
   int i,j,k;
   VARDECL(spx_word32_t *best_score);
   VARDECL(spx_word32_t *best_ener);
   spx_word32_t e0;
   VARDECL(spx_word32_t *corr);
   VARDECL(spx_word16_t *corr16);
   VARDECL(spx_word16_t *ener16);
   spx_word32_t *energy;
   int cshift=0, eshift=0;
   int scaledown = 0;
   ALLOC(corr16, end-start+1, spx_word16_t);
   ALLOC(ener16, end-start+1, spx_word16_t);
   ALLOC(corr, end-start+1, spx_word32_t);
   energy = corr;

   ALLOC(best_score, N, spx_word32_t);
   ALLOC(best_ener, N, spx_word32_t);
   for (i=0;i<N;i++)
   {
        best_score[i]=-1;
        best_ener[i]=0;
        pitch[i]=start;
   }

   for (i=-end;i<len;i++)
   {
      if (ABS16(sw[i])>16383)
      {
         scaledown=1;
         break;
      }
   }
   if (scaledown)
   {
      for (i=-end;i<len;i++)
      {
         sw[i]=SHR16(sw[i],1);
      }
   }
   energy[0]=inner_prod_ref(sw-start, sw-start, len);
   e0=inner_prod_ref(sw, sw, len);
   for (i=start;i<end;i++)
   {
      energy[i-start+1] = SUB32(ADD32(energy[i-start],SHR32(MULT16_16(sw[-i-1],sw[-i-1]),6)), SHR32(MULT16_16(sw[-i+len-1],sw[-i+len-1]),6));
      if (energy[i-start+1] < 0)
         energy[i-start+1] = 0;
   }

   eshift = normalize16_ref(energy, ener16, 32766, end-start+1);

   pitch_xcorr_ref(sw, sw-end, corr, len, end-start+1, stack);

   cshift = normalize16_ref(corr, corr16, 180, end-start+1);
   if (scaledown)
   {
      for (i=-end;i<len;i++)
      {
         sw[i]=SHL16(sw[i],1);
      }
   }

   for (i=start;i<=end;i++)
   {
      spx_word16_t tmp = MULT16_16_16(corr16[i-start],corr16[i-start]);
      if (MULT16_16(tmp,best_ener[N-1])>MULT16_16(best_score[N-1],ADD16(1,ener16[i-start])))
      {
         best_score[N-1]=tmp;
         best_ener[N-1]=ener16[i-start]+1;
         pitch[N-1]=i;
         for (j=0;j<N-1;j++)
         {
            if (MULT16_16(tmp,best_ener[j])>MULT16_16(best_score[j],ADD16(1,ener16[i-start])))
            {
               for (k=N-1;k>j;k--)
               {
                  best_score[k]=best_score[k-1];
                  best_ener[k]=best_ener[k-1];
                  pitch[k]=pitch[k-1];
               }
               best_score[j]=tmp;
               best_ener[j]=ener16[i-start]+1;
               pitch[j]=i;
               break;
            }
         }
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

/* ---- driver ---- */

static unsigned long rng_state = 0x12345678UL;
static unsigned rnd(void)
{
   rng_state = rng_state*1664525UL + 1013904223UL;
   return (rng_state >> 8) & 0xffff;
}

int main(void)
{
   static const struct { int start, end, len; } cfgs[] = {
      {17, 144, 160},  /* nb_celp open-loop */
      {17, 144, 40},   /* pitch_search_3tap subframe */
      {17, 144, 41},   /* odd len (tail chunk) */
      {20, 120, 40},   /* PLC */
      {20, 120, 10},   /* short */
      {17, 20, 3},     /* tiny */
      {17, 19, 2},     /* 3 lags */
   };
   const int ncfg = sizeof(cfgs)/sizeof(cfgs[0]);
   static const int Ns[] = {1, 2, 3, 4, 6};
   const int nn = sizeof(Ns)/sizeof(Ns[0]);
   long total = 0, fails = 0;
   int c, ni, trial;
   char stack[16384];

   for (c=0;c<ncfg;c++)
      for (ni=0;ni<nn;ni++)
         for (trial=0;trial<400;trial++)
         {
            int start = cfgs[c].start, end = cfgs[c].end, len = cfgs[c].len;
            int N = Ns[ni];
            int span = end + len;
            spx_word16_t *buf  = malloc((span+8)*sizeof(spx_word16_t));
            spx_word16_t *buf2 = malloc((span+8)*sizeof(spx_word16_t));
            int *p1 = malloc(N*sizeof(int)), *p2 = malloc(N*sizeof(int));
            spx_word16_t *g1 = malloc(N*sizeof(spx_word16_t));
            spx_word16_t *g2 = malloc(N*sizeof(spx_word16_t));
            int use_gain = trial & 1;
            int i, mode = trial % 4;
            spx_word16_t *sw  = buf + 4 + end;   /* room for negative offsets */
            spx_word16_t *sw2 = buf2 + 4 + end;

            for (i=-end;i<len;i++)
            {
               unsigned v = rnd();
               if (mode == 0)              /* full 16-bit noise */
                  sw[i] = (spx_word16_t)(v - 32768);
               else if (mode == 1)         /* speech-like small */
                  sw[i] = (spx_word16_t)((v % 4001) - 2000);
               else if (mode == 2)         /* saturation edge: force scaledown */
                  sw[i] = (spx_word16_t)((v % 2) ? 16383 + (v % 97) : -(int)(16383 + (v % 97)));
               else                        /* periodic + noise (real corr structure) */
                  sw[i] = (spx_word16_t)(3000*((i % 23 < 11) ? 1 : -1) + (int)(v % 400) - 200);
            }
            memcpy(buf2, buf, (span+8)*sizeof(spx_word16_t));

            open_loop_nbest_pitch_ref(sw, start, end, len, p1, use_gain?g1:NULL, N, stack);
            open_loop_nbest_pitch(sw2, start, end, len, p2, use_gain?g2:NULL, N, stack);

            total++;
            for (i=0;i<N;i++)
               if (p1[i] != p2[i] || (use_gain && g1[i] != g2[i]))
               {
                  if (fails < 5)
                     printf("MISMATCH cfg(%d,%d,%d) N=%d trial=%d mode=%d gain=%d: "
                            "ref[%d]=%d/%d got[%d]=%d/%d\n",
                            start, end, len, N, trial, mode, use_gain,
                            i, p1[i], use_gain?g1[i]:0, i, p2[i], use_gain?g2[i]:0);
                  fails++;
                  break;
               }
            /* scaledown must restore sw identically */
            if (memcmp(buf, buf2, (span+8)*sizeof(spx_word16_t)) != 0)
            {
               if (fails < 5)
               {
                  int d;
                  for (d=0;d<span+8;d++)
                     if (buf[d] != buf2[d])
                        break;
                  printf("SW-MISMATCH cfg(%d,%d,%d) N=%d trial=%d mode=%d "
                         "first at buf[%d] (sw[%d]): ref=%d rvv=%d\n",
                         start, end, len, N, trial, mode, d, d-4-end,
                         (int)buf[d], (int)buf2[d]);
               }
               fails++;
            }
            free(buf); free(buf2); free(p1); free(p2); free(g1); free(g2);
         }

   printf("%ld/%ld mismatches\n", fails, total);
   return fails != 0;
}

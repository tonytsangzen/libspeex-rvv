/* Differential test for the QMF kernels vs verbatim scalar references.
 * Build & run under qemu:
 *   riscv64-linux-gnu-gcc -O2 -march=rv64gcv -static -DHAVE_CONFIG_H \
 *     -DFIXED_POINT -D_USE_RVV -I<build> -Isrc/libspeex -Icmake/conf \
 *     -Isrc/include test/qmf_diff.c <build>/libspeex.a -lm -o /tmp/qmfdiff
 *   qemu-riscv64 /tmp/qmfdiff
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "filters.h"
#include "stack_alloc.h"
#include "arch.h"

#include <stdio.h>
#include <stdlib.h>

static int ref_debug = 0;
static int ref_which = 0;

static void qmf_decomp_ref(const spx_word16_t *xx, const spx_word16_t *aa, spx_word16_t *y1, spx_word16_t *y2, int N, int M, spx_word16_t *mem, char *stack)
{
   int i,j,k,M2;
   VARDECL(spx_word16_t *a);
   VARDECL(spx_word16_t *x);
   spx_word16_t *x2;

   ALLOC(a, M, spx_word16_t);
   ALLOC(x, N+M-1, spx_word16_t);
   x2=x+M-1;
   M2=M>>1;
   for (i=0;i<M;i++)
      a[M-i-1]= aa[i];
   for (i=0;i<M-1;i++)
      x[i]=mem[M-i-2];
   for (i=0;i<N;i++)
      x[i+M-1]=SHR16(xx[i],1);
   for (i=0;i<M-1;i++)
      mem[i]=SHR16(xx[N-i-1],1);
   for (i=0,k=0;i<N;i+=2,k++)
   {
      spx_word32_t y1k=0, y2k=0;
      for (j=0;j<M2;j++)
      {
         y1k=ADD32(y1k,MULT16_16(a[j],ADD16(x[i+j],x2[i-j])));
         y2k=SUB32(y2k,MULT16_16(a[j],SUB16(x[i+j],x2[i-j])));
         j++;
         y1k=ADD32(y1k,MULT16_16(a[j],ADD16(x[i+j],x2[i-j])));
         y2k=ADD32(y2k,MULT16_16(a[j],SUB16(x[i+j],x2[i-j])));
      }
      y1[k] = EXTRACT16(SATURATE(PSHR32(y1k,15),32767));
      y2[k] = EXTRACT16(SATURATE(PSHR32(y2k,15),32767));
   }
}

static void qmf_synth_ref(const spx_word16_t *x1, const spx_word16_t *x2, const spx_word16_t *a, spx_word16_t *y, int N, int M, spx_word16_t *mem1, spx_word16_t *mem2, char *stack)
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

         if (ref_debug && i == 0)
            printf("  REF[%d] j=%d: a0=%d a1=%d x11=%d x21=%d x10=%d x20=%d\n",
                   ref_which, j, a0, a1, x11, x21, x10, x20);

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

static unsigned rs = 0xcafef00du;
static int rnd16(void)
{
   rs = rs * 1103515245u + 12345u;
   return (int)((rs >> 8) & 0xffffu) - 32768;
}

static const int param_N[] = {160, 320, 160};
static const int param_M[] = {24, 64, 64};

int main(void)
{
   static spx_word16_t xx[512], y1a[256], y1b[256];
   static spx_word16_t y2a[256], y2b[256];
   static spx_word16_t aa[80], mem_a[80], mem_b[80];
   static spx_word16_t x1[512], x2[512], ya[1024], yb[1024];
   static spx_word16_t m1a[80], m1b[80], m2a[80], m2b[80];
   char stack[16384];
   long fails = 0, total = 0;
   int trial, i, pi, t;

   for (pi = 0; pi < 3; pi++)
   {
      const int NFRAMES = param_N[pi];
      const int MQMF = param_M[pi];
      for (trial = 0; trial < 150; trial++)
      {
         for (i = 0; i < MQMF; i++)
            aa[i] = (spx_word16_t)rnd16();
         for (i = 0; i < NFRAMES; i++)
         {
            unsigned r = rs >> 3;
            xx[i] = (spx_word16_t)((trial & 1) ? (int)(r % 65536) - 32768 : (int)(r % 40000) - 20000);
            rs = rs * 1103515245u + 12345u;
            x1[i] = (spx_word16_t)rnd16();
            x2[i] = (spx_word16_t)rnd16();
         }
         for (i = 0; i < MQMF; i++)
         {
            mem_a[i] = (spx_word16_t)rnd16();
            mem_b[i] = mem_a[i];
            m1a[i] = (spx_word16_t)rnd16(); m1b[i] = m1a[i];
            m2a[i] = (spx_word16_t)rnd16(); m2b[i] = m2a[i];
         }

         ref_debug = (trial == 0 && pi == 0);
         ref_which = 1;
         qmf_decomp_ref(xx, aa, y1a, y2a, NFRAMES, MQMF, mem_a, stack);
         qmf_decomp(xx, aa, y1b, y2b, NFRAMES, MQMF, mem_b, stack);
         ref_which = 2;
         ref_debug = (trial == 0 && pi == 0);
         qmf_synth_ref(x1, x2, aa, ya, NFRAMES, MQMF, m1a, m2a, stack);
         ref_debug = 0;
         qmf_synth(x1, x2, aa, yb, NFRAMES, MQMF, m1b, m2b, stack);
         for (i = 0; i < NFRAMES/2; i++)
         {
            total++;
            if (y1a[i] != y1b[i] || y2a[i] != y2b[i])
            {
               if (fails < 8)
                  printf("DECOMP MISMATCH pi=%d trial=%d k=%d ref=(%d,%d) got=(%d,%d)\n",
                         pi, trial, i, y1a[i], y2a[i], y1b[i], y2b[i]);
               fails++;
            }
         }
         for (i = 0; i < 2*NFRAMES; i++)
         {
            total++;
            if (ya[i] != yb[i])
            {
               if (fails < 8)
                  printf("SYNTH MISMATCH pi=%d trial=%d i=%d ref=%d got=%d\n", pi, trial, i, ya[i], yb[i]);
               fails++;
            }
         }
      }
      /* schedule-vs-scalar trace for group i=0 */
      if (pi == 0 && trial == 0)
      {
         const int NFRAMES = param_N[0];
         const int MQMF = param_M[0];
         int M2 = MQMF>>1, N2 = NFRAMES>>1;
         spx_word16_t a_loc[80], xx1[96], xx2[96];
         int x10i, x20i, j, k;
         for (i = 0; i < MQMF; i++) a_loc[i] = aa[i];
         for (i = 0; i < N2; i++) { xx1[i] = x1[N2-1-i]; xx2[i] = x2[N2-1-i]; }
         for (i = 0; i < M2; i++) { xx1[N2+i] = m1b[2*i+1]; xx2[N2+i] = m2b[2*i+1]; }
         /* scalar per-pair partials for y0 (group i=0) */
         x10i = N2-2; x20i = N2-2;
         {
            spx_word32_t y0 = 0, y1s = 0, y2s = 0, y3s = 0;
            printf("SCALAR group0 trace:\n");
            for (j = 0; j < M2; j += 2)
            {
               int x11i = N2-1+j, x21i = N2-1+j;
               y0 = MAC16_16(MAC16_16(y0, a_loc[2*j], xx1[x11i]), NEG16(a_loc[2*j]), xx2[x21i]);
               y2s = MAC16_16(MAC16_16(y2s, a_loc[2*j], xx1[x10i]), NEG16(a_loc[2*j]), xx2[x20i]);
               y1s = MAC16_16(MAC16_16(y1s, a_loc[2*j+1], xx1[x11i]), a_loc[2*j+1], xx2[x21i]);
               y3s = MAC16_16(MAC16_16(y3s, a_loc[2*j+1], xx1[x10i]), a_loc[2*j+1], xx2[x20i]);
               printf("  pair j=%d A: y0=%d y1=%d y2=%d y3=%d (x11=%d x10=%d)\n",
                      j, y0, y1s, y2s, y3s, xx1[x11i], xx1[x10i]);
               y0 = MAC16_16(MAC16_16(y0, a_loc[2*j+2], xx1[N2+j]), NEG16(a_loc[2*j+2]), xx2[N2+j]);
               y2s = MAC16_16(MAC16_16(y2s, a_loc[2*j+2], xx2[x21i]), NEG16(a_loc[2*j+2]), xx1[x11i]);
               y1s = MAC16_16(MAC16_16(y1s, a_loc[2*j+3], xx1[N2+j]), a_loc[2*j+3], xx2[N2+j]);
               y3s = MAC16_16(MAC16_16(y3s, a_loc[2*j+3], xx2[x21i]), a_loc[2*j+3], xx1[x11i]);
               printf("  pair j=%d B: y0=%d y1=%d y2=%d y3=%d (x10'=%d x20'=%d)\n",
                      j, y0, y1s, y2s, y3s, xx1[N2+j], xx2[N2+j]);
               x10i = N2+j; x20i = N2+j;
            }
         }
         /* my schedule simulation for y0 (group i=0) */
         {
            spx_word32_t acc0 = 0;
            int nw = 0, term;
            int s_arr[4*MQMF], s_idx[4*MQMF], s_sign[4*MQMF], s_cidx[4*MQMF];
            x10i = N2-2; x20i = N2-2;
            printf("SCHEDULE y0 trace:\n");
            for (j = 0; j < M2; j += 2)
            {
               int x11i = N2-1+j, x21i = N2-1+j;
               s_arr[nw]=0; s_idx[nw]=x11i; s_sign[nw]=1; s_cidx[nw]=2*j; nw++;
               s_arr[nw]=1; s_idx[nw]=x21i; s_sign[nw]=-1; s_cidx[nw]=2*j; nw++;
               s_arr[nw]=0; s_idx[nw]=x10i; s_sign[nw]=1; s_cidx[nw]=2*j; nw++;
               s_arr[nw]=1; s_idx[nw]=x20i; s_sign[nw]=-1; s_cidx[nw]=2*j; nw++;
               s_arr[nw]=0; s_idx[nw]=x11i; s_sign[nw]=1; s_cidx[nw]=2*j+2; nw++;
               s_arr[nw]=1; s_idx[nw]=N2+j; s_sign[nw]=-1; s_cidx[nw]=2*j+2; nw++;
               x10i = N2+j; x20i = N2+j;
            }
            for (term = 0; term < nw; term++)
            {
               int idx = s_idx[term]; /* lane 0: no lane offset */
               int v = (s_arr[term] ? xx2[idx] : xx1[idx]);
               acc0 += (int)s_sign[term] * a_loc[s_cidx[term]] * v;
               printf("  term%d: %s[%d] sign=%+d a[%d]=%d v=%d -> acc0=%d\n",
                      term, s_arr[term]?"xx2":"xx1", idx, s_sign[term],
                      s_cidx[term], a_loc[s_cidx[term]], v, acc0);
            }
         }
      }
      /* per-tap isolation on the first parameter set */
      if (pi == 0)
      {
         const int NFRAMES = param_N[0];
         const int MQMF = param_M[0];
         for (t = 0; t < MQMF; t++)
         {
            for (i = 0; i < MQMF; i++) aa[i] = 0;
            aa[t] = 12345;
            for (i = 0; i < NFRAMES; i++) { x1[i] = (spx_word16_t)rnd16(); x2[i] = (spx_word16_t)rnd16(); }
            for (i = 0; i < MQMF; i++) { m1a[i] = (spx_word16_t)rnd16(); m1b[i] = m1a[i]; m2a[i] = (spx_word16_t)rnd16(); m2b[i] = m2a[i]; }
            qmf_synth_ref(x1, x2, aa, ya, NFRAMES, MQMF, m1a, m2a, stack);
            qmf_synth(x1, x2, aa, yb, NFRAMES, MQMF, m1b, m2b, stack);
            {
               int bad = -1;
               for (i = 0; i < 2*NFRAMES; i++)
                  if (ya[i] != yb[i]) { bad = i; break; }
               printf("TAP-ISO t=%d bad=%d\n", t, bad);
               if (bad >= 0 && t < 4)
               {
                  printf("  ref y:"); for (i = 0; i < 8; i++) printf(" %d", ya[i]); printf("\n");
                  printf("  got y:"); for (i = 0; i < 8; i++) printf(" %d", yb[i]); printf("\n");
                  printf("  a:"); for (i = 0; i < 8; i++) printf(" %d", aa[i]); printf("\n");
                  printf("  x1[0..3]:"); for (i = 0; i < 4; i++) printf(" %d", x1[i]); printf("\n");
                  printf("  x2[0..3]:"); for (i = 0; i < 4; i++) printf(" %d", x2[i]); printf("\n");
               }
               if (bad >= 0) fails++;
            }
         }
      }
   }
   /* per-tap isolation for qmf_synth: single-coefficient sweeps */
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
   printf("%ld/%ld mismatches (with tap-iso)\n", fails, total);
   return fails != 0;
}

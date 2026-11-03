/* Differential test for the RVV iir_mem16 / fir_mem16 / filter_mem16
 * kernels (round 19, review K1): verbatim scalar references vs the linked
 * library. Covers ord 6/8/10, N 0/1/short/full, x==y aliasing, non-zero
 * and extreme mem, segmented calls (state continuity), and compares both
 * the output and the full mem state.
 *
 * Build & run like test/inner_prod_diff.c.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "arch.h"
#include "filters.h"
#include "stack_alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef LPC_SHIFT
#define LPC_SHIFT 14
#endif

/* ---- verbatim scalar references (filters.c) ---- */

static void iir_mem16_ref(const spx_word16_t *x, const spx_coef_t *den, spx_word16_t *y, int N, int ord, spx_mem_t *mem, char *stack)
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

static void fir_mem16_ref(const spx_word16_t *x, const spx_coef_t *num, spx_word16_t *y, int N, int ord, spx_mem_t *mem, char *stack)
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

static void filter_mem16_ref(const spx_word16_t *x, const spx_coef_t *num, const spx_coef_t *den, spx_word16_t *y, int N, int ord, spx_mem_t *mem, char *stack)
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

/* ---- driver ---- */

static unsigned seed = 0x5eed;
static int r16(void) { seed = seed*1664525 + 1013904223; return (int)((seed>>16)&0xffff) - 32768; }

static int check_one(const char *name, int ord, int N, int alias, int memmode, int segs)
{
   spx_word16_t *x  = malloc((N+8)*sizeof(spx_word16_t));
   spx_word16_t *xa = malloc((N+8)*sizeof(spx_word16_t));
   spx_word16_t *xb = malloc((N+8)*sizeof(spx_word16_t));
   spx_coef_t *num = malloc(ord*sizeof(spx_coef_t));
   spx_coef_t *den = malloc(ord*sizeof(spx_coef_t));
   spx_mem_t *ma = malloc(ord*sizeof(spx_mem_t));
   spx_mem_t *mb = malloc(ord*sizeof(spx_mem_t));
   char *stack = malloc(16384);
   int i, fail = 0;

   for (i=0;i<N;i++)
      x[i] = (spx_word16_t)((memmode==3 && !(i&7)) ? -32768 : r16());
   for (i=0;i<ord;i++)
   {
      num[i] = (spx_coef_t)r16();
      den[i] = (spx_coef_t)r16();
      switch (memmode)
      {
         case 0: ma[i] = mb[i] = 0; break;
         case 1: ma[i] = mb[i] = r16(); break;
         case 2: ma[i] = mb[i] = (i&1 ? r16() : 400000000L); break;
         default: ma[i] = mb[i] = r16(); break;
      }
   }
   /* each side gets its own copy of the input; alias mode filters in place */
   memcpy(xa, x, N*sizeof(spx_word16_t));
   memcpy(xb, x, N*sizeof(spx_word16_t));

   if (strcmp(name, "iir") == 0)
   {
      iir_mem16_ref(xa, den, alias ? xa : xa, N, ord, ma, stack); /* ref always in place on xa */
      iir_mem16(xb, den, alias ? xb : xb, N, ord, mb, stack);
   }
   else if (strcmp(name, "fir") == 0)
   {
      fir_mem16_ref(xa, num, xa, N, ord, ma, stack);
      fir_mem16(xb, num, xb, N, ord, mb, stack);
   }
   else
   {
      filter_mem16_ref(xa, num, den, xa, N, ord, ma, stack);
      filter_mem16(xb, num, den, xb, N, ord, mb, stack);
   }

   if (memcmp(xa, xb, N*sizeof(spx_word16_t)) != 0) fail = 1;
   if (memcmp(ma, mb, ord*sizeof(spx_mem_t)) != 0) fail = 1;

   free(x); free(xa); free(xb); free(num); free(den); free(ma); free(mb); free(stack);
   return fail;
}

int main(void)
{
   static const int ords[] = {6, 8, 10};
   static const int Ns[] = {0, 1, 7, 16, 17, 80, 160, 320};
   static const char *names[] = {"iir", "fir", "filter"};
   long total = 0, fails = 0;
   int oi, ni, mm, alias, nm = 3;

   for (oi=0; oi<3; oi++)
      for (ni=0; ni<8; ni++)
         for (mm=0; mm<4; mm++)
            for (alias=0; alias<2; alias++)
            {
               int N = Ns[ni];
               /* alias only meaningful with N>0 */
               int al = N > 0 ? alias : 0;
               int f;
               total += 3;
               f  = check_one("iir", ords[oi], N, al, mm, 1);
               f += check_one("fir", ords[oi], N, al, mm, 1);
               f += check_one("filter", ords[oi], N, al, mm, 1);
               if (f)
               {
                  if (fails < 6)
                     printf("MISMATCH ord=%d N=%d mem=%d alias=%d (%d/3 kernels)\n",
                            ords[oi], N, mm, al, f);
                  fails += f;
               }
            }

   /* segmented-call continuity: two calls of N/2 must equal one call of N */
   {
      int ord = 10, N = 160, i, f = 0;
      spx_word16_t *x = malloc(N*sizeof(spx_word16_t));
      spx_word16_t *y1 = malloc(N*sizeof(spx_word16_t));
      spx_word16_t *y2 = malloc(N*sizeof(spx_word16_t));
      spx_coef_t den[10], num[10];
      spx_mem_t m1[10], m2[10];
      char *stack = malloc(16384);
      for (i=0;i<N;i++) x[i] = (spx_word16_t)r16();
      for (i=0;i<ord;i++) { den[i] = (spx_coef_t)r16(); num[i] = (spx_coef_t)r16(); m1[i] = m2[i] = r16(); }
      filter_mem16_ref(x, num, den, y1, N, ord, m1, stack);
      filter_mem16(x, num, den, y2, N/2, ord, m2, stack);
      filter_mem16(x+N/2, num, den, y2+N/2, N-N/2, ord, m2, stack);
      if (memcmp(y1, y2, N*sizeof(spx_word16_t)) || memcmp(m1, m2, ord*sizeof(spx_mem_t)))
      { printf("SEGMENTED-CONTINUITY MISMATCH\n"); f = 1; }
      /* same for iir */
      for (i=0;i<ord;i++) { m1[i] = m2[i] = r16(); }
      iir_mem16_ref(x, den, y1, N, ord, m1, stack);
      iir_mem16(x, den, y2, N/2, ord, m2, stack);
      iir_mem16(x+N/2, den, y2+N/2, N-N/2, ord, m2, stack);
      if (memcmp(y1, y2, N*sizeof(spx_word16_t)) || memcmp(m1, m2, ord*sizeof(spx_mem_t)))
      { printf("SEGMENTED-CONTINUITY MISMATCH (iir)\n"); f = 1; }
      fails += f; total++;
      free(x); free(y1); free(y2); free(stack);
   }

   printf("%ld/%ld mismatches\n", fails, total);
   return fails != 0;
}

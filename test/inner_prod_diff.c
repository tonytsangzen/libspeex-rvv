/* Differential test: scalar reference inner_prod (copied verbatim from
 * ltp.c) vs the linked libspeex inner_prod (RVV override when enabled).
 * Searches for input-dependent divergence, which is exactly what the
 * on-device A/B gate reports as a bit-exactness failure.
 *
 * Built and run under qemu by scripts/rvv-difftest.sh (or manually):
 *   riscv64-linux-gnu-gcc -O2 -march=rv64gcv -static -DHAVE_CONFIG_H \
 *     -DFIXED_POINT -I<build> -Isrc/libspeex -Icmake/conf -Isrc/include \
 *     test/inner_prod_diff.c <build>/libspeex.a -lm -o /tmp/ipdiff
 *   qemu-riscv64 /tmp/ipdiff
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "arch.h"
#include "ltp.h"

#include <stdio.h>
#include <stdlib.h>

/* Verbatim copy of the ltp.c reference (must stay in sync). */
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

static unsigned rs = 0x87654321u;
static unsigned rnd16(void)
{
   rs = rs * 1103515245u + 12345u;
   return (rs >> 8) & 0xffffu;
}

int main(void)
{
   static spx_word16_t x[1024], y[1024];
   static const int lens[] = {0,1,2,3,4,5,8,10,16,17,20,40,41,80,81,160,161,200,320,400};
   long total = 0, fails = 0;
   int trial, li;

   for (trial = 0; trial < 4000; trial++)
   {
      int i;
      for (i = 0; i < 1024; i++)
      {
         unsigned r = rnd16();
         switch (trial & 3)
         {
            case 0: x[i] = (spx_word16_t)r; y[i] = (spx_word16_t)(r ^ 0x5a5a); break;
            case 1: x[i] = (r & 1) ? 32767 : (spx_word16_t)(-(int)(r >> 1)); y[i] = (r & 2) ? -32768 : (spx_word16_t)(r >> 2); break;
            case 2: x[i] = (spx_word16_t)(r % 2000 - 1000); y[i] = (spx_word16_t)(r % 700 - 350); break;
            case 3: x[i] = (r & 1) ? 32767 : (spx_word16_t)(-(int)(r >> 6)); y[i] = (spx_word16_t)(-(int)(r >> 3)); break;
         }
      }
      for (li = 0; li < (int)(sizeof(lens)/sizeof(lens[0])); li++)
      {
         int len = lens[li];
         const spx_word16_t *x2 = x + (rnd16() & 15);
         const spx_word16_t *y2 = y + (rnd16() & 15);
         spx_word32_t a = inner_prod_ref(x2, y2, len);
         spx_word32_t b = inner_prod(x2, y2, len);
         total++;
         if (a != b)
         {
            if (fails < 12)
               printf("MISMATCH trial=%d len=%d ref=%d got=%d (x0=%d y0=%d)\n",
                      trial, len, (int)a, (int)b, (int)x2[0], (int)y2[0]);
            fails++;
         }
      }
   }
   printf("%ld/%ld mismatches\n", fails, total);
   return fails != 0;
}

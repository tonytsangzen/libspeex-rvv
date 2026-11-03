/* Differential test for rvv_inner_prod_batch5 (round 16): five blocked-shift
 * dots in one sweep vs five verbatim scalar inner_prod references.
 * Build & run like test/inner_prod_diff.c.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "arch.h"
#include "ltp.h"

#include <stdio.h>
#include <stdlib.h>

void rvv_inner_prod_batch5(const spx_word16_t *a, const spx_word16_t *b, const spx_word16_t *x,
                           int len, spx_word32_t *out);

static spx_word32_t ip_ref(const spx_word16_t *x, const spx_word16_t *y, int len)
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

static unsigned seed = 0xc0ffee;
static int r16(void) { seed = seed*1664525 + 1013904223; return (int)((seed>>16)&0xffff) - 32768; }

int main(void)
{
   static const int lens[] = {4, 8, 20, 40, 79, 80, 81, 160, 161};
   long total = 0, fails = 0;
   int li, trial;

   for (li=0; li<(int)(sizeof(lens)/sizeof(lens[0])); li++)
      for (trial=0; trial<3000; trial++)
      {
         int len = lens[li];
         spx_word16_t *a  = malloc(len*sizeof(spx_word16_t));
         spx_word16_t *b  = malloc(len*sizeof(spx_word16_t));
         spx_word16_t *x  = malloc(len*sizeof(spx_word16_t));
         spx_word32_t ref[5], got[5];
         int i, mode = trial % 3;
         for (i=0;i<len;i++)
         {
            unsigned v = (unsigned)seed; seed = seed*1664525 + 1013904223;
            if (mode==0) a[i] = b[i] = x[i] = (spx_word16_t)((int)(v & 0xffff) - 32768);
            else if (mode==1) { a[i] = (spx_word16_t)((int)(v & 0xffff) - 32768); b[i] = (spx_word16_t)((int)(v>>3 & 0x3ff) - 512); x[i] = (spx_word16_t)((int)(v>>7 & 0x7f) - 64); }
            else { a[i] = b[i] = (spx_word16_t)((int)(v & 0xffff) - 32768); x[i] = a[i]; }
         }
         ref[0]=ip_ref(a,a,len); ref[1]=ip_ref(b,b,len); ref[2]=ip_ref(x,x,len);
         ref[3]=ip_ref(a,x,len); ref[4]=ip_ref(b,x,len);
         rvv_inner_prod_batch5(a,b,x,len,got);
         total++;
         for (i=0;i<5;i++)
            if (ref[i]!=got[i])
            {
               if (fails<5)
                  printf("MISMATCH len=%d trial=%d mode=%d dot%d: ref=%ld got=%ld\n",
                         len, trial, mode, i, (long)ref[i], (long)got[i]);
               fails++;
               break;
            }
         free(a); free(b); free(x);
      }
   printf("%ld/%ld mismatches\n", fails, total);
   return fails != 0;
}

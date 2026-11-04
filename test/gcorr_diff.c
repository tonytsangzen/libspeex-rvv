/* Differential test for rvv_gain_corr9 (round 23): the nine
 * pitch_gain_search_3tap correlations in one sweep vs nine verbatim
 * scalar inner_prod references. Build & run like test/inner_prod_diff.c.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "arch.h"
#include "ltp.h"

#include <stdio.h>
#include <stdlib.h>

void rvv_gain_corr9(const spx_word16_t *x0, const spx_word16_t *x1, const spx_word16_t *x2,
                    const spx_word16_t *nt, int len, spx_word32_t *c, spx_word32_t *a);

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

static unsigned seed = 0x9e37;
static int r16(void) { seed = seed*1664525 + 1013904223; return (int)((seed>>16)&0xffff) - 32768; }

int main(void)
{
   static const int lens[] = {4, 8, 20, 40, 41, 80, 160, 161};
   long total = 0, fails = 0;
   int li, trial;

   for (li=0; li<(int)(sizeof(lens)/sizeof(lens[0])); li++)
      for (trial=0; trial<1500; trial++)
      {
         int len = lens[li];
         spx_word16_t *x0 = malloc(len*sizeof(spx_word16_t));
         spx_word16_t *x1 = malloc(len*sizeof(spx_word16_t));
         spx_word16_t *x2 = malloc(len*sizeof(spx_word16_t));
         spx_word16_t *nt = malloc(len*sizeof(spx_word16_t));
         spx_word32_t rc[3], ra[6], gc[3], ga[6];
         int i, mode = trial % 3;
         for (i=0;i<len;i++)
         {
            unsigned v = (unsigned)seed; seed = seed*1664525 + 1013904223;
            if (mode == 0) { x0[i]=x1[i]=x2[i]=nt[i]=(spx_word16_t)((int)(v & 0xffff)-32768); }
            else if (mode == 1) { x0[i]=(spx_word16_t)((int)(v&0xffff)-32768); x1[i]=(spx_word16_t)((int)(v>>3 & 0x7ff)-1024); x2[i]=(spx_word16_t)((int)(v>>9 & 0x7f)-64); nt[i]=(spx_word16_t)((int)(v>>15 & 0x3ff)-512); }
            else { x0[i]=x2[i]=(spx_word16_t)((int)(v&0xffff)-32768); x1[i]=nt[i]=0; }
         }
         /* scalar reference: 3 corr dots + 6 upper-triangle dots */
         for (i=0;i<3;i++)
            rc[i] = ip_ref(i==0?x0:(i==1?x1:x2), nt, len);
         ra[0]=ip_ref(x0,x0,len); ra[1]=ip_ref(x0,x1,len); ra[2]=ip_ref(x0,x2,len);
         ra[3]=ip_ref(x1,x1,len); ra[4]=ip_ref(x1,x2,len); ra[5]=ip_ref(x2,x2,len);
         rvv_gain_corr9(x0, x1, x2, nt, len, gc, ga);
         total++;
         for (i=0;i<3;i++)
            if (rc[i] != gc[i])
            { if (fails<5) printf("MISMATCH len=%d trial=%d corr%d: ref=%ld got=%ld\n", len, trial, i, (long)rc[i], (long)gc[i]); fails++; goto next; }
         for (i=0;i<6;i++)
            if (ra[i] != ga[i])
            { if (fails<5) printf("MISMATCH len=%d trial=%d a%d: ref=%ld got=%ld\n", len, trial, i, (long)ra[i], (long)ga[i]); fails++; break; }
         next:
         free(x0); free(x1); free(x2); free(nt);
      }
   printf("%ld/%ld mismatches\n", fails, total);
   return fails != 0;
}

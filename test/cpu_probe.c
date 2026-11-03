/* X60 capability probe v3 (review R5 rewrite).
 *
 * Rules enforced:
 *  - every accumulator / load stream is consumed into a sink printed after
 *    the timing loop (nothing can be dead-code eliminated);
 *  - load addresses rotate per iteration (no hoisting);
 *  - the unit-vs-strided comparison changes its base address the same way;
 *  - results are reported in time-ticks and ns/op (tick rate calibrated
 *    against CLOCK_MONOTONIC); cycles are only derived under the stated
 *    1.6 GHz assumption and labeled as such.
 */
#include <riscv_vector.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static inline unsigned long long rdt(void)
{
   unsigned long long v;
   __asm__ volatile ("rdtime %0" : "=r"(v));
   return v;
}

#define REP 4000
static unsigned seed = 12345;
static int r16(void) { seed = seed*1664525 + 1013904223; return (int)((seed>>16)&0xffff) - 32768; }
static int bufs[8][1024];
static volatile unsigned long long sink;

int main(void)
{
   double tick_hz; int i, k;
   setvbuf(stdout, NULL, _IONBF, 0);

   for (k = 0; k < 8; k++)
      for (i = 0; i < 1024; i++) bufs[k][i] = r16();

   {
      struct timespec ts0, ts1; unsigned long long ta; double el;
      clock_gettime(CLOCK_MONOTONIC, &ts0);
      ta = rdt();
      do { clock_gettime(CLOCK_MONOTONIC, &ts1);
           el = (ts1.tv_sec-ts0.tv_sec) + (ts1.tv_nsec-ts0.tv_nsec)/1e9; } while (el < 0.3);
      tick_hz = (rdt()-ta)/el;
      printf("rdtime = %.2f MHz (calibrated)\n", tick_hz/1e6);
   }

   /* ---- vector MAC: dependent latency, then 2/4/8 independent streams ---- */
   {
      size_t vl = __riscv_vsetvlmax_e32m2();
      vint32m2_t x0 = __riscv_vle32_v_i32m2(bufs[0], vl);
      vint32m2_t x1 = __riscv_vle32_v_i32m2(bufs[1], vl);
      unsigned long long t0, t1; double ns;
      {
         vint32m2_t a = __riscv_vmv_v_x_i32m2(0, vl);
         t0 = rdt();
         for (i = 0; i < REP; i++)
            a = __riscv_vmacc_vx_i32m2(a, bufs[0][i & 7], x0, vl);
         t1 = rdt();
         sink ^= (unsigned long long)__riscv_vmv_x_s_i32m2_i32(a);
         ns = (double)(t1-t0)/tick_hz*1e9/REP;
         printf("vmacc dep-chain e32m2(%zu lanes): %.1f ns/vec-op\n", vl, ns);
      }
      for (k = 2; k <= 8; k <<= 1)
      {
         vint32m2_t a0=__riscv_vmv_v_x_i32m2(0,vl),a1=a0,a2=a0,a3=a0,a4=a0,a5=a0,a6=a0,a7=a0;
         t0 = rdt();
         for (i = 0; i < REP; i++)
         {
            a0 = __riscv_vmacc_vx_i32m2(a0, bufs[0][i & 7], x0, vl);
            a1 = __riscv_vmacc_vx_i32m2(a1, bufs[1][i & 7], x1, vl);
            if (k > 2) { a2 = __riscv_vmacc_vx_i32m2(a2, bufs[2][i & 7], x0, vl);
            a3 = __riscv_vmacc_vx_i32m2(a3, bufs[3][i & 7], x1, vl); }
            if (k > 4) { a4 = __riscv_vmacc_vx_i32m2(a4, bufs[4][i & 7], x0, vl);
            a5 = __riscv_vmacc_vx_i32m2(a5, bufs[5][i & 7], x1, vl);
            a6 = __riscv_vmacc_vx_i32m2(a6, bufs[6][i & 7], x0, vl);
            a7 = __riscv_vmacc_vx_i32m2(a7, bufs[7][i & 7], x1, vl); }
         }
         t1 = rdt();
         /* consume EVERY stream (review R5) */
         sink ^= (unsigned long long)(__riscv_vmv_x_s_i32m2_i32(a0)
            ^ __riscv_vmv_x_s_i32m2_i32(a1) ^ __riscv_vmv_x_s_i32m2_i32(a2)
            ^ __riscv_vmv_x_s_i32m2_i32(a3) ^ __riscv_vmv_x_s_i32m2_i32(a4)
            ^ __riscv_vmv_x_s_i32m2_i32(a5) ^ __riscv_vmv_x_s_i32m2_i32(a6)
            ^ __riscv_vmv_x_s_i32m2_i32(a7));
         ns = (double)(t1-t0)/tick_hz*1e9/(REP*(unsigned)k);
         printf("vmacc %d indep streams: %.1f ns/vec-op (%.1f lane-MAC/ns)\n",
                k, ns, (double)k*vl/((t1-t0)/tick_hz));
      }
   }

   /* ---- widening MAC e16m2 with memory-fed input ---- */
   {
      size_t vl = __riscv_vsetvlmax_e16m2();
      vint32m4_t a = __riscv_vmv_v_x_i32m4(0, vl);
      unsigned long long t0 = rdt(), t1;
      for (i = 0; i < REP; i++)
      {
         vint16m2_t x = __riscv_vle16_v_i16m2((short*)bufs[i & 1], vl);
         a = __riscv_vwmacc_vx_i32m4(a, bufs[2][i & 7], x, vl);
      }
      t1 = rdt();
      sink ^= (unsigned long long)__riscv_vmv_x_s_i32m4_i32(a);
      printf("vwmacc e16m2(%zu lanes)+load: %.1f ns/vec-op (%.2f lane-MAC/ns)\n",
             vl, (double)(t1-t0)/tick_hz*1e9/REP, (double)vl/((t1-t0)/tick_hz));
   }

   /* ---- unit vs strided load: SAME rotation, spans differ (reported) ---- */
   {
      static short wide[8*64*32];   /* strided case spans 2KB per load */
      size_t vl = __riscv_vsetvlmax_e16m2();
      vint32m4_t a = __riscv_vmv_v_x_i32m4(0, vl);
      unsigned long long t0, t1; int rep = REP*2; double ns;
      for (i = 0; i < 8*64*32; i++) wide[i] = (short)r16();
      t0 = rdt();
      for (i = 0; i < rep; i++)
      {
         /* unit: 32 contiguous int16 = 64B, base rotates over 2KB */
         vint16m2_t v = __riscv_vle16_v_i16m2(wide + (i & 31)*64, vl);
         a = __riscv_vwmacc_vx_i32m4(a, 3, v, vl);
      }
      t1 = rdt();
      sink ^= (unsigned long long)__riscv_vmv_x_s_i32m4_i32(a);
      ns = (double)(t1-t0)/tick_hz*1e9/rep;
      printf("vle16 unit (64B span): %.1f ns/vec-op\n", ns);
      t0 = rdt();
      for (i = 0; i < rep; i++)
      {
         /* strided: 32 lanes x stride 64B = 2KB span, same base rotation */
         vint16m2_t v = __riscv_vlse16_v_i16m2(wide + (i & 31), 64, vl);
         a = __riscv_vwmacc_vx_i32m4(a, 3, v, vl);
      }
      t1 = rdt();
      sink ^= (unsigned long long)__riscv_vmv_x_s_i32m4_i32(a);
      ns = (double)(t1-t0)/tick_hz*1e9/rep;
      printf("vlse16 stride 64B (2KB span): %.1f ns/vec-op\n", ns);
   }

   /* ---- memory copy 8MB (mtest3 form: clock_gettime bracketing; the
    * rdt-only bracket showed a 1000x pathological codegen on X60) ---- */
   {
      size_t n = 2*1024*1024;
      int *src = malloc(n*sizeof(int)), *dst = malloc(n*sizeof(int));
      struct timespec ts0, ts1;
      memset(src, 1, n*sizeof(int));
      memset(dst, 0, n*sizeof(int));
      for (i = 0; i < 6; i++)
      {
         size_t off = 0;
         unsigned long long acc = 0, xs;
         double us;
         clock_gettime(CLOCK_MONOTONIC, &ts0);
         while (off < n)
         {
            size_t v = __riscv_vsetvl_e32m2(n - off);
            vint32m2_t w = __riscv_vle32_v_i32m2(src + off, v);
            __riscv_vse32_v_i32m2(dst + off, w, v);
            acc ^= (unsigned long long)__riscv_vmv_x_s_i32m2_i32(w);
            off += v;
         }
         clock_gettime(CLOCK_MONOTONIC, &ts1);
         xs = rdt(); sink ^= acc ^ xs;
         us = (ts1.tv_sec-ts0.tv_sec)*1e6 + (ts1.tv_nsec-ts0.tv_nsec)/1e3;
         if (i >= 1)
            printf("memcpy rep%d: %.0f us (%.0f MB/s r+w)\n", i, us,
                   n*sizeof(int)*2.0/(us*1e-6)/1e6);
      }
      free(src); free(dst);
   }
   /* ---- scalar mul-add, 4 memory-fed chains, all consumed ---- */
   {
      long a0=0,a1=0,a2=0,a3=0;
      unsigned long long t0 = rdt(), t1;
      for (i = 0; i < REP; i++)
      {
         a0 += bufs[0][i & 1023] * 3; a1 += bufs[1][i & 1023] * 3;
         a2 += bufs[2][i & 1023] * 3; a3 += bufs[3][i & 1023] * 3;
      }
      t1 = rdt();
      sink ^= (unsigned long long)(a0^a1^a2^a3);
      printf("scalar 4 indep mul-add chains: %.1f ns/iter (%.2f MAC/ns)\n",
             (double)(t1-t0)/tick_hz*1e9/REP, 4.0/((t1-t0)/tick_hz));
   }
   printf("sink=%llu\n", sink);
   return 0;
}

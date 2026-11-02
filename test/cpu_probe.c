/* X60 capability probe v2: rdtime rate calibrated against clock_gettime,
 * every loop body memory-dependent (no compile-time folding).
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

static int bufs[8][1024];   /* 32KB total, L1-resident */

int main(void)
{
   unsigned long vl8 = __riscv_vsetvlmax_e8m8();
   double tick_hz; int i, k;
   setvbuf(stdout, NULL, _IONBF, 0);
   printf("VLEN = %lu bits (e8m8 VLMAX=%lu elements)\n", vl8, vl8);

   for (k = 0; k < 8; k++)
      for (i = 0; i < 1024; i++) bufs[k][i] = r16();

   /* ---- calibrate rdtime against wall clock ---- */
   {
      struct timespec ts0, ts1;
      unsigned long long ta;
      double el;
      clock_gettime(CLOCK_MONOTONIC, &ts0);
      ta = rdt();
      while (1)
      {
         clock_gettime(CLOCK_MONOTONIC, &ts1);
         el = (ts1.tv_sec - ts0.tv_sec) + (ts1.tv_nsec - ts0.tv_nsec)/1e9;
         if (el > 0.3) break;
      }
      tick_hz = (rdt() - ta)/el;
      printf("rdtime rate = %.2f MHz, tick/cycle @1.6GHz = %.3f\n",
             tick_hz/1e6, tick_hz/1.6e9);
   }

   /* ---- vector MAC throughput, e32m2 (VLEN2048 -> 128 lanes) ---- */
   {
      size_t vl = __riscv_vsetvlmax_e32m2();
      vint32m2_t x0 = __riscv_vle32_v_i32m2(bufs[0], vl);
      vint32m2_t x1 = __riscv_vle32_v_i32m2(bufs[1], vl);
      double cyc;
      unsigned long long t0, t1;
      {
         vint32m2_t a = __riscv_vmv_v_x_i32m2(0, vl);
         t0 = rdt();
         for (i = 0; i < REP; i++)
            a = __riscv_vmacc_vx_i32m2(a, bufs[0][i & 7], x0, vl);
         t1 = rdt();
         cyc = (double)(t1-t0)*1.6e9/tick_hz/REP;
         printf("vmacc dep-chain: %.2f cyc/vec-op (acc=%ld)\n",
                cyc, (long)__riscv_vmv_x_s_i32m2_i32(a));
      }
      {
         vint32m2_t a0 = __riscv_vmv_v_x_i32m2(0, vl), a1 = a0, a2 = a0, a3 = a0;
         t0 = rdt();
         for (i = 0; i < REP; i++)
         {
            a0 = __riscv_vmacc_vx_i32m2(a0, bufs[0][i & 7], x0, vl);
            a1 = __riscv_vmacc_vx_i32m2(a1, bufs[1][i & 7], x1, vl);
            a2 = __riscv_vmacc_vx_i32m2(a2, bufs[2][i & 7], x0, vl);
            a3 = __riscv_vmacc_vx_i32m2(a3, bufs[3][i & 7], x1, vl);
         }
         t1 = rdt();
         cyc = (double)(t1-t0)*1.6e9/tick_hz/REP;
         printf("vmacc 4 indep streams: %.2f cyc/vec-op (%.1f lane-MAC/cyc, acc=%ld)\n",
                cyc, 4.0*vl/cyc,
                (long)(__riscv_vmv_x_s_i32m2_i32(a0)^__riscv_vmv_x_s_i32m2_i32(a1)));
      }
      {
         vint32m2_t a0 = __riscv_vmv_v_x_i32m2(0, vl), a1 = a0, a2 = a0, a3 = a0;
         vint32m2_t a4 = a0, a5 = a0, a6 = a0, a7 = a0;
         t0 = rdt();
         for (i = 0; i < REP; i++)
         {
            a0 = __riscv_vmacc_vx_i32m2(a0, bufs[0][i & 7], x0, vl);
            a1 = __riscv_vmacc_vx_i32m2(a1, bufs[1][i & 7], x1, vl);
            a2 = __riscv_vmacc_vx_i32m2(a2, bufs[2][i & 7], x0, vl);
            a3 = __riscv_vmacc_vx_i32m2(a3, bufs[3][i & 7], x1, vl);
            a4 = __riscv_vmacc_vx_i32m2(a4, bufs[4][i & 7], x0, vl);
            a5 = __riscv_vmacc_vx_i32m2(a5, bufs[5][i & 7], x1, vl);
            a6 = __riscv_vmacc_vx_i32m2(a6, bufs[6][i & 7], x0, vl);
            a7 = __riscv_vmacc_vx_i32m2(a7, bufs[7][i & 7], x1, vl);
         }
         t1 = rdt();
         cyc = (double)(t1-t0)*1.6e9/tick_hz/REP;
         printf("vmacc 8 indep streams: %.2f cyc/vec-op (%.1f lane-MAC/cyc, acc=%ld)\n",
                cyc, 8.0*vl/cyc,
                (long)(__riscv_vmv_x_s_i32m2_i32(a0)^__riscv_vmv_x_s_i32m2_i32(a7)));
      }
   }

   /* ---- widening MAC e16m2 (256 lanes) ---- */
   {
      size_t vl = __riscv_vsetvlmax_e16m2();
      vint32m4_t a = __riscv_vmv_v_x_i32m4(0, vl);
      double cyc; unsigned long long t0, t1;
      t0 = rdt();
      for (i = 0; i < REP; i++)
      {
         vint16m2_t x = __riscv_vle16_v_i16m2((short*)bufs[i & 1], vl);
         a = __riscv_vwmacc_vx_i32m4(a, bufs[2][i & 7], x, vl);
      }
      t1 = rdt();
      cyc = (double)(t1-t0)*1.6e9/tick_hz/REP;
      printf("vwmacc e16m2 (%zu lanes): %.2f cyc/vec-op (%.1f lane-MAC/cyc, acc=%ld)\n",
             vl, cyc, (double)vl/cyc, (long)__riscv_vmv_x_s_i32m4_i32(a));
   }

   /* ---- load + MAC (4 streams, L1-resident) ---- */
   {
      size_t vl = __riscv_vsetvlmax_e32m2();
      vint32m2_t a0 = __riscv_vmv_v_x_i32m2(0, vl), a1 = a0, a2 = a0, a3 = a0;
      double cyc; unsigned long long t0, t1;
      t0 = rdt();
      for (i = 0; i < REP; i++)
      {
         a0 = __riscv_vmacc_vx_i32m2(a0, 3, __riscv_vle32_v_i32m2(bufs[0] + (i & 7)*128, vl), vl);
         a1 = __riscv_vmacc_vx_i32m2(a1, 3, __riscv_vle32_v_i32m2(bufs[1] + (i & 7)*128, vl), vl);
         a2 = __riscv_vmacc_vx_i32m2(a2, 3, __riscv_vle32_v_i32m2(bufs[2] + (i & 7)*128, vl), vl);
         a3 = __riscv_vmacc_vx_i32m2(a3, 3, __riscv_vle32_v_i32m2(bufs[3] + (i & 7)*128, vl), vl);
      }
      t1 = rdt();
      cyc = (double)(t1-t0)*1.6e9/tick_hz/REP;
      printf("vle32+vmacc 4-stream: %.2f cyc/group (%.1f vec-loads/cyc, acc=%ld)\n",
             cyc, 4.0/cyc, (long)__riscv_vmv_x_s_i32m2_i32(a0));
   }

   /* ---- strided vs unit load e16 ---- */
   {
      size_t vl = __riscv_vsetvlmax_e16m2();
      vint32m4_t a = __riscv_vmv_v_x_i32m4(0, vl);
      double cyc; unsigned long long t0, t1;
      int rep = REP*2;
      t0 = rdt();
      for (i = 0; i < rep; i++)
      {
         vint16m2_t v = __riscv_vle16_v_i16m2((short*)bufs[0], vl);
         a = __riscv_vwmacc_vx_i32m4(a, 3, v, vl);
      }
      t1 = rdt();
      cyc = (double)(t1-t0)*1.6e9/tick_hz/rep;
      printf("vle16 unit (%zu lanes): %.2f cyc/vec-op\n", vl, cyc);
      t0 = rdt();
      for (i = 0; i < rep; i++)
      {
         vint16m2_t v = __riscv_vlse16_v_i16m2((short*)bufs[i & 1] + (i & 15), 64, vl);
         a = __riscv_vwmacc_vx_i32m4(a, 3, v, vl);
      }
      t1 = rdt();
      cyc = (double)(t1-t0)*1.6e9/tick_hz/rep;
      printf("vlse16 stride 64B: %.2f cyc/vec-op (acc=%ld)\n",
             cyc, (long)__riscv_vmv_x_s_i32m4_i32(a));
   }

   /* ---- memory bandwidth: 8MB copy ---- */
   {
      size_t n = 2*1024*1024; /* int32 words = 8MB */
      int *src = malloc(n*sizeof(int)), *dst = malloc(n*sizeof(int));
      size_t vl = __riscv_vsetvlmax_e32m4();
      unsigned long long t0, t1; int rep = 6;
      double gbs;
      for (i = 0; i < (int)n; i += 1024) src[i] = i;
      for (i = 0; i < rep; i++)
      {
         size_t off = 0;
         unsigned long long c0 = rdt(), c1;
         while (off < n)
         {
            size_t v = __riscv_vsetvl_e32m4(n - off);
            __riscv_vse32_v_i32m4(dst + off, __riscv_vle32_v_i32m4(src + off, v), v);
            off += v;
         }
         c1 = rdt();
         if (i == 0 || i == rep-1)
            printf("memcpy rep%d: %.1f ms\n", i, (c1-c0)/tick_hz*1e3);
      }
      t0 = 0; t1 = 0;
      gbs = 0;
      (void)t0; (void)t1; (void)gbs;
      free(src); free(dst);
   }

   /* ---- raw_dists shape A/B: current (vlse16 + 1 acc) vs transposed
           (vle16 + 4-way split accumulators), len=20, 320 entries ---- */
   {
      int len = 20, entries = 320;
      static short cb[320*20];          /* entry-major: cb[e*len+j] */
      static short cbT[20*320];         /* transposed: cbT[j*entries+e] */
      int e, j;
      double cyc; unsigned long long t0, t1;
      int rep = 300;
      for (e = 0; e < entries; e++)
         for (j = 0; j < len; j++)
         {
            int v = r16();
            cb[e*len+j] = (short)v;
            cbT[j*entries+e] = (short)v;
         }
      /* A: per-batch of 32 entries via vlse16, single accumulator */
      {
         size_t vl = __riscv_vsetvl_e16m2(entries);
         vint32m4_t acc = __riscv_vmv_v_x_i32m4(0, vl);
         t0 = rdt();
         for (i = 0; i < rep; i++)
         {
            vint32m4_t dist = __riscv_vmv_v_x_i32m4(0, vl);
            for (j = 0; j < len; j++)
            {
               vint16m2_t col = __riscv_vlse16_v_i16m2(cb + j, 2*len, vl);
               dist = __riscv_vwmacc_vx_i32m4(dist, bufs[0][j & 7], col, vl);
            }
            acc = __riscv_vadd_vv_i32m4(acc, dist, vl);
         }
         t1 = rdt();
         cyc = (double)(t1-t0)*1.6e9/tick_hz/rep;
         printf("raw_dists A (vlse16, 1 acc), len=20: %.1f cyc/batch-of-%d (acc=%ld)\n",
                cyc, vl, (long)__riscv_vmv_x_s_i32m4_i32(acc));
      }
      /* B: transposed layout, 4-way split accumulators, unit loads */
      {
         size_t vl = __riscv_vsetvl_e16m2(entries);
         vint32m4_t acc = __riscv_vmv_v_x_i32m4(0, vl);
         t0 = rdt();
         for (i = 0; i < rep; i++)
         {
            vint32m4_t d0 = __riscv_vmv_v_x_i32m4(0, vl), d1 = d0, d2 = d0, d3 = d0;
            for (j = 0; j < len; j += 4)
            {
               if (j   < len) d0 = __riscv_vwmacc_vx_i32m4(d0, bufs[0][(j)   & 7], __riscv_vle16_v_i16m2(cbT + (j)*entries,   vl), vl);
               if (j+1 < len) d1 = __riscv_vwmacc_vx_i32m4(d1, bufs[0][(j+1) & 7], __riscv_vle16_v_i16m2(cbT + (j+1)*entries, vl), vl);
               if (j+2 < len) d2 = __riscv_vwmacc_vx_i32m4(d2, bufs[0][(j+2) & 7], __riscv_vle16_v_i16m2(cbT + (j+2)*entries, vl), vl);
               if (j+3 < len) d3 = __riscv_vwmacc_vx_i32m4(d3, bufs[0][(j+3) & 7], __riscv_vle16_v_i16m2(cbT + (j+3)*entries, vl), vl);
            }
            /* mod-2^32 combine: associative, bit-exact */
            d0 = __riscv_vadd_vv_i32m4(d0, d1, vl);
            d2 = __riscv_vadd_vv_i32m4(d2, d3, vl);
            d0 = __riscv_vadd_vv_i32m4(d0, d2, vl);
            acc = __riscv_vadd_vv_i32m4(acc, d0, vl);
         }
         t1 = rdt();
         cyc = (double)(t1-t0)*1.6e9/tick_hz/rep;
         printf("raw_dists B (vle16, 4 acc), len=20: %.1f cyc/batch-of-%d (acc=%ld)\n",
                cyc, vl, (long)__riscv_vmv_x_s_i32m4_i32(acc));
      }
   }

   /* ---- scalar baseline: 4 independent imul-add chains (memory fed) ---- */
   {
      long a0=0,a1=0,a2=0,a3=0;
      double cyc; unsigned long long t0, t1;
      t0 = rdt();
      for (i = 0; i < REP; i++)
      {
         a0 += bufs[0][i & 1023] * 3; a1 += bufs[1][i & 1023] * 3;
         a2 += bufs[2][i & 1023] * 3; a3 += bufs[3][i & 1023] * 3;
      }
      t1 = rdt();
      cyc = (double)(t1-t0)*1.6e9/tick_hz/REP;
      printf("scalar 4 indep mul-add chains: %.2f cyc/iter (acc=%ld)\n", cyc, a0+a3);
   }
   return 0;
}

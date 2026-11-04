/* vlseg4e16 vs 4x vlse16(stride 8B) in the inner_prod shape:
 * correctness vs scalar + timing. REQUIRES gcc-14+ (segment tuple
 * types; gcc-13 cannot compile this file) — not part of the default
 * CTest set. Round 22 K3 experiment: qemu-relative -23% for vlseg4;
 * on-device measurement pending board reconnection.
 */
#include <riscv_vector.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
typedef int32_t spx_word32_t;

static inline unsigned long long rdt(void){ unsigned long long v; __asm__ volatile("rdtime %0":"=r"(v)); return v; }
static unsigned seed = 0xabcdef;
static int r16(void){ seed = seed*1664525 + 1013904223; return (int)((seed>>16)&0xffff) - 32768; }
static volatile unsigned long long sink;

static spx_word32_t ip_seg(const short *x, const short *y, int len)
{
   size_t vlmax = __riscv_vsetvlmax_e32m2();
   vint32m2_t acc = __riscv_vmv_v_x_i32m2(0, vlmax);
   vint32m1_t zero = __riscv_vmv_v_x_i32m1(0, 1);
   size_t n = (size_t)len >> 2;
   size_t off = 0;
   while (n > 0)
   {
      size_t vl = __riscv_vsetvl_e16m1(n);
      vint16m1x4_t xs = __riscv_vlseg4e16_v_i16m1x4(x + off, vl);
      vint16m1x4_t ys = __riscv_vlseg4e16_v_i16m1x4(y + off, vl);
      vint16m1_t x0 = __riscv_vget_v_i16m1x4_i16m1(xs, 0);
      vint16m1_t x1 = __riscv_vget_v_i16m1x4_i16m1(xs, 1);
      vint16m1_t x2 = __riscv_vget_v_i16m1x4_i16m1(xs, 2);
      vint16m1_t x3 = __riscv_vget_v_i16m1x4_i16m1(xs, 3);
      vint16m1_t y0 = __riscv_vget_v_i16m1x4_i16m1(ys, 0);
      vint16m1_t y1 = __riscv_vget_v_i16m1x4_i16m1(ys, 1);
      vint16m1_t y2 = __riscv_vget_v_i16m1x4_i16m1(ys, 2);
      vint16m1_t y3 = __riscv_vget_v_i16m1x4_i16m1(ys, 3);
      vint32m2_t part = __riscv_vmv_v_x_i32m2(0, vlmax);
      part = __riscv_vwmacc_vv_i32m2(part, x0, y0, vl);
      part = __riscv_vwmacc_vv_i32m2(part, x1, y1, vl);
      part = __riscv_vwmacc_vv_i32m2(part, x2, y2, vl);
      part = __riscv_vwmacc_vv_i32m2(part, x3, y3, vl);
      part = __riscv_vsra_vx_i32m2(part, 6, vl);
      acc = __riscv_vadd_vv_i32m2_tu(acc, acc, part, vl);
      off += 4 * vl;
      n -= vl;
   }
   return (spx_word32_t)__riscv_vmv_x_s_i32m1_i32(__riscv_vredsum_vs_i32m2_i32m1(acc, zero, vlmax));
}

static spx_word32_t ip_strided(const short *x, const short *y, int len)
{
   size_t vlmax = __riscv_vsetvlmax_e32m2();
   vint32m2_t acc = __riscv_vmv_v_x_i32m2(0, vlmax);
   vint32m1_t zero = __riscv_vmv_v_x_i32m1(0, 1);
   size_t n = (size_t)len >> 2;
   size_t off = 0;
   while (n > 0)
   {
      size_t vl = __riscv_vsetvl_e16m1(n);
      vint16m1_t x0 = __riscv_vlse16_v_i16m1(x + off + 0, 8, vl);
      vint16m1_t x1 = __riscv_vlse16_v_i16m1(x + off + 1, 8, vl);
      vint16m1_t x2 = __riscv_vlse16_v_i16m1(x + off + 2, 8, vl);
      vint16m1_t x3 = __riscv_vlse16_v_i16m1(x + off + 3, 8, vl);
      vint16m1_t y0 = __riscv_vlse16_v_i16m1(y + off + 0, 8, vl);
      vint16m1_t y1 = __riscv_vlse16_v_i16m1(y + off + 1, 8, vl);
      vint16m1_t y2 = __riscv_vlse16_v_i16m1(y + off + 2, 8, vl);
      vint16m1_t y3 = __riscv_vlse16_v_i16m1(y + off + 3, 8, vl);
      vint32m2_t part = __riscv_vmv_v_x_i32m2(0, vlmax);
      part = __riscv_vwmacc_vv_i32m2(part, x0, y0, vl);
      part = __riscv_vwmacc_vv_i32m2(part, x1, y1, vl);
      part = __riscv_vwmacc_vv_i32m2(part, x2, y2, vl);
      part = __riscv_vwmacc_vv_i32m2(part, x3, y3, vl);
      part = __riscv_vsra_vx_i32m2(part, 6, vl);
      acc = __riscv_vadd_vv_i32m2_tu(acc, acc, part, vl);
      off += 4 * vl;
      n -= vl;
   }
   return (spx_word32_t)__riscv_vmv_x_s_i32m1_i32(__riscv_vredsum_vs_i32m2_i32m1(acc, zero, vlmax));
}

static spx_word32_t ip_scalar_ref(const short *x, const short *y, int len)
{
   /* matches the blocked-shift reference: per-4 sum, >>6, add */
   spx_word32_t sum = 0;
   int b = len >> 2;
   for (int i = 0; i < b; i++)
   {
      spx_word32_t part = 0;
      for (int k = 0; k < 4; k++)
         part += (spx_word32_t)((long)x[4*i+k] * (long)y[4*i+k]);
      sum += part >> 6;
   }
   return sum;
}

int main(void)
{
   static short X[64*1024], Y[64*1024];
   long fails = 0;
   for (int i = 0; i < 64*1024; i++) { X[i] = (short)r16(); Y[i] = (short)r16(); }
   /* correctness at several lengths */
   for (int len = 40; len <= 640; len <<= 1)
   {
      spx_word32_t r0 = ip_scalar_ref(X, Y, len);
      spx_word32_t r1 = ip_strided(X, Y, len);
      spx_word32_t r2 = ip_seg(X, Y, len);
      if (r1 != r0 || r2 != r0) { printf("len=%d MISMATCH ref=%ld strided=%ld seg=%ld\n", len, (long)r0, (long)r1, (long)r2); fails++; }
   }
   printf("correctness fails=%ld\n", fails);
   /* timing: len=160, many iterations. One input sample is MUTATED per
    * iteration — a fixed 8-offset rotation let gcc precompute all results
    * and reduce the loop to a table lookup (0.2 ns/call, review R5
    * pattern again). */
   for (int v = 0; v < 2; v++)
   {
      struct timespec t0, t1; unsigned long long c0 = rdt(), c1;
      int rep = 20000;
      clock_gettime(CLOCK_MONOTONIC, &t0);
      c0 = rdt();
      for (int i = 0; i < rep; i++)
      {
         X[(unsigned)i * 13 % 40] = (short)r16();   /* mutate: no hoisting */
         sink ^= (unsigned long long)(v ? ip_seg(X, Y, 160)
                                         : ip_strided(X, Y, 160));
      }
      c1 = rdt();
      clock_gettime(CLOCK_MONOTONIC, &t1);
      double us = ((double)(t1.tv_sec-t0.tv_sec)*1e6 + (t1.tv_nsec-t0.tv_nsec)/1e3) / rep;
      double tick_ns = ((c1 - c0) / 24e6) * 1e9 / rep;
      printf("%s len=160: wall %.1f ns/call, rdt %.1f ns/call\n",
             v ? "vlseg4 " : "strided", us, tick_ns);
   }
   printf("sink=%llu\n", sink);
   return fails != 0;
}

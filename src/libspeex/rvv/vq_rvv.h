/* RVV implementation of the codebook search entry-parallel kernels (vq.c).
 *
 * Shape rationale (round 2): the naive "one dot product + one redsum per
 * codebook entry" only matches gcc auto-vectorization. Instead all codebook
 * entries are processed in parallel — one lane per entry, the in[] sample
 * broadcast per j step — so no reduction is needed until the very end and
 * the per-entry cost is one strided load + one vwmacc_vx per sample.
 *
 * Bit-exactness: MAC16_16 in the scalar reference is a plain int32 add of
 * an exact int16*int16 product. Two's-complement addition is associative
 * modulo 2^32, so lane-wise accumulation over j (ascending, like the C
 * loop) is identical. The per-entry fixups (E>>1 correction, sign flip for
 * vq_nbest_sign) and the n-best insertion run entry-by-entry on the exact
 * raw distances, copied verbatim from the C reference (same global entry
 * order, same tie-breaking). Keeping the sign/abs fixup scalar sidesteps
 * toolchain differences in mask/merge intrinsics entirely.
 *
 * Only valid for the fixed-point build; otherwise vq.c falls back to C. */
#ifndef SPEEX_VQ_RVV_H
#define SPEEX_VQ_RVV_H

#include "rvv_arch.h"

#if defined(FIXED_POINT)

#define OVERRIDE_VQ_NBEST 1
#define OVERRIDE_VQ_NBEST_SIGN 1

/* Raw distances (sum of in[j]*codebook[entry*len+j], exact int32 wrap) for
 * entries [off, off+vl) into dtmp. Shared by both kernels below.
 *
 * Round 15: the codebook is the column-major resp2 produced by
 * compute_weighted_codebook (codebook[j*entries + e]) so the per-j vector
 * across entries is unit-stride (X60 vlse16 is ~8x slower than vle16), and
 * the accumulation is split into four interleaved partial sums — the MAC
 * latency is 5 cycles and a single chain runs at 1/5 of the pipe rate.
 * Two's-complement addition is associative modulo 2^32, so both changes
 * keep every raw distance bit-identical to the scalar reference. */
static inline vint32m2_t rvv_vq_raw_dists_vec(const spx_word16_t *in, const spx_word16_t *cbT,
                                              int len, int entries, size_t off, size_t vl)
{
   size_t vlmax = __riscv_vsetvlmax_e32m2();
   const spx_word16_t *col = cbT + off; /* cbT[j*entries + e] */
   vint32m2_t d0 = __riscv_vmv_v_x_i32m2(0, vlmax);
   vint32m2_t d1 = d0, d2 = d0, d3 = d0;
   int j = 0;

   for (; j + 3 < len; j += 4)
   {
      d0 = __riscv_vwmacc_vx_i32m2(d0, in[j],     __riscv_vle16_v_i16m1(col + (size_t)(j)  *entries, vl), vl);
      d1 = __riscv_vwmacc_vx_i32m2(d1, in[j + 1], __riscv_vle16_v_i16m1(col + (size_t)(j+1)*entries, vl), vl);
      d2 = __riscv_vwmacc_vx_i32m2(d2, in[j + 2], __riscv_vle16_v_i16m1(col + (size_t)(j+2)*entries, vl), vl);
      d3 = __riscv_vwmacc_vx_i32m2(d3, in[j + 3], __riscv_vle16_v_i16m1(col + (size_t)(j+3)*entries, vl), vl);
   }
   for (; j < len; j++)
      d0 = __riscv_vwmacc_vx_i32m2(d0, in[j], __riscv_vle16_v_i16m1(col + (size_t)j*entries, vl), vl);

   d0 = __riscv_vadd_vv_i32m2(d0, d1, vl);
   d2 = __riscv_vadd_vv_i32m2(d2, d3, vl);
   return __riscv_vadd_vv_i32m2(d0, d2, vl);
}

static inline void rvv_vq_raw_dists(const spx_word16_t *in, const spx_word16_t *cbT,
                                    int len, int entries, size_t off, size_t vl, spx_word32_t *dtmp)
{
   __riscv_vse32_v_i32m2(dtmp,
         rvv_vq_raw_dists_vec(in, cbT, len, entries, off, vl), vl);
}

#if !defined(DISABLE_ENCODER) && defined(OVERRIDE_VQ_NBEST)
/*Finds the indices of the n-best entries in a codebook*/
void vq_nbest(spx_word16_t *in, const spx_word16_t *codebook, int len, int entries, spx_word32_t *E, int N, int *nbest, spx_word32_t *best_dist, char *stack)
{
   size_t vlmax = __riscv_vsetvlmax_e32m2();
   spx_word32_t dtmp[vlmax]; /* VLA: VAR_ARRAYS build, length-agnostic */
   size_t off = 0;
   int i, k, used = 0;
#if !defined(R14_NO_VQTAIL)
   if (N == 1)
   {
      /* All split_cb callers: the C loop reduces to a first-wins argmin of
         E/2 - raw over the entries (entry 0 is force-accepted, later entries
         need strict <). Per-lane running min across ascending chunks keeps
         the earliest entry inside a lane; the scalar tail resolves ties by
         the smaller index, i.e. the first occurrence in C scan order. Init
         lanes carry (INT32_MAX, UINT32_MAX) so even a real INT32_MAX
         distance at index 0 wins the tie like the forced accept does. */
      vint32m2_t rv = __riscv_vmv_v_x_i32m2(0x7fffffff, vlmax);
      vuint32m2_t ri = __riscv_vmv_v_x_u32m2(0xffffffffu, vlmax);
      spx_word32_t lv[vlmax], li[vlmax];
      int bv;
      unsigned bi;
      int l;
      /* Seed with entry 0: the C reference force-accepts it, so a real
       * distance equal to the INT32_MAX sentinel must not be lost (review
       * R6) — the strict-less merge can never capture an equal value. The
       * scalar seed replicates MAC16_16 over the column-major codebook;
       * the vector path covers entries [1, entries). */
      {
         spx_word32_t raw = 0;
         const spx_word16_t *cb = codebook; /* column-major: cb[j*entries] */
         for (k = 0; k < len; k++)
            raw = MAC16_16(raw, in[k], cb[(size_t)k*entries]);
         bv = (int)SUB32(SHR32(E[0],1), raw);
         bi = 0;
         off = 1;
      }
      while (off < (size_t)entries)
      {
         size_t vl = __riscv_vsetvl_e16m1((size_t)entries - off);
         vint32m2_t e = __riscv_vle32_v_i32m2(E + off, vl);
         vint32m2_t dist;
         vbool16_t m;
         vuint32m2_t ci;
         e = __riscv_vsra_vx_i32m2(e, 1, vl);           /* SHR32(E,1) */
         dist = rvv_vq_raw_dists_vec(in, codebook, len, entries, off, vl);
         dist = __riscv_vrsub_vx_i32m2(dist, 0, vl);
         dist = __riscv_vadd_vv_i32m2(dist, e, vl);     /* SUB32(E/2, raw) = E/2 + (-raw) */
         m = __riscv_vmslt_vv_i32m2_b16(dist, rv, vl);
         /* _tu keeps the sentinel tail of the running candidate vectors;
          * they are stored and reduced at VLMAX below (review R3). */
         rv = __riscv_vmerge_vvm_i32m2_tu(rv, rv, dist, m, vl);
         ci = __riscv_vadd_vx_u32m2(__riscv_vid_v_u32m2(vl), (unsigned)off, vl);
         ri = __riscv_vmerge_vvm_u32m2_tu(ri, ri, ci, m, vl);
         off += vl;
      }
      __riscv_vse32_v_i32m2(lv, rv, vlmax);
      __riscv_vse32_v_u32m2(li, ri, vlmax);
      for (l = 0; l < (int)vlmax; l++)
         if (lv[l] < bv || (lv[l] == bv && li[l] < bi))
         {
            bv = (int)lv[l]; bi = (unsigned)li[l];
         }
      best_dist[0] = (spx_word32_t)bv;
      nbest[0] = (int)bi;
      return;
   }
#endif /* R14_NO_VQTAIL */
   while (off < (size_t)entries)
   {
      size_t vl = __riscv_vsetvl_e16m1((size_t)entries - off);
      size_t e;
      rvv_vq_raw_dists(in, codebook, len, entries, off, vl, dtmp);
      for (e = 0; e < vl; e++, off++)
      {
         spx_word32_t dist = dtmp[e];
         dist = SUB32(SHR32(E[off],1),dist);
         if ((int)off < N || dist < best_dist[N-1])
         {
            for (k=N-1; (k >= 1) && (k > used || dist < best_dist[k-1]); k--)
            {
               best_dist[k]=best_dist[k-1];
               nbest[k] = nbest[k-1];
            }
            best_dist[k]=dist;
            nbest[k]=(int)off;
            used++;
         }
      }
   }
}
#endif /* !defined(DISABLE_ENCODER) && defined(OVERRIDE_VQ_NBEST) */

#if !defined(DISABLE_WIDEBAND) && !defined(DISABLE_ENCODER) && defined(OVERRIDE_VQ_NBEST_SIGN)
/*Finds the indices of the n-best entries in a codebook with sign*/
void vq_nbest_sign(spx_word16_t *in, const spx_word16_t *codebook, int len, int entries, spx_word32_t *E, int N, int *nbest, spx_word32_t *best_dist, char *stack)
{
   size_t vlmax = __riscv_vsetvlmax_e32m2();
   spx_word32_t dtmp[vlmax];
   size_t off = 0;
   int i, k, sign, used = 0;
#if !defined(R14_NO_VQTAIL)
   if (N == 1)
   {
      /* C reduction for N=1: dist' = (raw>0 ? -raw : raw) + E/2, then the
         same first-wins argmin as vq_nbest. The winning entry's sign is
         recomputed scalar-side from a verbatim MAC16_16 dot (INT_MIN edge
         included: the wrap negate matches NEG-free C exactly). */
      vint32m2_t rv = __riscv_vmv_v_x_i32m2(0x7fffffff, vlmax);
      vuint32m2_t ri = __riscv_vmv_v_x_u32m2(0xffffffffu, vlmax);
      spx_word32_t lv[vlmax], li[vlmax];
      int bv, j;
      unsigned bi;
      int l;
      /* entry-0 seed: forced accept in the reference; a real distance equal
       * to the INT32_MAX sentinel would otherwise be lost by the strict-less
       * merge (review R6). Vector path covers entries [1, entries). */
      {
         spx_word32_t raw0 = 0;
         const spx_word16_t *cb0 = codebook; /* column-major: cb[j*entries] */
         for (j = 0; j < len; j++)
            raw0 = MAC16_16(raw0, in[j], cb0[(size_t)j*entries]);
         if (raw0 > 0)
            raw0 = -raw0;
         bv = (int)ADD32(raw0, SHR32(E[0],1));
         bi = 0;
         off = 1;
      }
      while (off < (size_t)entries)
      {
         size_t vl = __riscv_vsetvl_e16m1((size_t)entries - off);
         vint32m2_t e = __riscv_vle32_v_i32m2(E + off, vl);
         vint32m2_t raw, flip, dist;
         vbool16_t m;
         vuint32m2_t ci;
         raw = rvv_vq_raw_dists_vec(in, codebook, len, entries, off, vl);
         flip = __riscv_vmerge_vvm_i32m2(raw, __riscv_vrsub_vx_i32m2(raw, 0, vl),
               __riscv_vmsgt_vx_i32m2_b16(raw, 0, vl), vl);
         e = __riscv_vsra_vx_i32m2(e, 1, vl);
         dist = __riscv_vadd_vv_i32m2(flip, e, vl);     /* ADD32(+E/2) */
         m = __riscv_vmslt_vv_i32m2_b16(dist, rv, vl);
         rv = __riscv_vmerge_vvm_i32m2_tu(rv, rv, dist, m, vl);
         ci = __riscv_vadd_vx_u32m2(__riscv_vid_v_u32m2(vl), (unsigned)off, vl);
         ri = __riscv_vmerge_vvm_u32m2_tu(ri, ri, ci, m, vl);
         off += vl;
      }
      __riscv_vse32_v_i32m2(lv, rv, vlmax);
      __riscv_vse32_v_u32m2(li, ri, vlmax);
      for (l = 0; l < (int)vlmax; l++)
         if (lv[l] < bv || (lv[l] == bv && li[l] < bi))
         {
            bv = (int)lv[l]; bi = (unsigned)li[l];
         }
      {
         spx_word32_t raw = 0;
         const spx_word16_t *cb = codebook + bi; /* column-major: cb[j*entries] */
         for (j = 0; j < len; j++)
            raw = MAC16_16(raw, in[j], cb[(size_t)j*entries]);
         sign = (raw > 0) ? 0 : 1;
      }
      best_dist[0] = (spx_word32_t)bv;
      nbest[0] = (int)bi;
      if (sign)
         nbest[0] += entries;
      return;
   }
#endif /* R14_NO_VQTAIL */
   while (off < (size_t)entries)
   {
      size_t vl = __riscv_vsetvl_e16m1((size_t)entries - off);
      size_t e;
      rvv_vq_raw_dists(in, codebook, len, entries, off, vl, dtmp);
      for (e = 0; e < vl; e++, off++)
      {
         spx_word32_t dist = dtmp[e];
         if (dist>0)
         {
            sign=0;
            dist=-dist;
         } else
         {
            sign=1;
         }
         dist = ADD32(dist,SHR32(E[off],1));
         if ((int)off < N || dist < best_dist[N-1])
         {
            for (k=N-1; (k >= 1) && (k > used || dist < best_dist[k-1]); k--)
            {
               best_dist[k]=best_dist[k-1];
               nbest[k] = nbest[k-1];
            }
            best_dist[k]=dist;
            nbest[k]=(int)off;
            used++;
            if (sign)
               nbest[k]+=entries;
         }
      }
   }
}
#endif /* !defined(DISABLE_WIDEBAND) && !defined(DISABLE_ENCODER) && defined(OVERRIDE_VQ_NBEST_SIGN) */

#endif /* FIXED_POINT */

#endif /* SPEEX_VQ_RVV_H */

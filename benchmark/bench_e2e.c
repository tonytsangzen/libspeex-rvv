/* Weak: only defined when the library is built with SPEEX_PROF=ON. */
extern int speex_prof_enabled __attribute__((weak));

/* bench_e2e — end-to-end encode/decode throughput of the library under
 * test, public API only. This is the project's top-level speed metric;
 * kernel micro-benchmarks are added in this directory as RVV kernels
 * land (see benchmark/README.md).
 *
 *   bench_e2e <nb|wb|uwb> <quality> <seconds> [reps]
 *
 * Prints microseconds per frame and the realtime factor (audio seconds
 * produced per wall-clock second; >1 means faster than realtime).
 * Best of `reps` runs is reported to reduce scheduler noise.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <speex/speex.h>
#include <speex/speex_bits.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define CHUNK_STRIDE 1024

static uint32_t rng_state = 0x12345678u;
static int rng16(void)
{
   rng_state = rng_state * 1103515245u + 12345u;
   return (int)((rng_state >> 16) & 0x7fffu);
}

static double now_sec(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}

int main(int argc, char **argv)
{
   const SpeexMode *mode;
   int quality, seconds, reps = 3;
   int rate, frame_size = 0, nb_frames;
   spx_int16_t *pcm;
   unsigned char *stream;
   int *chunk_lens;
   void *enc, *dec;
   SpeexBits bits;
   double best_enc = 1e30, best_dec = 1e30;
   int rep, i;

   if (argc < 4 || argc > 5)
   {
      fprintf(stderr, "usage: %s <nb|wb|uwb> <quality> <seconds> [reps]\n", argv[0]);
      return 1;
   }
   if (strcmp(argv[1], "nb") == 0)        { mode = speex_lib_get_mode(SPEEX_MODEID_NB);  rate = 8000; }
   else if (strcmp(argv[1], "wb") == 0)   { mode = speex_lib_get_mode(SPEEX_MODEID_WB);  rate = 16000; }
   else if (strcmp(argv[1], "uwb") == 0)  { mode = speex_lib_get_mode(SPEEX_MODEID_UWB); rate = 32000; }
   else { fprintf(stderr, "unknown mode %s\n", argv[1]); return 1; }
   quality = atoi(argv[2]);
   seconds = atoi(argv[3]);
   if (argc == 5) reps = atoi(argv[4]);
   if (reps < 1) reps = 1;

   speex_mode_query(mode, SPEEX_MODE_FRAME_SIZE, &frame_size);
   nb_frames = rate * seconds / frame_size;

   /* Same deterministic signal as test/speex_ab.c gen. */
   pcm = malloc((size_t)nb_frames * frame_size * sizeof *pcm);
   for (i = 0; i < nb_frames * frame_size; i++)
   {
      double t = (double)i / rate;
      double freq = 200.0 + 800.0 * t / (seconds > 0 ? seconds : 1);
      double noise = (rng16() / 32768.0) - 1.0;
      pcm[i] = (spx_int16_t)lrint((0.6 * sin(2.0 * M_PI * freq * t) + 0.15 * noise) * 12000.0);
   }
   stream = malloc((size_t)nb_frames * CHUNK_STRIDE);
   chunk_lens = calloc(nb_frames, sizeof *chunk_lens);

   enc = speex_encoder_init(mode);
   dec = speex_decoder_init(mode);
   {
      spx_int32_t tmp = quality;
      speex_encoder_ctl(enc, SPEEX_SET_QUALITY, &tmp);
      tmp = 0;
      speex_encoder_ctl(enc, SPEEX_SET_VBR, &tmp);
      tmp = 1;
      speex_decoder_ctl(dec, SPEEX_SET_ENH, &tmp);
   }
   speex_bits_init(&bits);

   {
      /* BENCH_PHASE=enc|dec limits the timed phase (profiling aid); the
       * decode-only mode still runs one untimed encode to fill `stream`. */
      const char *phase = getenv("BENCH_PHASE");
      int do_enc = !phase || phase[0] != 'd';
      int do_dec = !phase || phase[0] != 'e';

      if (!do_enc)
      {
         /* keep the one untimed encode out of the profiler attribution */
         if (&speex_prof_enabled) speex_prof_enabled = 0;
         for (i = 0; i < nb_frames; i++)
         {
            unsigned char *chunk = stream + (size_t)i * CHUNK_STRIDE;
            speex_bits_reset(&bits);
            speex_encode_int(enc, pcm + (size_t)i * frame_size, &bits);
            chunk_lens[i] = speex_bits_write(&bits, (char *)chunk, CHUNK_STRIDE);
         }
         if (&speex_prof_enabled) speex_prof_enabled = 1;
      }

      for (rep = 0; rep < reps; rep++)
      {
         double t0, t1;
         if (do_enc)
         {
            t0 = now_sec();
            for (i = 0; i < nb_frames; i++)
            {
               unsigned char *chunk = stream + (size_t)i * CHUNK_STRIDE;
               speex_bits_reset(&bits);
               speex_encode_int(enc, pcm + (size_t)i * frame_size, &bits);
               chunk_lens[i] = speex_bits_write(&bits, (char *)chunk, CHUNK_STRIDE);
            }
            t1 = now_sec();
            if (t1 - t0 < best_enc) best_enc = t1 - t0;
         }

         if (do_dec)
         {
            t0 = now_sec();
            for (i = 0; i < nb_frames; i++)
            {
               unsigned char *chunk = stream + (size_t)i * CHUNK_STRIDE;
               spx_int16_t out[1024];
               speex_bits_read_from(&bits, (char *)chunk, chunk_lens[i]);
               speex_decode_int(dec, &bits, out);
            }
            t1 = now_sec();
            if (t1 - t0 < best_dec) best_dec = t1 - t0;
         }
      }

      printf("mode=%s q=%d frames=%d frame_size=%d rate=%d reps=%d\n",
             argv[1], quality, nb_frames, frame_size, rate, reps);
      if (do_enc)
         printf("encode: %8.2f us/frame, realtime x%7.1f\n",
                best_enc / nb_frames * 1e6, (double)nb_frames * frame_size / rate / best_enc);
      if (do_dec)
         printf("decode: %8.2f us/frame, realtime x%7.1f\n",
                best_dec / nb_frames * 1e6, (double)nb_frames * frame_size / rate / best_dec);
   }

   speex_bits_destroy(&bits);
   speex_encoder_destroy(enc);
   speex_decoder_destroy(dec);
   free(pcm);
   free(stream);
   free(chunk_lens);
   return 0;
}

/* speex_ab — deterministic encode/decode driver for A/B bit-exactness
 * testing between two libspeex builds (the port under src/ vs baseline/).
 *
 *   speex_ab gen    <rate> <seconds> <out.raw>
 *   speex_ab encode <nb|wb|uwb> <quality> <in.raw> <out.spx>
 *   speex_ab decode <nb|wb|uwb> <in.spx> <out.raw>
 *
 * Container format (.spx): a sequence of frames, each prefixed with a
 * little-endian uint32 holding the byte length of the speex bitstream
 * chunk that follows. test/run_ab.sh compares these files byte-for-byte
 * across builds, so any kernel change that alters the coded output is
 * caught immediately.
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

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const SpeexMode *mode_from_name(const char *name)
{
   if (strcmp(name, "nb") == 0)  return speex_lib_get_mode(SPEEX_MODEID_NB);
   if (strcmp(name, "wb") == 0)  return speex_lib_get_mode(SPEEX_MODEID_WB);
   if (strcmp(name, "uwb") == 0) return speex_lib_get_mode(SPEEX_MODEID_UWB);
   fprintf(stderr, "unknown mode '%s' (expected nb|wb|uwb)\n", name);
   return NULL;
}

/* Deterministic PRNG so every build/run encodes identical input. */
static uint32_t rng_state = 0x12345678u;
static int rng16(void)
{
   rng_state = rng_state * 1103515245u + 12345u;
   return (int)((rng_state >> 16) & 0x7fffu);
}

static int do_gen(int rate, int seconds, const char *path)
{
   FILE *f = fopen(path, "wb");
   long total = (long)rate * seconds;
   double phase = 0.0;
   long i;

   if (!f) { perror(path); return 1; }
   for (i = 0; i < total; i++)
   {
      /* Rising chirp (200..1000 Hz) plus noise: inside the narrowband
       * band so all three modes get comparable content. */
      double t = (double)i / rate;
      double freq = 200.0 + 800.0 * t / (seconds > 0 ? seconds : 1);
      double noise = (rng16() / 32768.0) - 1.0;
      double s = 0.6 * sin(phase) + 0.15 * noise;
      int16_t v = (int16_t)lrint(s * 12000.0);
      phase += 2.0 * M_PI * freq / rate;
      fwrite(&v, sizeof v, 1, f);
   }
   fclose(f);
   return 0;
}

static void put_u32le(FILE *f, uint32_t v)
{
   unsigned char b[4] = { (unsigned char)v, (unsigned char)(v >> 8),
                          (unsigned char)(v >> 16), (unsigned char)(v >> 24) };
   fwrite(b, 1, 4, f);
}

static int read_u32le(FILE *f, uint32_t *v)
{
   unsigned char b[4];
   if (fread(b, 1, 4, f) != 4) return 0;
   *v = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
   return 1;
}

static int do_encode(const SpeexMode *mode, int quality,
                     const char *inpath, const char *outpath)
{
   FILE *fin = fopen(inpath, "rb");
   FILE *fout = fopen(outpath, "wb");
   void *st;
   SpeexBits bits;
   spx_int32_t tmp = quality;
   int frame_size = 0;
   unsigned char chunk[8192];
   spx_int16_t *frame;

   if (!fin) { perror(inpath); return 1; }
   if (!fout) { perror(outpath); return 1; }
   st = speex_encoder_init(mode);
   if (!st) { fprintf(stderr, "speex_encoder_init failed\n"); return 1; }
   speex_bits_init(&bits);
   speex_encoder_ctl(st, SPEEX_SET_QUALITY, &tmp);
   tmp = 0;
   speex_encoder_ctl(st, SPEEX_SET_VBR, &tmp);
   speex_mode_query(mode, SPEEX_MODE_FRAME_SIZE, &frame_size);

   frame = malloc(frame_size * sizeof *frame);
   for (;;)
   {
      size_t got = fread(frame, sizeof *frame, frame_size, fin);
      int nb;
      if (got == 0 || (int)got < frame_size)
         break; /* partial trailing frame dropped, like upstream testenc */
      speex_bits_reset(&bits);
      speex_encode_int(st, frame, &bits);
      nb = speex_bits_write(&bits, (char *)chunk, sizeof chunk);
      put_u32le(fout, (uint32_t)nb);
      fwrite(chunk, 1, nb, fout);
   }
   free(frame);
   speex_bits_destroy(&bits);
   speex_encoder_destroy(st);
   fclose(fin);
   fclose(fout);
   return 0;
}

static int do_decode(const SpeexMode *mode,
                     const char *inpath, const char *outpath)
{
   FILE *fin = fopen(inpath, "rb");
   FILE *fout = fopen(outpath, "wb");
   void *dec;
   SpeexBits bits;
   spx_int32_t tmp = 1;
   int frame_size = 0;
   unsigned char chunk[8192];
   spx_int16_t *frame;

   if (!fin) { perror(inpath); return 1; }
   if (!fout) { perror(outpath); return 1; }
   dec = speex_decoder_init(mode);
   if (!dec) { fprintf(stderr, "speex_decoder_init failed\n"); return 1; }
   speex_bits_init(&bits);
   speex_decoder_ctl(dec, SPEEX_SET_ENH, &tmp);
   speex_mode_query(mode, SPEEX_MODE_FRAME_SIZE, &frame_size);

   frame = malloc(frame_size * sizeof *frame);
   for (;;)
   {
      uint32_t len;
      if (!read_u32le(fin, &len))
         break;
      if (len > sizeof chunk)
      {
         fprintf(stderr, "corrupt container: frame length %u\n", (unsigned)len);
         return 1;
      }
      if (fread(chunk, 1, len, fin) != len)
         break;
      speex_bits_read_from(&bits, (char *)chunk, (int)len);
      speex_decode_int(dec, &bits, frame);
      fwrite(frame, sizeof *frame, frame_size, fout);
   }
   free(frame);
   speex_bits_destroy(&bits);
   speex_decoder_destroy(dec);
   fclose(fin);
   fclose(fout);
   return 0;
}

int main(int argc, char **argv)
{
   const SpeexMode *mode;

   if (argc < 2)
   {
      fprintf(stderr,
              "usage: %s gen <rate> <seconds> <out.raw>\n"
              "       %s encode <nb|wb|uwb> <quality> <in.raw> <out.spx>\n"
              "       %s decode <nb|wb|uwb> <in.spx> <out.raw>\n",
              argv[0], argv[0], argv[0]);
      return 1;
   }

   if (strcmp(argv[1], "gen") == 0 && argc == 5)
      return do_gen(atoi(argv[2]), atoi(argv[3]), argv[4]);

   if (strcmp(argv[1], "encode") == 0 && argc == 6)
   {
      mode = mode_from_name(argv[2]);
      if (!mode) return 1;
      return do_encode(mode, atoi(argv[3]), argv[4], argv[5]);
   }

   if (strcmp(argv[1], "decode") == 0 && argc == 5)
   {
      mode = mode_from_name(argv[2]);
      if (!mode) return 1;
      return do_decode(mode, argv[3], argv[4]);
   }

   fprintf(stderr, "bad arguments\n");
   return 1;
}

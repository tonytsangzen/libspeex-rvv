/* Batch transcoding harness (plan P2): encode/decode K independent
 * streams with per-stream state across pthread workers.
 *
 * The library API is untouched - each worker owns its own encoder/
 * decoder/bits instances, so no shared mutable state exists and every
 * stream's output must be byte-identical to single-threaded encoding.
 * The tool verifies that with memcmp and reports serial-vs-parallel
 * wall time for 1/2/4/8 threads.
 *
 * usage: batch_parallel <nb|wb|uwb> <quality> <streams> <seconds> [threads...]
 *   threads defaults to "1 2 4 8"; each value runs the parallel encode
 *   + decode pass. Stream count should be >= the largest thread count.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <pthread.h>
#include <speex/speex.h>

#define MAX_CHUNK 2048

typedef struct {
   int frames;
   int frame_size;
   spx_int16_t *pcm_in;    /* frames * frame_size, pristine */
   spx_int16_t *pcm_work;  /* working copy: nb_encode applies its input
                            * highpass IN PLACE, so every encode pass must
                            * start from the pristine signal */
   spx_int16_t *pcm_out;   /* frames * frame_size (decoded) */
   unsigned char *bits;    /* frames * MAX_CHUNK */
   int *chunk_len;         /* frames */
   const SpeexMode *mode;
   int quality;
} stream_t;

typedef struct {
   stream_t *streams;
   int nstreams;
   int worker_id;
   int nworkers;
   int do_decode;          /* 0 = encode, 1 = decode */
} job_t;

static double now_sec(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static unsigned rng_state = 0x1234abcd;
static int rng16(void)
{
   rng_state = rng_state * 1664525 + 1013904223;
   return (int)((rng_state >> 16) & 0xffff);
}

static void gen_stream(stream_t *s, int seed)
{
   int i;
   rng_state = 0x1234abcd ^ (unsigned)seed * 2654435761u;
   for (i = 0; i < s->frames * s->frame_size; i++)
   {
      double t = (double)i / 8000.0;
      double noise = (rng16() / 32768.0) - 1.0;
      s->pcm_in[i] = (spx_int16_t)lrint((0.6 * sin(2.0 * 3.14159265358979 * (200.0 + 40.0 * seed) * t)
                                          + 0.15 * noise) * 12000.0);
   }
}

/* One worker processes its striped subset of streams sequentially; every
 * stream gets fresh state, so workers never share anything. */
static void *worker(void *arg)
{
   job_t *j = (job_t *)arg;
   int k;
   for (k = j->worker_id; k < j->nstreams; k += j->nworkers)
   {
      stream_t *s = &j->streams[k];
      void *st;
      SpeexBits bits;
      spx_int32_t q = s->quality;
      int i;
      if (j->do_decode)
         goto decode;   /* encode passes own the bits; decode reuses them */
      st = speex_encoder_init(s->mode);
      speex_bits_init(&bits);
      speex_encoder_ctl(st, SPEEX_SET_QUALITY, &q);
      {
         spx_int32_t vbr = 0;
         speex_encoder_ctl(st, SPEEX_SET_VBR, &vbr);
      }
      for (i = 0; i < s->frames; i++)
      {
         unsigned char *chunk = s->bits + (size_t)i * MAX_CHUNK;
         speex_bits_reset(&bits);
         speex_encode_int(st, s->pcm_work + (size_t)i * s->frame_size, &bits);
         s->chunk_len[i] = speex_bits_write(&bits, (char *)chunk, MAX_CHUNK);
      }
      speex_bits_destroy(&bits);
      speex_encoder_destroy(st);
      continue;
decode:
      st = speex_decoder_init(s->mode);
      speex_bits_init(&bits);
      {
         spx_int32_t enh = 1;
         speex_decoder_ctl(st, SPEEX_SET_ENH, &enh);
      }
      for (i = 0; i < s->frames; i++)
      {
         unsigned char *chunk = s->bits + (size_t)i * MAX_CHUNK;
         speex_bits_read_from(&bits, (char *)chunk, s->chunk_len[i]);
         speex_decode_int(st, &bits, s->pcm_out + (size_t)i * s->frame_size);
      }
      speex_bits_destroy(&bits);
      speex_decoder_destroy(st);
   }
   return NULL;
}

/* Run all streams through nworkers threads; returns wall seconds. */
static double run_pass(stream_t *streams, int nstreams, int nworkers, int do_decode)
{
   pthread_t tid[64];
   job_t jobs[64];
   double t0, t1;
   int w, k;
   /* restore pristine inputs: the encoder highpasses the frame buffer
    * in place, so re-encoding without a reset is NOT idempotent */
   if (!do_decode)
      for (k = 0; k < nstreams; k++)
         memcpy(streams[k].pcm_work, streams[k].pcm_in,
                (size_t)streams[k].frames * streams[k].frame_size * sizeof(spx_int16_t));
   t0 = now_sec();
   for (w = 0; w < nworkers; w++)
   {
      jobs[w].streams = streams;
      jobs[w].nstreams = nstreams;
      jobs[w].worker_id = w;
      jobs[w].nworkers = nworkers;
      jobs[w].do_decode = do_decode;
      pthread_create(&tid[w], NULL, worker, &jobs[w]);
   }
   for (w = 0; w < nworkers; w++)
      pthread_join(tid[w], NULL);
   t1 = now_sec();
   return t1 - t0;
}

/* The encode pass writes into the stream's bit buffers; snapshot them
 * so later passes can be compared byte-for-byte against the first. */
static void snapshot_bits(stream_t *streams, int nstreams,
                          unsigned char **ref, int **reflen)
{
   int k;
   for (k = 0; k < nstreams; k++)
   {
      size_t n = (size_t)streams[k].frames * MAX_CHUNK;
      ref[k] = malloc(n);
      memcpy(ref[k], streams[k].bits, n);
      reflen[k] = malloc(streams[k].frames * sizeof(int));
      memcpy(reflen[k], streams[k].chunk_len, streams[k].frames * sizeof(int));
   }
}

static int verify_bits(stream_t *streams, int nstreams, unsigned char **ref, int **reflen)
{
   int k, i;
   for (k = 0; k < nstreams; k++)
   {
      if (memcmp(reflen[k], streams[k].chunk_len, streams[k].frames * sizeof(int)) != 0)
      {
         for (i = 0; i < streams[k].frames; i++)
            if (reflen[k][i] != streams[k].chunk_len[i])
            {
               fprintf(stderr, "stream %d: chunk_len[%d] ref=%d now=%d\n",
                       k, i, reflen[k][i], streams[k].chunk_len[i]);
               break;
            }
         return k + 1;
      }
      for (i = 0; i < streams[k].frames; i++)
      {
         if (streams[k].chunk_len[i] > 0 &&
             memcmp(ref[k] + (size_t)i * MAX_CHUNK,
                    streams[k].bits + (size_t)i * MAX_CHUNK,
                    streams[k].chunk_len[i]) != 0)
         {
            fprintf(stderr, "stream %d: chunk %d content differs (len %d)\n",
                    k, i, streams[k].chunk_len[i]);
            return k + 1;
         }
      }
   }
   return 0;
}

static int verify_pcm(stream_t *streams, int nstreams, spx_int16_t **ref)
{
   int k;
   for (k = 0; k < nstreams; k++)
   {
      if (memcmp(ref[k], streams[k].pcm_out,
                 (size_t)streams[k].frames * streams[k].frame_size * sizeof(spx_int16_t)) != 0)
         return k + 1;
   }
   return 0;
}

int main(int argc, char **argv)
{
   const SpeexMode *mode;
   int rate = 8000, frame_size = 160;
   int nstreams = 8, seconds = 2, quality = 8;
   int threads[16], nthreads = 0;
   int argi;
   stream_t *streams;
   spx_int16_t **pcm_ref = NULL;
   unsigned char **bits_ref = NULL;
   int **len_ref = NULL;
   double t_serial, t_dec_serial;
   int k, i, fail = 0;

   if (argc < 5)
   {
      fprintf(stderr, "usage: %s <nb|wb|uwb> <quality> <streams> <seconds> [threads...]\n", argv[0]);
      return 1;
   }
   if (strcmp(argv[1], "nb") == 0)       { mode = speex_lib_get_mode(SPEEX_MODEID_NB);  rate = 8000; }
   else if (strcmp(argv[1], "wb") == 0)  { mode = speex_lib_get_mode(SPEEX_MODEID_WB);  rate = 16000; }
   else if (strcmp(argv[1], "uwb") == 0) { mode = speex_lib_get_mode(SPEEX_MODEID_UWB); rate = 32000; }
   else { fprintf(stderr, "unknown mode %s\n", argv[1]); return 1; }
   quality = atoi(argv[2]);
   nstreams = atoi(argv[3]);
   seconds = atoi(argv[4]);
   if (argc > 5)
      for (argi = 5; argi < argc && nthreads < 16; argi++)
         threads[nthreads++] = atoi(argv[argi]);
   else
   {
      int d[] = {1, 2, 4, 8};
      for (i = 0; i < 4; i++) threads[nthreads++] = d[i];
   }

   speex_mode_query(mode, SPEEX_MODE_FRAME_SIZE, &frame_size);
   {
      spx_int32_t fs = frame_size;
      (void)fs;
   }

   streams = calloc(nstreams, sizeof *streams);
   for (k = 0; k < nstreams; k++)
   {
      streams[k].frames = rate * seconds / frame_size;
      streams[k].frame_size = frame_size;
      streams[k].mode = mode;
      streams[k].quality = quality;
      streams[k].pcm_in = malloc((size_t)streams[k].frames * frame_size * sizeof(spx_int16_t));
      streams[k].pcm_work = malloc((size_t)streams[k].frames * frame_size * sizeof(spx_int16_t));
      streams[k].pcm_out = malloc((size_t)streams[k].frames * frame_size * sizeof(spx_int16_t));
      streams[k].bits = malloc((size_t)streams[k].frames * MAX_CHUNK);
      streams[k].chunk_len = malloc(streams[k].frames * sizeof(int));
      gen_stream(&streams[k], k + 1);
   }

   /* reference pass: single thread, serial over streams (encode+decode) */
   t_serial = run_pass(streams, nstreams, 1, 0);
   t_dec_serial = run_pass(streams, nstreams, 1, 1);
   bits_ref = malloc(nstreams * sizeof *bits_ref);
   len_ref = malloc(nstreams * sizeof *len_ref);
   pcm_ref = malloc(nstreams * sizeof *pcm_ref);
   snapshot_bits(streams, nstreams, bits_ref, len_ref);
   for (k = 0; k < nstreams; k++)
   {
      pcm_ref[k] = malloc((size_t)streams[k].frames * frame_size * sizeof(spx_int16_t));
      memcpy(pcm_ref[k], streams[k].pcm_out,
             (size_t)streams[k].frames * frame_size * sizeof(spx_int16_t));
   }

   printf("mode=%s q=%d streams=%d frames/stream=%d rate=%d\n",
          argv[1], quality, nstreams, streams[0].frames, rate);
   printf("serial: encode %.3fs  decode %.3fs\n", t_serial, t_dec_serial);

   for (i = 0; i < nthreads; i++)
   {
      int T = threads[i];
      double te, td, sp_e, sp_d;
      int bad;
      te = run_pass(streams, nstreams, T, 0);
      bad = verify_bits(streams, nstreams, bits_ref, len_ref);
      td = run_pass(streams, nstreams, T, 1);
      bad = bad ? bad : verify_pcm(streams, nstreams, pcm_ref);
      sp_e = t_serial / te;
      sp_d = t_dec_serial / td;
      printf("threads=%d: encode %.3fs (x%.2f)  decode %.3fs (x%.2f)  %s\n",
             T, te, sp_e, td, sp_d, bad ? "MISMATCH!" : "bit-exact");
      if (bad) { printf("  first diverging stream: %d\n", bad - 1); fail = 1; }
   }

   printf("== batch parallel: %s ==\n", fail ? "FAILED" : "ALL PASS");
   return fail;
}

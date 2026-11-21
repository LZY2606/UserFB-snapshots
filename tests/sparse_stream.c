/*
 * sparse_stream.c - regression tests for the sparse stream interface.
 *
 * Groups:
 *   dense   - short timelines, expanded against an independent dense
 *             reference that uses integer quotient/remainder and a
 *             finite-support inner product over the high-quality table.
 *   sparse  - huge timelines with real gaps; checks that silence costs
 *             one event and that local PCM matches the reference.
 *   stream  - splitting spans, pull capacity, empty pulls, prefix
 *             stability, repeated finish and post-finish rejection.
 *
 * Building and running this file requires the libresample sources; the
 * task_support build links them in.
 */

#include "../task_support/check.h"
#include "sparse_resample.h"

#include "resample_defs.h"
#include "filterkit.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REF_NMULT   35
#define REF_NWING   (Npc * (REF_NMULT - 1) / 2)
#define REF_ROLLOFF 0.90
#define REF_BETA    6.0
#define MAX_ERR     2.0e-5

/* ------------------------------------------------------------------ */
/* Small exact helpers (kept separate from the implementation). */

static uint64_t umuldiv_floor(uint64_t v, uint64_t mul, uint64_t div)
{
   uint64_t a = v / div;
   uint64_t b = v % div;
   return a * mul + (b * mul) / div;
}

static uint64_t umuldiv_ceil(uint64_t v, uint64_t mul, uint64_t div)
{
   uint64_t a = v / div;
   uint64_t b = v % div;
   return a * mul + (b * mul + div - 1) / div;
}

static void ref_phase(uint64_t k, uint32_t in, uint32_t out,
                      uint64_t *q, uint64_t *r)
{
   uint64_t a = k / out;
   uint64_t b = k % out;
   uint64_t nb = b * (uint64_t)in;
   *q = a * (uint64_t)in + nb / out;
   *r = nb % out;
}

/* ------------------------------------------------------------------ */
/* Independent dense reference.  The two wing loops reproduce the
 * documented table sampling of lrsFilterUp/lrsFilterUD, but sample the
 * source through a bounds-checked callback so zero extension is explicit
 * and nothing is shared with the production tap machinery. */

typedef double (*RefFetch)(void *ctx, int64_t frame, int channel);

static double ref_wing_up(const float *Imp, int Nwing,
                          RefFetch fetch, void *ctx, int ch,
                          int64_t base, double Ph, int Inc)
{
   double v = 0.0;
   int Hp = (int)(Ph * (double)Npc);
   int End = Nwing;
   int64_t idx = base;

   if (Inc == 1) {
      End--;
      if (Ph == 0.0)
         Hp += Npc;
   }
   while (Hp < End) {
      v += (double)Imp[Hp] * fetch(ctx, idx, ch);
      Hp += Npc;
      idx += Inc;
   }
   return v;
}

static double ref_wing_ud(const float *Imp, int Nwing,
                          RefFetch fetch, void *ctx, int ch,
                          int64_t base, double Ph, int Inc, double dhb)
{
   double v = 0.0;
   double Ho = Ph * dhb;
   int End = Nwing;
   int64_t idx = base;

   if (Inc == 1) {
      End--;
      if (Ph == 0.0)
         Ho += dhb;
   }
   while ((int)Ho < End) {
      v += (double)Imp[(int)Ho] * fetch(ctx, idx, ch);
      Ho += dhb;
      idx += Inc;
   }
   return v;
}

/* One output frame for all channels, with the reference's integer
 * quotient/remainder phase. */
static void ref_frame(const float *Imp, int Nwing, int down,
                      double dh, double factor, RefFetch fetch, void *ctx,
                      int channels, uint32_t in, uint32_t out, uint64_t k,
                      double *v)
{
   uint64_t q, r;
   double phi;
   int c;

   ref_phase(k, in, out, &q, &r);
   phi = (double)r / (double)out;

   for (c = 0; c < channels; c++) {
      double x;
      if (!down) {
         x  = ref_wing_up(Imp, Nwing, fetch, ctx, c, (int64_t)q, phi, -1);
         x += ref_wing_up(Imp, Nwing, fetch, ctx, c, (int64_t)q + 1,
                          1.0 - phi, 1);
      }
      else {
         x  = ref_wing_ud(Imp, Nwing, fetch, ctx, c, (int64_t)q, phi, -1, dh);
         x += ref_wing_ud(Imp, Nwing, fetch, ctx, c, (int64_t)q + 1,
                          1.0 - phi, 1, dh);
         x *= factor;
      }
      v[c] = x;
   }
}

/* Reference filter table, built once per process. */
static float *ref_imp;
static float *ref_impd;

static int ref_init(void)
{
   double *imp64;
   int i;

   if (ref_imp)
      return 0;
   imp64 = (double *)malloc((size_t)REF_NWING * sizeof(double));
   ref_imp = (float *)malloc((size_t)REF_NWING * sizeof(float));
   ref_impd = (float *)malloc((size_t)REF_NWING * sizeof(float));
   if (!imp64 || !ref_imp || !ref_impd)
      return 1;

   lrsLpFilter(imp64, REF_NWING, 0.5 * REF_ROLLOFF, REF_BETA, Npc);
   for (i = 0; i < REF_NWING; i++)
      ref_imp[i] = (float)imp64[i];
   for (i = 0; i < REF_NWING - 1; i++)
      ref_impd[i] = ref_imp[i + 1] - ref_imp[i];
   ref_impd[REF_NWING - 1] = -ref_imp[REF_NWING - 1];
   free(imp64);
   return 0;
}

/* Dense source callback. */
typedef struct {
   const float *src;
   uint64_t total;
   int channels;
} DenseCtx;

static double fetch_dense(void *p, int64_t frame, int c)
{
   DenseCtx *d = (DenseCtx *)p;
   if (frame < 0 || (uint64_t)frame >= d->total)
      return 0.0;
   return (double)d->src[(size_t)frame * (size_t)d->channels + (size_t)c];
}

/* Sparse source callback: a small sorted list of chunks on a huge
 * timeline.  Frames outside every chunk are zero. */
typedef struct {
   uint64_t start;
   uint64_t frames;
   const float *pcm;
} PushChunk;

typedef struct {
   const PushChunk *chunks;
   size_t count;
   int channels;
} ChunkCtx;

static double fetch_chunks(void *p, int64_t frame, int c)
{
   ChunkCtx *k = (ChunkCtx *)p;
   uint64_t u;
   size_t i;

   if (frame < 0)
      return 0.0;
   u = (uint64_t)frame;
   for (i = 0; i < k->count; i++) {
      const PushChunk *ch = &k->chunks[i];
      if (u >= ch->start && u < ch->start + ch->frames)
         return (double)ch->pcm[(size_t)(u - ch->start) *
                                (size_t)k->channels + (size_t)c];
   }
   return 0.0;
}

/* ------------------------------------------------------------------ */
/* Event collection and expansion. */

typedef struct {
   uint64_t start;
   uint64_t frames;
   int silence;
   float *pcm;
} Ev;

typedef struct {
   Ev *v;
   size_t n;
   size_t cap;
} EvList;

static void ev_list_init(EvList *L)
{
   L->v = NULL;
   L->n = 0;
   L->cap = 0;
}

static void ev_list_free(EvList *L)
{
   size_t i;
   for (i = 0; i < L->n; i++)
      free(L->v[i].pcm);
   free(L->v);
   ev_list_init(L);
}

static int ev_list_add(EvList *L, uint64_t start, uint64_t frames,
                       int silence, const float *pcm, int channels)
{
   Ev *e;

   if (L->n == L->cap) {
      size_t cap = L->cap ? L->cap * 2 : 16;
      Ev *n = (Ev *)realloc(L->v, cap * sizeof(Ev));
      if (!n)
         return 1;
      L->v = n;
      L->cap = cap;
   }
   e = &L->v[L->n];
   e->start = start;
   e->frames = frames;
   e->silence = silence;
   e->pcm = NULL;
   if (!silence) {
      if (frames > (uint64_t)(SIZE_MAX / ((size_t)channels * sizeof(float))))
         return 1;
      e->pcm = (float *)malloc((size_t)frames * (size_t)channels *
                               sizeof(float));
      if (!e->pcm)
         return 1;
      memcpy(e->pcm, pcm, (size_t)frames * (size_t)channels * sizeof(float));
   }
   L->n += 1;
   return 0;
}

/* Drain everything currently available, using the requested pull
 * capacity.  The event list keeps the pieces exactly as delivered. */
static int collect_events(lrs_sparse *st, int channels, size_t capacity,
                          EvList *L)
{
   float *buf;
   size_t guard = 0;

   ev_list_init(L);
   if (capacity == 0)
      return 1;
   if (capacity > (size_t)(SIZE_MAX / ((size_t)channels * sizeof(float))))
      return 1;
   buf = (float *)malloc(capacity * (size_t)channels * sizeof(float));
   if (!buf)
      return 1;

   for (;;) {
      uint64_t start = 0, frames = 0;
      int silence = 0;
      int r = lrs_sparse_pull(st, buf, capacity, &start, &frames, &silence);

      if (r == LRS_SPARSE_ERROR) {
         free(buf);
         return 1;
      }
      if (r != LRS_SPARSE_EVENT || frames == 0)
         break;
      if (ev_list_add(L, start, frames, silence, buf, channels)) {
         free(buf);
         return 1;
      }
      if (++guard > 20000000) {
         free(buf);
         return 1;
      }
   }
   free(buf);
   return 0;
}

/* Verify that the event pieces are contiguous from frame 0 and add up to
 * exactly expected_frames. */
static int ev_contiguous(const EvList *L, uint64_t expected_frames)
{
   uint64_t pos = 0;
   size_t i;

   for (i = 0; i < L->n; i++) {
      if (L->v[i].start != pos)
         return 1;
      pos += L->v[i].frames;
      if (pos < L->v[i].start)           /* overflow */
         return 1;
   }
   return pos == expected_frames ? 0 : 1;
}

/* Expand a contiguous event list into dense interleaved PCM.  Only used
 * for short timelines. */
static float *ev_expand(const EvList *L, int channels, uint64_t expected,
                        int *ok)
{
   float *out;
   uint64_t pos = 0;
   size_t i;

   *ok = 0;
   if (expected == 0) {
      out = (float *)malloc(sizeof(float));
      if (out)
         *ok = 1;
      return out;
   }
   if (expected > (uint64_t)(SIZE_MAX / ((size_t)channels * sizeof(float))))
      return NULL;
   out = (float *)calloc((size_t)expected * (size_t)channels, sizeof(float));
   if (!out)
      return NULL;

   for (i = 0; i < L->n; i++) {
      const Ev *e = &L->v[i];
      if (e->start != pos || e->frames > expected - pos) {
         free(out);
         return NULL;
      }
      if (!e->silence)
         memcpy(out + (size_t)pos * (size_t)channels, e->pcm,
                (size_t)e->frames * (size_t)channels * sizeof(float));
      pos += e->frames;
   }
   if (pos != expected) {
      free(out);
      return NULL;
   }
   *ok = 1;
   return out;
}

/* ------------------------------------------------------------------ */
/* Dense timeline construction. */

typedef struct {
   uint64_t start;
   uint64_t end;
} Island;

static void make_source(float *src, uint64_t total, int channels,
                        const Island *islands, int nislands, int layout)
{
   uint64_t t;
   int i, c;

   memset(src, 0, (size_t)total * (size_t)channels * sizeof(float));

   for (i = 0; i < nislands; i++) {
      const Island *is = &islands[i];
      for (t = is->start; t < is->end; t++) {
         for (c = 0; c < channels; c++) {
            double v;
            if (layout == 0) {
               /* first-frame and last-frame impulses plus a tone */
               v = 0.20 * sin((double)(t - is->start) * 0.21 + c * 0.7);
               if (t == is->start)
                  v += (c & 1) ? -1.0 : 1.0;
               if (t + 1 == is->end)
                  v += (c & 1) ? 0.5 : -0.5;
            }
            else {
               /* distinct tones so nearby islands cannot share history */
               v = 0.30 * sin((double)t * (0.13 + 0.07 * i) + c * 1.1);
               if (t == is->start)
                  v += 0.75;
            }
            src[(size_t)t * (size_t)channels + (size_t)c] = (float)v;
         }
      }
   }
}

/* Run the sparse converter over the islands of a dense source and compare
 * the full expanded output against the independent reference. */
static int dense_compare(uint32_t in, uint32_t out, int channels, int layout)
{
   static const Island edges[] = {
      {  0,  60 }, { 250, 310 }, { 540, 600 }
   };
   static const Island nearby[] = {
      { 200, 260 }, { 268, 328 }, { 336, 396 }
   };
   const Island *islands;
   int nislands;
   uint64_t total, expected, pos;
   float *src = NULL, *got = NULL, *ref = NULL;
   PushChunk chunks[3];
   EvList L;
   DenseCtx ctx;
   double maxerr = 0.0;
   int i, c, ok;
   lrs_sparse *st = NULL;

   if (ref_init())
      return 1;

   if (layout == 0) {
      islands = edges;
      nislands = 3;
      total = 600;
   }
   else {
      islands = nearby;
      nislands = 3;
      total = 500;
   }
   expected = umuldiv_ceil(total, out, in);

   src = (float *)malloc((size_t)total * (size_t)channels * sizeof(float));
   if (!src)
      return 1;
   make_source(src, total, channels, islands, nislands, layout);

   for (i = 0; i < nislands; i++) {
      chunks[i].start = islands[i].start;
      chunks[i].frames = islands[i].end - islands[i].start;
      chunks[i].pcm = src + (size_t)islands[i].start * (size_t)channels;
   }

   st = lrs_sparse_open(in, out, channels);
   TASK_CHECK(st != NULL);
   for (i = 0; i < nislands; i++)
      TASK_CHECK(lrs_sparse_push(st, chunks[i].start, chunks[i].frames,
                                 chunks[i].pcm) == LRS_SPARSE_OK);
   TASK_CHECK(lrs_sparse_finish(st, total) == LRS_SPARSE_OK);
   TASK_CHECK(lrs_sparse_output_length(st) == expected);

   TASK_CHECK(collect_events(st, channels, 37, &L) == 0);
   TASK_CHECK(ev_contiguous(&L, expected) == 0);
   got = ev_expand(&L, channels, expected, &ok);
   TASK_CHECK(got != NULL && ok);

   ref = (float *)malloc((size_t)expected * (size_t)channels *
                         sizeof(float));
   TASK_CHECK(ref != NULL);

   ctx.src = src;
   ctx.total = total;
   ctx.channels = channels;
   {
      int down = out < in;
      double factor = (double)out / (double)in;
      double dh = down ? factor * (double)Npc : (double)Npc;
      double *v = (double *)malloc((size_t)channels * sizeof(double));

      TASK_CHECK(v != NULL);
      for (pos = 0; pos < expected; pos++) {
         ref_frame(ref_imp, REF_NWING, down, dh, factor, fetch_dense,
                   &ctx, channels, in, out, pos, v);
         for (c = 0; c < channels; c++)
            ref[(size_t)pos * (size_t)channels + (size_t)c] = (float)v[c];
      }
      free(v);
   }

   for (pos = 0; pos < expected; pos++) {
      for (c = 0; c < channels; c++) {
         double d = fabs((double)got[(size_t)pos * (size_t)channels +
                                     (size_t)c] -
                         (double)ref[(size_t)pos * (size_t)channels +
                                     (size_t)c]);
         if (d > maxerr)
            maxerr = d;
      }
   }
   TASK_CHECK(isfinite(maxerr));
   TASK_CHECK(maxerr <= MAX_ERR);

   free(src);
   free(got);
   free(ref);
   ev_list_free(&L);
   lrs_sparse_close(st);
   return 0;
}

static int dense_edges(void)
{
   static const uint32_t in_rates[]  = { 48000, 44100,  8000, 192000, 96000 };
   static const uint32_t out_rates[] = { 44100, 48000, 192000,  8000, 48000 };
   static const int chans[] = { 1, 2, 6 };
   size_t r, c;

   for (c = 0; c < sizeof(chans) / sizeof(chans[0]); c++)
      for (r = 0; r < sizeof(in_rates) / sizeof(in_rates[0]); r++)
         TASK_CHECK(dense_compare(in_rates[r], out_rates[r], chans[c], 0) == 0);
   return 0;
}

static int dense_nearby(void)
{
   static const uint32_t in_rates[]  = { 48000, 44100, 96000,  8000 };
   static const uint32_t out_rates[] = { 44100, 48000, 48000, 192000 };
   static const int chans[] = { 1, 2, 6 };
   size_t r, c;

   for (c = 0; c < sizeof(chans) / sizeof(chans[0]); c++)
      for (r = 0; r < sizeof(in_rates) / sizeof(in_rates[0]); r++)
         TASK_CHECK(dense_compare(in_rates[r], out_rates[r], chans[c], 1) == 0);
   return 0;
}

static int dense_rates(void)
{
   static const uint32_t in_rates[]  = { 48000, 44100,  8000, 192000,
                                         96000, 192000, 44100,  8000 };
   static const uint32_t out_rates[] = { 44100, 48000, 192000,  8000,
                                         48000, 96000, 44100,  8000 };
   size_t r;

   for (r = 0; r < sizeof(in_rates) / sizeof(in_rates[0]); r++)
      TASK_CHECK(dense_compare(in_rates[r], out_rates[r], 2, 0) == 0);
   return 0;
}

/* ------------------------------------------------------------------ */
/* Sparse group. */

static float *make_tone(int channels, uint64_t frames)
{
   float *pcm = (float *)malloc((size_t)frames * (size_t)channels *
                                sizeof(float));
   uint64_t t;
   int c;

   if (!pcm)
      return NULL;
   for (t = 0; t < frames; t++)
      for (c = 0; c < channels; c++)
         pcm[(size_t)t * (size_t)channels + (size_t)c] =
            (float)(0.4 * sin((double)t * 0.031 + c * 0.9));
   return pcm;
}

/* A local window of output compared against the reference.  The rest of
 * the timeline must be silence events. */
static int big_gap_check(uint32_t in, uint32_t out, int channels,
                         uint64_t base, uint64_t len, uint64_t tail)
{
   uint64_t total = base + len + tail;
   uint64_t expected, k0, k1, win, j;
   uint64_t kmid;
   const uint64_t margin = 4000;
   PushChunk chunk;
   ChunkCtx ctx;
   float *pcm = NULL, *winpcm = NULL;
   EvList L;
   int i, c;
   lrs_sparse *st = NULL;
   double maxerr = 0.0;

   if (ref_init())
      return 1;
   expected = umuldiv_ceil(total, out, in);

   pcm = make_tone(channels, len);
   TASK_CHECK(pcm != NULL);
   chunk.start = base;
   chunk.frames = len;
   chunk.pcm = pcm;

   st = lrs_sparse_open(in, out, channels);
   TASK_CHECK(st != NULL);
   TASK_CHECK(lrs_sparse_push(st, chunk.start, chunk.frames, chunk.pcm) ==
              LRS_SPARSE_OK);
   TASK_CHECK(lrs_sparse_finish(st, total) == LRS_SPARSE_OK);
   TASK_CHECK(lrs_sparse_output_length(st) == expected);

   TASK_CHECK(collect_events(st, channels, 4096, &L) == 0);
   TASK_CHECK(ev_contiguous(&L, expected) == 0);

   /* Silence must be represented by a handful of events, not one per
    * silent frame. */
   TASK_CHECK(L.n <= 16);

   kmid = umuldiv_floor(base, out, in);
   k0 = kmid > margin ? kmid - margin : 0;
   win = umuldiv_floor(len, out, in) + 2 * margin + 64;
   k1 = k0 + win;
   if (k1 > expected)
      k1 = expected;
   win = k1 - k0;
   TASK_CHECK(win > 0);

   /* Every PCM event must lie inside the window; everything outside is
    * silence. */
   for (i = 0; i < (int)L.n; i++) {
      if (!L.v[i].silence) {
         TASK_CHECK(L.v[i].start >= k0);
         TASK_CHECK(L.v[i].start + L.v[i].frames <= k1);
      }
   }

   winpcm = (float *)calloc((size_t)win * (size_t)channels, sizeof(float));
   TASK_CHECK(winpcm != NULL);
   for (i = 0; i < (int)L.n; i++) {
      const Ev *e = &L.v[i];
      uint64_t a = e->start > k0 ? e->start : k0;
      uint64_t b = e->start + e->frames < k1 ? e->start + e->frames : k1;
      if (!e->silence && b > a) {
         memcpy(winpcm + (size_t)(a - k0) * (size_t)channels,
                e->pcm + (size_t)(a - e->start) * (size_t)channels,
                (size_t)(b - a) * (size_t)channels * sizeof(float));
      }
   }

   ctx.chunks = &chunk;
   ctx.count = 1;
   ctx.channels = channels;
   {
      int down = out < in;
      double factor = (double)out / (double)in;
      double dh = down ? factor * (double)Npc : (double)Npc;
      double *v = (double *)malloc((size_t)channels * sizeof(double));

      TASK_CHECK(v != NULL);
      for (j = 0; j < win; j++) {
         uint64_t k = k0 + j;
         ref_frame(ref_imp, REF_NWING, down, dh, factor, fetch_chunks,
                   &ctx, channels, in, out, k, v);
         for (c = 0; c < channels; c++) {
            double d = fabs((double)winpcm[(size_t)j * (size_t)channels +
                                           (size_t)c] - v[c]);
            if (d > maxerr)
               maxerr = d;
         }
      }
      free(v);
   }
   TASK_CHECK(maxerr <= MAX_ERR);

   free(pcm);
   free(winpcm);
   ev_list_free(&L);
   lrs_sparse_close(st);
   return 0;
}

static int sparse_huge_gap(void)
{
   /* Comfortably beyond 2^50 source frames. */
   const uint64_t base = (1ULL << 50) + 12345;
   TASK_CHECK(big_gap_check(48000, 44100, 2, base, 200, 1000) == 0);
   TASK_CHECK(big_gap_check(96000, 48000, 1, base + 7000, 300, 5000) == 0);
   TASK_CHECK(big_gap_check(8000, 192000, 6, base + 11000, 128, 4096) == 0);
   return 0;
}

static int sparse_silence(void)
{
   const uint64_t total = (1ULL << 50) + 777;
   uint64_t expected = umuldiv_ceil(total, 44100, 48000);
   EvList L;
   lrs_sparse *st = lrs_sparse_open(48000, 44100, 2);

   TASK_CHECK(st != NULL);
   TASK_CHECK(lrs_sparse_finish(st, total) == LRS_SPARSE_OK);
   TASK_CHECK(lrs_sparse_output_length(st) == expected);
   TASK_CHECK(collect_events(st, 2, 1024, &L) == 0);
   TASK_CHECK(ev_contiguous(&L, expected) == 0);
   TASK_CHECK(L.n == 1);
   TASK_CHECK(L.v[0].silence == 1);
   TASK_CHECK(L.v[0].start == 0);
   TASK_CHECK(L.v[0].frames == expected);
   ev_list_free(&L);
   lrs_sparse_close(st);

   /* A finished empty timeline has zero length and no events. */
   st = lrs_sparse_open(48000, 48000, 1);
   TASK_CHECK(st != NULL);
   TASK_CHECK(lrs_sparse_finish(st, 0) == LRS_SPARSE_OK);
   TASK_CHECK(lrs_sparse_output_length(st) == 0);
   TASK_CHECK(collect_events(st, 1, 16, &L) == 0);
   TASK_CHECK(L.n == 0);
   ev_list_free(&L);
   lrs_sparse_close(st);
   return 0;
}

static int sparse_zero_length(void)
{
   const uint64_t total = 200000;
   float *tone;
   EvList plain, probed;
   lrs_sparse *st;
   int i, ok;
   float *a, *b;
   uint64_t ea, eb;

   tone = make_tone(2, 500);
   TASK_CHECK(tone != NULL);

   /* Reference timeline: one island at 100000. */
   st = lrs_sparse_open(48000, 44100, 2);
   TASK_CHECK(st != NULL);
   TASK_CHECK(lrs_sparse_push(st, 100000, 500, tone) == LRS_SPARSE_OK);
   TASK_CHECK(lrs_sparse_finish(st, total) == LRS_SPARSE_OK);
   ea = lrs_sparse_output_length(st);
   TASK_CHECK(collect_events(st, 2, 71, &plain) == 0);
   lrs_sparse_close(st);

   /* Same timeline with zero-length probes sprinkled in, including a
    * zero-length push after finish and with a NULL buffer. */
   st = lrs_sparse_open(48000, 44100, 2);
   TASK_CHECK(st != NULL);
   TASK_CHECK(lrs_sparse_push(st, 3, 0, NULL) == LRS_SPARSE_OK);
   TASK_CHECK(lrs_sparse_push(st, 100000, 500, tone) == LRS_SPARSE_OK);
   TASK_CHECK(lrs_sparse_push(st, 90000, 0, NULL) == LRS_SPARSE_OK);
   TASK_CHECK(lrs_sparse_push(st, 100500, 0, NULL) == LRS_SPARSE_OK);
   TASK_CHECK(lrs_sparse_finish(st, total) == LRS_SPARSE_OK);
   TASK_CHECK(lrs_sparse_push(st, 150000, 0, NULL) == LRS_SPARSE_OK);
   eb = lrs_sparse_output_length(st);
   TASK_CHECK(eb == ea);
   TASK_CHECK(collect_events(st, 2, 5, &probed) == 0);
   lrs_sparse_close(st);

   a = ev_expand(&plain, 2, ea, &i);
   TASK_CHECK(a != NULL && i);
   b = ev_expand(&probed, 2, eb, &ok);
   TASK_CHECK(b != NULL && ok);
   TASK_CHECK(memcmp(a, b, (size_t)ea * 2 * sizeof(float)) == 0);

   free(a);
   free(b);
   free(tone);
   ev_list_free(&plain);
   ev_list_free(&probed);
   return 0;
}

/* ------------------------------------------------------------------ */
/* Stream group. */

/* Run a complete timeline and expand it; used for bit-exact comparisons
 * between differently chunked or differently pulled runs. */
static int run_timeline(uint32_t in, uint32_t out, int channels,
                        const PushChunk *chunks, size_t nchunks,
                        uint64_t total, size_t capacity,
                        float **expanded, uint64_t *length)
{
   lrs_sparse *st = lrs_sparse_open(in, out, channels);
   EvList L;
   uint64_t expected;
   int ok;
   size_t i;

   *expanded = NULL;
   *length = 0;
   TASK_CHECK(st != NULL);
   for (i = 0; i < nchunks; i++)
      TASK_CHECK(lrs_sparse_push(st, chunks[i].start, chunks[i].frames,
                                 chunks[i].pcm) == LRS_SPARSE_OK);
   TASK_CHECK(lrs_sparse_finish(st, total) == LRS_SPARSE_OK);
   expected = lrs_sparse_output_length(st);
   TASK_CHECK(expected == umuldiv_ceil(total, out, in));
   TASK_CHECK(collect_events(st, channels, capacity, &L) == 0);
   TASK_CHECK(ev_contiguous(&L, expected) == 0);
   *expanded = ev_expand(&L, channels, expected, &ok);
   TASK_CHECK(*expanded != NULL && ok);
   *length = expected;
   ev_list_free(&L);
   lrs_sparse_close(st);
   return 0;
}

static int same_bytes(const float *a, const float *b, uint64_t frames,
                      int channels)
{
   return memcmp(a, b, (size_t)frames * (size_t)channels *
                          sizeof(float)) == 0;
}

static int stream_split(void)
{
   const uint64_t total = 20000;
   const uint64_t start = 5000;
   const uint64_t len = 900;
   int channels = 2;
   float *src = make_tone(channels, len);
   PushChunk whole;
   PushChunk *pieces;
   float *a = NULL, *b = NULL;
   uint64_t la = 0, lb = 0;
   uint64_t pos, piece;
   size_t np = 0, i;

   TASK_CHECK(src != NULL);
   pieces = (PushChunk *)malloc((size_t)len * sizeof(PushChunk));
   TASK_CHECK(pieces != NULL);
   whole.start = start;
   whole.frames = len;
   whole.pcm = src;

   /* Split the island into adjacent pieces of irregular sizes. */
   pos = 0;
   piece = 1;
   while (pos < len) {
      uint64_t n = piece % 7 + 1;
      if (n > len - pos)
         n = len - pos;
      pieces[np].start = start + pos;
      pieces[np].frames = n;
      pieces[np].pcm = src + (size_t)pos * (size_t)channels;
      np++;
      pos += n;
      piece += 3;
   }

   TASK_CHECK(run_timeline(44100, 48000, channels, &whole, 1, total, 1000,
                           &a, &la) == 0);
   TASK_CHECK(run_timeline(44100, 48000, channels, pieces, np, total, 1000,
                           &b, &lb) == 0);
   TASK_CHECK(la == lb);
   TASK_CHECK(same_bytes(a, b, la, channels));

   /* Downsampling from 96 kHz has a much wider support; splitting must
    * still be transparent. */
   free(a);
   free(b);
   a = b = NULL;
   TASK_CHECK(run_timeline(96000, 48000, channels, &whole, 1, total, 173,
                           &a, &la) == 0);
   TASK_CHECK(run_timeline(96000, 48000, channels, pieces, np, total, 173,
                           &b, &lb) == 0);
   TASK_CHECK(la == lb);
   TASK_CHECK(same_bytes(a, b, la, channels));

   free(a);
   free(b);
   free(src);
   free(pieces);
   (void)i;
   return 0;
}

static int stream_capacity(void)
{
   const uint64_t total = 30000;
   float *src = make_tone(6, 700);
   PushChunk chunk;
   float *ref = NULL, *got = NULL;
   uint64_t lref = 0, lgot = 0;
   static const size_t caps[] = { 1, 2, 7, 64, 4096 };
   size_t i;

   TASK_CHECK(src != NULL);
   chunk.start = 12000;
   chunk.frames = 700;
   chunk.pcm = src;

   TASK_CHECK(run_timeline(48000, 44100, 6, &chunk, 1, total, 4096,
                           &ref, &lref) == 0);
   for (i = 0; i < sizeof(caps) / sizeof(caps[0]); i++) {
      TASK_CHECK(run_timeline(48000, 44100, 6, &chunk, 1, total, caps[i],
                              &got, &lgot) == 0);
      TASK_CHECK(lgot == lref);
      TASK_CHECK(same_bytes(ref, got, lref, 6));
      free(got);
      got = NULL;
   }
   free(ref);
   free(src);
   return 0;
}

static int stream_empty_pull(void)
{
   const uint64_t total = 40000;
   float *src = make_tone(2, 400);
   PushChunk chunk;
   float *ref = NULL, *got = NULL;
   uint64_t lref = 0;
   float *tiny = (float *)malloc(16 * sizeof(float));
   EvList L;
   lrs_sparse *st;
   uint64_t start = 123, frames = 99;
   int silence = 7;

   TASK_CHECK(src != NULL);
   TASK_CHECK(tiny != NULL);
   chunk.start = 20000;
   chunk.frames = 400;
   chunk.pcm = src;

   TASK_CHECK(run_timeline(44100, 48000, 2, &chunk, 1, total, 999,
                           &ref, &lref) == 0);

   /* Interleave zero-capacity pulls with pushes and normal pulls. */
   st = lrs_sparse_open(44100, 48000, 2);
   TASK_CHECK(st != NULL);
   TASK_CHECK(lrs_sparse_pull(st, tiny, 0, &start, &frames, &silence) ==
              LRS_SPARSE_OK);
   TASK_CHECK(frames == 0);
   TASK_CHECK(lrs_sparse_push(st, chunk.start, chunk.frames, chunk.pcm) ==
              LRS_SPARSE_OK);
   TASK_CHECK(lrs_sparse_pull(st, tiny, 0, &start, &frames, &silence) ==
              LRS_SPARSE_OK);
   TASK_CHECK(frames == 0);
   TASK_CHECK(lrs_sparse_finish(st, total) == LRS_SPARSE_OK);
   TASK_CHECK(lrs_sparse_pull(st, tiny, 0, &start, &frames, &silence) ==
              LRS_SPARSE_OK);
   TASK_CHECK(frames == 0);
   TASK_CHECK(collect_events(st, 2, 999, &L) == 0);
   lrs_sparse_close(st);

   TASK_CHECK(ev_contiguous(&L, lref) == 0);
   {
      int ok;
      got = ev_expand(&L, 2, lref, &ok);
      TASK_CHECK(got != NULL && ok);
   }
   TASK_CHECK(same_bytes(ref, got, lref, 2));

   free(ref);
   free(got);
   free(src);
   free(tiny);
   ev_list_free(&L);
   return 0;
}

static int stream_prefix_finish(void)
{
   const uint64_t total = 50000;
   float *src1 = make_tone(2, 500);
   float *src2 = make_tone(2, 600);
   PushChunk both[2];
   PushChunk first;
   float *full = NULL, *part1 = NULL, *joined = NULL;
   uint64_t lfull = 0, l1 = 0;
   uint64_t start, frames;
   int silence, ok;
   EvList L1, L2;
   lrs_sparse *st;

   TASK_CHECK(src1 != NULL && src2 != NULL);

   /* The two islands are only 10 source frames apart, well inside the
    * filter's support, so they share one convolution. */
   first.start = 20000;
   first.frames = 500;
   first.pcm = src1;
   both[0] = first;
   both[1].start = 20510;
   both[1].frames = 600;
   both[1].pcm = src2;

   TASK_CHECK(run_timeline(44100, 48000, 2, both, 2, total, 333,
                           &full, &lfull) == 0);

   /* Staged run: push island 1, drain the confirmed prefix, then push
    * island 2, finish, drain the rest, and repeat finish. */
   st = lrs_sparse_open(44100, 48000, 2);
   TASK_CHECK(st != NULL);
   TASK_CHECK(lrs_sparse_push(st, first.start, first.frames, first.pcm) ==
              LRS_SPARSE_OK);
   TASK_CHECK(collect_events(st, 2, 333, &L1) == 0);
   {
      uint64_t pos = 0;
      size_t i;
      for (i = 0; i < L1.n; i++) {
         TASK_CHECK(L1.v[i].start == pos);
         pos += L1.v[i].frames;
      }
      l1 = pos;
   }
   TASK_CHECK(l1 > 0 && l1 < lfull);

   TASK_CHECK(lrs_sparse_push(st, both[1].start, both[1].frames,
                              both[1].pcm) == LRS_SPARSE_OK);
   TASK_CHECK(lrs_sparse_finish(st, total) == LRS_SPARSE_OK);
   TASK_CHECK(lrs_sparse_finish(st, total) == LRS_SPARSE_OK);
   TASK_CHECK(lrs_sparse_finish(st, total - 1) == LRS_SPARSE_ERROR);
   TASK_CHECK(collect_events(st, 2, 333, &L2) == 0);
   /* A repeated finish must not have produced duplicate events. */
   {
      float tiny[8];
      TASK_CHECK(lrs_sparse_pull(st, tiny, 4, &start, &frames, &silence) ==
                 LRS_SPARSE_OK);
   }
   TASK_CHECK(frames == 0);
   /* Non-empty pushes are rejected after finish. */
   TASK_CHECK(lrs_sparse_push(st, 30000, 1, src1) == LRS_SPARSE_ERROR);
   lrs_sparse_close(st);

   part1 = ev_expand(&L1, 2, l1, &ok);
   TASK_CHECK(part1 != NULL && ok);
   /* L2's first event may repeat the start of the prefix that L1 already
    * delivered when the capacity cut a PCM event; compare by position. */
   joined = (float *)malloc((size_t)lfull * 2 * sizeof(float));
   TASK_CHECK(joined != NULL);
   memset(joined, 0, (size_t)lfull * 2 * sizeof(float));
   memcpy(joined, part1, (size_t)l1 * 2 * sizeof(float));
   {
      size_t i;
      for (i = 0; i < L2.n; i++) {
         const Ev *e = &L2.v[i];
         uint64_t a = e->start;
         uint64_t b = e->start + e->frames;
         if (a < l1)
            a = l1;
         if (b > lfull)
            b = lfull;
         if (!e->silence && b > a)
            memcpy(joined + (size_t)a * 2,
                   e->pcm + (size_t)(a - e->start) * 2,
                   (size_t)(b - a) * 2 * sizeof(float));
      }
   }

   /* The staged prefix must equal the head of the single-pass result. */
   TASK_CHECK(same_bytes(part1, full, l1, 2));
   TASK_CHECK(same_bytes(joined, full, lfull, 2));

   free(full);
   free(part1);
   free(joined);
   free(src1);
   free(src2);
   ev_list_free(&L1);
   ev_list_free(&L2);
   return 0;
}

static int stream_errors(void)
{
   float pcm[8] = { 0 };
   lrs_sparse *st;

   TASK_CHECK(lrs_sparse_open(7999, 48000, 2) == NULL);
   TASK_CHECK(lrs_sparse_open(48000, 192001, 2) == NULL);
   TASK_CHECK(lrs_sparse_open(48000, 44100, 3) == NULL);
   TASK_CHECK(lrs_sparse_open(48000, 44100, 0) == NULL);

   st = lrs_sparse_open(48000, 44100, 2);
   TASK_CHECK(st != NULL);
   TASK_CHECK(lrs_sparse_push(st, 100, 2, NULL) == LRS_SPARSE_ERROR);
   TASK_CHECK(lrs_sparse_push(st, 100, 2, pcm) == LRS_SPARSE_OK);
   TASK_CHECK(lrs_sparse_push(st, 101, 1, pcm) == LRS_SPARSE_ERROR);
   TASK_CHECK(lrs_sparse_push(st, 102, 1, pcm) == LRS_SPARSE_OK);
   TASK_CHECK(lrs_sparse_finish(st, 102) == LRS_SPARSE_ERROR);
   TASK_CHECK(lrs_sparse_finish(st, 103) == LRS_SPARSE_OK);
   TASK_CHECK(lrs_sparse_push(st, 103, 1, pcm) == LRS_SPARSE_ERROR);
   TASK_CHECK(lrs_sparse_pull(st, NULL, 1, NULL, NULL, NULL) ==
              LRS_SPARSE_ERROR);
   lrs_sparse_close(st);
   lrs_sparse_close(NULL);
   return 0;
}

/* ------------------------------------------------------------------ */
/* Registration. */

const struct task_case task_cases[] = {
   { "dense",  "edges",        dense_edges },
   { "dense",  "nearby",       dense_nearby },
   { "dense",  "rates",        dense_rates },
   { "sparse", "huge_gap",     sparse_huge_gap },
   { "sparse", "silence",      sparse_silence },
   { "sparse", "zero_length",  sparse_zero_length },
   { "stream", "split",        stream_split },
   { "stream", "capacity",     stream_capacity },
   { "stream", "empty_pull",   stream_empty_pull },
   { "stream", "prefix_finish", stream_prefix_finish },
   { "stream", "errors",       stream_errors }
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);

/**********************************************************************

  sparse_resample.c

  Sparse-stream interface for libresample.

  A source timeline is stored as a list of PCM chunks separated by
  implicit zeros.  Only chunks are allocated; a silence is just a hole
  between two chunks, so a gap costs no memory and no work no matter how
  many source frames it spans.

  Output is produced lazily.  Pushing a chunk makes the source known up
  to the end of that chunk; every output frame whose whole filter support
  lies before that point can be produced immediately, and is queued as
  events for lrs_sparse_pull().  The output phase of frame k is computed
  with integer quotient and remainder on k, never by accumulating a
  floating-point time or by converting k to double, so oracle arithmetic
  stays exact far beyond 2^50 source frames.

  Dual-licensed as LGPL and BSD; see README.md and LICENSE* files.

**********************************************************************/

#include "../include/sparse_resample.h"

#include "resample_defs.h"
#include "filterkit.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Filter quality.  These match resample_open(1, ...) exactly. */
#define SPARSE_NMULT     35
#define SPARSE_ROLLOFF   0.90
#define SPARSE_BETA      6.0

typedef struct sparse_chunk {
   uint64_t start;              /* first source frame of this chunk */
   uint64_t end;                /* one past the last source frame     */
   float   *pcm;                /* frames * channels, interleaved     */
} sparse_chunk;

typedef struct sparse_event {
   struct sparse_event *next;
   uint64_t start;              /* absolute output frame              */
   uint64_t frames;             /* total frames in the event          */
   uint64_t offset;             /* frames already delivered           */
   float   *pcm;                /* NULL for a silence event           */
   int      is_silence;
} sparse_event;

struct lrs_sparse {
   uint32_t in_rate;
   uint32_t out_rate;
   int      channels;
   int      down;               /* out_rate < in_rate                 */

   float   *Imp;
   float   *ImpD;
   int      Nwing;
   double   factor;
   double   dh;                 /* decimation filter step, down only  */
   float    LpScl;
   uint64_t halfspan;           /* max source samples touched per wing*/
   float   *taps;               /* channels * tap_len scratch         */
   int      tap_len;
   int      center;             /* tap index corresponding to q       */

   sparse_chunk *chunks;
   size_t        chunk_count;
   size_t        chunk_cap;

   sparse_event *ev_head;
   sparse_event *ev_tail;

   uint64_t next_k;             /* next output frame to produce       */
   uint64_t prev_end;           /* end of the last non-empty chunk    */
   int      finished;
   uint64_t total_frames;
   uint64_t output_frames;
};

/* ------------------------------------------------------------------ */
/* Exact integer helpers.  All intermediate products stay below 2^64
   for every supported rate pair; the quotient decomposition avoids the
   k * in_rate product that would overflow for very large timelines. */

static uint64_t ceil_mul_div(uint64_t v, uint64_t mul, uint64_t div)
{
   uint64_t a = v / div;
   uint64_t b = v % div;
   uint64_t prod = b * mul;      /* b < div, mul <= 192000            */
   return a * mul + (prod + div - 1) / div;
}

/* q = floor(k * in / out), r = k * in - q * out. */
static void phase_at(uint64_t k, uint32_t in, uint32_t out,
                     uint64_t *q, uint64_t *r)
{
   uint64_t a = k / out;
   uint64_t b = k % out;
   uint64_t nb = b * (uint64_t)in;
   *q = a * (uint64_t)in + nb / out;
   *r = nb % out;
}

/* First output frame whose source coordinate may touch source frame s,
   accounting for the filter reach on the other side. */
static uint64_t k_lo_for(uint64_t s, uint64_t half,
                         uint32_t in, uint32_t out)
{
   if (s <= half)
      return 0;
   return ceil_mul_div(s - half, out, in);
}

/* One past the last output frame whose source coordinate may touch
   source frame e. */
static uint64_t k_hi_for(uint64_t e, uint64_t half,
                         uint32_t in, uint32_t out)
{
   return ceil_mul_div(e + half, out, in);
}

/* ------------------------------------------------------------------ */
/* Filter table setup, copied from resample_open()'s high-quality path
   so the coefficients are bit-for-bit the same. */

static int build_filter(lrs_sparse *st)
{
   double *Imp64;
   int i;

   st->Nwing = Npc * (SPARSE_NMULT - 1) / 2;

   Imp64 = (double *)malloc((size_t)st->Nwing * sizeof(double));
   st->Imp = (float *)malloc((size_t)st->Nwing * sizeof(float));
   st->ImpD = (float *)malloc((size_t)st->Nwing * sizeof(float));
   if (!Imp64 || !st->Imp || !st->ImpD) {
      free(Imp64);
      return -1;
   }

   lrsLpFilter(Imp64, st->Nwing, 0.5 * SPARSE_ROLLOFF, SPARSE_BETA, Npc);

   for (i = 0; i < st->Nwing; i++)
      st->Imp[i] = (float)Imp64[i];
   for (i = 0; i < st->Nwing - 1; i++)
      st->ImpD[i] = st->Imp[i + 1] - st->Imp[i];
   st->ImpD[st->Nwing - 1] = -st->Imp[st->Nwing - 1];

   free(Imp64);
   return 0;
}

static uint64_t compute_halfspan(const lrs_sparse *st)
{
   double steps;

   if (st->down) {
      /* lrsFilterUD() walks the table with step dh up to Nwing. */
      steps = ceil(((double)st->Nwing - 1.0) / st->dh);
   }
   else {
      /* lrsFilterUp() walks the table with step Npc. */
      steps = ceil((double)st->Nwing / (double)Npc);
   }
   /* +4 covers the shifted right wing pointer and rounding margins. */
   return (uint64_t)steps + 4;
}

/* ------------------------------------------------------------------ */
/* Chunk bookkeeping. */

static int add_chunk(lrs_sparse *st, uint64_t start, uint64_t frames,
                     const float *pcm)
{
   sparse_chunk *c;
   size_t samples;

   if (frames > (uint64_t)(SIZE_MAX / (size_t)st->channels))
      return -1;
   samples = (size_t)frames * (size_t)st->channels;
   if (st->chunk_count == st->chunk_cap) {
      size_t cap = st->chunk_cap ? st->chunk_cap * 2 : 8;
      sparse_chunk *n = (sparse_chunk *)realloc(st->chunks,
                                                cap * sizeof(sparse_chunk));
      if (!n)
         return -1;
      st->chunks = n;
      st->chunk_cap = cap;
   }

   c = &st->chunks[st->chunk_count];
   c->pcm = (float *)malloc(samples * sizeof(float));
   if (!c->pcm)
      return -1;
   memcpy(c->pcm, pcm, samples * sizeof(float));
   c->start = start;
   c->end = start + frames;
   st->chunk_count += 1;
   return 0;
}

/* Remove chunks that can no longer contribute to any future output.
   The output cursor advances monotonically, so a chunk ending at or
   before q(next_k) - halfspan is out of reach forever. */
static void drop_old_chunks(lrs_sparse *st)
{
   uint64_t q, r, threshold;
   size_t i, drop = 0;

   if (st->finished && st->next_k >= st->output_frames) {
      drop = st->chunk_count;
   }
   else {
      phase_at(st->next_k, st->in_rate, st->out_rate, &q, &r);
      if (q <= st->halfspan)
         return;
      threshold = q - st->halfspan;
      while (drop < st->chunk_count && st->chunks[drop].end <= threshold)
         drop++;
   }

   if (!drop)
      return;

   for (i = 0; i < drop; i++)
      free(st->chunks[i].pcm);
   memmove(st->chunks, st->chunks + drop,
           (st->chunk_count - drop) * sizeof(sparse_chunk));
   st->chunk_count -= drop;

   /* Do not let the chunk array itself grow without bound. */
   if (st->chunk_cap > 32 && st->chunk_count < st->chunk_cap / 4) {
      size_t cap = st->chunk_cap / 2;
      sparse_chunk *n = (sparse_chunk *)realloc(st->chunks,
                                                cap * sizeof(sparse_chunk));
      if (n) {
         st->chunks = n;
         st->chunk_cap = cap;
      }
   }
}

/* ------------------------------------------------------------------ */
/* Event queue. */

static void append_event(lrs_sparse *st, uint64_t start, uint64_t frames,
                         float *pcm, int is_silence)
{
   sparse_event *e = (sparse_event *)malloc(sizeof(sparse_event));
   if (!e) {
      free(pcm);
      return;              /* memory failure is reported by the caller */
   }
   e->next = NULL;
   e->start = start;
   e->frames = frames;
   e->offset = 0;
   e->pcm = pcm;
   e->is_silence = is_silence;

   if (st->ev_tail)
      st->ev_tail->next = e;
   else
      st->ev_head = e;
   st->ev_tail = e;
}

/* Appended silence events are merged with an adjacent one so that a
   timeline of many pushes still yields a small, bounded event count. */
static void emit_silence(lrs_sparse *st, uint64_t start, uint64_t frames)
{
   sparse_event *tail = st->ev_tail;

   if (frames == 0)
      return;
   if (tail && tail->is_silence && tail->start + tail->frames == start) {
      tail->frames += frames;
      return;
   }
   append_event(st, start, frames, NULL, 1);
}

/* ------------------------------------------------------------------ */
/* Filtering. */

static void fill_taps(lrs_sparse *st, uint64_t q)
{
   int ch = st->channels;
   int tl = st->tap_len;
   int64_t base = (int64_t)q - (int64_t)st->halfspan;
   int64_t hi = base + (int64_t)tl;
   size_t i;

   memset(st->taps, 0, (size_t)tl * (size_t)ch * sizeof(float));

   for (i = 0; i < st->chunk_count; i++) {
      const sparse_chunk *c = &st->chunks[i];
      int64_t cs = (int64_t)c->start;
      int64_t ce = (int64_t)c->end;
      int64_t a, b, f;
      int cch;

      if (ce <= base)
         continue;
      if (cs >= hi)
         break;

      a = cs > base ? cs : base;
      b = ce < hi ? ce : hi;
      for (f = a; f < b; f++) {
         int ti = (int)(f - base);
         const float *src = c->pcm +
            (size_t)((uint64_t)f - c->start) * (size_t)ch;
         for (cch = 0; cch < ch; cch++)
            st->taps[(size_t)cch * (size_t)tl + (size_t)ti] = src[cch];
      }
   }
}

static void compute_frame(lrs_sparse *st, uint64_t q, uint64_t r,
                          float *out)
{
   double phi = (double)r / (double)st->out_rate;
   int ch = st->channels;
   int tl = st->tap_len;
   int c;

   fill_taps(st, q);

   for (c = 0; c < ch; c++) {
      float *X = st->taps + (size_t)c * (size_t)tl;
      float *Xp = X + st->center;
      float v;

      if (!st->down) {
         v = lrsFilterUp(st->Imp, st->ImpD, (UWORD)st->Nwing, FALSE,
                         Xp, phi, -1);
         v += lrsFilterUp(st->Imp, st->ImpD, (UWORD)st->Nwing, FALSE,
                          Xp + 1, 1.0 - phi, 1);
      }
      else {
         v = lrsFilterUD(st->Imp, st->ImpD, (UWORD)st->Nwing, FALSE,
                         Xp, phi, -1, st->dh);
         v += lrsFilterUD(st->Imp, st->ImpD, (UWORD)st->Nwing, FALSE,
                          Xp + 1, 1.0 - phi, 1, st->dh);
      }
      out[c] = v * st->LpScl;
   }
}

/* Produce one PCM event covering output frames [start, start+frames). */
static int emit_pcm(lrs_sparse *st, uint64_t start, uint64_t frames)
{
   size_t count;
   float *pcm;
   uint64_t q, r, j;

   if (frames == 0)
      return 0;
   if (frames > (uint64_t)(SIZE_MAX / ((size_t)st->channels * sizeof(float))))
      return -1;
   count = (size_t)frames * (size_t)st->channels;
   pcm = (float *)malloc(count * sizeof(float));
   if (!pcm)
      return -1;

   phase_at(start, st->in_rate, st->out_rate, &q, &r);
   for (j = 0; j < frames; j++) {
      compute_frame(st, q, r, pcm + (size_t)j * (size_t)st->channels);
      /* Advance the exact output phase by one output frame. */
      r += st->in_rate;
      q += r / st->out_rate;
      r %= st->out_rate;
   }

   append_event(st, start, frames, pcm, 0);
   return 0;
}

/* Produce every output frame below limit.  Frames whose whole support
   lies in the holes between chunks are emitted as silence without
   computing anything. */
static int emit_events(lrs_sparse *st, uint64_t limit)
{
   uint64_t k = st->next_k;
   size_t i;

   if (limit <= k)
      return 0;

   for (i = 0; i < st->chunk_count; i++) {
      const sparse_chunk *c = &st->chunks[i];
      uint64_t lo = k_lo_for(c->start, st->halfspan,
                             st->in_rate, st->out_rate);
      uint64_t hi = k_hi_for(c->end, st->halfspan,
                             st->in_rate, st->out_rate);
      uint64_t e;

      if (hi <= k)
         continue;
      if (lo < k)
         lo = k;
      if (lo > limit)
         lo = limit;

      if (lo > k) {
         emit_silence(st, k, lo - k);
         k = lo;
      }
      if (k >= limit)
         break;

      e = hi < limit ? hi : limit;
      if (e > k) {
         if (emit_pcm(st, k, e - k))
            return -1;
         k = e;
      }
      if (k >= limit)
         break;
   }

   if (k < limit) {
      emit_silence(st, k, limit - k);
      k = limit;
   }
   st->next_k = k;
   return 0;
}

/* Output frames that pushing up to known_end can already fix. */
static uint64_t confirmed_limit(const lrs_sparse *st, uint64_t known_end)
{
   if (known_end <= st->halfspan)
      return 0;
   return ceil_mul_div(known_end - st->halfspan,
                       st->out_rate, st->in_rate);
}

/* ------------------------------------------------------------------ */
/* Public API. */

lrs_sparse *lrs_sparse_open(uint32_t input_rate,
                            uint32_t output_rate,
                            int channels)
{
   lrs_sparse *st;

   if (input_rate < 8000 || input_rate > 192000 ||
       output_rate < 8000 || output_rate > 192000)
      return NULL;
   if (channels != 1 && channels != 2 && channels != 6)
      return NULL;

   st = (lrs_sparse *)calloc(1, sizeof(*st));
   if (!st)
      return NULL;

   st->in_rate = input_rate;
   st->out_rate = output_rate;
   st->channels = channels;
   st->down = output_rate < input_rate;
   st->factor = (double)output_rate / (double)input_rate;
   st->dh = st->down ? st->factor * (double)Npc : (double)Npc;
   st->LpScl = st->down ? (float)st->factor : 1.0f;

   if (build_filter(st)) {
      free(st->Imp);
      free(st->ImpD);
      free(st);
      return NULL;
   }

   st->halfspan = compute_halfspan(st);
   st->tap_len = (int)(2 * st->halfspan + 3);
   st->center = (int)st->halfspan;
   st->taps = (float *)malloc((size_t)st->tap_len *
                              (size_t)channels * sizeof(float));
   if (!st->taps) {
      free(st->Imp);
      free(st->ImpD);
      free(st);
      return NULL;
   }

   return st;
}

int lrs_sparse_push(lrs_sparse *state,
                    uint64_t start,
                    uint64_t frames,
                    const float *pcm)
{
   lrs_sparse *st = state;
   uint64_t end, limit;

   if (!st)
      return LRS_SPARSE_ERROR;

   /* A zero-length span leaves the stream completely unchanged. */
   if (frames == 0)
      return LRS_SPARSE_OK;

   if (st->finished)
      return LRS_SPARSE_ERROR;
   if (!pcm)
      return LRS_SPARSE_ERROR;
   if (start < st->prev_end)
      return LRS_SPARSE_ERROR;

   end = start + frames;
   if (end < start)                    /* uint64_t overflow */
      return LRS_SPARSE_ERROR;

   if (add_chunk(st, start, frames, pcm))
      return LRS_SPARSE_ERROR;

   st->prev_end = end;

   limit = confirmed_limit(st, end);
   if (emit_events(st, limit))
      return LRS_SPARSE_ERROR;

   drop_old_chunks(st);
   return LRS_SPARSE_OK;
}

int lrs_sparse_pull(lrs_sparse *state,
                    float *out_pcm,
                    size_t capacity_frames,
                    uint64_t *out_start,
                    uint64_t *out_frames,
                    int *out_is_silence)
{
   lrs_sparse *st = state;
   sparse_event *e;

   if (!st || !out_start || !out_frames || !out_is_silence)
      return LRS_SPARSE_ERROR;
   if (capacity_frames > 0 && !out_pcm)
      return LRS_SPARSE_ERROR;

   *out_frames = 0;

   /* An empty pull is a no-op, not an error. */
   if (capacity_frames == 0)
      return LRS_SPARSE_OK;

   e = st->ev_head;
   if (!e)
      return LRS_SPARSE_OK;

   if (e->is_silence) {
      *out_start = e->start;
      *out_frames = e->frames;
      *out_is_silence = 1;
      st->ev_head = e->next;
      if (!st->ev_head)
         st->ev_tail = NULL;
      free(e);
      return LRS_SPARSE_EVENT;
   }

   {
      uint64_t remain = e->frames - e->offset;
      uint64_t n = remain < (uint64_t)capacity_frames ?
                   remain : (uint64_t)capacity_frames;
      size_t samples = (size_t)n * (size_t)st->channels;

      memcpy(out_pcm, e->pcm + e->offset * (size_t)st->channels,
             samples * sizeof(float));
      *out_start = e->start + e->offset;
      *out_frames = n;
      *out_is_silence = 0;
      e->offset += n;

      if (e->offset >= e->frames) {
         st->ev_head = e->next;
         if (!st->ev_head)
            st->ev_tail = NULL;
         free(e->pcm);
         free(e);
      }
      return LRS_SPARSE_EVENT;
   }
}

int lrs_sparse_finish(lrs_sparse *state, uint64_t total_frames)
{
   lrs_sparse *st = state;

   if (!st)
      return LRS_SPARSE_ERROR;

   if (st->finished) {
      /* finish() may be repeated with the same total; nothing else may
         change after the stream has ended. */
      if (total_frames != st->total_frames)
         return LRS_SPARSE_ERROR;
      return LRS_SPARSE_OK;
   }

   if (total_frames < st->prev_end)
      return LRS_SPARSE_ERROR;

   st->finished = 1;
   st->total_frames = total_frames;
   st->output_frames = ceil_mul_div(total_frames, st->out_rate, st->in_rate);

   /* Past the end of the timeline everything is a known zero, so the
      complete output is determined now. */
   if (emit_events(st, st->output_frames))
      return LRS_SPARSE_ERROR;

   drop_old_chunks(st);
   return LRS_SPARSE_OK;
}

uint64_t lrs_sparse_output_length(const lrs_sparse *state)
{
   if (!state || !state->finished)
      return 0;
   return state->output_frames;
}

void lrs_sparse_close(lrs_sparse *state)
{
   size_t i;

   if (!state)
      return;

   for (i = 0; i < state->chunk_count; i++)
      free(state->chunks[i].pcm);
   free(state->chunks);

   while (state->ev_head) {
      sparse_event *n = state->ev_head->next;
      free(state->ev_head->pcm);
      free(state->ev_head);
      state->ev_head = n;
   }

   free(state->taps);
   free(state->Imp);
   free(state->ImpD);
   free(state);
}

/**********************************************************************

  sparse_resample.h

  Sparse-stream interface for libresample.

  Some tools export multichannel audio as a list of "islands": short
  spans of PCM separated by silences that can be hours long.  Expanding
  those silences into PCM before resampling is wasteful.  This interface
  resamples the islands directly, emits silence as scene events instead
  of PCM, and never allocates or works in proportion to the length of
  the gaps.

  Dual-licensed as LGPL and BSD; see README.md and LICENSE* files.

**********************************************************************

  Model
  -----

  A source timeline is a sequence of non-overlapping spans given in
  strictly increasing source-frame order.  A span has a uint64_t start
  frame, a uint64_t frame count, and interleaved float PCM whose
  magnitude is at most 1.  Source frames not covered by a span are
  zero.  The timeline starts at frame 0; call lrs_sparse_finish() with
  the total number of source frames when the stream ends.

  Output is a sequence of consecutive events starting at output frame 0
  and together covering exactly

      ceil(total_source_frames * output_rate / input_rate)

  output frames.  An event is either a PCM span (interleaved floats) or a
  silence span (no PCM).  Output frame k corresponds to source
  coordinate k * input_rate / output_rate, evaluated by integer
  quotient and remainder so that large timelines keep exact phase.

  A timeline is accepted in as many non-empty pushes as the caller likes,
  and results may be pulled with any buffer capacity.  Splitting a span
  into adjacent pieces, changing the pull capacity, or pulling with a
  zero-capacity buffer never changes the concatenated result.  The
  resampling filter is continuous across span boundaries: spans that are
  closer together than the filter's support take part in the same
  convolution.

  Before the stream is finished only output that cannot be changed by
  later legal spans is reported; gaps are confirmed only when a later
  span is published, or by lrs_sparse_finish().  finish() may be called
  repeatedly; it does not duplicate events, and after the stream is
  finished non-empty pushes are rejected.

  Numerical reference
  -------------------

  The arithmetic matches libresample's high-quality mode exactly
  (Nmult = 35, Rolloff = 0.90, Beta = 6, coefficient interpolation off).
  For output frame k write q = k * input_rate / output_rate in integer
  arithmetic and r = k * input_rate % output_rate, then

      phi = (double)r / output_rate

  The left wing uses lrsFilterUp()/lrsFilterUD() phase phi on source
  sample q and the right wing uses phase 1 - phi on source sample q + 1,
  with the second wing starting at q + 1 and stepping outward.  For
  output_rate < input_rate the table step is factor * Npc and the sum is
  scaled by factor, where factor = output_rate / input_rate.  Samples
  beyond either end of the source timeline are zero.

  Example
  -------

      #include "sparse_resample.h"

      #include <math.h>
      #include <stdint.h>
      #include <stdio.h>

      static void drain(lrs_sparse *st, float *buf, size_t cap)
      {
         uint64_t start, count;
         int silence, e, i;

         for (;;) {
            e = lrs_sparse_pull(st, buf, cap, &start, &count, &silence);
            if (e == LRS_SPARSE_ERROR) return;
            if (count == 0) break;            // nothing more right now
            printf("event at %llu: %s %llu frames\n",
                   (unsigned long long)start,
                   silence ? "silence" : "pcm",
                   (unsigned long long)count);
            if (!silence)
               for (i = 0; i < (int)(count * 2); ++i)
                  printf(" %g", buf[i]);
         }
      }

      int main(void)
      {
         lrs_sparse *st;
         float buf[2048];
         int i;

         st = lrs_sparse_open(48000, 44100, 2);
         if (!st) return 1;

         // A 1000-frame island at source frame 100000; everything
         // before and after it is silent.
         for (i = 0; i < 1000; ++i) {
            buf[i * 2 + 0] = (float)( 0.25 * sin(i * 0.05));
            buf[i * 2 + 1] = (float)(-0.25 * sin(i * 0.05));
         }
         if (lrs_sparse_push(st, 100000, 1000, buf) != LRS_SPARSE_OK)
            return 1;
         drain(st, buf, 2048);

         // End the timeline at 200000 source frames; the remaining gap
         // and the trailing zero extension become pullable.
         if (lrs_sparse_finish(st, 200000) != LRS_SPARSE_OK)
            return 1;
         drain(st, buf, 2048);

         lrs_sparse_close(st);
         return 0;
      }

  (This example is also available as tests/sparse_example.c.)

**********************************************************************/

#ifndef LIBRESAMPLE_SPARSE_INCLUDED
#define LIBRESAMPLE_SPARSE_INCLUDED

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/* Status codes returned by the sparse entry points.
 *
 * lrs_sparse_open(), lrs_sparse_push() and lrs_sparse_finish() return
 * LRS_SPARSE_OK or LRS_SPARSE_ERROR.  lrs_sparse_pull() returns
 * LRS_SPARSE_EVENT when it stored an event in the caller's buffer,
 * LRS_SPARSE_OK when no event is available yet (zero capacity, or
 * nothing pending), and LRS_SPARSE_ERROR on an invalid call.  Check
 * *out_frames to tell an empty pull from a zero-length event:
 * *out_frames is 0 whenever no event was stored.
 */
enum {
   LRS_SPARSE_ERROR       = -1,  /* invalid argument or allocation    */
   LRS_SPARSE_OK          =  0,  /* success / no event available      */
   LRS_SPARSE_EVENT       =  1   /* pull stored one event             */
};

typedef struct lrs_sparse lrs_sparse;

/*
 * Open a sparse stream converter.
 *
 *   input_rate   integer source sample rate, 8000..192000 Hz
 *   output_rate  integer destination sample rate, 8000..192000 Hz
 *   channels     number of interleaved channels, 1, 2 or 6
 *
 * Returns NULL on an invalid rate, channel count or allocation failure.
 */
lrs_sparse *lrs_sparse_open(uint32_t input_rate,
                            uint32_t output_rate,
                            int channels);

/*
 * Publish one source span.
 *
 *   start   source frame of the first sample; strictly greater than the
 *           end of the previous span (0 for the first span)
 *   frames  number of frames in pcm; zero is accepted and leaves the
 *           stream state unchanged
 *   pcm     frames * channels interleaved floats, magnitude <= 1
 *
 * Data is copied or referenced only until this call returns; processing
 * is deferred to lrs_sparse_pull().  Returns LRS_SPARSE_OK on success
 * and LRS_SPARSE_ERROR for a NULL pcm with frames > 0, a
 * duplicate/overlapping start, a count beyond the total announced by an
 * earlier finish(), or any non-empty push after the stream finished.
 */
int lrs_sparse_push(lrs_sparse *state,
                    uint64_t start,
                    uint64_t frames,
                    const float *pcm);

/*
 * Pull the next output event.
 *
 * The caller's buffer holds capacity_frames frames of interleaved PCM.
 * On LRS_SPARSE_EVENT the event is described by *out_start and
 * *out_frames.  A PCM event (*out_is_silence == 0) has up to
 * capacity_frames of its frames copied to out_pcm; if the event is
 * longer than the buffer, the remaining frames are reported by later
 * calls at the following output start.  A silence event
 * (*out_is_silence != 0) carries no PCM and is never split by capacity.
 *
 * Returns LRS_SPARSE_EVENT when an event was stored, LRS_SPARSE_OK when
 * nothing can be produced yet (including capacity_frames == 0), and
 * LRS_SPARSE_ERROR on an invalid call (including out_pcm == NULL with
 * capacity_frames > 0).  *out_frames == 0 means nothing was stored:
 * push more spans or call lrs_sparse_finish().
 */
int lrs_sparse_pull(lrs_sparse *state,
                    float *out_pcm,
                    size_t capacity_frames,
                    uint64_t *out_start,
                    uint64_t *out_frames,
                    int *out_is_silence);

/*
 * Announce the end of the stream.
 *
 *   total_frames  total number of source frames in the timeline
 *
 * The remaining gaps and the trailing zero extension are confirmed and
 * become pullable.  Repeated calls must pass the same total; a
 * different total is an error.  Returns LRS_SPARSE_OK on success and
 * LRS_SPARSE_ERROR if the total is smaller than an already published
 * span or an argument is invalid.
 */
int lrs_sparse_finish(lrs_sparse *state, uint64_t total_frames);

/*
 * The full output length:
 *
 *   ceil(total_source_frames * output_rate / input_rate)
 *
 * Returns 0 for a NULL state or before finish() has been called.  Useful
 * for sizing a dense output buffer; the sparse API itself never builds
 * the whole timeline.
 */
uint64_t lrs_sparse_output_length(const lrs_sparse *state);

/* Free the converter.  NULL is accepted. */
void lrs_sparse_close(lrs_sparse *state);

#ifdef __cplusplus
} /* extern "C" */
#endif /* __cplusplus */

#endif /* LIBRESAMPLE_SPARSE_INCLUDED */

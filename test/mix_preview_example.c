/* See COPYING file for copyright and license details. */

/* Example for EBUR128_MODE_MIX_PREVIEW: feed the audio once, then query the
 * integrated loudness of several downmix matrices without replaying the
 * PCM. */

#include "ebur128.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SAMPLERATE 48000UL
#define CHANNELS 2
#define SECONDS 3

int main(void) {
  const size_t frames = SAMPLERATE * SECONDS;
  double* pcm;
  ebur128_state* st;
  size_t i;
  double loudness;

  /* row-major mix matrices: one row per output channel, one column per
   * physical input channel */
  double mono_sum[2] = { 1.0, 1.0 };        /* L + R to mono */
  double mono_diff[2] = { 1.0, -1.0 };      /* L - R to mono */
  double stereo_ident[4] = { 1.0, 0.0, 0.0, 1.0 };
  double w_mono[1] = { 1.0 };
  double w_stereo[2] = { 1.0, 1.0 };

  pcm = (double*) malloc(frames * CHANNELS * sizeof(double));
  if (!pcm) {
    return 1;
  }
  for (i = 0; i < frames; ++i) {
    double t = (double) i / (double) SAMPLERATE;
    double l = 0.3 * sin(2.0 * M_PI * 440.0 * t);
    /* partly correlated with the left channel, so that the (L+R) and
     * (L-R) downmixes differ */
    pcm[2 * i] = l;
    pcm[2 * i + 1] = 0.5 * l + 0.1 * sin(2.0 * M_PI * 554.0 * t + 0.3);
  }

  st = ebur128_init(CHANNELS, SAMPLERATE, EBUR128_MODE_MIX_PREVIEW);
  if (!st) {
    free(pcm);
    return 1;
  }
  /* optional: keep only the last 10 seconds of preview blocks */
  ebur128_set_max_history(st, 10000);

  if (ebur128_add_frames_double(st, pcm, frames) != EBUR128_SUCCESS) {
    ebur128_destroy(&st);
    free(pcm);
    return 1;
  }
  free(pcm);

  if (ebur128_loudness_mix(st, stereo_ident, 2, w_stereo, &loudness) ==
      EBUR128_SUCCESS) {
    printf("stereo passthrough: %.2f LUFS\n", loudness);
  }
  if (ebur128_loudness_mix(st, mono_sum, 1, w_mono, &loudness) ==
      EBUR128_SUCCESS) {
    printf("mono (L+R):       %.2f LUFS\n", loudness);
  }
  if (ebur128_loudness_mix(st, mono_diff, 1, w_mono, &loudness) ==
      EBUR128_SUCCESS) {
    printf("mono (L-R):       %.2f LUFS\n", loudness);
  }

  ebur128_destroy(&st);
  return 0;
}

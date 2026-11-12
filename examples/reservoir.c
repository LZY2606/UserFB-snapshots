/* examples/reservoir.c
 *
 * Encode a synthetic signal (silence, then noise, then silence) to MP3
 * with the cross-frame bit reservoir enabled, and write the result to
 * the file given as argument. The quiet frames bank main-data capacity
 * that the noisy frames can then spend.
 *
 *   usage: reservoir <output.mp3>
 */

#include "layer3.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BLOCKS 48 /* complete input blocks (>= 40) */

int main(int argc, char **argv) {
  shine_config_t cfg;
  shine_t s;
  FILE *out;
  int16_t inter[2 * SHINE_MAX_SAMPLES];
  uint32_t rng = 0x12345678u;
  int n, b, i;

  if (argc != 2) {
    fprintf(stderr, "usage: %s <output.mp3>\n", argv[0]);
    return 1;
  }

  memset(&cfg, 0, sizeof(cfg));
  shine_set_config_mpeg_defaults(&cfg.mpeg);
  cfg.wave.channels = PCM_STEREO;
  cfg.wave.samplerate = 48000;
  cfg.mpeg.bitr = 128;
  cfg.mpeg.mode = STEREO;

  s = shine_initialise_reservoir(&cfg, 511);
  if (!s) {
    fprintf(stderr, "could not initialise the encoder\n");
    return 1;
  }

  out = fopen(argv[1], "wb");
  if (!out) {
    perror(argv[1]);
    shine_close(s);
    return 1;
  }

  n = shine_samples_per_pass(s);
  for (b = 0; b < BLOCKS; b++) {
    int noisy = b >= 8 && b < 40;
    int written = 0;
    unsigned char *data;

    for (i = 0; i < n; i++) {
      rng = 1664525u * rng + 1013904223u;
      inter[2 * i] = noisy ? (int16_t)((int)(rng >> 18) - 8192) : 0;
      rng = 1664525u * rng + 1013904223u;
      inter[2 * i + 1] = noisy ? (int16_t)((int)(rng >> 18) - 8192) : 0;
    }

    /* With the reservoir enabled this may return zero bytes while
     * frames wait for their main data; the backlog stays bounded. */
    data = shine_encode_buffer_interleaved(s, inter, &written);
    if (written)
      fwrite(data, 1, (size_t)written, out);
  }

  /* Deliver the frames still queued. */
  {
    int written = 0;
    unsigned char *data = shine_flush(s, &written);
    if (written)
      fwrite(data, 1, (size_t)written, out);
  }

  fclose(out);
  shine_close(s);
  printf("reservoir: wrote %d blocks (%d samples each) to %s\n", BLOCKS, n,
         argv[1]);
  return 0;
}

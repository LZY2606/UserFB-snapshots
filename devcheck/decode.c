/* Decode real encoder output through the fixed, independent minimp3 source. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD
#include "vendor/minimp3.h"
int main(int argc, char **argv) {
  assert(argc == 2);
  FILE *f = fopen(argv[1], "rb"); assert(f);
  assert(!fseek(f, 0, SEEK_END)); long len = ftell(f); assert(len > 0);
  rewind(f); unsigned char *p = malloc((size_t)len); assert(p);
  assert(fread(p, 1, (size_t)len, f) == (size_t)len); fclose(f);
  mp3dec_t dec; mp3dec_init(&dec);
  int frames = 0, rate = 0, channels = 0; long samples = 0, pos = 0;
  uint64_t energy = 0;
  while (pos < len) {
    mp3dec_frame_info_t info; mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    int n = mp3dec_decode_frame(&dec, p + pos, (int)(len - pos), pcm, &info);
    assert(n > 0 && info.frame_bytes > 0 && info.frame_offset == 0);
    if (rate) assert(rate == info.hz && channels == info.channels);
    rate = info.hz; channels = info.channels;
    for (int j = 0; j < n * channels; ++j)
      energy += (uint64_t)((int64_t)pcm[j] * pcm[j]);
    samples += n; pos += info.frame_bytes; ++frames;
  }
  assert(frames > 10 && energy > 1000000 && len == pos);
  printf("SHINE_OLD_CLI_OK frames=%d rate=%d channels=%d samples=%ld energy=%llu trailing=%ld\n",
         frames, rate, channels, samples, (unsigned long long)energy, len - pos);
  free(p); return 0;
}

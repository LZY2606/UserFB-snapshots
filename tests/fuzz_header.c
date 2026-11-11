/*
 * libFuzzer entry point for tar header parsing.
 *
 * The input bytes are treated purely as in-memory header data: nothing
 * is extracted and no external path is ever written. Build with
 * -DMTAR_FUZZ_STANDALONE for a small driver that replays files given on
 * the command line (or a built-in smoke corpus when none are given).
 */

#include "microtar.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct {
  const uint8_t *data;
  size_t size;
} fuzzbuf_t;

static int fuzz_read(mtar_t *tar, void *p, unsigned n) {
  fuzzbuf_t *fb = (fuzzbuf_t*) tar->stream;
  if (tar->pos > fb->size || n > fb->size - tar->pos) {
    return MTAR_EREADFAIL;
  }
  memcpy(p, fb->data + tar->pos, n);
  return 0;
}

static int fuzz_seek(mtar_t *tar, unsigned pos) {
  (void) tar;
  (void) pos;
  return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  fuzzbuf_t fb;
  mtar_t tar;
  mtar_header_t h;
  unsigned i;

  fb.data = data;
  fb.size = size;
  memset(&tar, 0, sizeof tar);
  tar.read = fuzz_read;
  tar.seek = fuzz_seek;
  tar.stream = &fb;

  /* Walk a bounded chain of headers so multi-record inputs are explored
   * without unbounded looping on crafted sizes */
  for (i = 0; i < 8; i++) {
    if (mtar_read_header(&tar, &h) != MTAR_ESUCCESS) {
      break;
    }
    if (mtar_next(&tar) != MTAR_ESUCCESS) {
      break;
    }
  }
  return 0;
}

#ifdef MTAR_FUZZ_STANDALONE

#include <stdio.h>
#include <stdlib.h>

static void run_bytes(const uint8_t *data, size_t size) {
  LLVMFuzzerTestOneInput(data, size);
}

static void run_file(const char *path) {
  FILE *f = fopen(path, "rb");
  uint8_t *buf;
  long n;
  if (!f) {
    perror(path);
    exit(1);
  }
  fseek(f, 0, SEEK_END);
  n = ftell(f);
  fseek(f, 0, SEEK_SET);
  buf = (uint8_t*) malloc(n ? (size_t) n : 1);
  if (n && fread(buf, 1, (size_t) n, f) != (size_t) n) {
    perror(path);
    exit(1);
  }
  fclose(f);
  run_bytes(buf, (size_t) n);
  free(buf);
}

int main(int argc, char **argv) {
  int i;
  if (argc > 1) {
    for (i = 1; i < argc; i++) {
      run_file(argv[i]);
    }
    return 0;
  }
  /* Built-in smoke corpus: empty input, null record, the original crash
   * layout and deterministic pseudo-random blocks of assorted sizes */
  {
    static const size_t sizes[] = { 0, 1, 511, 512, 513, 1024, 4096 };
    uint8_t buf[4096];
    uint32_t s = 0x12345678u;
    size_t k, j;
    run_bytes(buf, 0);
    memset(buf, 0, sizeof buf);
    run_bytes(buf, 512);
    memset(buf, 0, sizeof buf);
    memcpy(buf, "ok", 2);
    memcpy(buf + 100, "0000644", 7);
    memcpy(buf + 108, "0000000", 7);
    memcpy(buf + 116, "0000000", 7);
    memcpy(buf + 124, "00000000000", 11);
    memcpy(buf + 136, "00000000000", 11);
    buf[156] = '0';
    memset(buf + 157, 'L', 512 - 157);
    memset(buf + 148, ' ', 8);
    {
      unsigned sum = 0;
      for (j = 0; j < 512; j++) {
        sum += buf[j];
      }
      snprintf((char*) buf + 148, 8, "%06o", sum);
      buf[155] = ' ';
    }
    run_bytes(buf, 512);
    for (k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
      for (j = 0; j < sizes[k]; j++) {
        s = s * 1664525u + 1013904223u;
        buf[j] = (uint8_t) (s >> 24);
      }
      run_bytes(buf, sizes[k]);
    }
  }
  puts("fuzz smoke PASS");
  return 0;
}

#endif /* MTAR_FUZZ_STANDALONE */

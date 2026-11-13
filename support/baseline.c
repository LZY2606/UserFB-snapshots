/* Starting-point witness, not a streaming decoder or target regression suite. */
#include "lodepng.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if(!(x)) { fprintf(stderr, "baseline line %d\n", __LINE__); return 1; } } while(0)

static int run(unsigned btype) {
  const unsigned w = 33, h = 37;
  const size_t rawsize = (size_t)w * h * 4;
  unsigned char* raw = (unsigned char*)malloc(rawsize);
  unsigned char* png = 0;
  unsigned char* out = 0;
  unsigned char* packed = 0;
  unsigned char* inflated = 0;
  unsigned char* split = 0;
  unsigned char filters[37];
  unsigned rw, rh, err;
  size_t n = 0, p, packedsize = 0, inflatedsize = 0, splitsize = 33, i;
  LodePNGState state;
  CHECK(raw);
  for(i = 0; i < rawsize; ++i) raw[i] = (unsigned char)((i % 132) * 11 + (i / 132) % 7);
  for(i = 0; i < h; ++i) filters[i] = (unsigned char)(i % 5);
  lodepng_state_init(&state);
  state.encoder.auto_convert = 0;
  state.encoder.filter_palette_zero = 0;
  state.encoder.filter_strategy = LFS_PREDEFINED;
  state.encoder.predefined_filters = filters;
  state.encoder.zlibsettings.btype = btype;
  err = lodepng_encode(&png, &n, raw, w, h, &state);
  CHECK(err == 0);
  CHECK(lodepng_decode32(&out, &rw, &rh, png, n) == 0);
  CHECK(rw == w && rh == h && memcmp(out, raw, rawsize) == 0);
  free(out); out = 0;
  for(p = 33; p + 12 <= n;) {
    unsigned len = lodepng_chunk_length(png + p);
    CHECK((size_t)len <= n - p - 12);
    if(lodepng_chunk_type_equals(png + p, "IDAT")) {
      unsigned char* next = (unsigned char*)realloc(packed, packedsize + len);
      CHECK(next); packed = next;
      memcpy(packed + packedsize, png + p + 8, len); packedsize += len;
    }
    p += len + 12;
  }
  CHECK(packedsize > 6 && ((packed[2] >> 1) & 3) == btype);
  CHECK(lodepng_zlib_decompress(&inflated, &inflatedsize, packed, packedsize,
                               &lodepng_default_decompress_settings) == 0);
  CHECK(inflatedsize == (size_t)h * (w * 4 + 1));
  for(i = 0; i < h; ++i) CHECK(inflated[i * (w * 4 + 1)] == filters[i]);
  split = (unsigned char*)malloc(33); CHECK(split); memcpy(split, png, 33);
  for(p = 0; p < packedsize;) {
    size_t amount = packedsize - p;
    if(amount > 17) amount = 17;
    CHECK(lodepng_chunk_create(&split, &splitsize, amount, "IDAT", packed + p) == 0);
    p += amount;
  }
  CHECK(lodepng_chunk_create(&split, &splitsize, 0, "IEND", 0) == 0);
  CHECK(lodepng_decode32(&out, &rw, &rh, split, splitsize) == 0);
  CHECK(rw == w && rh == h && memcmp(out, raw, rawsize) == 0);
  printf("LODEPNG_BASELINE btype=%u rgba=33x37 filters=5 split_idat=ok pixels=ok\n", btype);
  free(out); out = 0;
  err = lodepng_decode32(&out, &rw, &rh, split, splitsize - 12);
  CHECK(err != 0);
  printf("LODEPNG_WHOLE_INPUT btype=%u missing_iend_error=%u\n", btype, err);
  free(out); free(raw); free(png); free(packed); free(inflated); free(split);
  lodepng_state_cleanup(&state);
  return 0;
}

int main(void) {
  unsigned t;
  for(t = 0; t < 3; ++t) if(run(t)) return 1;
  puts("LODEPNG_BASELINE_COMPLETE variants=3");
  return 0;
}

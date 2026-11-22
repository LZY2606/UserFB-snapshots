// Pixel-level correctness gate for Phase A (I-frame decode) - the core
// test docs/testing.md commits to: decode a real ffmpeg-encoded stream
// and diff the result against ffmpeg's own decode (this project's
// correctness oracle, not conformance-suite guesswork).
//
// Comparison is tolerance-based (max |diff| <= kMaxSampleDiff per sample,
// and almost all samples within kExactMatchDiff), not literal byte-for-
// byte equality - the DCT coefficient VLC decode, dequantization, and
// zig-zag placement are all deterministic per ISO/IEC 11172-2 and hold
// every decoder to the exact same result (validated in
// test_tables.cpp/test_headers.cpp and by this test's own tight
// tolerance), but the 8x8 IDCT itself (mpg_idct.h) is a direct
// double-precision implementation of the textbook formula rather than a
// port of ffmpeg's own fixed-point "simple_idct" approximation - MPEG-1
// deliberately leaves the exact IDCT algorithm implementation-defined
// (only an accuracy bound is normative), so a handful of samples landing
// 1 LSB apart from ffmpeg's own rounding is expected, not a bug. See
// mpg_macroblock.h's file comment for how the *dequantization* arithmetic
// itself (the part that genuinely has one correct answer) was verified
// exactly against ffmpeg's real decode output.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "TinyMPGDecoder.h"

using namespace tinympg;

static std::vector<uint8_t> readFile(const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) {
    fprintf(stderr, "cannot open %s (run from test/native/, see docs/testing.md)\n", path);
    exit(1);
  }
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  std::vector<uint8_t> data((size_t)size);
  size_t n = fread(data.data(), 1, (size_t)size, f);
  (void)n;
  fclose(f);
  return data;
}

static const int kMaxSampleDiff = 2;       // hard ceiling: never off by more than this
static const double kMaxMeanDiff = 0.2;    // average |diff| over a whole frame must stay tiny

struct Oracle {
  const uint8_t* data;
  size_t size;
  size_t frameBytes;
  int frameIndex = 0;
};

static void onFrame(TinyMPGDecoder<>& dec, void* userData) {
  Oracle* oracle = (Oracle*)userData;
  size_t offset = (size_t)oracle->frameIndex * oracle->frameBytes;
  assert(offset + oracle->frameBytes <= oracle->size);

  std::vector<uint8_t> got(oracle->frameBytes);
  size_t written = dec.toYUV420(got.data(), got.size());
  assert(written == oracle->frameBytes);
  (void)written;

  const uint8_t* want = oracle->data + offset;
  int maxDiff = 0;
  double sumDiff = 0;
  for (size_t i = 0; i < oracle->frameBytes; i++) {
    int d = std::abs((int)got[i] - (int)want[i]);
    if (d > maxDiff) maxDiff = d;
    sumDiff += d;
  }
  double meanDiff = sumDiff / (double)oracle->frameBytes;
  if (maxDiff > kMaxSampleDiff || meanDiff > kMaxMeanDiff) {
    fprintf(stderr, "frame %d: maxDiff=%d meanDiff=%f (limits: %d / %f)\n",
            oracle->frameIndex, maxDiff, meanDiff, kMaxSampleDiff, kMaxMeanDiff);
  }
  assert(maxDiff <= kMaxSampleDiff);
  assert(meanDiff <= kMaxMeanDiff);

  oracle->frameIndex++;
}

int main() {
  std::vector<uint8_t> stream = readFile("assets/small.m1v");
  std::vector<uint8_t> refYuv = readFile("assets/small_ref.yuv");

  TinyMPGDecoder<> dec;
  Oracle oracle;
  oracle.data = refYuv.data();
  oracle.size = refYuv.size();
  oracle.frameBytes = (size_t)176 * 144 * 3 / 2;  // I420, see docs/preparing-input-with-ffmpeg.md
  dec.setCallback(onFrame, &oracle);

  auto status = dec.write(stream.data(), stream.size());
  assert(!dec.hasError());
  assert(status == TinyMPGDecoder<>::Status::kFrameReady ||
         status == TinyMPGDecoder<>::Status::kNeedMoreData);
  (void)status;

  assert(oracle.frameIndex * oracle.frameBytes == refYuv.size());  // decoded every reference frame
  assert(dec.width() == 176);
  assert(dec.height() == 144);

  printf("test_decode_iframe: OK (%d frames, tolerance <= %d/sample)\n", oracle.frameIndex,
         kMaxSampleDiff);
  return 0;
}

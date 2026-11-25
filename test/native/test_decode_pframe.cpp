// Pixel-level correctness gate for Phase B (P-frame decode, see
// docs/scope.md) - decodes a real ffmpeg-encoded stream with one I-picture
// followed by nine P-pictures and checks the result against ffmpeg's own
// decode, the same tolerance-based approach test_decode_iframe.cpp uses
// (see that file's comment for why byte-exact equality isn't the bar: the
// IDCT is only held to an accuracy bound, not a specific algorithm).
//
// The tolerance here is a little looser than test_decode_iframe.cpp's:
// P-pictures predict from the *previously decoded* picture, so any
// sub-LSB IDCT rounding difference from ffmpeg's own decode compounds
// slightly, frame over frame, across a GOP - still nowhere near enough to
// indicate an actual bug (mpg_macroblock.h's file comment covers how the
// dequantization arithmetic itself, and mpg_motion.h's half-pel averaging,
// were verified exactly against ffmpeg's real decode output, not just
// this aggregate statistic).
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

static const int kMaxSampleDiff = 4;     // hard ceiling: never off by more than this
static const double kMaxMeanDiff = 0.3;  // average |diff| over a whole frame must stay tiny

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
  std::vector<uint8_t> stream = readFile("assets/pframe.m1v");
  std::vector<uint8_t> refYuv = readFile("assets/pframe_ref.yuv");

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
  assert(oracle.frameIndex == 10);  // 1 I-picture + 9 P-pictures, see docs/preparing-input-with-ffmpeg.md
  assert(dec.width() == 176);
  assert(dec.height() == 144);

  printf("test_decode_pframe: OK (%d frames, tolerance <= %d/sample)\n", oracle.frameIndex,
         kMaxSampleDiff);
  return 0;
}

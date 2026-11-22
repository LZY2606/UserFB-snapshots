// Pixel-level correctness gate for Phase C (B-frame decode, see
// docs/scope.md) - decodes a real ffmpeg-encoded stream with a repeating
// I,P,B,B GOP structure (display order: I,B,B,P,B,B,P,B,B,I) and checks
// the result against ffmpeg's own decode, the same tolerance-based
// approach test_decode_iframe.cpp/test_decode_pframe.cpp use (see
// test_decode_iframe.cpp's comment for why byte-exact equality isn't the
// bar: the IDCT is only held to an accuracy bound, not a specific
// algorithm).
//
// This is also the test that exercises output reordering (mpg_decoder.h's
// file comment) end to end: TinyMPGDecoder's frame callback fires once
// per *display-order* picture, not decode order, and the assertions below
// (frame count, dimensions) would fail if that reordering - or the
// forward/backward reference-picture bookkeeping and bidirectional
// averaging Phase C adds - were wrong in a way that lost or duplicated a
// picture, even before looking at any pixel data.
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
  std::vector<uint8_t> stream = readFile("assets/bframe.m1v");
  std::vector<uint8_t> refYuv = readFile("assets/bframe_ref.yuv");

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
  assert(oracle.frameIndex == 10);  // display order: I,B,B,P,B,B,P,B,B,I - see docs/preparing-input-with-ffmpeg.md
  assert(dec.width() == 176);
  assert(dec.height() == 144);

  printf("test_decode_bframe: OK (%d frames, tolerance <= %d/sample)\n", oracle.frameIndex,
         kMaxSampleDiff);
  return 0;
}

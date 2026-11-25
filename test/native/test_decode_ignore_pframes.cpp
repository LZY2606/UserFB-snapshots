// Correctness gate for TinyMPGDecoder::setIgnorePFrames() (decoder/
// mpg_decoder.h) - decodes the same 1 I-picture + 9 P-pictures stream
// test_decode_pframe.cpp uses, but with P-frames set to be ignored, and
// checks that: (1) only the I-picture is ever delivered to the frame
// callback - none of the 9 P-pictures produce a callback invocation or an
// error status, just silent discarding, and (2) the one picture that *is*
// delivered still matches ffmpeg's own reference decode of that same
// I-picture exactly as well as the non-ignoring path does (same tolerance
// as test_decode_pframe.cpp - ignoring P-frames must not perturb how the
// I-picture itself decodes).
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

static const int kMaxSampleDiff = 4;     // same tolerance as test_decode_pframe.cpp
static const double kMaxMeanDiff = 0.3;

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
  assert(maxDiff <= kMaxSampleDiff);
  assert(meanDiff <= kMaxMeanDiff);

  oracle->frameIndex++;
}

int main() {
  std::vector<uint8_t> stream = readFile("assets/pframe.m1v");
  std::vector<uint8_t> refYuv = readFile("assets/pframe_ref.yuv");

  TinyMPGDecoder<> dec;
  assert(!dec.ignorePFrames());  // off by default
  dec.setIgnorePFrames(true);
  assert(dec.ignorePFrames());

  Oracle oracle;
  oracle.data = refYuv.data();
  oracle.size = refYuv.size();
  oracle.frameBytes = (size_t)176 * 144 * 3 / 2;  // I420, matches refYuv's frame 0 (the I-picture)
  dec.setCallback(onFrame, &oracle);

  auto status = dec.write(stream.data(), stream.size());
  assert(!dec.hasError());  // ignored P-pictures must never surface as an error
  assert(status == TinyMPGDecoder<>::Status::kFrameReady ||
         status == TinyMPGDecoder<>::Status::kNeedMoreData);
  (void)status;

  assert(oracle.frameIndex == 1);  // only the I-picture - all 9 P-pictures silently discarded
  assert(dec.width() == 176);
  assert(dec.height() == 144);

  printf("test_decode_ignore_pframes: OK (%d frame(s) delivered, tolerance <= %d/sample)\n",
         oracle.frameIndex, kMaxSampleDiff);
  return 0;
}

// Regression gate for a real bug found on real hardware (see docs/scope.md
// and the commit that added this test): calling write() again after
// inputExhausted() was already true - a normal, documented use case
// (setInput()'s own comment: "Call setInput() again once inputExhausted()
// is true") - used to re-deliver the *previously already-delivered* last
// picture one extra time before correctly resuming, because
// Decoder::finalizePicture()'s boundary-triggered "release refFuture_'s
// content" path didn't check refFutureShown_ the way the end-of-input
// flush path already did. Concretely: 30 repeated write() calls of the
// same 10-I-picture stream used to deliver 119 pictures over 3 reps'
// worth of calls (3 the first time, 4 every time after - one stale
// redelivery each time), not the correct 30*10 = 300. Found via
// TinyMPGDecoder's DecodeFromProgmem example benchmarking against a real
// ESP32, not by inspection - the same "verify against real behavior, not
// just what looks right" discipline every dequantization shift in this
// codebase was calibrated with. Fixed in decoder/mpg_decoder.h.
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

int main() {
  std::vector<uint8_t> stream = readFile("assets/small.m1v");  // 10 I-pictures, see test_decode_iframe.cpp

  TinyMPGDecoder<> dec;
  int frameCount = 0;
  dec.setCallback([](TinyMPGDecoder<>&, void* userData) { (*(int*)userData)++; }, &frameCount);

  static const int kReps = 30;
  for (int rep = 0; rep < kReps; rep++) {
    auto status = dec.write(stream.data(), stream.size());
    assert(!dec.hasError());
    assert(status == TinyMPGDecoder<>::Status::kFrameReady ||
           status == TinyMPGDecoder<>::Status::kNeedMoreData);
    (void)status;
  }

  // Exactly 10 pictures delivered per repetition, no stale redelivery and
  // no picture ever dropped - see this file's own top comment for the bug
  // this pins down.
  if (frameCount != kReps * 10) {
    fprintf(stderr, "frameCount=%d, expected %d\n", frameCount, kReps * 10);
  }
  assert(frameCount == kReps * 10);

  printf("test_decode_resumed_input: OK (%d frames over %d repetitions)\n", frameCount, kReps);
  return 0;
}

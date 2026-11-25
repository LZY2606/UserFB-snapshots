// Correctness gate for Decoder::setInput()'s `isLastChunk` streaming mode
// (decoder/mpg_decoder.h's "Streaming/incremental input" section) - found
// necessary via real-world testing: arduino-audio-tools' DemuxerMPG feeds
// TinyMPGDecoder::write() with chunks that don't align to start-code-unit
// boundaries (its own PES packetization, not picture-aligned), which the
// original isLastChunk=true-only contract handled by either processing a
// truncated unit as if complete or silently dropping its tail - both wrong,
// and (found empirically) especially damaging combined with
// setIgnorePFrames(true), which relies on cleanly parsing each picture's
// header to decide whether to skip it.
//
// Feeds the same 1 I-picture + 9 P-pictures stream test_decode_pframe.cpp/
// test_decode_ignore_pframes.cpp use, but in small, deliberately
// non-unit-aligned chunks (isLastChunk=false for every chunk but the
// last), and checks the result exactly matches a monolithic single-buffer
// decode (isLastChunk=true, the original/default behavior) - both with
// ignorePFrames off and on.
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

struct Capture {
  std::vector<std::vector<uint8_t>> frames;  // each I420-packed, toYUV420()
  int width = 0, height = 0;
};

static void onFrame(TinyMPGDecoder<>& dec, void* userData) {
  Capture* cap = (Capture*)userData;
  cap->width = dec.width();
  cap->height = dec.height();
  size_t needed = (size_t)cap->width * cap->height * 3 / 2;
  std::vector<uint8_t> yuv(needed);
  size_t written = dec.toYUV420(yuv.data(), yuv.size());
  assert(written == needed);
  (void)written;
  cap->frames.push_back(std::move(yuv));
}

// Decodes `stream` fed in `chunkSize`-byte pieces (isLastChunk=false for
// every piece but the last) if `chunkSize` > 0, or as one monolithic
// isLastChunk=true call if `chunkSize` == 0 (the baseline).
static Capture decode(const std::vector<uint8_t>& stream, bool ignoreP, size_t chunkSize) {
  Capture cap;
  TinyMPGDecoder<> dec;
  dec.setIgnorePFrames(ignoreP);
  dec.setCallback(onFrame, &cap);

  if (chunkSize == 0) {
    auto status = dec.write(stream.data(), stream.size());
    assert(!dec.hasError());
    (void)status;
    return cap;
  }

  size_t pos = 0;
  while (pos < stream.size()) {
    size_t n = std::min(chunkSize, stream.size() - pos);
    bool isLast = (pos + n) >= stream.size();
    auto status = dec.write(stream.data() + pos, n, isLast);
    assert(!dec.hasError());
    (void)status;
    pos += n;
  }
  return cap;
}

static void checkIdentical(const Capture& streamed, const Capture& baseline, const char* label) {
  if (streamed.frames.size() != baseline.frames.size()) {
    fprintf(stderr, "%s: frame count mismatch: streamed=%zu baseline=%zu\n", label,
            streamed.frames.size(), baseline.frames.size());
  }
  assert(streamed.frames.size() == baseline.frames.size());
  assert(streamed.width == baseline.width && streamed.height == baseline.height);
  for (size_t i = 0; i < streamed.frames.size(); i++) {
    if (streamed.frames[i] != baseline.frames[i]) {
      fprintf(stderr, "%s: frame %zu differs between streamed and baseline decode\n", label, i);
    }
    assert(streamed.frames[i] == baseline.frames[i]);  // byte-exact - same decoder, same bits, just re-chunked
  }
}

int main() {
  std::vector<uint8_t> stream = readFile("assets/pframe.m1v");  // 1 I + 9 P, see test_decode_pframe.cpp

  // A handful of deliberately awkward, non-unit-aligned chunk sizes.
  const size_t kChunkSizes[] = {17, 37, 100, 401};

  for (bool ignoreP : {false, true}) {
    Capture baseline = decode(stream, ignoreP, /*chunkSize=*/0);
    assert(baseline.frames.size() == (ignoreP ? 1u : 10u));  // 10 pictures total, or just the 1 I-picture if ignoring P

    for (size_t chunkSize : kChunkSizes) {
      Capture streamed = decode(stream, ignoreP, chunkSize);
      checkIdentical(streamed, baseline, ignoreP ? "ignorePFrames=true" : "ignorePFrames=false");
    }
  }

  printf("test_decode_streaming_input: OK (%zu chunk size(s), both ignorePFrames settings)\n",
         sizeof(kChunkSizes) / sizeof(kChunkSizes[0]));
  return 0;
}

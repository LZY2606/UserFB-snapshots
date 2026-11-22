// Correctness gate for the intra-only encoder (Phase 1, see
// docs/scope.md's Encoder section): encodes a real decoded video frame
// (assets/small_ref.yuv's first frame - ffmpeg's own decode of
// assets/small.m1v, already this project's correctness oracle for the
// decoder tests, reused here as encoder *input*) with TinyMPGEncoder,
// then decodes the result back with TinyMPGDecoder (this project's own,
// already ffmpeg-verified decoder - see test_decode_iframe.cpp) and
// checks the round-trip against the original source.
//
// Unlike the decoder tests' tolerance (maxDiff <= 2, meanDiff <= 0.2),
// this one is deliberately much looser (see kMaxSampleDiff/kMinPsnrDb
// below) - the decoder tests only have to absorb this project's own
// double-precision IDCT differing from ffmpeg's fixed-point one, a tiny
// implementation detail; this test additionally goes through *real, lossy
// quantization* (the whole point of a video encoder - see
// quantizeAc()/quantizeDc(), encoder/mpg_macroblock_encode.h), so a
// meaningfully larger, QP-dependent difference from the source is
// expected and correct, not a bug - real MPEG-1 encoders show the same
// shape of result at a comparable quantizer_scale. PSNR (not just
// max/mean absolute difference) is checked for that reason: a PSNR floor
// is a distortion metric that actually reflects "does this still look
// like a reasonable, working encoder" the way a raw sample-difference
// ceiling alone can't - this specific floor (see kMinPsnrDb) was set from
// this test's own measured ~37.5dB at qscale=8, with headroom, not
// picked in the abstract.
//
// Development-time verification (not re-run here, but what actually
// established this encoder produces *real*, spec-conformant MPEG-1, not
// just something this project's own decoder happens to accept): the
// encoder's output was probed with `ffprobe` (correctly identified as
// mpeg1video, right width/height/pix_fmt) and decoded with real `ffmpeg`
// at several QPs - including qscale=1 (near-lossless) and a deliberately
// high-frequency/noisy synthetic pattern to exercise the AC escape-coding
// path - with `ffmpeg`'s own decode matching TinyMPGDecoder's decode of
// the same bitstream exactly, confirming the round-trip this test checks
// isn't just this project's two halves agreeing with each other in a way
// a real player wouldn't. See docs/testing.md for the exact commands.
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "TinyMPGDecoder.h"
#include "TinyMPGEncoder.h"

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

static const int kQscale = 8;
static const int kMaxSampleDiff = 60;    // hard ceiling: never off by more than this
static const double kMinPsnrDb = 33.0;   // measured ~37.5dB at kQscale on this source frame

struct Oracle {
  const uint8_t* data;
  size_t size;
  int frameIndex = 0;
};

static void onFrame(TinyMPGDecoder<>& dec, void* userData) {
  Oracle* oracle = (Oracle*)userData;

  std::vector<uint8_t> got(oracle->size);
  size_t written = dec.toYUV420(got.data(), got.size());
  assert(written == oracle->size);
  (void)written;

  int maxDiff = 0;
  double sse = 0;
  for (size_t i = 0; i < oracle->size; i++) {
    int d = std::abs((int)got[i] - (int)oracle->data[i]);
    if (d > maxDiff) maxDiff = d;
    sse += (double)d * (double)d;
  }
  double mse = sse / (double)oracle->size;
  double psnr = mse > 0 ? 10.0 * std::log10(255.0 * 255.0 / mse) : 999.0;
  if (maxDiff > kMaxSampleDiff || psnr < kMinPsnrDb) {
    fprintf(stderr, "round-trip: maxDiff=%d psnr=%f dB (limits: %d / >=%f dB)\n", maxDiff, psnr,
            kMaxSampleDiff, kMinPsnrDb);
  }
  assert(maxDiff <= kMaxSampleDiff);
  assert(psnr >= kMinPsnrDb);
  oracle->frameIndex++;
}

int main() {
  const int width = 176, height = 144;
  const size_t frameBytes = (size_t)width * height * 3 / 2;  // I420

  std::vector<uint8_t> all = readFile("assets/small_ref.yuv");
  assert(all.size() >= frameBytes);
  std::vector<uint8_t> src(all.begin(), all.begin() + (long)frameBytes);

  const uint8_t* srcY = src.data();
  const uint8_t* srcU = srcY + (size_t)width * height;
  const uint8_t* srcV = srcU + (size_t)(width / 2) * (height / 2);

  TinyMPGEncoder<> enc;
  assert(enc.setSize(width, height));
  assert(enc.setQp(kQscale));

  std::vector<uint8_t> bitstream(200000);
  size_t n = enc.encodeFrame(srcY, srcU, srcV, bitstream.data(), bitstream.size());
  assert(n > 0);
  assert(n < bitstream.size());

  TinyMPGDecoder<> dec;
  Oracle oracle;
  oracle.data = src.data();
  oracle.size = frameBytes;
  dec.setCallback(onFrame, &oracle);

  auto status = dec.write(bitstream.data(), n);
  assert(!dec.hasError());
  assert(status == TinyMPGDecoder<>::Status::kFrameReady ||
         status == TinyMPGDecoder<>::Status::kNeedMoreData);
  (void)status;

  assert(oracle.frameIndex == 1);  // exactly one self-contained I-picture
  assert(dec.width() == width);
  assert(dec.height() == height);

  printf("test_encode_iframe: OK (%zu bytes for a %dx%d frame at qscale=%d)\n", n, width, height,
         kQscale);
  return 0;
}

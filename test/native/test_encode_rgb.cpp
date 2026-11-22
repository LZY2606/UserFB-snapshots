// Correctness test for the encoder's packed-source convenience overloads
// (encodeFrameRgb888()/encodeFrameRgb666()/encodeFrameRgb565()/
// encodeFrameYuv422(), and setPackedStride()) - see TinyMPGEncoder.h and
// encoder/mpg_color_convert.h. Each overload converts its packed source
// format to YUV 4:2:0 internally before encoding through the exact same
// encodePicture() path test_encode_iframe.cpp exercises directly, so this
// test focuses on what's specific to the conversion step itself: that
// each overload (a) produces a valid, decodable bitstream at all, and (b)
// round-trips to approximately the right colors - not on re-verifying
// the encoder's own DCT/quantization correctness (test_encode_iframe.cpp
// already does that, against a real decoded video frame). A small
// synthetic gradient image (not real video content) is enough for that -
// deterministic and free of any external asset dependency.
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include "TinyMPGDecoder.h"
#include "TinyMPGEncoder.h"

using namespace tinympg;

static const int kWidth = 32, kHeight = 32;
static const int kQscale = 8;
static const int kMaxSampleDiff = 60;  // same generous ceiling as test_encode_iframe.cpp

// Builds a simple R/G/B gradient test image, not real video content -
// deterministic and dependency-free (see file comment).
static std::vector<uint8_t> makeRgb888() {
  std::vector<uint8_t> rgb((size_t)kWidth * kHeight * 3);
  for (int y = 0; y < kHeight; y++) {
    for (int x = 0; x < kWidth; x++) {
      uint8_t* p = &rgb[((size_t)y * kWidth + x) * 3];
      p[0] = (uint8_t)(x * 255 / (kWidth - 1));
      p[1] = (uint8_t)(y * 255 / (kHeight - 1));
      p[2] = 128;
    }
  }
  return rgb;
}

struct Check {
  const uint8_t* expectedRgb;  // kWidth*kHeight*3, R/G/B
  bool ok = false;
};

static void onFrame(TinyMPGDecoder<>& dec, void* userData) {
  Check* check = (Check*)userData;
  std::vector<uint8_t> got(dec.width() * dec.height() * 3);
  size_t written = dec.toRGB888(got.data(), got.size());
  assert(written == got.size());

  int maxDiff = 0;
  for (size_t i = 0; i < got.size(); i++) {
    int d = std::abs((int)got[i] - (int)check->expectedRgb[i]);
    if (d > maxDiff) maxDiff = d;
  }
  if (maxDiff > kMaxSampleDiff) {
    fprintf(stderr, "round-trip maxDiff=%d (limit %d)\n", maxDiff, kMaxSampleDiff);
  }
  assert(maxDiff <= kMaxSampleDiff);
  check->ok = true;
}

static void decodeAndCheck(const uint8_t* bitstream, size_t n, const uint8_t* expectedRgb) {
  assert(n > 0);
  TinyMPGDecoder<> dec;
  Check check{expectedRgb};
  dec.setCallback(onFrame, &check);
  dec.write(bitstream, n);
  assert(check.ok);
}

int main() {
  std::vector<uint8_t> rgb888 = makeRgb888();
  std::vector<uint8_t> bitstream(65536);

  TinyMPGEncoder<> enc;
  assert(enc.setSize(kWidth, kHeight));
  assert(enc.setQp(kQscale));

  // RGB888, tightly packed.
  {
    size_t n = enc.encodeFrameRgb888(rgb888.data(), bitstream.data(), bitstream.size());
    assert(n > 0);
    decodeAndCheck(bitstream.data(), n, rgb888.data());
  }

  // RGB888 with a padded (non-tightly-packed) stride, via setPackedStride().
  {
    int padded = kWidth * 3 + 16;
    std::vector<uint8_t> rgbPadded((size_t)padded * kHeight, 0xAA);
    for (int y = 0; y < kHeight; y++) {
      memcpy(&rgbPadded[(size_t)y * padded], &rgb888[(size_t)y * kWidth * 3], kWidth * 3);
    }
    enc.setPackedStride(padded);
    size_t n = enc.encodeFrameRgb888(rgbPadded.data(), bitstream.data(), bitstream.size());
    assert(n > 0);
    decodeAndCheck(bitstream.data(), n, rgb888.data());
    enc.setPackedStride(0);  // restore default for the remaining overloads below
  }

  // RGB565 (5-6-5 packed) - built from the same source gradient, so the
  // expected-round-trip comparison absorbs RGB565's own inherent
  // quantization loss along with the codec's, same generous tolerance.
  {
    std::vector<uint16_t> rgb565((size_t)kWidth * kHeight);
    for (size_t i = 0; i < rgb565.size(); i++) {
      uint8_t r = rgb888[i * 3 + 0], g = rgb888[i * 3 + 1], b = rgb888[i * 3 + 2];
      rgb565[i] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    }
    size_t n = enc.encodeFrameRgb565(rgb565.data(), bitstream.data(), bitstream.size());
    assert(n > 0);
    decodeAndCheck(bitstream.data(), n, rgb888.data());
  }

  // RGB666 (6 bits/channel, left-justified in bits 7:2).
  {
    std::vector<uint8_t> rgb666(rgb888.size());
    for (size_t i = 0; i < rgb666.size(); i++) rgb666[i] = (uint8_t)(rgb888[i] & 0xFC);
    size_t n = enc.encodeFrameRgb666(rgb666.data(), bitstream.data(), bitstream.size());
    assert(n > 0);
    decodeAndCheck(bitstream.data(), n, rgb888.data());
  }

  // YUYV 4:2:2 - built directly via the BT.601 RGB->YUV math (mirroring
  // encoder/mpg_color_convert.h's own conversion) so this only exercises
  // encodeFrameYuv422()'s luma-copy/chroma-averaging path, not a second,
  // independent conversion implementation.
  {
    std::vector<uint8_t> yuyv((size_t)kWidth * kHeight * 2);
    for (int y = 0; y < kHeight; y++) {
      for (int x = 0; x < kWidth; x += 2) {
        const uint8_t* p0 = &rgb888[((size_t)y * kWidth + x) * 3];
        const uint8_t* p1 = &rgb888[((size_t)y * kWidth + x + 1) * 3];
        uint8_t y0v, u0, v0, y1v, u1, v1;
        rgbToYuv8(p0[0], p0[1], p0[2], &y0v, &u0, &v0);
        rgbToYuv8(p1[0], p1[1], p1[2], &y1v, &u1, &v1);
        uint8_t* dst = &yuyv[((size_t)y * kWidth + x) * 2];
        dst[0] = y0v;
        dst[1] = u0;  // one chroma sample per pixel pair, matching YUYV's own subsampling
        dst[2] = y1v;
        dst[3] = v0;
      }
    }
    size_t n = enc.encodeFrameYuv422(yuyv.data(), bitstream.data(), bitstream.size());
    assert(n > 0);
    decodeAndCheck(bitstream.data(), n, rgb888.data());
  }

  printf("test_encode_rgb: OK\n");
  return 0;
}

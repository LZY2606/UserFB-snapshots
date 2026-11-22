/*
 * TinyMPGEncoder example: encodes a synthetic QCIF gradient (no camera/SD
 * card needed) to a real, independently-decodable MPEG-1 elementary
 * stream picture, and benchmarks encode speed (kBenchmarkReps repetitions,
 * avg/min/max fps - see examples/DecodeFromProgmem for the same idea on
 * the decode side). Shows the recommended way to drive the encoder:
 * configure size/qp once via setSize()/setQp(), then just
 * encoder.encodeFrame(srcY, srcU, srcV, dst, dstCapacity) per picture -
 * every call produces a complete, self-contained I-picture (see
 * docs/encoding.md for why: this is a Phase 1, intra-only encoder, no
 * motion estimation/P-frames yet).
 *
 * Unlike TinyH264Encoder (or TinyMPGDecoder), Encoder holds no picture
 * buffer at all and needs no Allocator/PSRAM consideration - MPEG-1 intra
 * macroblocks have no spatial prediction to keep a reconstruction around
 * for (see docs/scope.md's Encoder section) - so there's no memory-budget
 * story here the way examples/DecodeFromProgmem has to think about.
 *
 * For a real application, feed encodeFrame() with real camera frames'
 * Y/U/V planes instead of the synthetic gradient below - the calling
 * pattern is identical either way.
 */

#include <TinyMPGEncoder.h>

using namespace tinympg;

static const int kWidth = 176;   // QCIF, must be a multiple of 16
static const int kHeight = 144;
static const int kQp = 8;        // 1-31, lower = higher quality/larger output
static const int kNumFrames = 4; // pictures per benchmark repetition

/*
 * Encode the kNumFrames-picture sequence this many times back to back to
 * get a stable timing average instead of judging performance off a
 * handful of frames alone - same idea as examples/DecodeFromProgmem's
 * kBenchmarkReps.
 */
static const int kBenchmarkReps = 30;

TinyMPGEncoder encoder;

static uint8_t srcY[kWidth * kHeight];
static uint8_t srcU[(kWidth / 2) * (kHeight / 2)];
static uint8_t srcV[(kWidth / 2) * (kHeight / 2)];
static uint8_t bitstream[16384];

static int frameCount = 0;
static uint64_t totalEncodeUs = 0;
static uint32_t minFrameUs = 0xFFFFFFFF;
static uint32_t maxFrameUs = 0;

static void printFreeHeap(const char* label) {
  Serial.print(label);
  Serial.print(": ");
#if defined(ARDUINO_ARCH_ESP32)
  Serial.print(ESP.getFreeHeap());
  Serial.println(" bytes");
#elif defined(ARDUINO_ARCH_RP2040)
  Serial.print(rp2040.getFreeHeap());
  Serial.println(" bytes");
#else
  Serial.println("(not available on this core)");
#endif
}

/*
 * Fills the source buffers with a diagonal luma gradient shifted by
 * `shiftX` pixels (flat mid-gray chroma) - real, non-trivial content for
 * the encoder's DCT/quantization path, not just a flat color the DC term
 * alone could represent.
 */
static void makeTestPattern(int shiftX) {
  for (int y = 0; y < kHeight; y++) {
    for (int x = 0; x < kWidth; x++) {
      int v = ((x + shiftX + y) * 255) / (kWidth + kHeight);
      srcY[y * kWidth + x] = (uint8_t)(v & 0xFF);
    }
  }
  for (int i = 0; i < (kWidth / 2) * (kHeight / 2); i++) {
    srcU[i] = 128;
    srcV[i] = 128;
  }
}

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);
  Serial.println("TinyMPGEncoder EncodeSyntheticFrame example");
  printFreeHeap("Free heap before encode");

  if (!encoder.setSize(kWidth, kHeight)) {
    Serial.println("setSize() failed - width/height must each be a multiple of 16");
    return;
  }
  if (!encoder.setQp(kQp)) {
    Serial.println("setQp() failed - qscale must be 1-31");
    return;
  }

  /*
   * Encodes the same kNumFrames-picture sequence kBenchmarkReps times back
   * to back (see kBenchmarkReps' own comment above) - every picture is a
   * self-contained I-picture (Phase 1, see docs/scope.md's Encoder
   * section), so there's no keyframe-interval/GOP-structure concept here
   * the way TinyH264Encoder's own EncodeSyntheticFrame example has.
   */
  for (int rep = 0; rep < kBenchmarkReps; rep++) {
    for (int i = 0; i < kNumFrames; i++) {
      makeTestPattern(i * 8);  // a little more horizontal shift each frame

      uint32_t startUs = micros();
      size_t n = encoder.encodeFrame(srcY, srcU, srcV, bitstream, sizeof(bitstream));
      uint32_t encodeUs = micros() - startUs;
      frameCount++;

      if (n == 0) {
        Serial.print("Frame ");
        Serial.print(frameCount);
        Serial.println(": encode failed");
        continue;
      }

      if (frameCount > 1) {
        totalEncodeUs += encodeUs;
        if (encodeUs < minFrameUs) minFrameUs = encodeUs;
        if (encodeUs > maxFrameUs) maxFrameUs = encodeUs;
      }

      Serial.print("Frame ");
      Serial.print(frameCount);
      Serial.print(": ");
      Serial.print((unsigned)n);
      Serial.print(" bytes, ");
      Serial.print(encodeUs);
      Serial.print(" us (");
      Serial.print(encodeUs > 0 ? 1000000.0f / encodeUs : 0.0f, 2);
      Serial.print(" fps, running avg=");
      if (frameCount > 1) {
        uint32_t avgUs = (uint32_t)(totalEncodeUs / (uint32_t)(frameCount - 1));
        Serial.print(avgUs > 0 ? 1000000.0f / avgUs : 0.0f, 2);
        Serial.println(" fps)");
      } else {
        Serial.println("n/a)");
      }
    }
  }

  Serial.print("Encoded ");
  Serial.print(frameCount);
  Serial.print(" frame(s) over ");
  Serial.print(kBenchmarkReps);
  Serial.println(" repetition(s) of the sequence.");
  if (frameCount > 1) {
    uint32_t avgUs = (uint32_t)(totalEncodeUs / (uint32_t)(frameCount - 1));
    Serial.print("Per-frame encode time: avg=");
    Serial.print(avgUs);
    Serial.print(" us (");
    Serial.print(1000000.0f / avgUs, 1);
    Serial.print(" fps), min=");
    Serial.print(minFrameUs);
    Serial.print(" us (");
    Serial.print(1000000.0f / minFrameUs, 1);
    Serial.print(" fps), max=");
    Serial.print(maxFrameUs);
    Serial.print(" us (");
    Serial.print(1000000.0f / maxFrameUs, 1);
    Serial.println(" fps)");
  }

  printFreeHeap("Free heap after encode");
}

void loop() {}

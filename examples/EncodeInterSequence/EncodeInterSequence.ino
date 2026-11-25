/*
 * TinyMPGEncoder zero-motion inter (P-picture) mode example: encodes a
 * short sequence of gently-changing frames as one MPEG-1 video sequence
 * (one I-picture, then zero-motion P-pictures - see
 * encoder/mpg_inter_encode.h and docs/encoding.md's "Inter (P-picture)
 * mode"), instead of the default one-independent-I-picture-per-call
 * output. This is the mode for the "mostly static scene" case the
 * library targets: no motion search is done, a P-picture only sends the
 * signed residual between the new frame and the previous *reconstructed*
 * frame at the same position, so a still or near-still frame costs a
 * handful of bytes instead of a whole intra frame.
 *
 * The example also shows the two sequence-control calls the mode adds:
 *   - setInterMode(true)  turns the I/P sequence mode on (default off)
 *   - resetSequence()     forces the next frame to restart at an I-picture
 * and the failure contract: a 0 return (output buffer too small here)
 * consumes no frame number and leaves the reference untouched, so
 * retrying with a larger buffer produces the same bytes as if the failed
 * call never happened.
 *
 * No camera/SD card needed: the source is the same synthetic diagonal
 * texture as EncodeSyntheticFrame, shifted by at most a pixel or two
 * between frames (deliberately low motion - that is where zero-motion P
 * coding actually wins).
 */

#include <TinyMPGEncoder.h>

using namespace tinympg;

static const int kWidth = 176;   // QCIF, must be a multiple of 16
static const int kHeight = 144;
static const int kQp = 8;        // 1-31, lower = higher quality/larger output
static const int kFrames = 6;    // 1 I-picture + 5 P-pictures per sequence

TinyMPGEncoder encoder;

static uint8_t srcY[kWidth * kHeight];
static uint8_t srcU[(kWidth / 2) * (kHeight / 2)];
static uint8_t srcV[(kWidth / 2) * (kHeight / 2)];

// Generous first, then deliberately undersized on one frame to demonstrate
// the retry contract (see the shortBuffer call below).
static uint8_t bitstream[16384];
static uint8_t tinyBuffer[64];

// Fills the source planes with a non-flat diagonal texture whose level
// shifts only slightly per frame - low motion, the regime zero-motion P
// coding is meant for (a fast-moving scene would send residuals for
// nearly every block anyway and gain little).
static void makeFrame(int t) {
  for (int y = 0; y < kHeight; y++) {
    for (int x = 0; x < kWidth; x++) {
      int v = ((x + y) * 255) / (kWidth + kHeight) + (t - kFrames / 2) * 2;
      if (v < 0) v = 0;
      if (v > 255) v = 255;
      srcY[y * kWidth + x] = (uint8_t)v;
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
  Serial.println("TinyMPGEncoder EncodeInterSequence example");

  if (!encoder.setSize(kWidth, kHeight)) {
    Serial.println("setSize() failed - width/height must each be a multiple of 16");
    return;
  }
  if (!encoder.setQp(kQp)) {
    Serial.println("setQp() failed - qscale must be 1-31");
    return;
  }
  encoder.setInterMode(true);  // first successful frame is I, the rest are zero-motion P

  for (int t = 0; t < kFrames; t++) {
    makeFrame(t);

    // Frame 2 is first attempted into a deliberately too-small buffer.
    uint8_t* out = (t == 2) ? tinyBuffer : bitstream;
    size_t cap = (t == 2) ? sizeof(tinyBuffer) : sizeof(bitstream);
    size_t n = encoder.encodeFrame(srcY, srcU, srcV, out, cap);

    if (n == 0) {
      // Nothing was committed: frame number and reference are unchanged.
      Serial.print("Frame ");
      Serial.print(t);
      Serial.print(": buffer too small (status ");
      Serial.print((int)encoder.lastStatus());
      Serial.println("), retrying with the full-size buffer");
      n = encoder.encodeFrame(srcY, srcU, srcV, bitstream, sizeof(bitstream));
    }

    Serial.print("Frame ");
    Serial.print(t);
    Serial.print(t == 0 ? ": I-picture, " : ": P-picture, ");
    Serial.print((unsigned)n);
    Serial.println(" bytes");
  }

  // A reset ends the sequence: the next encoded frame starts a fresh
  // sequence with its own I-picture (and sequence/GOP headers) again.
  encoder.resetSequence();
  makeFrame(0);
  size_t n = encoder.encodeFrame(srcY, srcU, srcV, bitstream, sizeof(bitstream));
  Serial.print("After resetSequence(): next frame is an I-picture again, ");
  Serial.print((unsigned)n);
  Serial.println(" bytes");
}

void loop() {}

/*
 * TinyH264Encoder example: optional P-macroblock partition motion search.
 *
 * Shows the whole public call pattern for
 * TinyH264Encoder<>::setPartitionSearch() - the one-line switch that
 * turns the default P_16x16-only inter coding into an exhaustive
 * P_16x16/P_16x8/P_8x16/P_8x8 (8x8/8x4/4x8/4x4 sub-partitions) search
 * over the same integer-pel window set by setMotionSearchRange(). See
 * docs/partition-search.md for the exact shape/cost/tie-break rules and
 * docs/encoding.md for the rest of the encoder API.
 *
 * This is a plain C++ program (no Arduino main()-replacement): it encodes
 * a short synthetic sequence with the feature both off and on, decodes
 * each stream back with TinyH264Decoder, and prints the sizes - a real,
 * runnable demonstration that only the intended pieces of the API are
 * needed. Build with, e.g.:
 *
 *   c++ -std=c++17 -O2 -I../.. \
 *       PartitionSearch.cpp ../../src/encoder/h264_partition_search.cpp \
 *       -o partition_search_example
 *
 * (On an embedded target, drive encodeFrame() per camera frame exactly as
 * EncodeSyntheticFrame.ino does - only the setPartitionSearch() call
 * below differs.)
 */

#include <cstdio>
#include <cstring>
#include <vector>

#include <TinyH264Encoder.h>
#include <TinyH264Decoder.h>

using namespace tinyh264;

static const int kWidth = 176;   // QCIF, must be a multiple of 16
static const int kHeight = 144;
static const int kNumFrames = 8;
static const int kQp = 26;
static const int kRange = 8;     // +/-pixel integer search window

// A textured frame whose four quadrants move in different directions - the
// content that actually makes partition shapes pay off. Flat mid-gray
// chroma keeps the example focused on luma motion.
static void makeFrame(int idx, std::vector<uint8_t>* y, std::vector<uint8_t>* u,
                      std::vector<uint8_t>* v) {
  y->resize((size_t)kWidth * kHeight);
  u->assign((size_t)(kWidth / 2) * (kHeight / 2), 128);
  v->assign((size_t)(kWidth / 2) * (kHeight / 2), 128);
  for (int py = 0; py < kHeight; py++) {
    for (int px = 0; px < kWidth; px++) {
      int qx = px >= kWidth / 2, qy = py >= kHeight / 2;
      int dx = (qx ? 2 : -2) * idx;
      int dy = (qy ? 2 : -2) * idx;
      int sx = (px + dx + kWidth) % kWidth;
      int sy = (py + dy + kHeight) % kHeight;
      (*y)[(size_t)py * kWidth + px] =
          (uint8_t)((sx * 5 + sy * 9 + ((sx >> 2) * 37) + ((sy >> 2) * 53)) &
                    0xFF);
    }
  }
}

// Encodes the sequence with partition search on/off, returns total bytes and
// decodes the stream back to confirm it is valid.
static size_t runExample(bool partitionSearch) {
  TinyH264Encoder<> encoder;
  encoder.setSize(kWidth, kHeight);
  encoder.setQp(kQp);
  encoder.setMotionSearchRange(kRange);
  // The one call this example is about:
  if (!encoder.setPartitionSearch(partitionSearch)) {
    printf("setPartitionSearch(%d) rejected by this backend\n",
           (int)partitionSearch);
    return 0;
  }

  std::vector<uint8_t> bitstream((size_t)kWidth * kHeight * 3);
  size_t total = 0;
  for (int i = 0; i < kNumFrames; i++) {
    std::vector<uint8_t> y, u, v;
    makeFrame(i, &y, &u, &v);
    size_t n = encoder.encodeFrame(y.data(), u.data(), v.data(),
                                   bitstream.data() + total,
                                   bitstream.size() - total);
    if (n == 0) {
      printf("encodeFrame failed at frame %d\n", i);
      return 0;
    }
    total += n;
  }

  TinyH264Decoder<> decoder;
  int frames = 0;
  decoder.setCallback(
      [](TinyH264Decoder<>&, void* ud) { (*(int*)ud)++; }, &frames);
  decoder.write(bitstream.data(), total);
  printf("partitionSearch=%s: %zu bytes, %d frames decoded%s\n",
         partitionSearch ? "on " : "off", total, frames,
         decoder.hasError() ? " (DECODE ERROR)" : "");
  return total;
}

int main() {
  size_t off = runExample(false);
  size_t on = runExample(true);
  if (off && on) {
    printf("partition search changed the P-frame size: %zu -> %zu bytes\n",
           off, on);
  }
  return 0;
}

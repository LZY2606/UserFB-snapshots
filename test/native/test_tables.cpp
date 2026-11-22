// Structural sanity checks for the static tables in common/mpg_tables.h and
// common/mpg_vlc_tables.h: the zig-zag scan is a permutation, and every VLC
// table entry is prefix-free / round-trippable (encode its own code bits,
// decode via BitReader::readVlc(), get the same index back) - mirrors
// TinyH264's test_cavlc_tables.cpp. See docs/testing.md.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include "mpg_bitreader.h"
#include "mpg_tables.h"
#include "mpg_vlc_tables.h"

using namespace tinympg;

// Minimal MSB-first bit writer, test-only (the library itself has no
// encoder yet - see docs/scope.md).
class BitWriter {
 public:
  void putBits(uint32_t value, int bits) {
    for (int i = bits - 1; i >= 0; i--) {
      putBit((value >> i) & 1u);
    }
  }
  std::vector<uint8_t> takeBytes() {
    if (bitPos_ != 0) bytes_.push_back(cur_);  // pad the trailing partial byte
    bitPos_ = 0;
    cur_ = 0;
    std::vector<uint8_t> out = bytes_;
    bytes_.clear();
    return out;
  }

 private:
  void putBit(uint32_t bit) {
    cur_ = (uint8_t)((cur_ << 1) | bit);
    if (++bitPos_ == 8) {
      bytes_.push_back(cur_);
      cur_ = 0;
      bitPos_ = 0;
    }
  }
  std::vector<uint8_t> bytes_;
  uint8_t cur_ = 0;
  int bitPos_ = 0;
};

static void testZigzagIsPermutation() {
  bool seen[64] = {false};
  bool ok = true;
  for (int i = 0; i < 64; i++) {
    if (kZigzag[i] >= 64 || seen[kZigzag[i]]) ok = false;
    seen[kZigzag[i]] = true;
  }
  assert(ok);
}

static void testDefaultMatricesNonZero() {
  for (int i = 0; i < 64; i++) {
    assert(kDefaultIntraMatrix[i] > 0);
    assert(kDefaultNonIntraMatrix[i] > 0);
  }
  assert(kDefaultIntraMatrix[0] == 8);       // DC entry, relied on by decodeIntraBlock()
  assert(kDefaultNonIntraMatrix[0] == 16);
}

// Encodes `codes[i]`/`bits[i]` for every entry, decodes it back via
// readVlc(), and checks the round trip returns index i - proves the table
// is genuinely prefix-free (a non-prefix-free table would either fail to
// decode or decode to the wrong index for at least one entry).
static void checkTableRoundTrips(const char* name, const uint16_t* codes,
                                  const uint8_t* bits, int n, int maxBits) {
  for (int i = 0; i < n; i++) {
    BitWriter bw;
    bw.putBits(codes[i], bits[i]);
    // Pad with zero bits so a short trailing code still has maxBits worth
    // of peekable data available (peekBits() itself zero-pads past the
    // buffer end anyway, but padding explicitly here keeps this test
    // independent of that behavior).
    bw.putBits(0, maxBits);
    std::vector<uint8_t> data = bw.takeBytes();
    BitReader br(data.data(), data.size());
    int decoded = br.readVlc(codes, bits, n, maxBits);
    if (decoded != i) {
      fprintf(stderr, "%s: entry %d (code=0x%x bits=%d) decoded as %d\n", name,
              i, codes[i], bits[i], decoded);
    }
    assert(decoded == i);
    assert((int)br.bitsConsumed() == bits[i]);
  }
}

int main() {
  testZigzagIsPermutation();
  testDefaultMatricesNonZero();

  checkTableRoundTrips("dc_lum", kDcLumCode, kDcLumBits, 12, kDcLumMaxBits);
  checkTableRoundTrips("dc_chroma", kDcChromaCode, kDcChromaBits, 12, kDcChromaMaxBits);
  checkTableRoundTrips("ac", kAcCode, kAcBits, kAcTableSize, kAcMaxBits);
  checkTableRoundTrips("mb_addr_inc", kMbAddrIncCode, kMbAddrIncBits,
                       kMbAddrIncTableSize, kMbAddrIncMaxBits);

  // kAcRun/kAcLevel must cover every non-escape/EOB AC table entry with a
  // level in the table's documented positive-magnitude convention.
  for (int i = 0; i < kAcEscapeIndex; i++) {
    assert(kAcLevel[i] >= 1);
    assert(kAcRun[i] >= 0);
  }

  printf("test_tables: OK\n");
  return 0;
}

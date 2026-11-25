// Tests mpg_bitreader.h's u(n) reads, byte alignment, and generic VLC table
// walk (readVlc()) in isolation - no real MPEG-1 bitstream involved, see
// docs/testing.md.
#include <cassert>
#include <cstdio>
#include "mpg_bitreader.h"

using namespace tinympg;

static void testFixedLengthReads() {
  // 0xB5 0x3C = 1011 0101 0011 1100
  const uint8_t data[] = {0xB5, 0x3C};
  BitReader br(data, sizeof(data));
  assert(br.u(4) == 0xB);
  assert(br.u(4) == 0x5);
  assert(br.u(8) == 0x3C);
  assert(!br.error());
}

static void testFlagAndSkip() {
  const uint8_t data[] = {0x80};  // 1000 0000
  BitReader br(data, sizeof(data));
  assert(br.flag() == true);
  assert(br.flag() == false);
  br.skipBits(2);
  assert(br.bitsConsumed() == 4);
}

static void testByteAlignment() {
  const uint8_t data[] = {0xFF, 0x00};
  BitReader br(data, sizeof(data));
  br.u(3);
  assert(!br.byteAligned());
  br.byteAlign();
  assert(br.byteAligned());
  assert(br.bitsConsumed() == 8);
  assert(br.u(8) == 0x00);
}

static void testErrorPastEnd() {
  const uint8_t data[] = {0xFF};
  BitReader br(data, sizeof(data));
  br.u(8);
  assert(!br.error());
  br.u(1);  // one bit past the end of the buffer
  assert(br.error());
}

static void testPeekDoesNotConsume() {
  const uint8_t data[] = {0xA5};  // 1010 0101
  BitReader br(data, sizeof(data));
  assert(br.peekBits(4) == 0xA);
  assert(br.peekBits(4) == 0xA);  // still 0xA - peek must not advance
  assert(br.u(4) == 0xA);
  assert(br.u(4) == 0x5);
}

static void testReadVlc() {
  // A tiny 3-entry prefix-free table: '0' -> 0, '10' -> 1, '11' -> 2.
  static const uint16_t codes[3] = {0x0, 0x2, 0x3};
  static const uint8_t bits[3] = {1, 2, 2};

  {
    const uint8_t data[] = {0b01011100};  // '0','1'0,'1'1,'1'0,'0'
    BitReader br(data, sizeof(data));
    int a = br.readVlc(codes, bits, 3, 2);
    int b = br.readVlc(codes, bits, 3, 2);
    int c = br.readVlc(codes, bits, 3, 2);
    int d = br.readVlc(codes, bits, 3, 2);
    assert(a == 0);  // '0'
    assert(b == 1);  // '10'
    assert(c == 2);  // '11'
    assert(d == 1);  // '10'
    assert(!br.error());
    (void)a; (void)b; (void)c; (void)d;
  }
  {
    // A deliberately incomplete table (only '0' and '10' - no code for a
    // leading '11') to exercise readVlc()'s no-match error path: an
    // all-ones buffer can't match either entry.
    static const uint16_t codes2[2] = {0x0, 0x2};
    static const uint8_t bits2[2] = {1, 2};
    const uint8_t data[] = {0xFF};
    BitReader br(data, sizeof(data));
    int idx = br.readVlc(codes2, bits2, 2, 2);
    assert(idx == -1);
    assert(br.error());
    (void)idx;
  }
}

int main() {
  testFixedLengthReads();
  testFlagAndSkip();
  testByteAlignment();
  testErrorPastEnd();
  testPeekDoesNotConsume();
  testReadVlc();
  printf("test_bitreader: OK\n");
  return 0;
}

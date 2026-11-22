#pragma once
#include <stdint.h>
#include <stddef.h>

/*
 * Header-only. MSB-first bit reader over a single start-code-delimited
 * unit's payload (see decoder/mpg_decoder.h - MPEG-1 has no
 * emulation-prevention layer like H.264's RBSP, so this reads directly out
 * of the caller's own buffer, no scratch copy needed). Implements u(n)
 * fixed-length reads plus a generic VLC table walk (readVlc()) - MPEG-1's
 * syntax has no Exp-Golomb codes (ue(v)/se(v)), unlike H.264; every
 * variable-length element here is a genuine Huffman-style VLC table
 * (Annex B), so this class looks different from TinyH264's BitReader in
 * that respect even though both are MSB-first bit readers at heart.
 *
 * Header-only / all-inline: this sits on the hottest path of the decoder
 * (every syntax element in every macroblock goes through here), so we let
 * the compiler inline everything rather than paying call overhead on a
 * single-core MCU.
 */

namespace tinympg {

/**
 * MSB-first bitstream reader over one start-code-delimited unit's raw
 * bytes. Every parser in this library (sequence/picture header, slice,
 * macroblock layer, DCT residuals) is built directly on top of this
 * class.
 */
class BitReader {
 public:
  /**
   * Wraps `size` bytes at `data` (not copied/owned - caller must keep the
   * buffer alive for the BitReader's lifetime) for reading from bit 0.
   */
  BitReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

  /**
   * u(n): reads the next n bits (0 <= n <= 32) as an unsigned integer,
   * MSB first.
   */
  uint32_t u(int n) {
    uint32_t val = 0;
    while (n-- > 0) val = (val << 1) | readBit();
    return val;
  }

  /// u(1) read as a bool - convenience for one-bit flag syntax elements.
  bool flag() { return u(1) != 0; }

  /// Advances the read position by n bits without returning a value.
  void skipBits(int n) {
    while (n-- > 0) readBit();
  }

  /**
   * Returns the next `n` bits (0 <= n <= 24), MSB first, *without*
   * consuming them - used to look ahead before committing to a VLC match
   * (see readVlc()). Bits past the end of the buffer read as 0, the same
   * convention real encoders' byte-aligned zero padding before the next
   * start code naturally satisfies (see kMbAddrIncEndIndex's "end of
   * slice" guard code in mpg_vlc_tables.h) - this never sets error().
   */
  uint32_t peekBits(int n) const {
    uint32_t val = 0;
    size_t bytePos = bytePos_;
    int bitPos = bitPos_;
    for (int i = 0; i < n; i++) {
      uint32_t bit = 0;
      if (bytePos < size_) bit = (data_[bytePos] >> (7 - bitPos)) & 1u;
      val = (val << 1) | bit;
      if (++bitPos == 8) {
        bitPos = 0;
        bytePos++;
      }
    }
    return val;
  }

  /**
   * Generic VLC table walk: matches the next bits against a prefix-free
   * code table of `n` entries (`codes[i]`/`bits[i]`, each entry's code
   * left-justified in its own `bits[i]`-bit width), consuming exactly the
   * matched entry's bit length on success. Correct for any true
   * prefix-free table regardless of scan order (see mpg_vlc_tables.h's
   * file comment for why, and the performance caveat). Returns the
   * matched entry's index, or -1 (and sets error()) if no entry matches
   * within `maxBits` peeked bits - only possible on a corrupt/truncated
   * bitstream, since every table used here includes full coverage of its
   * syntax element's legal codes.
   */
  int readVlc(const uint16_t* codes, const uint8_t* bits, int n, int maxBits) {
    uint32_t peeked = peekBits(maxBits);
    for (int i = 0; i < n; i++) {
      int b = bits[i];
      if ((peeked >> (maxBits - b)) == codes[i]) {
        skipBits(b);
        return i;
      }
    }
    error_ = true;
    return -1;
  }

  /// True if the read position is currently on a byte boundary.
  bool byteAligned() const { return bitPos_ == 0; }
  /// Advances the read position to the start of the next byte if not already aligned.
  void byteAlign() {
    if (bitPos_ != 0) {
      bitPos_ = 0;
      bytePos_++;
    }
  }

  /// Total number of bits consumed so far from the start of the buffer.
  size_t bitsConsumed() const { return bytePos_ * 8 + (size_t)bitPos_; }
  /// The underlying buffer pointer (not owned).
  const uint8_t* data() const { return data_; }
  /// Size in bytes of the underlying buffer.
  size_t size() const { return size_; }

  /**
   * Set once a consuming read (u()/skipBits()/a matched readVlc()) has run
   * past the end of the buffer, or readVlc() found no matching code.
   * Callers should abandon the current slice/picture when this is set,
   * since all subsequently-read values are meaningless.
   */
  bool error() const { return error_; }

 private:
  /**
   * Reads and consumes a single bit; sets error_ and returns 0 past
   * end-of-buffer rather than reading out of bounds.
   */
  uint32_t readBit() {
    if (bytePos_ >= size_) {
      error_ = true;
      return 0;
    }
    uint32_t bit = (data_[bytePos_] >> (7 - bitPos_)) & 1u;
    if (++bitPos_ == 8) {
      bitPos_ = 0;
      bytePos_++;
    }
    return bit;
  }

  const uint8_t* data_;
  size_t size_;
  size_t bytePos_ = 0;
  int bitPos_ = 0;  // next bit to read within data_[bytePos_], 0 = MSB
  bool error_ = false;
};

}  // namespace tinympg

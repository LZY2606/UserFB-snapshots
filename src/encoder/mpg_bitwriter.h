#pragma once
#include <stdint.h>
#include <stddef.h>

/*
 * Header-only. MSB-first bitstream writer into a caller-owned buffer - the
 * encode-side mirror of decoder/mpg_bitreader.h's BitReader. Implements
 * u(n) fixed-length writes plus writeVlc() (given a (code, bits) pair
 * looked up from one of common/mpg_vlc_tables.h's tables) and byteAlign()
 * for the zero-padding MPEG-1 expects between the last macroblock of a
 * slice and the next start code (see decoder/mpg_vlc_tables.h's
 * kMbAddrIncEndIndex comment - that all-zero guard code is exactly this
 * padding, from the decode side). No emulation-prevention/stuffing logic
 * is needed here (unlike H.264's BitWriter): MPEG-1's VLC tables are
 * constructed so the 3-byte prefix 00 00 01 cannot occur anywhere except a
 * genuine start code (see decoder/mpg_decoder.h's file comment) - as long
 * as every emitted syntax element is a real, spec-conformant code, that
 * property holds automatically, nothing extra to enforce here.
 */

namespace tinympg {

/**
 * MSB-first bitstream writer over a fixed, caller-owned buffer. Never
 * allocates - `data`/`capacity` is sized by the caller, matching this
 * library's decode-side convention of never allocating in the hot path.
 */
class BitWriter {
 public:
  /**
   * Wraps `capacity` bytes at `data` (not owned - caller must keep the
   * buffer alive for the BitWriter's lifetime) for writing from bit 0.
   */
  BitWriter(uint8_t* data, size_t capacity) : data_(data), capacity_(capacity) {}

  /// u(n): writes the low n bits (0 <= n <= 32) of `val`, MSB first.
  void u(uint32_t val, int n) {
    for (int i = n - 1; i >= 0; i--) writeBit((val >> i) & 1u);
  }

  /// u(1) written from a bool - convenience for one-bit flag syntax elements.
  void flag(bool b) { writeBit(b ? 1u : 0u); }

  /**
   * Writes one VLC entry, `bits` bits of `code` (left-justified in that
   * width) - the write-side counterpart of BitReader::readVlc(), used with
   * a (code, bits) pair looked up by symbol from one of
   * common/mpg_vlc_tables.h's tables rather than walking the table to
   * match an unknown symbol the way decode does.
   */
  void writeVlc(uint32_t code, int bits) { u(code, bits); }

  /// True if the write position is currently on a byte boundary.
  bool byteAligned() const { return bitPos_ == 0; }

  /**
   * Pads with zero bits up to the next byte boundary - MPEG-1's own
   * padding convention between the last macroblock of a slice and the
   * next (byte-aligned) start code, unlike H.264's rbsp_trailing_bits()
   * (which writes a mandatory stop-one-bit first): MPEG-1 has no
   * equivalent stop bit, so plain zero padding is both sufficient and
   * exactly what decoder/mpg_vlc_tables.h's kMbAddrIncEndIndex guard code
   * (all-zero bits) is built to recognize as "end of slice", not an
   * error.
   */
  void byteAlign() {
    while (bitPos_ != 0) writeBit(0);
  }

  /**
   * Writes a 4-byte start code (00 00 01 XX) - `type` is the low byte
   * (e.g. 0xB3 for sequence_header_code, 0x00 for picture_start_code, or a
   * slice's row number 0x01-0xAF for slice_start_code, ISO/IEC 11172-2
   * clause 2.4.3.1). Byte-aligns first (a no-op if already aligned, which
   * every call site's own byteAlign() after the previous unit should
   * already guarantee - defensive, not load-bearing).
   */
  void startCode(uint8_t type) {
    byteAlign();
    u(0x000001u, 24);
    u(type, 8);
  }

  /// Total number of bits written so far.
  size_t bitsWritten() const { return bytePos_ * 8 + (size_t)bitPos_; }
  /**
   * Number of whole bytes written so far, rounding up a partial byte -
   * only meaningful once the caller has byte-aligned the stream (every
   * real MPEG-1 unit boundary does, via byteAlign()/startCode()).
   */
  size_t bytesWritten() const { return bytePos_ + (bitPos_ != 0 ? 1 : 0); }

  /**
   * Set when a write has run past the end of the buffer (undersized
   * caller-provided capacity). Callers should treat the buffer contents
   * as incomplete/unusable when this is set, same convention as
   * BitReader::error().
   */
  bool error() const { return error_; }

 private:
  /// Writes a single bit; sets error_ instead of writing out of bounds.
  void writeBit(uint32_t bit) {
    if (bytePos_ >= capacity_) {
      error_ = true;
      return;
    }
    if (bitPos_ == 0) data_[bytePos_] = 0;
    data_[bytePos_] |= (uint8_t)((bit & 1u) << (7 - bitPos_));
    if (++bitPos_ == 8) {
      bitPos_ = 0;
      bytePos_++;
    }
  }

  uint8_t* data_;
  size_t capacity_;
  size_t bytePos_ = 0;
  int bitPos_ = 0;
  bool error_ = false;
};

}  // namespace tinympg

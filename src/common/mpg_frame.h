#pragma once
#include <stdint.h>
#include <string.h>
#include "../StdAllocator.h"
#include "../mpg_config.h"

/*
 * Header-only. A single YUV 4:2:0 planar picture buffer. Sized to
 * maxWidth x maxHeight (default MPG_MAX_WIDTH/MPG_MAX_HEIGHT, see
 * mpg_config.h - overridable per Decoder instance via setMaxDimension(),
 * see decoder/mpg_decoder.h), same allocate-once/reuse-per-picture
 * approach as TinyH264's common/h264_frame.h.
 *
 * Y, U, and V live in *three separate* Buffer<Allocator> buffers (see
 * below) rather than one merged contiguous allocation - see h264_frame.h's
 * file comment (TinyH264) for the full heap-fragmentation rationale on a
 * plain ESP32; the same reasoning applies unchanged here.
 *
 * MPEG-1's horizontal_size/vertical_size (the *display* dimensions) need
 * not be multiples of 16, unlike the macroblock grid actually decoded into
 * this buffer (the *coded* dimensions, rounded up to the next multiple of
 * 16) - width()/height() expose the display size (what a caller should
 * treat as "the picture"), while strideY/strideC stay sized to the coded
 * (macroblock-aligned) buffer layout underneath. Any macroblock-edge
 * padding past the display edge is simply never read by row/plane
 * accessors bounded by width()/height().
 */

namespace tinympg {

/**
 * Minimal RAII byte buffer backing each of Frame<Allocator>'s Y/U/V
 * planes, in place of std::vector<uint8_t, Allocator> - deliberately not
 * a general-purpose vector, because std::vector's contract assumes
 * Allocator::allocate() either succeeds or throws, and *writes through*
 * the pointer it returns (to value-initialize the new elements) before a
 * caller ever gets to inspect it. With a no-throw Allocator (see
 * StdAllocator.h's StdAllocator, used as this project's default) that
 * returns nullptr on failure instead of throwing, that write-before-check
 * ordering is a null-pointer write - this was verified empirically on
 * this project's own toolchain (g++/libstdc++): it segfaults at -O0, and
 * at -O2 the optimizer proves the null write is undefined behavior and
 * elides it entirely, leaving the vector in an inconsistent state
 * (size() > 0 but data() == nullptr) that crashes or corrupts memory
 * later instead, at some less obvious point. Buffer sidesteps this by
 * calling Allocator::allocate() itself and checking the returned pointer
 * *before* anything is written through it - never delegating that check
 * to a container that assumes it can't fail.
 *
 * Deliberately narrow: a Frame only ever allocates a plane once
 * (ensureAllocated()) and either keeps it or releases it outright
 * (release()) - there's no mid-life resize/grow the way std::vector
 * supports, so this doesn't implement one.
 */
template <typename Allocator>
class Buffer {
 public:
  Buffer() = default;
  Buffer(const Buffer&) = delete;
  Buffer& operator=(const Buffer&) = delete;
  Buffer(Buffer&& other) noexcept { moveFrom(other); }
  Buffer& operator=(Buffer&& other) noexcept {
    if (this != &other) {
      release();
      moveFrom(other);
    }
    return *this;
  }
  ~Buffer() { release(); }

  bool empty() const { return data_ == nullptr; }
  uint8_t* data() { return data_; }
  const uint8_t* data() const { return data_; }

  /**
   * Allocates `n` bytes if this Buffer doesn't already hold one (a no-op,
   * returning true, otherwise - matching the old std::vector-based
   * ensureAllocated()'s "allocate once" contract). Returns false, without
   * throwing and without writing through a possibly-null pointer, if the
   * allocator reports failure - either a null return from a no-throw
   * Allocator (the expected case, see StdAllocator.h), or - for a
   * throwing Allocator like plain std::allocator<uint8_t> - a caught
   * std::bad_alloc, when this is built with C++ exceptions enabled.
   */
  bool allocate(size_t n) {
    if (data_) return true;
    uint8_t* p = tryAllocate(n);
    if (!p) return false;
    data_ = p;
    size_ = n;
    return true;
  }

  /// Frees this Buffer's storage (a no-op if not currently allocated).
  void release() {
    if (data_) {
      alloc_.deallocate(data_, size_);
      data_ = nullptr;
      size_ = 0;
    }
  }

 private:
  uint8_t* tryAllocate(size_t n) {
#if __cpp_exceptions
    try {
      return alloc_.allocate(n);
    } catch (...) {
      return nullptr;
    }
#else
    return alloc_.allocate(n);
#endif
  }

  void moveFrom(Buffer& other) {
    data_ = other.data_;
    size_ = other.size_;
    alloc_ = other.alloc_;
    other.data_ = nullptr;
    other.size_ = 0;
  }

  uint8_t* data_ = nullptr;
  size_t size_ = 0;
  Allocator alloc_;
};

/**
 * A single YUV 4:2:0 planar decoded picture. A Decoder<Allocator> keeps
 * one of these resident as the picture currently being reconstructed,
 * plus (from Phase B on - see docs/scope.md) one more as the held
 * reference picture P-pictures predict from. The Allocator template
 * parameter controls where the backing storage lives (regular heap by
 * default via StdAllocator, see StdAllocator.h; see PSRAMAllocatorESP32.h
 * to place it in PSRAM instead).
 */
template <typename Allocator = StdAllocator<uint8_t>>
struct Frame {
  /**
   * Allocation ceiling this Frame's buffers are sized to, in luma
   * samples - defaults to the compile-time MPG_MAX_WIDTH/MPG_MAX_HEIGHT
   * (mpg_config.h), overridable per instance via setMaxDimension() before
   * first use.
   */
  int maxWidth = MPG_MAX_WIDTH;
  int maxHeight = MPG_MAX_HEIGHT;

  /**
   * Overrides maxWidth/maxHeight for this Frame's own allocation size. If
   * buffers are already allocated, releases them first so the *next*
   * ensureAllocated() call picks up the new size - safe to call at any
   * time, not just before first use.
   */
  void setMaxDimension(int w, int h) {
    maxWidth = w;
    maxHeight = h;
    if (!dataY.empty()) release();
  }

  Buffer<Allocator> dataY;
  Buffer<Allocator> dataU;
  Buffer<Allocator> dataV;

  int width = 0;       ///< display (horizontal_size) luma width in samples
  int height = 0;      ///< display (vertical_size) luma height in samples
  int codedWidth = 0;  ///< macroblock-aligned luma width backing the buffer
  int codedHeight = 0; ///< macroblock-aligned luma height backing the buffer
  int strideY = 0;     ///< samples per luma row (== codedWidth)
  int strideC = 0;     ///< samples per chroma row (== codedWidth / 2)

  /**
   * Allocates the three backing buffers at their maximum (maxWidth x
   * maxHeight-derived, see setMaxDimension()) sizes if not already done.
   * Safe to call more than once; a no-op after the first call, so it
   * never re-allocates in the per-frame decode hot path. Returns false -
   * without crashing - if the allocator (see StdAllocator.h) couldn't
   * satisfy one of the three allocations; on failure, any of the three
   * that *did* succeed are released again first, so a Frame never sits
   * around half-allocated and a later retry attempts all three fresh.
   */
  bool ensureAllocated() {
    if (!dataY.empty()) return true;
    bool ok = dataY.allocate((size_t)maxWidth * maxHeight) &&
              dataU.allocate((size_t)(maxWidth / 2) * (maxHeight / 2)) &&
              dataV.allocate((size_t)(maxWidth / 2) * (maxHeight / 2));
    if (!ok) {
      release();
      return false;
    }
    return true;
  }

  /**
   * Sets this picture's actual display dimensions (from the active
   * sequence_header) plus the macroblock-aligned coded dimensions the
   * decoder actually writes into, and derives the plane strides from the
   * latter. Must be called before any row accessor; allocates backing
   * storage on first use via ensureAllocated(), whose success/failure
   * this returns - the display/coded dimensions and strides below are
   * left unset on failure, matching a Frame that was never sized at all.
   */
  bool setSize(int displayW, int displayH, int codedW, int codedH) {
    if (!ensureAllocated()) return false;
    width = displayW;
    height = displayH;
    codedWidth = codedW;
    codedHeight = codedH;
    strideY = codedW;
    strideC = codedW / 2;
    return true;
  }

  /**
   * Releases the three backing buffers back to the allocator and resets
   * dimensions to zero - the counterpart to ensureAllocated()/setSize().
   */
  void release() {
    dataY.release();
    dataU.release();
    dataV.release();
    width = height = codedWidth = codedHeight = strideY = strideC = 0;
  }

  /// Pointer to the start of the luma plane.
  uint8_t* y() { return dataY.data(); }
  /// Pointer to the start of the Cb (blue-difference chroma) plane.
  uint8_t* u() { return dataU.data(); }
  /// Pointer to the start of the Cr (red-difference chroma) plane.
  uint8_t* v() { return dataV.data(); }
  /// const overload of y().
  const uint8_t* y() const { return dataY.data(); }
  /// const overload of u().
  const uint8_t* u() const { return dataU.data(); }
  /// const overload of v().
  const uint8_t* v() const { return dataV.data(); }

  /// Pointer to the first luma sample of row `row`.
  uint8_t* yRow(int row) { return y() + (size_t)row * strideY; }
  /// Pointer to the first Cb sample of chroma row `row`.
  uint8_t* uRow(int row) { return u() + (size_t)row * strideC; }
  /// Pointer to the first Cr sample of chroma row `row`.
  uint8_t* vRow(int row) { return v() + (size_t)row * strideC; }
  /// const overload of yRow(), for reading a decoded picture.
  const uint8_t* yRow(int row) const { return y() + (size_t)row * strideY; }
  /// const overload of uRow(), for reading a decoded picture.
  const uint8_t* uRow(int row) const { return u() + (size_t)row * strideC; }
  /// const overload of vRow(), for reading a decoded picture.
  const uint8_t* vRow(int row) const { return v() + (size_t)row * strideC; }

};

/**
 * Packs a Frame's Y/U/V planes into one tightly-packed, contiguous buffer -
 * standard "I420" layout (Y bytes, then U bytes, then V bytes), sized to
 * this picture's *display* width/height, no row padding. `dst` must have
 * room for width*height + 2*(width/2)*(height/2) uint8_t entries; never
 * allocates. Row-by-row (not one bulk memcpy of the whole plane): unlike
 * TinyH264's Frame, strideY here can differ from width whenever the coded
 * (macroblock-aligned) size exceeds the display size, so each row must be
 * copied individually rather than assuming strideY == width.
 */
template <typename Allocator>
inline void convertFrameToYuv420(const Frame<Allocator>& frame, uint8_t* dst) {
  uint8_t* dstY = dst;
  uint8_t* dstU = dst + (size_t)frame.width * frame.height;
  uint8_t* dstV = dstU + (size_t)(frame.width / 2) * (frame.height / 2);
  for (int row = 0; row < frame.height; row++) {
    memcpy(dstY + (size_t)row * frame.width, frame.yRow(row), frame.width);
  }
  for (int row = 0; row < frame.height / 2; row++) {
    memcpy(dstU + (size_t)row * (frame.width / 2), frame.uRow(row), frame.width / 2);
    memcpy(dstV + (size_t)row * (frame.width / 2), frame.vRow(row), frame.width / 2);
  }
}

/**
 * Windowed/tiled counterpart of convertFrameToYuv420() above - packs just a
 * `width` x `height` rectangle starting at (originX, originY) into `dst`
 * (I420 layout, no row padding). `originX`/`originY` must be even (chroma
 * is subsampled 2x) and, together with width/height, must stay within the
 * source frame - not bounds-checked.
 */
template <typename Allocator>
inline void convertFrameToYuv420(const Frame<Allocator>& frame, int originX,
                                  int originY, int width, int height,
                                  uint8_t* dst) {
  uint8_t* dstY = dst;
  uint8_t* dstU = dst + (size_t)width * height;
  uint8_t* dstV = dstU + (size_t)(width / 2) * (height / 2);
  for (int row = 0; row < height; row++) {
    memcpy(dstY + (size_t)row * width, frame.yRow(originY + row) + originX,
           width);
  }
  for (int row = 0; row < height / 2; row++) {
    memcpy(dstU + (size_t)row * (width / 2),
           frame.uRow((originY >> 1) + row) + (originX >> 1), width / 2);
    memcpy(dstV + (size_t)row * (width / 2),
           frame.vRow((originY >> 1) + row) + (originX >> 1), width / 2);
  }
}

}  // namespace tinympg

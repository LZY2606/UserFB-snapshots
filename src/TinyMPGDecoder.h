#pragma once
#include "StdAllocator.h"
#include "decoder/mpg_decoder.h"
#include "decoder/mpg_rgb.h"
#ifdef ESP32
#include "PSRAMAllocatorESP32.h"
#endif

/*
 * TinyMPGDecoder: a minimal, header-only MPEG-1 (ISO/IEC 11172-2) video
 * decoder for microcontrollers (I-, P-, and B-pictures - see
 * docs/scope.md). This is the public-facing API, in the same `tinympg`
 * namespace as everything under decoder/mpg_*.h (implementation detail,
 * but kept in one consistent namespace throughout the library) - shaped to
 * closely match TinyH264's TinyH264Decoder<Allocator> (see
 * docs/decoding.md).
 *
 * Usage:
 *   using namespace tinympg;
 *
 *   void onFrame(TinyMPGDecoder<> &decoder, void *userData) {
 *     // decoder.width()/height()/y()/u()/v()/strideY()/strideUV()
 *   }
 *   TinyMPGDecoder<> decoder;
 *   decoder.setCallback(onFrame);
 *   decoder.write(mpeg1EsData, size);  // one or more picture_start_code-
 *                                      // delimited access units;
 *                                      // onFrame() runs once per decoded
 *                                      // picture before write() returns.
 *   if (decoder.hasError()) { ... }    // bitstream error, unsupported
 *                                      // feature (e.g. a D picture), or
 *                                      // an out-of-memory allocation
 *                                      // failure - see Status below
 *
 * The Allocator template parameter (default StdAllocator<uint8_t>, see
 * StdAllocator.h) is used for the picture buffer (see common/mpg_frame.h)
 * - pass a custom allocator to place it in PSRAM or a dedicated pool
 * instead of the default heap, e.g. on an ESP32-S3 with PSRAM:
 *   TinyMPGDecoder<PSRAMAllocatorESP32<uint8_t>> decoder;
 * Any Allocator used here should follow the same "return nullptr, don't
 * throw" contract StdAllocator/PSRAMAllocatorESP32 do (see
 * StdAllocator.h's file comment for why) so an out-of-memory condition
 * surfaces as Status::kAllocationError instead of crashing.
 *
 * See mpg_config.h for the resolution/memory budget (MPG_MAX_WIDTH/
 * MPG_MAX_HEIGHT default to CIF).
 */

namespace tinympg {

/**
 * Public-facing decoder API: wraps the internal Decoder<Allocator> (see
 * decoder/mpg_decoder.h) behind a small push-data-get-callback interface
 * suitable for feeding elementary-stream data straight off a file,
 * PROGMEM buffer, or network socket without the caller needing to know
 * anything about start codes, slices, or macroblocks. `Allocator`
 * controls where the decoded picture buffer (Frame::y/u/v) is allocated -
 * see the file comment above for the PSRAM usage example.
 */
template <typename Allocator = StdAllocator<uint8_t>>
class TinyMPGDecoder {
 public:
  /// Outcome of the most recent write()/decodeNext() call.
  enum class Status {
    kFrameReady,       ///< a new picture is ready via width()/height()/y()/u()/v()
    kNeedMoreData,     ///< call write() with more data and keep decoding
    kUnsupported,      ///< stream uses an unimplemented feature (e.g. a D picture)
    kError,            ///< corrupt/truncated bitstream
    kAllocationError,  ///< a picture buffer allocation failed (out of memory) - see StdAllocator.h; treat this as terminal for the current decoder instance the same way kError is
  };

  /**
   * Called once per decoded picture, from within write(). `decoder` is the
   * same object write() was called on (so width()/y()/u()/v()/etc. are
   * valid inside the callback); `userData` is whatever was passed to
   * setCallback().
   */
  using FrameCallback = void (*)(TinyMPGDecoder& decoder, void* userData);

  /**
   * Registers the function to invoke once per decoded picture during
   * write(). Pass nullptr to disable (the default); `userData` is passed
   * through unchanged to each callback invocation.
   */
  void setCallback(FrameCallback cb, void* userData = nullptr) {
    callback_ = cb;
    userData_ = userData;
  }

  /**
   * Runtime alternative to `#define MPG_MAX_WIDTH ...`/`#define
   * MPG_MAX_HEIGHT ...` before `#include`ing this header - overrides the
   * picture-buffer allocation ceiling for this decoder instance (see
   * Decoder::setMaxDimension(), decoder/mpg_decoder.h). A stream whose
   * sequence_header declares a resolution bigger than this is rejected as
   * kUnsupported. Call before the first decode to size the buffer for
   * your actual content up front instead of the compile-time
   * MPG_MAX_WIDTH/MPG_MAX_HEIGHT default (mpg_config.h).
   */
  void setMaxDimension(int maxWidth, int maxHeight) {
    decoder_.setMaxDimension(maxWidth, maxHeight);
  }
  /// The current allocation-ceiling width - see setMaxDimension().
  int maxWidth() const { return decoder_.maxWidth(); }
  /// The current allocation-ceiling height - see setMaxDimension().
  int maxHeight() const { return decoder_.maxHeight(); }

  /// See Decoder::setIgnorePFrames() - when set, P-pictures are silently
  /// discarded (no slices decoded, no callback fired, no error status)
  /// instead of being decoded; I- and B-pictures are unaffected. Off by
  /// default.
  void setIgnorePFrames(bool active) { decoder_.setIgnorePFrames(active); }
  bool ignorePFrames() const { return decoder_.ignorePFrames(); }

  /**
   * Reserves this decoder's picture buffer up front instead of the
   * default allocate-on-first-decode behavior. Call once, typically from
   * setup(), if you want any allocation failure to surface
   * deterministically before the stream starts rather than mid-decode.
   * Returns false (and sets lastStatus() to kAllocationError) if
   * allocation failed - out of memory, not a crash - see StdAllocator.h.
   */
  bool begin() {
    bool ok = decoder_.begin();
    lastStatus_ = ok ? Status::kNeedMoreData : Status::kAllocationError;
    return ok;
  }

  /**
   * Releases this decoder's picture buffer and resets it to a fresh,
   * just-constructed state (no cached sequence_header). Not required
   * before destruction; only useful for reclaiming memory before that,
   * e.g. between unrelated streams, while this object is still alive.
   * Safe to call begin()/write() again afterward to start over.
   */
  void end() {
    decoder_.end();
    lastStatus_ = Status::kNeedMoreData;
  }

  /**
   * Feeds one buffer of MPEG-1 video elementary stream data
   * (picture_start_code / slice_start_code / etc. delimited) and
   * immediately decodes everything it can from it, invoking the frame
   * callback (if set, via setCallback()) once per decoded picture. The
   * buffer must remain valid for the duration of this call. Returns the
   * status of the *last* thing that happened while draining the buffer:
   * kFrameReady if the last unit produced a picture, kNeedMoreData once
   * the buffer is fully drained with nothing pending, or
   * kUnsupported/kError/kAllocationError if decoding stopped early because
   * of one - same value as lastStatus() would give right after this call.
   *
   * `isLastChunk` (default true) - see Decoder::setInput() (decoder/
   * mpg_decoder.h) for the full rationale. Leave it at the default when
   * `data` is one self-contained buffer (a whole clip/picture at a time,
   * as every example/test in this project does). Pass false when
   * forwarding a live/container-demuxed stream (e.g. arduino-audio-tools'
   * DemuxerMPG) in chunks that might split a picture_header or slice
   * across separate write() calls - a unit that isn't provably complete
   * yet is held internally and completed by a later write() call instead
   * of being processed prematurely or having its tail silently dropped.
   * If internally carrying that data over requires an allocation that
   * fails, this returns kAllocationError (same as a picture-buffer
   * allocation failure - see StdAllocator.h) without decoding anything
   * from `data`.
   */
  Status write(const uint8_t* data, size_t size, bool isLastChunk = true) {
    if (!decoder_.setInput(data, size, isLastChunk)) {
      lastStatus_ = Status::kAllocationError;
      return lastStatus_;
    }
    while (true) {
      DecodeStatus raw = decoder_.next();
      if (raw == DecodeStatus::kOk) {
        lastStatus_ = Status::kFrameReady;
        if (callback_) callback_(*this, userData_);
        continue;
      }
      if (raw == DecodeStatus::kNeedMoreData) {
        /*
         * This alone doesn't mean "buffer drained" - it's also returned
         * after processing a non-picture-completing unit (sequence
         * header, extension, user data, ...), which just means "keep
         * calling next()". Only stop once the start-code reader itself
         * has run out of start codes.
         */
        if (decoder_.inputExhausted()) {
          lastStatus_ = Status::kNeedMoreData;
          break;
        }
        continue;
      }
      lastStatus_ = toStatus(raw);  // kError / kUnsupported
      break;
    }
    return lastStatus_;
  }

  /**
   * Steps the decoder by exactly one start-code-delimited unit, for
   * callers that want to drive decoding manually instead of using the
   * write()+callback loop. Does not invoke the frame callback - check the
   * return value (or lastStatus()) for kFrameReady instead.
   */
  Status decodeNext() { return lastStatus_ = toStatus(decoder_.next()); }

  /// The Status returned by the most recent write() or decodeNext() call.
  Status lastStatus() const { return lastStatus_; }
  /// True if the most recent call ended in kError, kUnsupported, or kAllocationError.
  bool hasError() const {
    return lastStatus_ == Status::kError || lastStatus_ == Status::kUnsupported ||
           lastStatus_ == Status::kAllocationError;
  }

  /*
   * Valid after the frame callback fires (or decodeNext() returns
   * kFrameReady). YUV 4:2:0 planar, 8-bit.
   */
  int width() const { return decoder_.frame().width; }    ///< display picture width in luma samples
  int height() const { return decoder_.frame().height; }  ///< display picture height in luma samples
  const uint8_t* y() const { return decoder_.frame().y(); }  ///< luma plane, strideY() bytes/row
  const uint8_t* u() const { return decoder_.frame().u(); }  ///< Cb plane, strideUV() bytes/row
  const uint8_t* v() const { return decoder_.frame().v(); }  ///< Cr plane, strideUV() bytes/row
  int strideY() const { return decoder_.frame().strideY; }   ///< bytes per row of y()
  int strideUV() const { return decoder_.frame().strideC; }  ///< bytes per row of u()/v()

  /*
   * Single-sample convenience accessors, for callers that want one pixel
   * at a time rather than raw plane/stride pointer math. Not
   * bounds-checked - caller must ensure 0 <= px < width() and
   * 0 <= py < height(). Parameters are named px/py so they don't shadow
   * this class's own y() accessor when read inside these method bodies.
   */

  /// Luma (brightness) sample at pixel (px, py), full resolution.
  uint8_t getY(int px, int py) const { return y()[(size_t)py * strideY() + px]; }
  /// Cb (blue-difference chroma) sample at pixel (px, py) - subsampled 2x in each dimension (4:2:0).
  uint8_t getU(int px, int py) const {
    return u()[(size_t)(py >> 1) * strideUV() + (px >> 1)];
  }
  /// Cr (red-difference chroma) sample at pixel (px, py) - see getU() for the subsampling note.
  uint8_t getV(int px, int py) const {
    return v()[(size_t)(py >> 1) * strideUV() + (px >> 1)];
  }

  /**
   * Converts the most recently decoded picture to RGB565 (16-bit, 5-6-5
   * packed, little-endian) into a caller-provided buffer - the format
   * most embedded display libraries (TFT_eSPI, Adafruit_GFX, LovyanGFX,
   * ...) expect. Output is width()*height(). `dstCapacity` is the
   * buffer's size in uint16_t entries (not bytes); if it's smaller than
   * width()*height(), nothing is written and this returns 0. On success,
   * returns the number of uint16_t entries written. Never allocates on
   * success. See decoder/mpg_rgb.h for the exact YUV-to-RGB conversion
   * used. Also see toRGB666()/toRGB888() for 18-bit and full 24-bit color
   * instead.
   */
  size_t toRGB565(uint16_t* dst, size_t dstCapacity) const {
    size_t needed = (size_t)width() * (size_t)height();
    if (dstCapacity < needed) return 0;
    convertFrameToRgb565(decoder_.frame(), dst);
    return needed;
  }

  /**
   * Converts a `dx` x `dy` rectangle of the most recently decoded
   * picture, starting at (x, y), to RGB565 into a caller-provided buffer
   * (packed with no row padding - dst's row stride is exactly dx) -
   * useful for pushing a decoded picture to a display in tiles, or to
   * spread the conversion work across multiple loop() iterations instead
   * of one large blocking call. `x`/`y` must be even (chroma is
   * subsampled 2x) and, together with dx/dy, must stay within
   * width()/height() - not bounds-checked (only the destination buffer
   * size is). `dstCapacity` must be at least dx*dy.
   */
  size_t toRGB565(int x, int y, int dx, int dy, uint16_t* dst, size_t dstCapacity) const {
    size_t needed = (size_t)dx * (size_t)dy;
    if (dstCapacity < needed) return 0;
    convertFrameToRgb565(decoder_.frame(), x, y, dx, dy, dst);
    return needed;
  }

  /**
   * Converts the most recently decoded picture to RGB666 (18-bit color: 3
   * bytes per pixel, R/G/B order, each byte's 6 significant bits
   * left-justified in bits 7:2) into a caller-provided buffer -
   * `dstCapacity` (in uint8_t entries) must be at least width()*height()*3,
   * checked the same way as toRGB565().
   */
  size_t toRGB666(uint8_t* dst, size_t dstCapacity) const {
    size_t needed = (size_t)width() * (size_t)height() * 3;
    if (dstCapacity < needed) return 0;
    convertFrameToRgb666(decoder_.frame(), dst);
    return needed;
  }

  /// Windowed/tiled counterpart of toRGB666() - see toRGB565(x,y,dx,dy,...). `dstCapacity` must be at least dx*dy*3.
  size_t toRGB666(int x, int y, int dx, int dy, uint8_t* dst, size_t dstCapacity) const {
    size_t needed = (size_t)dx * (size_t)dy * 3;
    if (dstCapacity < needed) return 0;
    convertFrameToRgb666(decoder_.frame(), x, y, dx, dy, dst);
    return needed;
  }

  /**
   * Converts the most recently decoded picture to RGB888 (24-bit color: 3
   * bytes per pixel, R/G/B order, full 8-bit precision per channel) into
   * a caller-provided buffer - `dstCapacity` (in uint8_t entries) must be
   * at least width()*height()*3, checked the same way as toRGB565().
   */
  size_t toRGB888(uint8_t* dst, size_t dstCapacity) const {
    size_t needed = (size_t)width() * (size_t)height() * 3;
    if (dstCapacity < needed) return 0;
    convertFrameToRgb888(decoder_.frame(), dst);
    return needed;
  }

  /// Windowed/tiled counterpart of toRGB888() - see toRGB565(x,y,dx,dy,...). `dstCapacity` must be at least dx*dy*3.
  size_t toRGB888(int x, int y, int dx, int dy, uint8_t* dst, size_t dstCapacity) const {
    size_t needed = (size_t)dx * (size_t)dy * 3;
    if (dstCapacity < needed) return 0;
    convertFrameToRgb888(decoder_.frame(), x, y, dx, dy, dst);
    return needed;
  }

  /**
   * Copies the most recently decoded picture into a caller-provided
   * buffer as one tightly-packed YUV 4:2:0 planar block (standard "I420"
   * layout: Y bytes, then U bytes, then V bytes, no row padding - unlike
   * y()/u()/v() above, which point into this decoder's own internal frame
   * storage and are only valid until the next picture is decoded). Output
   * is width()*height(). `dstCapacity` (in uint8_t entries) must be at
   * least width()*height() + 2*(width()/2)*(height()/2); returns 0
   * without writing if too small, or the number of bytes written on
   * success. Never allocates.
   */
  size_t toYUV420(uint8_t* dst, size_t dstCapacity) const {
    int w = width(), h = height();
    size_t needed = (size_t)w * h + 2 * (size_t)(w / 2) * (size_t)(h / 2);
    if (dstCapacity < needed) return 0;
    convertFrameToYuv420(decoder_.frame(), dst);
    return needed;
  }

  /**
   * Windowed/tiled counterpart of toYUV420() - packs just a `dx` x `dy`
   * rectangle starting at (x, y) instead of the whole picture. `x`/`y`
   * must be even (chroma is subsampled 2x) and, together with dx/dy, must
   * stay within width()/height() - not bounds-checked (only the
   * destination buffer size is). `dstCapacity` must be at least
   * dx*dy + 2*(dx/2)*(dy/2).
   */
  size_t toYUV420(int x, int y, int dx, int dy, uint8_t* dst, size_t dstCapacity) const {
    size_t needed = (size_t)dx * dy + 2 * (size_t)(dx / 2) * (size_t)(dy / 2);
    if (dstCapacity < needed) return 0;
    convertFrameToYuv420(decoder_.frame(), x, y, dx, dy, dst);
    return needed;
  }

 protected:
  /**
   * Maps the internal Decoder's DecodeStatus onto this class's public
   * Status enum (kept separate so the internal enum can evolve without
   * changing the public API's values).
   */
  static Status toStatus(DecodeStatus s) {
    switch (s) {
      case DecodeStatus::kOk:
        return Status::kFrameReady;
      case DecodeStatus::kNeedMoreData:
        return Status::kNeedMoreData;
      case DecodeStatus::kUnsupported:
        return Status::kUnsupported;
      case DecodeStatus::kAllocationError:
        return Status::kAllocationError;
      case DecodeStatus::kError:
      default:
        return Status::kError;
    }
  }

  Decoder<Allocator> decoder_;
  FrameCallback callback_ = nullptr;
  void* userData_ = nullptr;
  Status lastStatus_ = Status::kNeedMoreData;
};

}  // namespace tinympg

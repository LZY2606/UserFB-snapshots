#pragma once
#include <stdint.h>
#include <stddef.h>
#include "mpg_bitwriter.h"
#include "mpg_color_convert.h"
#include "mpg_headers_write.h"
#include "mpg_macroblock_encode.h"
#include "../common/mpg_frame.h"
#include "../common/mpg_tables.h"
#include "../mpg_config.h"
#include "../StdAllocator.h"

/*
 * Header-only. Top-level intra-only MPEG-1 encoder: sequence_header +
 * group_of_pictures_header + picture_header + one slice per macroblock
 * row, ISO/IEC 11172-2 clause 2.4.3. See docs/scope.md's Encoder section
 * for why this is intra-only at all (no motion estimation - a deliberate,
 * documented scope decision, not a missing feature).
 *
 * Unlike Decoder<Allocator> (decoder/mpg_decoder.h), this class holds *no*
 * reconstructed picture buffer: MPEG-1 intra macroblocks have no spatial
 * prediction (docs/scope.md's Phase A section) and this encoder never
 * reconstructs a picture to predict a later one from (there is no later
 * one to predict, and no rate-control feedback loop yet either - see the
 * same section's "open decisions"). Every macroblock is encoded directly
 * from the caller's own source planes into the output bitstream, with
 * only a few bytes of per-slice state (the three DC predictors) threaded
 * through - no heap allocation in encodePicture() itself, matching this
 * library's decode-side hot-path philosophy even more literally than the
 * decoder itself needs to. The Allocator template parameter is only used
 * for the small YUV 4:2:0 scratch buffers the encodeFrameRgb888()/
 * encodeFrameRgb666()/encodeFrameRgb565()/encodeFrameYuv422() convenience
 * overloads convert into before handing off to encodePicture().
 *
 * Every encodePicture() call writes a complete, independently-decodable
 * unit (its own sequence_header/group_of_pictures_header/picture_header,
 * not just resent on the first call) - see mpg_headers_write.h's file
 * comment for why.
 */

namespace tinympg {

/// Outcome of one Encoder::encodePicture() call.
enum class EncodeStatus {
  kOk,            ///< dst now holds a complete encoded picture
  kInvalidSize,   ///< width/height weren't set, aren't multiples of 16, or exceed maxWidth()/maxHeight()
  kInvalidQp,     ///< quantizer_scale wasn't set (via setQp()) or is out of MPEG-1's 1..31 range
  kBufferTooSmall,  ///< dst didn't have room for the encoded picture
};

/**
 * Intra-only MPEG-1 (Phase 1 encoder, see docs/scope.md) encoder: call
 * setSize()/setQp() once, then encodePicture() once per picture (or once
 * total, for the "compress a single image" use case README.md mentions).
 * `Allocator` (default StdAllocator<uint8_t>, see StdAllocator.h) is only
 * used for the small YUV 4:2:0 scratch buffers the encodeFrameRgb888()/
 * encodeFrameRgb666()/encodeFrameRgb565()/encodeFrameYuv422() convenience
 * overloads convert into before handing off to encodePicture() - plain
 * encodePicture() itself still never allocates (see this file's own
 * comment above), matching this library's decode-side hot-path
 * philosophy for the codec-only path.
 */
template <typename Allocator = StdAllocator<uint8_t>>
class Encoder {
 public:
  /**
   * Sets the picture dimensions every subsequent encodePicture() call
   * uses. Must each be a multiple of 16 (no edge-padding/cropping support
   * yet, matching TinyH264Encoder's own first-cut restriction) and not
   * exceed maxWidth()/maxHeight(). Returns false (and leaves the previous
   * size, if any, in effect) if either check fails.
   */
  bool setSize(int width, int height) {
    if (width <= 0 || height <= 0) return false;
    if ((width % 16) != 0 || (height % 16) != 0) return false;
    if (width > maxWidth_ || height > maxHeight_) return false;
    width_ = width;
    height_ = height;
    mbWidth_ = width / 16;
    mbHeight_ = height / 16;
    return true;
  }
  int width() const { return width_; }
  int height() const { return height_; }

  /**
   * Overrides the MPG_MAX_WIDTH/MPG_MAX_HEIGHT compile-time defaults
   * (mpg_config.h) for this Encoder instance's own sanity bound on
   * setSize() - this class holds no picture buffer, so unlike
   * Decoder::setMaxDimension() there's no allocation-ceiling behind this,
   * just an upper limit on what setSize() will accept.
   */
  void setMaxDimension(int maxWidth, int maxHeight) {
    maxWidth_ = maxWidth;
    maxHeight_ = maxHeight;
  }
  int maxWidth() const { return maxWidth_; }
  int maxHeight() const { return maxHeight_; }

  /**
   * Sets quantizer_scale (1..31, lower = higher quality/larger output),
   * applied uniformly across the whole picture - no per-macroblock
   * adaptation and no rate control (target-bitrate mode) in this phase,
   * see docs/scope.md's Encoder section. Returns false (and leaves the
   * previous value, if any, in effect) if out of range.
   */
  bool setQp(int qscale) {
    if (qscale < 1 || qscale > 31) return false;
    qscale_ = qscale;
    return true;
  }
  int qp() const { return qscale_; }

  /**
   * Sets frame_rate_code (Table 6-4) from a whole-number fps value -
   * only the common integer rates (24/25/30/50/60) map to their own
   * code; anything else falls back to 25 (code 3), the same "pick a
   * reasonable default rather than reject" spirit as this file's header-
   * writing placeholders (see mpg_headers_write.h). Purely cosmetic for
   * this encoder's own output (nothing here paces encodePicture() calls
   * to match it) - real-time pacing is the caller's responsibility, same
   * as TinyH264Encoder.
   */
  void setFrameRate(int fps) {
    switch (fps) {
      case 24: frameRateCode_ = 2; break;
      case 25: frameRateCode_ = 3; break;
      case 30: frameRateCode_ = 5; break;
      case 50: frameRateCode_ = 6; break;
      case 60: frameRateCode_ = 8; break;
      default: frameRateCode_ = 3; break;
    }
  }

  /**
   * Overrides the row stride the encodeFrameRgb888()/encodeFrameRgb666()/
   * encodeFrameRgb565()/encodeFrameYuv422() convenience overloads assume
   * for their packed source buffer, for source planes with row padding
   * (e.g. a camera driver's own buffer layout) instead of the
   * tightly-packed default each of those methods documents. The unit is
   * whatever that specific overload's own source pointer type is (bytes
   * for RGB888/RGB666/YUYV, uint16_t entries for RGB565) - only one
   * override is stored at a time, so set it again (or back to 0) before
   * switching which overload you call next. Pass 0 to restore the
   * tightly-packed default.
   */
  void setPackedStride(int stride) { packedStride_ = stride; }
  int packedStride() const { return packedStride_; }

  /**
   * Encodes one picture from RGB888 source data (3 bytes/pixel, R/G/B
   * order - matches decoder/mpg_rgb.h's toRGB888() convention) instead of
   * separate YUV planes. Converts internally to YUV 4:2:0 (see
   * mpg_color_convert.h) before encoding - a real, if modest, precision
   * loss on top of the usual DCT quantization, not a lossless passthrough.
   * Row stride defaults to `width()*3` (tightly packed) unless overridden
   * via setPackedStride(). Requires a prior successful setSize()/setQp()
   * call, same as encodePicture(); returns kInvalidSize if either wasn't
   * called (or setSize() failed, or the scratch buffer allocation itself
   * failed - out of memory, see StdAllocator.h) and any other
   * encodePicture() failure reason otherwise, through the returned
   * EncodeStatus.
   */
  EncodeStatus encodeFrameRgb888(const uint8_t* rgb, uint8_t* dst, size_t dstCapacity,
                                  size_t* bytesWritten) {
    if (!ensureScratch()) return fail(bytesWritten, EncodeStatus::kInvalidSize);
    int stride = packedStride_ != 0 ? packedStride_ : width_ * 3;
    convertRgb888ToYuv420(rgb, stride, width_, height_, scratchY_.data(), width_,
                           scratchU_.data(), scratchV_.data(), width_ / 2);
    return encodePicture(scratchY_.data(), width_, scratchU_.data(), scratchV_.data(),
                          width_ / 2, dst, dstCapacity, bytesWritten);
  }

  /**
   * Same as encodeFrameRgb888(), for RGB666 source data (3 bytes/pixel,
   * each byte's 6 significant bits left-justified in bits 7:2 - matches
   * decoder/mpg_rgb.h's toRGB666() convention). Row stride defaults to
   * `width()*3` (tightly packed) unless overridden via setPackedStride().
   */
  EncodeStatus encodeFrameRgb666(const uint8_t* rgb666, uint8_t* dst, size_t dstCapacity,
                                  size_t* bytesWritten) {
    if (!ensureScratch()) return fail(bytesWritten, EncodeStatus::kInvalidSize);
    int stride = packedStride_ != 0 ? packedStride_ : width_ * 3;
    convertRgb666ToYuv420(rgb666, stride, width_, height_, scratchY_.data(), width_,
                           scratchU_.data(), scratchV_.data(), width_ / 2);
    return encodePicture(scratchY_.data(), width_, scratchU_.data(), scratchV_.data(),
                          width_ / 2, dst, dstCapacity, bytesWritten);
  }

  /**
   * Same as encodeFrameRgb888(), for RGB565 source data (uint16_t/pixel,
   * 5-6-5 packed - matches decoder/mpg_rgb.h's toRGB565() convention). Row
   * stride (in uint16_t entries) defaults to `width()` (tightly packed)
   * unless overridden via setPackedStride().
   */
  EncodeStatus encodeFrameRgb565(const uint16_t* rgb565, uint8_t* dst, size_t dstCapacity,
                                  size_t* bytesWritten) {
    if (!ensureScratch()) return fail(bytesWritten, EncodeStatus::kInvalidSize);
    int stride = packedStride_ != 0 ? packedStride_ : width_;
    convertRgb565ToYuv420(rgb565, stride, width_, height_, scratchY_.data(), width_,
                           scratchU_.data(), scratchV_.data(), width_ / 2);
    return encodePicture(scratchY_.data(), width_, scratchU_.data(), scratchV_.data(),
                          width_ / 2, dst, dstCapacity, bytesWritten);
  }

  /**
   * Same as encodeFrameRgb888(), for YUYV-order packed YUV 4:2:2 source
   * data (Y0 U0 Y1 V0 per horizontal pixel pair - the common
   * camera-module convention, e.g. OV2640/OV7670 output). Row stride
   * defaults to `width()*2` (tightly packed) unless overridden via
   * setPackedStride().
   */
  EncodeStatus encodeFrameYuv422(const uint8_t* yuyv, uint8_t* dst, size_t dstCapacity,
                                  size_t* bytesWritten) {
    if (!ensureScratch()) return fail(bytesWritten, EncodeStatus::kInvalidSize);
    int stride = packedStride_ != 0 ? packedStride_ : width_ * 2;
    convertYuyv422ToYuv420(yuyv, stride, width_, height_, scratchY_.data(), width_,
                            scratchU_.data(), scratchV_.data(), width_ / 2);
    return encodePicture(scratchY_.data(), width_, scratchU_.data(), scratchV_.data(),
                          width_ / 2, dst, dstCapacity, bytesWritten);
  }

  /**
   * Encodes one I-picture from raw YUV 4:2:0 planar source data (three
   * separate planes/strides, matching TinyMPGDecoder's own y()/u()/v()
   * convention) into `dst`. Requires a prior successful setSize()/setQp()
   * call. Returns kOk (with the encoded byte count in `*bytesWritten`) or
   * a specific failure reason (see EncodeStatus) - `dst` should not be
   * treated as valid/usable on anything but kOk, the same "nothing usable
   * on failure" convention TinyH264Encoder's 0-return uses, just spelled
   * out as an enum instead of overloading a size_t.
   */
  EncodeStatus encodePicture(const uint8_t* srcY, int srcStrideY, const uint8_t* srcU,
                              const uint8_t* srcV, int srcStrideC, uint8_t* dst,
                              size_t dstCapacity, size_t* bytesWritten) {
    *bytesWritten = 0;
    if (width_ <= 0 || height_ <= 0) return EncodeStatus::kInvalidSize;
    if (qscale_ <= 0) return EncodeStatus::kInvalidQp;

    BitWriter bw(dst, dstCapacity);
    writeSequenceHeader(bw, width_, height_, aspectRatioCode_, frameRateCode_);
    writeGroupOfPicturesHeader(bw);
    writePictureHeader(bw);

    for (int mbY = 0; mbY < mbHeight_; mbY++) {
      bw.startCode((uint8_t)(1 + mbY));  // slice_start_code: one slice per macroblock row
      bw.u((uint32_t)qscale_, 5);        // quantizer_scale
      bw.flag(false);                    // extra_bit_slice: none

      int32_t lastDc[3] = {128, 128, 128};  // reset per slice, clause 2.4.3.7
      for (int mbX = 0; mbX < mbWidth_; mbX++) {
        bw.writeVlc(kMbAddrIncCode[0], kMbAddrIncBits[0]);  // macroblock_address_increment = 1 (no gap)
        bw.flag(true);  // macroblock_type: intra, no quant change (fixed qscale for the whole picture)
        encodeIntraMacroblock(bw, srcY, srcStrideY, srcU, srcV, srcStrideC, mbX, mbY,
                               lastDc, kDefaultIntraMatrix, qscale_);
      }
    }
    bw.byteAlign();

    if (bw.error()) return EncodeStatus::kBufferTooSmall;
    *bytesWritten = bw.bytesWritten();
    return EncodeStatus::kOk;
  }

 private:
  /**
   * Lazily (re)allocates the scratch YUV 4:2:0 planes the
   * encodeFrameRgb888()/encodeFrameRgb666()/encodeFrameRgb565()/
   * encodeFrameYuv422() overloads convert into, sized to the current
   * width_/height_. Re-allocates if setSize() changed the dimensions
   * since the last call (tracked via scratchWidth_/scratchHeight_) -
   * everything else in this class allocates at most once, but these
   * scratch planes reasonably need to track setSize() being called more
   * than once with different dimensions on a single long-lived Encoder
   * instance. Returns false if width_/height_ aren't set yet, or if an
   * allocation failed (out of memory - see StdAllocator.h).
   */
  bool ensureScratch() {
    if (width_ <= 0 || height_ <= 0) return false;
    if (!scratchY_.empty() && scratchWidth_ == width_ && scratchHeight_ == height_) return true;
    scratchY_.release();
    scratchU_.release();
    scratchV_.release();
    bool ok = scratchY_.allocate((size_t)width_ * height_) &&
              scratchU_.allocate((size_t)(width_ / 2) * (height_ / 2)) &&
              scratchV_.allocate((size_t)(width_ / 2) * (height_ / 2));
    if (!ok) {
      scratchY_.release();
      scratchU_.release();
      scratchV_.release();
      return false;
    }
    scratchWidth_ = width_;
    scratchHeight_ = height_;
    return true;
  }

  /// Convenience for the RGB/YUV422 overloads' early-out failure paths.
  EncodeStatus fail(size_t* bytesWritten, EncodeStatus status) {
    *bytesWritten = 0;
    return status;
  }

  int width_ = 0, height_ = 0;
  int mbWidth_ = 0, mbHeight_ = 0;
  int maxWidth_ = MPG_MAX_WIDTH;
  int maxHeight_ = MPG_MAX_HEIGHT;
  int qscale_ = 0;
  uint8_t aspectRatioCode_ = 1;  // square pixels, no aspect correction
  uint8_t frameRateCode_ = 3;    // 25fps default, see setFrameRate()
  int packedStride_ = 0;         // 0 = use each overload's own tightly-packed default

  Buffer<Allocator> scratchY_, scratchU_, scratchV_;
  int scratchWidth_ = 0, scratchHeight_ = 0;
};

}  // namespace tinympg

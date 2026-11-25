#pragma once
#include "encoder/mpg_encoder.h"
#include "StdAllocator.h"
#ifdef ESP32
#include "PSRAMAllocatorESP32.h"
#endif

/*
 * TinyMPGEncoder: a minimal, header-only intra-only MPEG-1 (ISO/IEC
 * 11172-2) video encoder for microcontrollers, Phase 1 (I-picture only,
 * see docs/scope.md's Encoder section - no motion estimation, a
 * deliberate scope decision). This is the public-facing API, in the same
 * `tinympg` namespace as everything under encoder/mpg_*.h.
 *
 * Usage:
 *   using namespace tinympg;
 *
 *   TinyMPGEncoder<> encoder;
 *   encoder.setSize(176, 144);  // must be a multiple of 16
 *   encoder.setQp(8);           // 1-31, lower = higher quality/larger output
 *   uint8_t bitstream[65536];
 *   size_t n = encoder.encodeFrame(y, u, v, bitstream, sizeof(bitstream));
 *   if (n == 0) { ... }  // see encoder.lastStatus() for why
 *   // bitstream[0..n) is a complete, independently-decodable MPEG-1
 *   // elementary stream picture (sequence_header + group_of_pictures_
 *   // header + picture_header + slices) - decodable by TinyMPGDecoder,
 *   // ffmpeg, or any conformant MPEG-1 decoder.
 *
 * The Allocator template parameter (default StdAllocator<uint8_t>, see
 * StdAllocator.h) is only used for the small YUV 4:2:0 scratch buffers
 * the encodeFrameRgb888()/encodeFrameRgb666()/encodeFrameRgb565()/
 * encodeFrameYuv422() convenience overloads convert into before encoding -
 * encodeFrame()/encodeFrameExplicit() themselves still never allocate
 * (see encoder/mpg_encoder.h's file comment), matching this library's
 * decode-side hot-path philosophy for the plain YUV-in path. Pass a
 * custom allocator (e.g. PSRAMAllocatorESP32<uint8_t> on ESP32) to place
 * those scratch planes in PSRAM instead of the default heap, the same
 * way TinyMPGDecoder's own Allocator parameter works.
 */

namespace tinympg {

/**
 * Public-facing encoder API: wraps the internal Encoder (see
 * encoder/mpg_encoder.h) behind a size_t-return, TinyH264Encoder-shaped
 * interface (encodeFrame() returning 0 on failure, nothing usable in
 * `dst` in that case) rather than exposing EncodeStatus directly on the
 * hot path - lastStatus() is there for callers that want the specific
 * reason.
 * @note: the Allocator template parameter is unused in this intra-only encoder,
 * but is kept for API symmetry with TinyMPGDecoder and future expansion
 */
template <typename Allocator = StdAllocator<uint8_t>>
class TinyMPGEncoder {
 public:
  /// See Encoder::setSize() - must be called (successfully) before
  /// encodeFrame().
  bool setSize(int width, int height) {
    return encoder_.setSize(width, height);
  }
  int width() const { return encoder_.width(); }
  int height() const { return encoder_.height(); }

  /// See Encoder::setMaxDimension().
  void setMaxDimension(int maxWidth, int maxHeight) {
    encoder_.setMaxDimension(maxWidth, maxHeight);
  }
  int maxWidth() const { return encoder_.maxWidth(); }
  int maxHeight() const { return encoder_.maxHeight(); }

  /// See Encoder::setQp() - must be called (successfully) before encodeFrame().
  bool setQp(int qscale) { return encoder_.setQp(qscale); }
  int qp() const { return encoder_.qp(); }

  /// See Encoder::setFrameRate().
  void setFrameRate(int fps) { encoder_.setFrameRate(fps); }

  /// See Encoder::setPackedStride() - only relevant to the encodeFrameRgb888()/
  /// encodeFrameRgb666()/encodeFrameRgb565()/encodeFrameYuv422() overloads below.
  void setPackedStride(int stride) { encoder_.setPackedStride(stride); }
  int packedStride() const { return encoder_.packedStride(); }

  /**
   * Encodes one picture from raw YUV 4:2:0 planar source data into `dst`.
   * Returns the number of bytes written, or 0 on failure (setSize()/
   * setQp() never called or rejected, or `dst` too small) - check
   * lastStatus() for which. Nothing usable is written to `dst` on
   * failure, same convention as TinyH264Encoder::encodeFrame().
   */
  size_t encodeFrame(const uint8_t* srcY, const uint8_t* srcU,
                     const uint8_t* srcV, uint8_t* dst, size_t dstCapacity) {
    int strideY = encoder_.width();
    int strideC = encoder_.width() / 2;
    return encodeFrameExplicit(srcY, strideY, srcU, srcV, strideC, dst,
                               dstCapacity);
  }

  /**
   * Explicit-stride counterpart of encodeFrame(), for source planes with
   * row padding (e.g. a camera driver's own buffer layout) instead of
   * tightly-packed rows.
   */
  size_t encodeFrameExplicit(const uint8_t* srcY, int srcStrideY,
                             const uint8_t* srcU, const uint8_t* srcV,
                             int srcStrideC, uint8_t* dst, size_t dstCapacity) {
    size_t n = 0;
    lastStatus_ = encoder_.encodePicture(srcY, srcStrideY, srcU, srcV,
                                         srcStrideC, dst, dstCapacity, &n);
    return lastStatus_ == EncodeStatus::kOk ? n : 0;
  }

  /// The EncodeStatus of the most recent encodeFrame()/encodeFrameExplicit()
  /// call.
  EncodeStatus lastStatus() const { return lastStatus_; }

  /**
   * Same as encodeFrame(), for RGB888 source data (3 bytes/pixel, R/G/B
   * order - matches TinyMPGDecoder::toRGB888()'s convention) instead of
   * separate YUV planes. Converts internally to YUV 4:2:0 (see
   * encoder/mpg_color_convert.h) before encoding - a real, if modest,
   * precision loss on top of the usual DCT quantization, not a lossless
   * passthrough. Row stride defaults to `width()*3` (tightly packed)
   * unless overridden via setPackedStride().
   */
  size_t encodeFrameRgb888(const uint8_t* rgb, uint8_t* dst, size_t dstCapacity) {
    size_t n = 0;
    lastStatus_ = encoder_.encodeFrameRgb888(rgb, dst, dstCapacity, &n);
    return lastStatus_ == EncodeStatus::kOk ? n : 0;
  }

  /**
   * Same as encodeFrame(), for RGB666 source data (3 bytes/pixel, each
   * byte's 6 significant bits left-justified in bits 7:2 - matches
   * TinyMPGDecoder::toRGB666()'s convention). Row stride defaults to
   * `width()*3` (tightly packed) unless overridden via setPackedStride().
   */
  size_t encodeFrameRgb666(const uint8_t* rgb666, uint8_t* dst, size_t dstCapacity) {
    size_t n = 0;
    lastStatus_ = encoder_.encodeFrameRgb666(rgb666, dst, dstCapacity, &n);
    return lastStatus_ == EncodeStatus::kOk ? n : 0;
  }

  /**
   * Same as encodeFrame(), for RGB565 source data (uint16_t/pixel, 5-6-5
   * packed - matches TinyMPGDecoder::toRGB565()'s convention). Row
   * stride (in uint16_t entries) defaults to `width()` (tightly packed)
   * unless overridden via setPackedStride().
   */
  size_t encodeFrameRgb565(const uint16_t* rgb565, uint8_t* dst, size_t dstCapacity) {
    size_t n = 0;
    lastStatus_ = encoder_.encodeFrameRgb565(rgb565, dst, dstCapacity, &n);
    return lastStatus_ == EncodeStatus::kOk ? n : 0;
  }

  /**
   * Same as encodeFrame(), for YUYV-order packed YUV 4:2:2 source data
   * (Y0 U0 Y1 V0 per horizontal pixel pair - the common camera-module
   * convention, e.g. OV2640/OV7670 output). Row stride defaults to
   * `width()*2` (tightly packed) unless overridden via setPackedStride().
   */
  size_t encodeFrameYuv422(const uint8_t* yuyv, uint8_t* dst, size_t dstCapacity) {
    size_t n = 0;
    lastStatus_ = encoder_.encodeFrameYuv422(yuyv, dst, dstCapacity, &n);
    return lastStatus_ == EncodeStatus::kOk ? n : 0;
  }

 private:
  Encoder<Allocator> encoder_;
  EncodeStatus lastStatus_ = EncodeStatus::kInvalidSize;
};

}  // namespace tinympg

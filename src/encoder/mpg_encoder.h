#pragma once
#include <stdint.h>
#include <stddef.h>
#include <utility>
#include "mpg_bitwriter.h"
#include "mpg_color_convert.h"
#include "mpg_headers_write.h"
#include "mpg_inter_encode.h"
#include "mpg_macroblock_encode.h"
#include "../common/mpg_frame.h"
#include "../common/mpg_tables.h"
#include "../mpg_config.h"
#include "../StdAllocator.h"

/*
 * Header-only (templates) plus mpg_inter_encode.cpp's non-template inter
 * helpers. Top-level MPEG-1 encoder: sequence_header +
 * group_of_pictures_header + picture_header + one slice per macroblock
 * row, ISO/IEC 11172-2 clause 2.4.3. See docs/scope.md's Encoder section
 * for the scope decisions (intra-only baseline, plus an optional
 * zero-motion P mode, no general motion search - a deliberate scope line).
 *
 * Two modes share this one class. The default (inter mode off,
 * setInterMode()) is the original intra-only Phase-1 behavior: every
 * encodePicture() writes a complete, independently-decodable I-picture
 * (its own sequence/GOP/picture headers), and that path still allocates
 * nothing. setInterMode(true) adds the optional zero-motion P-picture
 * sequence mode: the first successful picture is I and the rest are
 * zero-displacement P pictures predicted from the previous
 * *reconstructed* picture (see mpg_inter_encode.h). That mode holds two
 * Frame<Allocator> picture buffers (the committed reference and the
 * in-progress reconstruction, swapped only on a fully successful encode)
 * allocated through the same Allocator template the RGB/YUYV scratch
 * planes use - the first real picture storage this encoder ever owned,
 * since intra-only MPEG-1 has no spatial prediction to keep a
 * reconstruction for.
 *
 * In the default mode every encodePicture() writes a complete,
 * independently-decodable unit; in inter mode the
 * sequence_header/group_of_pictures_header are written once, only before
 * the sequence's first I-picture (see mpg_headers_write.h's file comment
 * for both cases).
 */

namespace tinympg {

/// Outcome of one Encoder::encodePicture() call.
enum class EncodeStatus {
  kOk,            ///< dst now holds a complete encoded picture
  kInvalidSize,   ///< width/height weren't set, aren't multiples of 16, or exceed maxWidth()/maxHeight()
  kInvalidQp,     ///< quantizer_scale wasn't set (via setQp()) or is out of MPEG-1's 1..31 range
  kBufferTooSmall,  ///< dst didn't have room for the encoded picture
  kAllocationError,  ///< inter mode's reconstructed/reference Frame allocation failed (out of memory, see StdAllocator.h)
};

/**
 * MPEG-1 encoder: call setSize()/setQp() once, then encodePicture() once
 * per picture (or once total, for the "compress a single image" use case
 * README.md mentions). Intra-only by default; setInterMode(true) opts
 * into the zero-motion I/P sequence mode (see this file's top comment and
 * mpg_inter_encode.h). `Allocator` (default StdAllocator<uint8_t>, see
 * StdAllocator.h) backs the small YUV 4:2:0 scratch buffers the
 * encodeFrameRgb888()/encodeFrameRgb666()/encodeFrameRgb565()/
 * encodeFrameYuv422() convenience overloads convert into, and - in inter
 * mode only - the two reconstructed-picture Frames the P reference chain
 * needs. The plain YUV encodePicture() path in the default intra-only
 * mode still never allocates, matching the decode-side hot-path
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
   * size, if any, in effect) if either check fails. A *rejected* setSize()
   * changes nothing, including the inter-mode sequence state (it doesn't
   * interrupt a sequence in progress); a successful call whose dimensions
   * actually differ from the current ones ends the current sequence, so
   * the next successful picture starts again from an I-picture (its own
   * sequence_header/GOP header) - see setInterMode()/resetSequence().
   */
  bool setSize(int width, int height) {
    if (width <= 0 || height <= 0) return false;
    if ((width % 16) != 0 || (height % 16) != 0) return false;
    if (width > maxWidth_ || height > maxHeight_) return false;
    bool changed = (width != width_ || height != height_);
    width_ = width;
    height_ = height;
    mbWidth_ = width / 16;
    mbHeight_ = height / 16;
    if (changed) resetSequence();
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
   * Enables/disables the optional zero-motion inter (P-picture) mode -
   * off by default, in which case every encodePicture() call keeps the
   * original, fully self-contained I-picture output (identical bytes, see
   * this file's top comment). When enabled, the first *successful* picture
   * of each sequence is an I-picture and every subsequent successful
   * picture is a P-picture, predicted at zero displacement from the most
   * recently *reconstructed* picture (never the caller's raw source - see
   * encoder/mpg_inter_encode.h); sequence_header/group_of_pictures_header
   * are written once, only before the sequence's first I-picture, and
   * picture temporal_reference counts successful pictures from 0, modulo
   * 1024. Toggling the mode actually changes it, calling
   * resetSequence(), or a successful dimension-changing setSize() all end
   * the current sequence (the next successful picture restarts from I);
   * a rejected setSize() does not. No motion search/B-pictures/rate
   * control - see docs/scope.md.
   */
  void setInterMode(bool active) {
    if (interMode_ == active) return;
    interMode_ = active;
    resetSequence();
  }
  bool interMode() const { return interMode_; }

  /**
   * Ends the current picture sequence: the next successful encodePicture()
   * call starts a fresh sequence with an I-picture (and, in inter mode,
   * resends sequence_header/group_of_pictures_header and restarts
   * temporal_reference at 0), discarding any previously committed
   * reference picture. Harmless (a no-op beyond resetting the counters)
   * with inter mode off, where every picture is already independent.
   */
  void resetSequence() {
    sequenceStarted_ = false;
    haveReference_ = false;
    temporalReference_ = 0;
    refFrame_.release();
    reconFrame_.release();
  }

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
   * Encodes one picture from raw YUV 4:2:0 planar source data (three
   * separate planes/strides, matching TinyMPGDecoder's own y()/u()/v()
   * convention) into `dst`. Requires a prior successful setSize()/setQp()
   * call. Returns kOk (with the encoded byte count in `*bytesWritten`) or
   * a specific failure reason (see EncodeStatus) - `dst` should not be
   * treated as valid/usable on anything but kOk, the same "nothing usable
   * on failure" convention TinyH264Encoder's 0-return uses, just spelled
   * out as an enum instead of overloading a size_t.
   *
   * With inter mode off (the default) this is unchanged Phase-1 behavior:
   * every call writes its own sequence_header/GOP/picture header and an
   * all-intra picture, independently decodable. With inter mode on, the
   * first successful picture of a sequence is I (with the sequence/GOP
   * headers), later ones are zero-motion P-pictures referencing the last
   * successful picture's reconstruction; a failure (undersized dst, or an
   * inter-picture allocation failure) writes nothing usable, consumes no
   * temporal_reference, and leaves the committed reference untouched -
   * retrying with a bigger buffer produces byte-identical output to an
   * instance that never failed.
   */
  EncodeStatus encodePicture(const uint8_t* srcY, int srcStrideY, const uint8_t* srcU,
                              const uint8_t* srcV, int srcStrideC, uint8_t* dst,
                              size_t dstCapacity, size_t* bytesWritten) {
    *bytesWritten = 0;
    if (width_ <= 0 || height_ <= 0) return EncodeStatus::kInvalidSize;
    if (qscale_ <= 0) return EncodeStatus::kInvalidQp;

    bool isInter = interMode_ && sequenceStarted_ && haveReference_;
    if (interMode_ && !ensureInterFrames()) return EncodeStatus::kAllocationError;

    BitWriter bw(dst, dstCapacity);
    if (!interMode_ || !sequenceStarted_) {
      // sequence_header/GOP only ever precede a sequence's first I-picture.
      writeSequenceHeader(bw, width_, height_, aspectRatioCode_, frameRateCode_);
      writeGroupOfPicturesHeader(bw);
    }
    writePictureHeader(bw, temporalReference_, isInter ? 2 : 1);

    EncodeStatus st = isInter
                          ? encodePSlices(bw, srcY, srcStrideY, srcU, srcV, srcStrideC)
                          : encodeISlices(bw, srcY, srcStrideY, srcU, srcV, srcStrideC);
    if (st != EncodeStatus::kOk) return st;
    bw.byteAlign();
    if (bw.error()) return EncodeStatus::kBufferTooSmall;

    // Commit only after the whole picture encoded without error: the
    // temporal counter advances and the reference swaps only on success,
    // so a failed call leaves the sequence state exactly as it was.
    if (interMode_) {
      std::swap(refFrame_, reconFrame_);  // this picture's reconstruction is the next reference
      haveReference_ = true;
      sequenceStarted_ = true;
      temporalReference_ = (temporalReference_ + 1) & 0x3FF;  // mod 1024, inter mode only
    }
    *bytesWritten = bw.bytesWritten();
    return EncodeStatus::kOk;
  }

 private:
  /**
   * Allocates (once per current size) the two inter-mode Frame<Allocator>
   * picture buffers: refFrame_ holds the most recently committed
   * reconstruction and reconFrame_ the picture currently being built.
   * Sized via Frame::setSize() at the coded (== display here, sizes are
   * 16-multiples) dimensions; returns false on an allocator failure,
   * leaving any previously committed reference untouched (Frame::setSize()
   * only fails inside ensureAllocated(), before any write).
   */
  bool ensureInterFrames() {
    if (!refFrame_.dataY.empty() && !reconFrame_.dataY.empty()) return true;
    refFrame_.release();
    reconFrame_.release();
    // Honor this Encoder's own setMaxDimension() ceiling (Frame defaults to
    // the MPG_MAX_* compile-time bounds, see common/mpg_frame.h).
    refFrame_.setMaxDimension(maxWidth_, maxHeight_);
    reconFrame_.setMaxDimension(maxWidth_, maxHeight_);
    bool ok = refFrame_.setSize(width_, height_, width_, height_) &&
              reconFrame_.setSize(width_, height_, width_, height_);
    if (!ok) {
      refFrame_.release();
      reconFrame_.release();
    }
    return ok;
  }

  /// All-intra slice loop, inter mode off: every macroblock explicit, no skips.
  EncodeStatus encodeISlices(BitWriter& bw, const uint8_t* srcY, int srcStrideY,
                              const uint8_t* srcU, const uint8_t* srcV, int srcStrideC) {
    for (int mbY = 0; mbY < mbHeight_; mbY++) {
      bw.startCode((uint8_t)(1 + mbY));  // slice_start_code: one slice per macroblock row
      bw.u((uint32_t)qscale_, 5);        // quantizer_scale
      bw.flag(false);                    // extra_bit_slice: none

      int32_t lastDc[3] = {128, 128, 128};  // reset per slice, clause 2.4.3.7
      for (int mbX = 0; mbX < mbWidth_; mbX++) {
        bw.writeVlc(kMbAddrIncCode[0], kMbAddrIncBits[0]);  // macroblock_address_increment = 1 (no gap)
        bw.flag(true);  // macroblock_type: intra, no quant change (fixed qscale for the whole picture)
        if (interMode_) {
          // Inter-mode I-picture: reconstruct into reconFrame_ so the P
          // pictures that follow reference a decoder-identical picture.
          encodeIntraMacroblockRecon(bw, srcY, srcStrideY, srcU, srcV, srcStrideC, mbX, mbY,
                                      lastDc, kDefaultIntraMatrix, qscale_, reconFrame_);
        } else {
          encodeIntraMacroblock(bw, srcY, srcStrideY, srcU, srcV, srcStrideC, mbX, mbY,
                                 lastDc, kDefaultIntraMatrix, qscale_);
        }
      }
      if (bw.error()) return EncodeStatus::kBufferTooSmall;
    }
    return EncodeStatus::kOk;
  }

  /**
   * Zero-motion P-picture slice loop (one slice per macroblock row). The
   * row's first and last macroblock positions are always coded explicitly
   * (a no-residual position there uses the explicit-(0,0)-MV, no-CBP
   * macroblock_type rather than a skip, so a slice never starts or ends on
   * an implicit skip and even an all-zero-residual final block closes the
   * slice legally); zero-residual macroblocks in between are skipped by a
   * macroblock_address_increment gap, which the decoder reconstructs from
   * the inherited (0,0) motion vector. Nonzero blocks carry their real CBP
   * and inter coefficients (see encoder/mpg_inter_encode.h).
   */
  EncodeStatus encodePSlices(BitWriter& bw, const uint8_t* srcY, int srcStrideY,
                              const uint8_t* srcU, const uint8_t* srcV, int srcStrideC) {
    for (int mbY = 0; mbY < mbHeight_; mbY++) {
      bw.startCode((uint8_t)(1 + mbY));
      bw.u((uint32_t)qscale_, 5);
      bw.flag(false);  // extra_bit_slice: none

      int pendingSkip = 0;  // zero-CBP macroblocks accumulated since the last explicit one
      for (int mbX = 0; mbX < mbWidth_; mbX++) {
        int32_t levels[6][64];
        int cbp = computeInterMacroblock(srcY, srcStrideY, srcU, srcV, srcStrideC, refFrame_,
                                          reconFrame_, mbX, mbY, qscale_, levels);
        bool isRowEdge = (mbX == 0 || mbX == mbWidth_ - 1);
        if (cbp == 0 && !isRowEdge) {
          pendingSkip++;  // middle of the row: leave it for an address increment
          continue;
        }

        writeMbAddrIncrement(bw, pendingSkip + 1);  // gap skips + this explicit macroblock
        pendingSkip = 0;
        if (cbp == 0) {
          writeInterMotionOnlyMacroblock(bw);  // explicit zero MV, no residual
        } else {
          writeInterMacroblockSyntax(bw, cbp, levels);
        }
      }
      if (bw.error()) return EncodeStatus::kBufferTooSmall;
    }
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

  // Optional zero-motion inter mode (setInterMode()) - see
  // encoder/mpg_inter_encode.h. All stay at their fresh-sequence defaults
  // while inter mode is off, which then never touches the two Frames.
  bool interMode_ = false;
  bool sequenceStarted_ = false;  // a sequence's first I-picture has been committed
  bool haveReference_ = false;    // refFrame_ holds a committed reconstruction
  int temporalReference_ = 0;     // successful-picture counter, mod 1024
  Frame<Allocator> refFrame_;     // most recently committed reconstructed picture
  Frame<Allocator> reconFrame_;   // picture currently being reconstructed (swapped in on success)

  Buffer<Allocator> scratchY_, scratchU_, scratchV_;
  int scratchWidth_ = 0, scratchHeight_ = 0;
};

}  // namespace tinympg

#pragma once
#include <stdint.h>
#include <string.h>
#include "mpg_bitreader.h"
#include "../common/mpg_tables.h"

/*
 * Header-only. sequence_header() and picture_header() parsing (ISO/IEC
 * 11172-2 clauses 2.4.3.2/2.4.3.6). group_of_pictures_header() carries no
 * field this decoder needs (neither I- nor P-frame decode uses time_code/
 * closed_gop/broken_link), so decoder/mpg_decoder.h recognizes and skips
 * it without a dedicated parser here. Each parser only reads as many
 * fields as are actually needed, in bitstream order - this project's
 * start-code framing (see mpg_decoder.h) already bounds each unit to its
 * own byte range, so unlike H.264's RBSP-continuation parsing there's no
 * need to consume every trailing bit just to find the next syntax
 * element.
 */

namespace tinympg {

/// Parsed sequence_header() fields this decoder needs, plus the active (custom or default) quantizer matrices.
struct SequenceInfo {
  bool valid = false;
  uint32_t width = 0;   ///< horizontal_size_value: display luma width
  uint32_t height = 0;  ///< vertical_size_value: display luma height
  uint32_t codedWidth = 0;   ///< width rounded up to a multiple of 16
  uint32_t codedHeight = 0;  ///< height rounded up to a multiple of 16
  uint32_t mbWidth = 0;      ///< codedWidth / 16
  uint32_t mbHeight = 0;     ///< codedHeight / 16
  uint8_t aspectRatioCode = 0;  ///< index into kAspectRatioTable
  uint8_t frameRateCode = 0;    ///< index into kFrameRateTable
  uint16_t intraMatrix[64];     ///< raster order, from load_intra_quantizer_matrix or the default
  uint16_t nonIntraMatrix[64];  ///< raster order, from load_non_intra_quantizer_matrix or the default - used by P-picture inter blocks (Phase B, see mpg_macroblock.h::decodeInterBlock())
};

/**
 * Parses sequence_header() (clause 2.4.3.2) through
 * load_non_intra_quantizer_matrix. `br` must start at the first bit after
 * the sequence_header_code itself (the 4-byte start code that identified
 * this as a sequence_header unit, see mpg_decoder.h). Returns false (and
 * leaves *seq unchanged) on a structurally invalid header (zero width/
 * height, or a bitstream that runs out of bits before
 * load_non_intra_quantizer_matrix).
 */
inline bool parseSequenceHeader(BitReader& br, SequenceInfo* seq) {
  SequenceInfo s;
  s.width = br.u(12);
  s.height = br.u(12);
  if (s.width == 0 || s.height == 0) return false;
  s.codedWidth = (s.width + 15) & ~15u;
  s.codedHeight = (s.height + 15) & ~15u;
  s.mbWidth = s.codedWidth / 16;
  s.mbHeight = s.codedHeight / 16;
  s.aspectRatioCode = (uint8_t)br.u(4);
  s.frameRateCode = (uint8_t)br.u(4);
  br.u(18);       // bit_rate_value: informational only, not needed
  br.skipBits(1); // marker_bit
  br.u(10);       // vbv_buffer_size: informational only, not needed
  br.skipBits(1); // constrained_parameters_flag

  if (br.flag()) {  // load_intra_quantizer_matrix
    for (int i = 0; i < 64; i++) {
      s.intraMatrix[kZigzag[i]] = (uint16_t)br.u(8);
    }
  } else {
    memcpy(s.intraMatrix, kDefaultIntraMatrix, sizeof(s.intraMatrix));
  }

  if (br.flag()) {  // load_non_intra_quantizer_matrix
    for (int i = 0; i < 64; i++) {
      s.nonIntraMatrix[kZigzag[i]] = (uint16_t)br.u(8);
    }
  } else {
    memcpy(s.nonIntraMatrix, kDefaultNonIntraMatrix, sizeof(s.nonIntraMatrix));
  }

  if (br.error()) return false;
  s.valid = true;
  *seq = s;
  return true;
}

/// Parsed picture_header() fields this decoder needs.
struct PictureInfo {
  uint16_t temporalReference = 0;
  uint8_t codingType = 0;  ///< picture_coding_type: 1=I, 2=P, 3=B, 4=D
  bool fullPelForwardVector = false;   ///< P/B only: forward motion vectors are in full-pel (not half-pel) units
  uint8_t forwardFCode = 0;            ///< P/B only: 1..7, scales motion_residual's raw-bit width (Table 6-driven, clause 2.4.3.6)
  bool fullPelBackwardVector = false;  ///< B only: backward motion vectors are in full-pel units
  uint8_t backwardFCode = 0;           ///< B only: 1..7, same role as forwardFCode for backward motion vectors
};

/**
 * Parses picture_header() (clause 2.4.3.6): temporal_reference,
 * picture_coding_type, vbv_delay (read and discarded - not needed by
 * this decoder), and - for P- and B-pictures - full_pel_forward_vector/
 * forward_f_code, plus - for B-pictures only - full_pel_backward_vector/
 * backward_f_code (needed to decode motion vectors, see
 * decoder/mpg_motion.h). `br` must start at the first bit after the
 * picture_start_code itself. Returns false on a structurally invalid
 * header (out-of-range picture_coding_type, forbidden f_code == 0 for a
 * direction this picture type actually uses, or truncated bits).
 */
inline bool parsePictureHeader(BitReader& br, PictureInfo* pic) {
  PictureInfo p;
  p.temporalReference = (uint16_t)br.u(10);
  p.codingType = (uint8_t)br.u(3);
  if (br.error() || p.codingType < 1 || p.codingType > 4) return false;
  br.u(16);  // vbv_delay: informational only, not needed

  if (p.codingType == 2 || p.codingType == 3) {  // P or B
    p.fullPelForwardVector = br.flag();
    p.forwardFCode = (uint8_t)br.u(3);
    if (br.error()) return false;
    if (p.codingType == 2 && p.forwardFCode == 0) return false;  // forbidden, clause 2.4.3.6
  }
  if (p.codingType == 3) {  // B only
    p.fullPelBackwardVector = br.flag();
    p.backwardFCode = (uint8_t)br.u(3);
    if (br.error() || p.backwardFCode == 0) return false;  // forbidden, clause 2.4.3.6
  }

  if (br.error()) return false;
  *pic = p;
  return true;
}

}  // namespace tinympg

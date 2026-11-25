#pragma once
#include <stdint.h>
#include "mpg_bitwriter.h"

/*
 * Header-only. sequence_header()/group_of_pictures_header()/
 * picture_header() writing (ISO/IEC 11172-2 clauses 2.4.3.2/2.4.3.5/
 * 2.4.3.6) - the write-side counterpart of decoder/mpg_headers.h's
 * parsers, but not a strict mirror of its *scope*: the decoder only reads
 * the fields it actually needs (documented in that file's own comment),
 * while a real bitstream needs every mandatory field written with a
 * legal value, including several the decoder itself discards
 * (vbv_delay, bit_rate_value, ...) - those get spec-defined "unspecified"
 * placeholder values (see writeSequenceHeader()/writePictureHeader()'s
 * own comments), not arbitrary ones, so a real decoder's VBV modeling
 * isn't confused by a value that looks like a real (but wrong) claim
 * about this encoder's output bitrate.
 *
 * This encoder emits one self-contained sequence_header + a single-
 * picture group_of_pictures_header + picture_header ahead of every
 * encoded picture (see encoder/mpg_encoder.h) rather than only resending
 * them on the first call/a resolution change - the simplest structure
 * that makes every encoded picture independently decodable on its own,
 * matching this library's "compression of a single image" use case
 * (README.md) as well as a real video sequence.
 */

namespace tinympg {

namespace detail {
static const uint8_t kEncStartCodeSequenceHeader = 0xB3;
static const uint8_t kEncStartCodeGroupStart = 0xB8;
static const uint8_t kEncStartCodePicture = 0x00;
}  // namespace detail

/**
 * Writes sequence_header() through load_non_intra_quantizer_matrix,
 * always with both load flags 0 (this encoder always uses the default
 * intra/non-intra quantizer matrices - see docs/encoder-scope planning:
 * custom matrix support isn't Phase-1 scope). `width`/`height` are the
 * *display* dimensions (must already be multiples of 16 - Phase 1 has no
 * edge-padding/cropping support, see Encoder::setSize()). `bitRateValue`
 * 0x3FFFF and `vbvBufferSize` 0x3FF are ISO/IEC 11172-2's own
 * "unspecified/at least this much" escape values (the spec-conformant
 * placeholder to use when - as here - this encoder does no real rate
 * control to report an honest figure for), not arbitrary numbers.
 * `constrained_parameters_flag` is always written 1 - this encoder's
 * whole scope is "constrained parameters" MPEG-1 (progressive, 4:2:0, no
 * custom matrices), matching docs/scope.md's Phase A framing on the
 * decode side.
 */
inline void writeSequenceHeader(BitWriter& bw, int width, int height,
                                 uint8_t aspectRatioCode, uint8_t frameRateCode) {
  bw.startCode(detail::kEncStartCodeSequenceHeader);
  bw.u((uint32_t)width, 12);
  bw.u((uint32_t)height, 12);
  bw.u(aspectRatioCode, 4);
  bw.u(frameRateCode, 4);
  bw.u(0x3FFFF, 18);  // bit_rate_value: "unspecified" escape, no rate control here
  bw.flag(true);      // marker_bit
  bw.u(0x3FF, 10);     // vbv_buffer_size_value: "at least this much" escape
  bw.flag(true);       // constrained_parameters_flag
  bw.flag(false);      // load_intra_quantizer_matrix: use the default matrix
  bw.flag(false);      // load_non_intra_quantizer_matrix: use the default matrix
}

/**
 * Writes group_of_pictures_header() with a fixed 00:00:00:00 time_code
 * (a common, always-valid placeholder real streams also use for their
 * first/only GOP), closed_gop = 1 (accurate here: this encoder always
 * writes exactly one self-contained I-picture per GOP, so nothing in it
 * ever depends on a previous GOP), and broken_link = 0 (only meaningful
 * when closed_gop == 0).
 */
inline void writeGroupOfPicturesHeader(BitWriter& bw) {
  bw.startCode(detail::kEncStartCodeGroupStart);
  bw.flag(false);  // drop_frame_flag
  bw.u(0, 5);       // time_code_hours
  bw.u(0, 6);       // time_code_minutes
  bw.flag(true);    // marker_bit
  bw.u(0, 6);       // time_code_seconds
  bw.u(0, 6);       // time_code_pictures
  bw.flag(true);    // closed_gop
  bw.flag(false);   // broken_link
}

/**
 * Writes picture_header() for an I-picture (picture_coding_type always 1
 * - this encoder never writes anything else, see docs/scope.md's Encoder
 * section). `temporalReference` is always 0 here (every GOP holds exactly
 * one picture - see writeGroupOfPicturesHeader()'s own comment - so
 * there's never a second picture in the same GOP to distinguish). No
 * full_pel_forward_vector/forward_f_code follow (P/B-picture-only
 * fields, clause 2.4.3.6). `vbv_delay` 0xFFFF is the spec's own
 * "unspecified" escape value, the picture-level counterpart of
 * writeSequenceHeader()'s bit_rate_value placeholder.
 */
inline void writePictureHeader(BitWriter& bw) {
  bw.startCode(detail::kEncStartCodePicture);
  bw.u(0, 10);      // temporal_reference
  bw.u(1, 3);       // picture_coding_type = I
  bw.u(0xFFFF, 16); // vbv_delay: "unspecified" escape
}

}  // namespace tinympg

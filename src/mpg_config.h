#pragma once
/*
 * Compile-time resource limits. Tuned for a plain ESP32 (no/little PSRAM).
 *
 * Decoder<Allocator> (see decoder/mpg_decoder.h) always holds three
 * resident picture buffers: curFrame_ (the picture currently being
 * reconstructed) and two held references, refPast_/refFuture_ - a
 * B-picture predicts from the anchor (I/P) picture immediately before and
 * after it in *display* order (clause 7.6.1, Phase C - see docs/scope.md),
 * both of which must be held simultaneously; a P-picture only ever
 * predicts from refFuture_ (Phase B). Unlike TinyH264's
 * H264_MAX_REF_FRAMES (a runtime-adjustable *count*, sizing an array for
 * H.264's reference picture list), MPEG-1 needs exactly this fixed pair
 * regardless of GOP structure, so there's no equivalent
 * MPG_MAX_REF_FRAMES/setMaxRefFrames() knob here - nothing to size an
 * array by, just two named members. All three buffers are allocated even
 * for an all-intra stream that never uses refPast_/refFuture_, since
 * Phase A/B/C share one decode path. Frame's Y/U/V planes are 3 separate
 * heap allocations each (see common/mpg_frame.h), not one merged buffer -
 * same heap-fragmentation rationale TinyH264 documents for its own Frame.
 *
 * CIF (352x288): 152064 bytes/frame (101376 Y + 25344 U + 25344 V), so
 * 456192 bytes for all three resident buffers. QCIF (176x144) is 1/4 of
 * that. Lower MPG_MAX_WIDTH/MPG_MAX_HEIGHT (via -D or #define before
 * including any TinyMPG header), or call
 * TinyMPGDecoder::setMaxDimension() at runtime, to fit a smaller budget.
 */

#ifndef MPG_MAX_WIDTH
#define MPG_MAX_WIDTH 352
#endif

#ifndef MPG_MAX_HEIGHT
#define MPG_MAX_HEIGHT 288
#endif

// Macroblocks are always 16x16 luma / 8x8 chroma in MPEG-1's mandatory 4:2:0.
#define MPG_MB_SIZE 16
#define MPG_MAX_MB_WIDTH ((MPG_MAX_WIDTH + MPG_MB_SIZE - 1) / MPG_MB_SIZE)
#define MPG_MAX_MB_HEIGHT ((MPG_MAX_HEIGHT + MPG_MB_SIZE - 1) / MPG_MB_SIZE)
#define MPG_MAX_MBS (MPG_MAX_MB_WIDTH * MPG_MAX_MB_HEIGHT)

/*
 * Defensive upper bound on how many bytes of a single start-code-delimited
 * unit (sequence_header, picture_header, or one slice's worth of
 * macroblocks) this decoder will process at once. Unlike TinyH264's
 * H264_MAX_NAL_SIZE, this is not a scratch-buffer size - MPEG-1 has no
 * emulation-prevention layer to strip, so units are parsed directly out of
 * the caller's own buffer (see decoder/mpg_decoder.h) - it exists purely to
 * reject a pathologically large unit (corrupt stream) rather than spending
 * unbounded time on it.
 */
#ifndef MPG_MAX_ES_BUFFER
#define MPG_MAX_ES_BUFFER 262144
#endif

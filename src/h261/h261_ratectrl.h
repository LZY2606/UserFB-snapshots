/*
Copyright (c) 2013, Ralf Willenbacher
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in
   the documentation and/or other materials provided with the
   distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
*/

#ifndef __H261_RATECTRL_H__
#define __H261_RATECTRL_H__

Void h261_init_ratectrl( h261_context_t *ps_ctx, Int32 i_quant, Int32 i_bitrate, Int32 i_vbv_size, Int32 i_mode );

Void h261_ratectrl_init_picture( h261_context_t *ps_ctx, h261_picture_parameters_t *ps_picture );

Void h261_ratectrl_update_picture( h261_context_t *ps_ctx, Int32 i_bitstream_length );

Void h261_ratectrl_collect_picture_stats( h261_context_t *ps_ctx, h261_picture_parameters_t *ps_picture );

Void h261_ratectrl_finish( h261_context_t *ps_ctx );


/*
 * GOB budget mode.
 *
 * In this optional mode every GOB is trial encoded once for each base
 * GQUANT 1..31, always starting from the same committed encoder state.
 * The candidate with the smallest reconstruction SSE that still fits the
 * link budget / buffer constraint is committed.  The original fixed
 * quantiser and VBR modes remain available and are byte compatible.
 *
 * Units used below:
 *  - bitrate      : bits per second, valid 1..100000000.
 *  - fps_num/den  : frame rate rational fps_num / fps_den in frames/s;
 *                   both terms are 1..1000000 and 1 <= fps_num/fps_den <= 120.
 *  - buffer_bits  : bits (not bytes), valid 1..1000000000.
 *  - all stats "*_bits"/"bits"/"quota"/"coded_total"/"target_total" are
 *                   counts of effective bits; sub-byte writer residue that
 *                   is carried across frames is counted exactly once and
 *                   final byte padding is never counted.
 *  - "sse"        : sum of squared sample errors over the GOB Y/Cb/Cr area
 *                   (reconstruction versus the original input), unit
 *                   sample-value^2.
 *  - debt_after   : D after settling this group, in bits; it is not reset
 *                   at frame boundaries and is never clamped to zero.
 *
 * overflow is 0 when at least one candidate satisfied
 * D_before + bits - quota <= buffer_bits, and 1 when every candidate
 * overflowed the buffer.  In the overflow case the smallest-bitstream
 * candidate is still committed as a complete, legal GOB: frames are never
 * dropped and the bitstream is never truncated.
 *
 * The stats record passed to the observe callback is owned by the encoder
 * instance and is only borrowed by the callback for the duration of that
 * call; it must not be retained or freed.  The callback may be NULL.  The
 * opaque pointer is stored verbatim in the instance and forwarded to the
 * callback.
 */
typedef struct h261_gob_budget_stats_s
{
	Int32 frame_num;     /* input picture parameter frame number */
	Int32 gob_number;    /* GOB number parsed from the coded GOB header */
	Int32 gquant;        /* committed base GQUANT, 1..31 */
	Int32 overflow;      /* 1 if no candidate fit buffer_bits, else 0 */
	Int32 trials;        /* number of real candidate encodings performed */

	Int64 bits;          /* effective bits of this group (picture header on the first GOB of a frame) */
	Int64 quota;         /* budget increment for this group, bits */
	Int64 coded_total;   /* cumulative effective bits since init, bits */
	Int64 target_total;  /* cumulative budget floor(K*bitrate*fps_den/(fps_num*G)), bits */
	Int64 debt_after;    /* D after settling this group, bits */
	Int64 sse;           /* selected candidate Y/Cb/Cr SSE, sample-value^2 */
} h261_gob_budget_stats_t;

typedef Void ( *h261_gob_budget_observe_fn )( const h261_gob_budget_stats_t *ps_stats, Void *pv_opaque );

typedef struct h261_gob_budget_config_s
{
	Int32 bitrate;       /* bits/s, 1..100000000 */
	Int32 fps_num;       /* frame rate numerator, 1..1000000 */
	Int32 fps_den;       /* frame rate denominator, 1..1000000 */
	Int32 buffer_bits;   /* buffer limit in bits, 1..1000000000 */
	h261_gob_budget_observe_fn observe; /* optional per-GOB callback, may be NULL */
	Void *opaque;        /* forwarded to observe, may be NULL */
} h261_gob_budget_config_t;

/*
 * Enable/restart GOB budget mode.  Returns 1 on success and restarts the
 * budget history (K, debt and cumulative counters start at zero).  Returns
 * 0 without changing any control state when ps_config is NULL or contains
 * invalid values.  Calling h261_init_ratectrl() afterwards disables GOB
 * budget mode again.
 */
int h261_ratectrl_init_gob_budget( h261_context_t *ps_ctx, const h261_gob_budget_config_t *ps_config );

/* Releases GOB budget workspace (safe to call when disabled). */
Void h261_gob_budget_destroy( h261_context_t *ps_ctx );

/* Internal encoder-core hooks. */
Void h261_gob_budget_record_picture_header( h261_context_t *ps_ctx, Int32 i_before_bits, Int32 i_after_bits );
Void h261_gob_budget_capture_frame_num( h261_context_t *ps_ctx, Int32 i_frame_num );
Void h261_gob_budget_encode_gobs( h261_context_t *ps_ctx );

/* Test/clock support: advance the exact cumulative-budget clock by one
 * group without encoding.  Returns the new target total and, when
 * pi64_quota is non-NULL, the group quota.  No-op returning 0 when disabled. */
Int64 h261_gob_budget_debug_clock_tick( h261_context_t *ps_ctx, Int64 *pi64_quota );

#endif /* __H261_RATECTRL_H__ */

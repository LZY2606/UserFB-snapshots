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

Void h261_init_ratectrl( h261_context_t *ps_ctx, Int32 i_quant, Int32 i_bitrate, Int32 i_vbv_size, Int32 i_mode );

Void h261_ratectrl_init_picture( h261_context_t *ps_ctx, h261_picture_parameters_t *ps_picture );

Void h261_ratectrl_update_picture( h261_context_t *ps_ctx, Int32 i_bitstream_length );

Void h261_ratectrl_collect_picture_stats( h261_context_t *ps_ctx, h261_picture_parameters_t *ps_picture );

Void h261_ratectrl_finish( h261_context_t *ps_ctx );


/*
 * Optional per-GOB budget rate control mode.
 *
 * When enabled, every GOB is trial encoded with each base GQUANT from 1 to
 * 31; the candidate with the smallest reconstruction SSE that still fits the
 * link buffer is committed. The fixed-quantizer (H261_RATECTRL_MODE_QUANT)
 * and whole-frame VBR (H261_RATECTRL_MODE_VBR) modes are unaffected. The
 * mode is switched off again by calling h261_init_ratectrl().
 */

/* Per-GOB statistics handed to the observe callback. */
typedef struct h261_gob_budget_stats_s {
	Int32 i_frame_num;       /* input frame number of the picture */
	Int32 i_gob_number;      /* GOB number as written into the bitstream (1..12) */
	Int32 i_gquant;          /* base GQUANT of the committed candidate (1..31) */
	Int32 i_overflow;        /* 1 if no candidate fit the buffer, else 0 */
	Int32 i_trials;          /* number of real candidate encodings for this GOB */
	Int64 i_bits;            /* effective bits spent on this GOB */
	Int64 i_quota;           /* budget quota granted to this GOB, in bits */
	Int64 i_target_total;    /* cumulative link budget after this GOB, in bits */
	Int64 i_coded_total;     /* cumulative effective bits after this GOB */
	Int64 i_debt_after;      /* settlement debt D after this GOB, in bits */
	Int64 i_sse;             /* Y/Cb/Cr reconstruction SSE of the committed GOB */
} h261_gob_budget_stats_t;

typedef Void ( *h261_gob_budget_observe_t )( const h261_gob_budget_stats_t *ps_stats, Void *p_opaque );

typedef struct {
	Int32 i_bitrate;                         /* link bitrate, 1..100000000 bit/s */
	Int32 i_fps_num;                         /* frame rate numerator, 1..1000000 */
	Int32 i_fps_den;                         /* frame rate denominator, 1..1000000 */
	Int32 i_buffer_bits;                     /* buffer size, 1..1000000000 bits */
	h261_gob_budget_observe_t pf_observe;    /* may be NULL */
	Void *p_opaque;                          /* passed back to pf_observe */
} h261_gob_budget_config_t;

/*
 * Enable the GOB budget mode. Returns 1 on success, 0 if ps_config is NULL or
 * any field is out of range (in which case the control state is untouched).
 * On success the budget history (debt, cumulative counters) is restarted.
 * Reinitialising with h261_init_ratectrl() disables the mode again.
 * The configured frame rate only drives the budget clock; it does not change
 * the meaning of temporal_reference.
 */
Int32 h261_ratectrl_init_gob_budget( h261_context_t *ps_ctx, const h261_gob_budget_config_t *ps_config );

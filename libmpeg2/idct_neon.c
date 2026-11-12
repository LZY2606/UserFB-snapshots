/*
 * idct_neon.c
 * Copyright (C) 2026
 *
 * AArch64 NEON IDCT kernel for libmpeg2.
 *
 * This file is part of mpeg2dec, a free MPEG-2 video stream decoder.
 * See http://libmpeg2.sourceforge.net/ for updates.
 *
 * mpeg2dec is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * mpeg2dec is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "config.h"

#if defined(ARCH_AARCH64) && defined(__aarch64__)

#include <inttypes.h>

#include "mpeg2.h"
#include "attributes.h"
#include "mpeg2_internal.h"

#include <arm_neon.h>

/*
 * Coefficient layout
 * ------------------
 * mpeg2_idct_init() runs the generic setup before installing these
 * kernels, and that setup rotates the zig-zag scans:
 *
 *     j = mpeg2_scan_norm[i];
 *     mpeg2_scan_norm[i] = ((j & 0x36) >> 1) | ((j & 0x09) << 2);
 *
 * The slice parser stores coefficients through decoder->scan, so
 * decoder->DCTblock is not in natural (row, column) order.  These kernels
 * make no assumption about the layout: like idct.c they treat block[0..7]
 * as one transform axis (the row pass) and block[8*k] as the other (the
 * column pass).  Feeding them the same array the C kernels read is what
 * makes the output bit-identical.
 *
 * Arithmetic
 * ----------
 * Each lane reproduces idct_row()/idct_col() exactly:
 *
 *   - the BUTTERFLY regrouping W0*(d0+d1) + (W1-W0)*d1 keeps the same
 *     intermediate magnitudes and the same 32-bit wrap behaviour;
 *   - the ((t0 + t1) >> 8) * 181 step sits at the same point;
 *   - >> 12 for the row pass, >> 17 for the column pass;
 *   - a truncating 32->16 narrow after each pass (C assigns to int16_t);
 *   - the per-row DC shortcut (uint16_t)(block[0] >> 1), which is not
 *     the same value as the full butterfly and must not be replaced by it;
 *   - the DC fast path of idct_add, which clears only block[0] and
 *     block[63].
 */

#define W1 2841
#define W2 2676
#define W3 2408
#define W5 1609
#define W6 1108
#define W7 565

#define W2_M_W6 (W2 - W6)
#define W2_P_W6 (W2 + W6)
#define W1_M_W7 (W1 - W7)
#define W1_P_W7 (W1 + W7)
#define W5_M_W3 (W5 - W3)
#define W5_P_W3 (W5 + W3)

/* Invocation counters, read by tests/aarch64_contract.c. */
unsigned long mpeg2_neon_idct_calls[2];	/* [0] copy, [1] add */

static inline void transpose8x8_s16 (int16x8_t x[8])
{
    int16x8_t t0, t1, t2, t3, t4, t5, t6, t7;
    int16x8_t u0, u1, u2, u3, u4, u5, u6, u7;
    int16x8_t v0, v1, v2, v3, v4, v5, v6, v7;

    t0 = vtrn1q_s16 (x[0], x[1]);
    t1 = vtrn2q_s16 (x[0], x[1]);
    t2 = vtrn1q_s16 (x[2], x[3]);
    t3 = vtrn2q_s16 (x[2], x[3]);
    t4 = vtrn1q_s16 (x[4], x[5]);
    t5 = vtrn2q_s16 (x[4], x[5]);
    t6 = vtrn1q_s16 (x[6], x[7]);
    t7 = vtrn2q_s16 (x[6], x[7]);

    u0 = vtrn1q_s32 (t0, t2);
    u1 = vtrn2q_s32 (t0, t2);
    u2 = vtrn1q_s32 (t1, t3);
    u3 = vtrn2q_s32 (t1, t3);
    u4 = vtrn1q_s32 (t4, t6);
    u5 = vtrn2q_s32 (t4, t6);
    u6 = vtrn1q_s32 (t5, t7);
    u7 = vtrn2q_s32 (t5, t7);

    v0 = vtrn1q_s64 (u0, u4);
    v1 = vtrn2q_s64 (u0, u4);
    v2 = vtrn1q_s64 (u1, u5);
    v3 = vtrn2q_s64 (u1, u5);
    v4 = vtrn1q_s64 (u2, u6);
    v5 = vtrn2q_s64 (u2, u6);
    v6 = vtrn1q_s64 (u3, u7);
    v7 = vtrn2q_s64 (u3, u7);

    /* This vtrn ladder emits columns in bit-reversed order
       (0, 4, 2, 6, 1, 5, 3, 7); reorder to a true transpose. */
    x[0] = v0; x[4] = v1; x[2] = v2; x[6] = v3;
    x[1] = v4; x[5] = v5; x[3] = v6; x[7] = v7;
}

/*
 * The 1-D transform, four independent lanes per call.  in[k] holds the
 * k-th tap of four parallel transforms; out[k] receives the k-th output.
 * Identical arithmetic to idct_row()/idct_col(); only the DC bias and the
 * final shift differ between the two passes, so those are compile-time
 * macro parameters (NEON immediate shifts must be constants).
 */
#define DEFINE_IDCT_1D(name, bias, shift)				\
static inline void name (int32x4_t in[8], int32x4_t out[8])		\
{									\
    int32x4_t d0, d1, d2, d3;						\
    int32x4_t a0, a1, a2, a3, b0, b1, b2, b3;				\
    int32x4_t t0, t1, t2, t3, tmp;					\
									\
    d0 = vaddq_s32 (vshlq_n_s32 (in[0], 11), vdupq_n_s32 (bias));	\
    d1 = in[1];								\
    d2 = vshlq_n_s32 (in[2], 11);					\
    d3 = in[3];								\
    t0 = vaddq_s32 (d0, d2);						\
    t1 = vsubq_s32 (d0, d2);						\
    tmp = vmulq_n_s32 (vaddq_s32 (d3, d1), W6);				\
    t2 = vaddq_s32 (tmp, vmulq_n_s32 (d1, W2_M_W6));			\
    t3 = vsubq_s32 (tmp, vmulq_n_s32 (d3, W2_P_W6));			\
    a0 = vaddq_s32 (t0, t2);						\
    a1 = vaddq_s32 (t1, t3);						\
    a2 = vsubq_s32 (t1, t3);						\
    a3 = vsubq_s32 (t0, t2);						\
									\
    d0 = in[4];								\
    d1 = in[5];								\
    d2 = in[6];								\
    d3 = in[7];								\
    tmp = vmulq_n_s32 (vaddq_s32 (d3, d0), W7);				\
    t0 = vaddq_s32 (tmp, vmulq_n_s32 (d0, W1_M_W7));			\
    t1 = vsubq_s32 (tmp, vmulq_n_s32 (d3, W1_P_W7));			\
    tmp = vmulq_n_s32 (vaddq_s32 (d1, d2), W3);				\
    t2 = vaddq_s32 (tmp, vmulq_n_s32 (d2, W5_M_W3));			\
    t3 = vsubq_s32 (tmp, vmulq_n_s32 (d1, W5_P_W3));			\
    b0 = vaddq_s32 (t0, t2);						\
    b3 = vaddq_s32 (t1, t3);						\
    t0 = vsubq_s32 (t0, t2);						\
    t1 = vsubq_s32 (t1, t3);						\
    b1 = vmulq_n_s32 (vshrq_n_s32 (vaddq_s32 (t0, t1), 8), 181);	\
    b2 = vmulq_n_s32 (vshrq_n_s32 (vsubq_s32 (t0, t1), 8), 181);	\
									\
    out[0] = vshrq_n_s32 (vaddq_s32 (a0, b0), shift);			\
    out[1] = vshrq_n_s32 (vaddq_s32 (a1, b1), shift);			\
    out[2] = vshrq_n_s32 (vaddq_s32 (a2, b2), shift);			\
    out[3] = vshrq_n_s32 (vaddq_s32 (a3, b3), shift);			\
    out[4] = vshrq_n_s32 (vsubq_s32 (a3, b3), shift);			\
    out[5] = vshrq_n_s32 (vsubq_s32 (a2, b2), shift);			\
    out[6] = vshrq_n_s32 (vsubq_s32 (a1, b1), shift);			\
    out[7] = vshrq_n_s32 (vsubq_s32 (a0, b0), shift);			\
}

DEFINE_IDCT_1D (idct_row_pass, 2048, 12)
DEFINE_IDCT_1D (idct_col_pass, 65536, 17)

/*
 * Reconstruct the eight rows from the coefficient workspace.  The caller
 * owns the workspace; this never writes to it.
 */
static inline void idct_to_rows (const int16_t * block, int16x8_t rows[8])
{
    int16x8_t x[8], u[8];
    int32x4_t in_lo[8], in_hi[8], out_lo[8], out_hi[8];
    int16x8_t zero_mask;
    int16x8_t dc16;
    int32x4_t zero_lo, zero_hi, dc_lo, dc_hi;
    int i;

    /* rows -> one vector of eight rows per tap (columns) */
    for (i = 0; i < 8; i++)
	x[i] = vld1q_s16 (block + 8 * i);
    transpose8x8_s16 (x);

    /* C shortcut: the whole AC part of a row must be zero. */
    zero_mask = vorrq_s16 (vorrq_s16 (vorrq_s16 (x[1], x[2]),
				      vorrq_s16 (x[3], x[4])),
			   vorrq_s16 (vorrq_s16 (x[5], x[6]), x[7]));
    zero_mask = vceqq_s16 (zero_mask, vdupq_n_s16 (0));

    dc16 = vshrq_n_s16 (x[0], 1);	/* (int16_t) of (uint16_t) (DC >> 1) */
    zero_lo = vmovl_s16 (vget_low_s16 (zero_mask));
    zero_hi = vmovl_high_s16 (zero_mask);
    dc_lo = vmovl_s16 (vget_low_s16 (dc16));
    dc_hi = vmovl_high_s16 (dc16);

    for (i = 0; i < 8; i++) {
	in_lo[i] = vmovl_s16 (vget_low_s16 (x[i]));
	in_hi[i] = vmovl_high_s16 (x[i]);
    }

    idct_row_pass (in_lo, out_lo);
    idct_row_pass (in_hi, out_hi);
    for (i = 0; i < 8; i++) {
	out_lo[i] = vbslq_s32 (zero_lo, dc_lo, out_lo[i]);
	out_hi[i] = vbslq_s32 (zero_hi, dc_hi, out_hi[i]);
	/* truncating narrow, matching the assignment to int16_t block[] */
	u[i] = vcombine_s16 (vmovn_s32 (out_lo[i]), vmovn_s32 (out_hi[i]));
    }

    /* now one vector per row, holding the eight row-pass results */
    transpose8x8_s16 (u);

    for (i = 0; i < 8; i++) {
	in_lo[i] = vmovl_s16 (vget_low_s16 (u[i]));
	in_hi[i] = vmovl_high_s16 (u[i]);
    }
    idct_col_pass (in_lo, out_lo);
    idct_col_pass (in_hi, out_hi);

    for (i = 0; i < 8; i++)
	rows[i] = vcombine_s16 (vmovn_s32 (out_lo[i]),
				vmovn_s32 (out_hi[i]));
}

/* CLIP() from idct.c, done on the full int32 sum. */
static inline uint8x8_t clip32 (int32x4_t lo, int32x4_t hi)
{
    int32x4_t zero = vdupq_n_s32 (0);
    int32x4_t max = vdupq_n_s32 (255);

    lo = vminq_s32 (vmaxq_s32 (lo, zero), max);
    hi = vminq_s32 (vmaxq_s32 (hi, zero), max);
    return vqmovun_s16 (vcombine_s16 (vmovn_s32 (lo), vmovn_s32 (hi)));
}

static inline void add_row (uint8_t * dest, int16x8_t residual)
{
    int16x8_t d = vreinterpretq_s16_u16 (vmovl_u8 (vld1_u8 (dest)));
    int32x4_t lo = vaddq_s32 (vmovl_s16 (vget_low_s16 (residual)),
			      vmovl_s16 (vget_low_s16 (d)));
    int32x4_t hi = vaddq_s32 (vmovl_high_s16 (residual),
			      vmovl_high_s16 (d));

    vst1_u8 (dest, clip32 (lo, hi));
}

static inline void zero_block_neon (int16_t * block)
{
    int16x8_t z = vdupq_n_s16 (0);
    int i;

    for (i = 0; i < 8; i++)
	vst1q_s16 (block + 8 * i, z);
}

void mpeg2_idct_copy_neon (int16_t * block, uint8_t * dest, const int stride)
{
    int16x8_t rows[8];
    int i;

    mpeg2_neon_idct_calls[0]++;

    idct_to_rows (block, rows);
    for (i = 0; i < 8; i++)
	vst1_u8 (dest + i * stride, vqmovun_s16 (rows[i]));
    zero_block_neon (block);
}

void mpeg2_idct_add_neon (const int last, int16_t * block,
			  uint8_t * dest, const int stride)
{
    int i;

    mpeg2_neon_idct_calls[1]++;

    if (last != 129 || (block[0] & (7 << 4)) == (4 << 4)) {
	int16x8_t rows[8];

	idct_to_rows (block, rows);
	for (i = 0; i < 8; i++)
	    add_row (dest + i * stride, rows[i]);
	zero_block_neon (block);
    } else {
	/* DC-only: same arithmetic and the same partial clear as idct.c.
	   DC and dest are both small here, but the int16 add is exact and
	   the saturating narrow is the CLIP(0..255). */
	int32_t dc = ((int32_t) block[0] + 64) >> 7;
	int16x8_t dcv = vdupq_n_s16 ((int16_t) dc);

	block[0] = 0;
	block[63] = 0;
	for (i = 0; i < 8; i++) {
	    int16x8_t d = vreinterpretq_s16_u16 (
		vmovl_u8 (vld1_u8 (dest + i * stride)));
	    vst1_u8 (dest + i * stride, vqmovun_s16 (vaddq_s16 (dcv, d)));
	}
    }
}

#endif /* ARCH_AARCH64 && __aarch64__ */

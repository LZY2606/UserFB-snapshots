/*
 * motion_comp_neon.c
 * Copyright (C) 2026
 *
 * AArch64 NEON motion compensation for libmpeg2.
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

unsigned long mpeg2_neon_mc_calls;
/* Per-table-slot invocation counts, in MPEG2_MC_EXTERN order:
   0-3 put 16 (o,x,y,xy), 4-7 put 8, 8-11 avg 16, 12-15 avg 8. */
unsigned long mpeg2_neon_mc_slots[16];

/*
 * The four prediction positions from motion_comp.c:
 *
 *   o   dest = ref[i]
 *   x   dest = avg2 (ref[i], ref[i+1])                  horizontal half pel
 *   y   dest = avg2 (ref[i], (ref+stride)[i])           vertical half pel
 *   xy  dest = avg4 (ref[i], ref[i+1],                  both half pel
 *               (ref+stride)[i], (ref+stride)[i+1])
 *
 * with
 *
 *   avg2(a,b)     = (a + b + 1) >> 1
 *   avg4(a,b,c,d) = (a + b + c + d + 2) >> 2
 *
 * avg4 must be evaluated as a single four-term sum: splitting it into two
 * avg2 calls double-rounds and changes the result for odd sums.  The NEON
 * code below therefore keeps the true four-way sum plus 2 and shifts once.
 *
 * put writes dest = prediction; avg writes dest = avg2 (prediction, dest).
 * Both must handle unaligned ref/dest and arbitrary strides.  The 8 wide
 * variants touch exactly eight pixels per row: the horizontal half-pel
 * terms need ref[i+1] for i in 0..7, which reads ref[0..8] (nine bytes),
 * never more.
 */

static inline uint8x8_t avg2_u8 (uint8x8_t a, uint8x8_t b)
{
    /* (a + b + 1) >> 1 over uint8 lanes: vrhadd rounds up */
    return vrhadd_u8 (a, b);
}

/*
 * (a + b + c + d + 2) >> 2 on eight lanes.  Widening to 16 bits and
 * adding four times keeps the sum exact; the +2 is the rounding bias.
 */
static inline uint8x8_t avg4_u8 (uint8x8_t a, uint8x8_t b,
				 uint8x8_t c, uint8x8_t d)
{
    uint16x8_t s = vaddq_u16 (vaddl_u8 (a, b), vaddl_u8 (c, d));
    uint16x8_t bias = vdupq_n_u16 (2);

    return vmovn_u16 (vshrq_n_u16 (vaddq_u16 (s, bias), 2));
}

/* horizontal half pel: ref[i] and ref[i+1] */
static inline uint8x8_t pred_x (const uint8_t * ref)
{
    return avg2_u8 (vld1_u8 (ref), vld1_u8 (ref + 1));
}

/* vertical half pel: ref[i] and (ref + stride)[i] */
static inline uint8x8_t pred_y (const uint8_t * ref, int stride)
{
    return avg2_u8 (vld1_u8 (ref), vld1_u8 (ref + stride));
}

/* both: the four-point average of the 2x2 neighbourhood */
static inline uint8x8_t pred_xy (const uint8_t * ref, int stride)
{
    return avg4_u8 (vld1_u8 (ref), vld1_u8 (ref + 1),
		    vld1_u8 (ref + stride), vld1_u8 (ref + stride + 1));
}

/* ---- put / avg, 16 pixels wide ---------------------------------------- */

#define MC_PUT_16(name, expr, slot)					\
void MC_put_##name##_16_neon (uint8_t * dest, const uint8_t * ref,	\
			     const int stride, int height)		\
{									\
    mpeg2_neon_mc_calls++;						\
    mpeg2_neon_mc_slots[slot]++;					\
    do {								\
	uint8x16_t p = expr;						\
	vst1q_u8 (dest, p);						\
	ref += stride;							\
	dest += stride;							\
    } while (--height);							\
}

#define MC_AVG_16(name, expr, slot)					\
void MC_avg_##name##_16_neon (uint8_t * dest, const uint8_t * ref,	\
			     const int stride, int height)		\
{									\
    mpeg2_neon_mc_calls++;						\
    mpeg2_neon_mc_slots[slot]++;					\
    do {								\
	uint8x16_t p = expr;						\
	uint8x16_t d = vld1q_u8 (dest);					\
	vst1q_u8 (dest, vrhaddq_u8 (p, d));				\
	ref += stride;							\
	dest += stride;							\
    } while (--height);							\
}

/* ---- put / avg, 8 pixels wide ---------------------------------------- */

#define MC_PUT_8(name, expr, slot)					\
void MC_put_##name##_8_neon (uint8_t * dest, const uint8_t * ref,	\
			    const int stride, int height)		\
{									\
    mpeg2_neon_mc_calls++;						\
    mpeg2_neon_mc_slots[slot]++;					\
    do {								\
	uint8x8_t p = expr;						\
	vst1_u8 (dest, p);						\
	ref += stride;							\
	dest += stride;							\
    } while (--height);							\
}

#define MC_AVG_8(name, expr, slot)					\
void MC_avg_##name##_8_neon (uint8_t * dest, const uint8_t * ref,	\
			    const int stride, int height)		\
{									\
    mpeg2_neon_mc_calls++;						\
    mpeg2_neon_mc_slots[slot]++;					\
    do {								\
	uint8x8_t p = expr;						\
	uint8x8_t d = vld1_u8 (dest);					\
	vst1_u8 (dest, vrhadd_u8 (p, d));				\
	ref += stride;							\
	dest += stride;							\
    } while (--height);							\
}

/* 16-wide predictions.  avg4 needs the four-way sum, so it is spelled out. */
#define PRED_O_16(r)		vld1q_u8 (r)
#define PRED_X_16(r)		vrhaddq_u8 (vld1q_u8 (r), vld1q_u8 ((r) + 1))
#define PRED_Y_16(r, s)		vrhaddq_u8 (vld1q_u8 (r), vld1q_u8 ((r) + (s)))
#define PRED_XY_16(r, s)						\
    avg4q_u8 (vld1q_u8 (r), vld1q_u8 ((r) + 1),			\
	      vld1q_u8 ((r) + (s)), vld1q_u8 ((r) + (s) + 1))

static inline uint8x16_t avg4q_u8 (uint8x16_t a, uint8x16_t b,
				   uint8x16_t c, uint8x16_t d)
{
    uint16x8_t lo = vaddq_u16 (vaddl_u8 (vget_low_u8 (a), vget_low_u8 (b)),
			       vaddl_u8 (vget_low_u8 (c), vget_low_u8 (d)));
    uint16x8_t hi = vaddq_u16 (vaddl_high_u8 (a, b), vaddl_high_u8 (c, d));
    uint16x8_t bias = vdupq_n_u16 (2);

    return vcombine_u8 (vmovn_u16 (vshrq_n_u16 (vaddq_u16 (lo, bias), 2)),
			vmovn_u16 (vshrq_n_u16 (vaddq_u16 (hi, bias), 2)));
}

MC_PUT_16 (o, PRED_O_16 (ref), 0)
MC_PUT_16 (x, PRED_X_16 (ref), 1)
MC_PUT_16 (y, PRED_Y_16 (ref, stride), 2)
MC_PUT_16 (xy, PRED_XY_16 (ref, stride), 3)

MC_AVG_16 (o, PRED_O_16 (ref), 8)
MC_AVG_16 (x, PRED_X_16 (ref), 9)
MC_AVG_16 (y, PRED_Y_16 (ref, stride), 10)
MC_AVG_16 (xy, PRED_XY_16 (ref, stride), 11)

MC_PUT_8 (o, vld1_u8 (ref), 4)
MC_PUT_8 (x, pred_x (ref), 5)
MC_PUT_8 (y, pred_y (ref, stride), 6)
MC_PUT_8 (xy, pred_xy (ref, stride), 7)

MC_AVG_8 (o, vld1_u8 (ref), 12)
MC_AVG_8 (x, pred_x (ref), 13)
MC_AVG_8 (y, pred_y (ref, stride), 14)
MC_AVG_8 (xy, pred_xy (ref, stride), 15)

MPEG2_MC_EXTERN (neon)

#endif /* ARCH_AARCH64 && __aarch64__ */

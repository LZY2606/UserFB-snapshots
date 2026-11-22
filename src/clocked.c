/*
   libltc - clocked (measured-grid) label rendering

   Copyright (C) 2006-2022 Robin Gareus <robin@gareus.org>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU Lesser General Public License as
   published by the Free Software Foundation, either version 3 of the
   License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library.
   If not, see <http://www.gnu.org/licenses/>.
*/

#include <string.h>
#include <stddef.h>

#include "ltc.h"

#ifndef CLOCKED_SAMPLE_CENTER
#define CLOCKED_SAMPLE_CENTER 128 /* unsigned 8-bit center */
#endif

/* Extract one of the first 64 logical bits from a frame using the
 * named bitfield semantics, so the result is independent of the
 * native bitfield memory layout and endianness.
 */
static unsigned char frame_bit(const LTCFrame *f, int bit) {
	switch (bit) {
		case  0: case  1: case  2: case  3:
			return (f->frame_units >> (bit - 0)) & 1;
		case  4: case  5: case  6: case  7:
			return (f->user1 >> (bit - 4)) & 1;

		case  8: case  9:
			return (f->frame_tens >> (bit - 8)) & 1;
		case 10: return f->dfbit & 1;
		case 11: return f->col_frame & 1;
		case 12: case 13: case 14: case 15:
			return (f->user2 >> (bit - 12)) & 1;

		case 16: case 17: case 18: case 19:
			return (f->secs_units >> (bit - 16)) & 1;
		case 20: case 21: case 22: case 23:
			return (f->user3 >> (bit - 20)) & 1;

		case 24: case 25: case 26:
			return (f->secs_tens >> (bit - 24)) & 1;
		case 27: return f->biphase_mark_phase_correction & 1;
		case 28: case 29: case 30: case 31:
			return (f->user4 >> (bit - 28)) & 1;

		case 32: case 33: case 34: case 35:
			return (f->mins_units >> (bit - 32)) & 1;
		case 36: case 37: case 38: case 39:
			return (f->user5 >> (bit - 36)) & 1;

		case 40: case 41: case 42:
			return (f->mins_tens >> (bit - 40)) & 1;
		case 43: return f->binary_group_flag_bit0 & 1;
		case 44: case 45: case 46: case 47:
			return (f->user6 >> (bit - 44)) & 1;

		case 48: case 49: case 50: case 51:
			return (f->hours_units >> (bit - 48)) & 1;
		case 52: case 53: case 54: case 55:
			return (f->user7 >> (bit - 52)) & 1;

		case 56: case 57:
			return (f->hours_tens >> (bit - 56)) & 1;
		case 58: return f->binary_group_flag_bit1 & 1;
		case 59: return f->binary_group_flag_bit2 & 1;
		case 60: case 61: case 62: case 63:
			return (f->user8 >> (bit - 60)) & 1;
		default:
			return 0;
	}
}

int ltc_encoder_render_clocked(const LTCFrame *label,
		const LTCFrameClock *clock,
		enum LTC_TV_STANDARD standard,
		int reverse,
		ltcsnd_sample_t low,
		ltcsnd_sample_t high,
		ltcsnd_sample_t *output,
		size_t length)
{
	/* the forward sync word sent as bits 64..79: 0011111111111101 */
	static const unsigned char sync_bits[16] = {
		0,0,1,1,1,1,1,1,1,1,1,1,1,1,0,1
	};
	unsigned char bits[LTC_FRAME_BIT_COUNT];
	LTCFrame work;
	ltc_off_t base, span;
	ltc_off_t b;
	unsigned char level;
	size_t cursor = 0;

	/* --- parameter validation; on any failure leave output untouched --- */
	if (!label || !clock || !output) {
		return -1;
	}
	if (clock->valid != 1) {
		return -1;
	}
	if (clock->start_high != 0 && clock->start_high != 1) {
		return -1;
	}
	if (reverse != 0 && reverse != 1) {
		return -1;
	}
	if (standard != LTC_TV_525_60 && standard != LTC_TV_625_50
			&& standard != LTC_TV_1125_60 && standard != LTC_TV_FILM_24) {
		return -1;
	}
	if (!(low < CLOCKED_SAMPLE_CENTER && CLOCKED_SAMPLE_CENTER < high)) {
		return -1;
	}
	if (label->sync_word != 0xBFFC && label->sync_word != 0xFCBF) {
		return -1;
	}

	/* Non-negative, strictly increasing edges with at least two
	 * samples per bit. Non-negativity also guarantees every span
	 * below is computed without signed overflow.
	 */
	if (clock->edge[0] < 0) {
		return -1;
	}
	for (b = 0; b < LTC_FRAME_BIT_COUNT; ++b) {
		if (clock->edge[b + 1] <= clock->edge[b]
				|| clock->edge[b + 1] - clock->edge[b] < 2) {
			return -1;
		}
	}

	/* total span; reject before writing anything if it cannot fit. */
	base = clock->edge[0];
	span = clock->edge[LTC_FRAME_BIT_COUNT] - base;
	if (span < 0 || (unsigned long long)span > (unsigned long long)((size_t)-1)) {
		return -1;
	}
	if ((size_t)span != length) {
		return -1;
	}

	/* correct parity on a private copy; the caller's label is untouched */
	memcpy(&work, label, sizeof(LTCFrame));
	ltc_frame_set_parity(&work, standard);

	/* assemble logical bit 0..79 from field semantics, fixed sync word */
	for (b = 0; b < 64; ++b) {
		bits[b] = frame_bit(&work, b);
	}
	for (b = 0; b < 16; ++b) {
		bits[64 + b] = sync_bits[b];
	}

	/* render unfiltered two-level PCM, never moving a bit start */
	level = clock->start_high ? 1 : 0;
	for (b = 0; b < LTC_FRAME_BIT_COUNT; ++b) {
		const ltc_off_t width = clock->edge[b + 1] - clock->edge[b];
		const ltc_off_t half = width / 2; /* floor(width/2) */
		const unsigned char value = reverse
			? bits[LTC_FRAME_BIT_COUNT - 1 - b]
			: bits[b];
		ltc_off_t k;

		if (b != 0) {
			level ^= 1; /* toggle at every bit start */
		}

		if (value) {
			/* mid-bit toggle at floor(width/2) */
			for (k = 0; k < half; ++k) {
				output[cursor++] = level ? high : low;
			}
			level ^= 1;
			for (k = half; k < width; ++k) {
				output[cursor++] = level ? high : low;
			}
		} else {
			for (k = 0; k < width; ++k) {
				output[cursor++] = level ? high : low;
			}
		}
	}

	return 0;
}

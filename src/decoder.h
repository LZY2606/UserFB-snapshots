/*
   libltc - en+decode linear timecode

   Copyright (C) 2006-2012 Robin Gareus <robin@gareus.org>

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

#include "ltc.h"
#ifndef SAMPLE_CENTER // also defined in encoder.h
#define SAMPLE_CENTER 128 // unsigned 8 bit.
#endif

/** Measured-edge ring. A forward grid looks back at most 80 ticks,
 *  a reverse grid 96 ticks; 128 leaves headroom while a completed
 *  grid is always copied out before its slots can be overwritten.
 */
#define LTC_EDGE_RING_SIZE 128

struct LTCDecoder {
	LTCFrameExt* queue;
	LTCFrameClock* clock_queue; ///< measured bit-grids, parallel to queue slots
	int queue_len;
	int queue_read_off;
	int queue_write_off;

	unsigned char biphase_state;
	unsigned char biphase_prev;
	unsigned char snd_to_biphase_state;
	int snd_to_biphase_cnt;		///< counts the samples in the current period
	int snd_to_biphase_lmt;	///< specifies when a state-change is considered biphase-clock or 2*biphase-clock
	double snd_to_biphase_period;	///< track length of a period - used to set snd_to_biphase_lmt

	ltcsnd_sample_t snd_to_biphase_min;
	ltcsnd_sample_t snd_to_biphase_max;

	unsigned short decoder_sync_word;
	LTCFrame ltc_frame;
	int bit_cnt;

	ltc_off_t frame_start_off;
	ltc_off_t frame_start_prev;

	float biphase_tics[LTC_FRAME_BIT_COUNT];
	int biphase_tic;

	/* Raw bit-start measurements. One entry is recorded per
	 * parse_ltc call (one per decoded bit, at the bit's start edge)
	 * using the transition position before tracker smoothing, so
	 * these are actual sample positions - never re-derived from a
	 * nominal frame-rate or the smoothed biphase period. Consecutive
	 * duplicate ticks (a zero bit's double tick at one position) are
	 * collapsed to a single entry.
	 */
	ltc_off_t edge_ring[LTC_EDGE_RING_SIZE];
	unsigned char edge_level_ring[LTC_EDGE_RING_SIZE]; ///< physical level at that edge, 0/1
	long long edge_total; ///< number of distinct bit-start edges recorded
	ltc_off_t tick_raw; ///< raw (unsmoothed) position of the current biphase tick
	unsigned char tick_level; ///< physical level entered at tick_raw
};


void decode_ltc(LTCDecoder *d, ltcsnd_sample_t *sound, size_t size, ltc_off_t posinfo);

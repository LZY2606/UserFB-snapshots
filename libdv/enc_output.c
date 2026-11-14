/* 
 *  enc_output_filters
 *
 *     Copyright (C) Peter Schlaile - Feb 2001
 *
 *  This file is part of libdv, a free DV (IEC 61834/SMPTE 314M)
 *  codec.
 *
 *  libdv is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU Lesser Public License as published by
 *  the Free Software Foundation; either version 2.1, or (at your
 *  option) any later version.
 *   
 *  libdv is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser Public License for more details.
 *   
 *  You should have received a copy of the GNU Lesser Public License
 *  along with libdv; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA. 
 *
 *  The libdv homepage is http://libdv.sourceforge.net/.  
 */
 
#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "enc_audio_input.h"
#include "enc_output.h"
#include "headers.h"

FILE* out_fp = NULL;

static int raw_init(FILE* fp)
{
	out_fp = fp;
	return (fp == NULL) ? -1 : 0;
}

static void raw_finish()
{
}

static int dv_audio_unshuffle_60[5][9] = {
  { 0, 15, 30, 10, 25, 40,  5, 20, 35 },
  { 3, 18, 33, 13, 28, 43,  8, 23, 38 },
  { 6, 21, 36,  1, 16, 31, 11, 26, 41 },
  { 9, 24, 39,  4, 19, 34, 14, 29, 44 },
  {12, 27, 42,  7, 22, 37,  2, 17, 32 },
};

static int dv_audio_unshuffle_50[6][9] = {
  {  0, 18, 36, 13, 31, 49,  8, 26, 44 },
  {  3, 21, 39, 16, 34, 52, 11, 29, 47 },
  {  6, 24, 42,  1, 19, 37, 14, 32, 50 }, 
  {  9, 27, 45,  4, 22, 40, 17, 35, 53 }, 
  { 12, 30, 48,  7, 25, 43,  2, 20, 38 },
  { 15, 33, 51, 10, 28, 46,  5, 23, 41 },
};

/* Reconstruct the 16 bit value a 12 bit non-linear DV audio code
 * expands to.  This mirrors dv_upsample() in audio.c (see IEC 61834-2,
 * Figure 16) and must stay in agreement with the decoder: code 0x800 is
 * the reserved error code and is never produced by the encoder. */
static int dv_12bit_reconstruct(int code)
{
	int sample = (code & 0x800) ? code - 0x1000 : code;
	int shift  = (code & 0xf00) >> 8;

	if (shift >= 0x2 && shift <= 0x7) {
		shift--;
		return (sample - (256 * shift)) << shift;
	} else if (shift >= 0x8 && shift <= 0xd) {
		shift = 0xe - shift;
		return ((sample + ((256 * shift) + 1)) << shift) - 1;
	}
	return sample;
}

/* Map a code ordering index onto the actual 12 bit codes such that the
 * reconstructed values are strictly increasing: indices 0..2046 are the
 * negative codes 0x801..0xfff, indices 2047..4094 are 0x000..0x7ff. */
static int dv_12bit_ordered_code(int index)
{
	return (index < 2047) ? (0x801 + index) : (index - 2047);
}

/* Quantize a 16 bit sample to the nearest 12 bit non-linear DV code.
 * Ties are broken towards the smaller absolute reconstructed value, and
 * then towards the smaller unsigned code.  The reserved error code
 * 0x800 is excluded from the search space. */
static unsigned int dv_12bit_encode(int sample)
{
	int lo, hi, mid, mid_code, mid_val;

	/* clamp to the representable range */
	if (sample <= dv_12bit_reconstruct(0x801))
		return 0x801;
	if (sample >= dv_12bit_reconstruct(0x7ff))
		return 0x7ff;

	/* binary search: find the first index whose reconstruction is
	 * >= sample */
	lo = 0;
	hi = 4094;
	while (lo < hi) {
		mid = (lo + hi) / 2;
		mid_val = dv_12bit_reconstruct(dv_12bit_ordered_code(mid));
		if (mid_val < sample)
			lo = mid + 1;
		else
			hi = mid;
	}
	mid_code = dv_12bit_ordered_code(lo);
	mid_val  = dv_12bit_reconstruct(mid_code);
	if (mid_val == sample || lo == 0)
		return mid_code;

	{
		int lo_code = dv_12bit_ordered_code(lo - 1);
		int lo_val  = dv_12bit_reconstruct(lo_code);
		int d_lo    = sample - lo_val;
		int d_hi    = mid_val - sample;
		if (d_lo < d_hi)
			return lo_code;
		if (d_lo > d_hi)
			return mid_code;
		/* equal distance: prefer the smaller absolute
		 * reconstructed value, then the smaller unsigned code */
		if (abs(lo_val) != abs(mid_val))
			return (abs(lo_val) < abs(mid_val)) ? lo_code : mid_code;
		return (lo_code < mid_code) ? lo_code : mid_code;
	}
}

static void put_12_bit(unsigned char * target, unsigned char* wave_buf,
		dv_enc_audio_info_t * audio, int dif_seg, int isPAL,
		int pair)
{
	int bp;
	int audio_dif;
	unsigned char* p = target;
	int samples = audio->bytesperframe / (2 * audio->channels);

	/* Two channels share each 3 byte group: channel 2*pair takes the
	 * first 12 bit code, channel 2*pair+1 the second.  Physical slots
	 * beyond the valid samples are written as zero codes so no stale
	 * audio from a previous frame survives. */
	if (isPAL) {
		for (audio_dif = 0; audio_dif < 9; audio_dif++) {
			for (bp = 8; bp < 80; bp += 3) {
				int i = dv_audio_unshuffle_50[dif_seg]
					[audio_dif] + (bp - 8)/3 * 54;
				unsigned int cy = 0, cz = 0;
				if (i < samples) {
					const unsigned char *wy = wave_buf
						+ audio->bytealignment * i
						+ 4*pair;
					const unsigned char *wz = wy + 2;
					cy = dv_12bit_encode((int16_t)
						((wy[0] << 8) | wy[1]));
					cz = dv_12bit_encode((int16_t)
						((wz[0] << 8) | wz[1]));
				}
				p[bp]     = cy >> 4;
				p[bp + 1] = cz >> 4;
				p[bp + 2] = ((cy & 0xf) << 4) | (cz & 0xf);
			}
			p += 16 * 80;
		}
	} else {
		for (audio_dif = 0; audio_dif < 9; audio_dif++) {
			for (bp = 8; bp < 80; bp += 3) {
				int i = dv_audio_unshuffle_60[dif_seg]
					[audio_dif] + (bp - 8)/3 * 45;
				unsigned int cy = 0, cz = 0;
				if (i < samples) {
					const unsigned char *wy = wave_buf
						+ audio->bytealignment * i
						+ 4*pair;
					const unsigned char *wz = wy + 2;
					cy = dv_12bit_encode((int16_t)
						((wy[0] << 8) | wy[1]));
					cz = dv_12bit_encode((int16_t)
						((wz[0] << 8) | wz[1]));
				}
				p[bp]     = cy >> 4;
				p[bp + 1] = cz >> 4;
				p[bp + 2] = ((cy & 0xf) << 4) | (cz & 0xf);
			}
			p += 16 * 80;
		}
	}
}

static void put_16_bit(unsigned char * target, unsigned char* wave_buf,
		dv_enc_audio_info_t * audio, int dif_seg, int isPAL,
		int channel)
{
	int bp;
	int audio_dif;
	unsigned char* p = target;

	if (isPAL) {
		for (audio_dif = 0; audio_dif < 9; audio_dif++) {
			for (bp = 8; bp < 80; bp += 2) {
				int i = dv_audio_unshuffle_50[dif_seg]
					[audio_dif] + (bp - 8)/2 * 54;
				p[bp] = wave_buf[
					audio->bytealignment * i
					+ 2*channel];
				p[bp + 1] = wave_buf[
					audio->bytealignment * i + 1
					+ 2*channel];
				if (p[bp] == 0x80 && p[bp+1] == 0x00) {
					p[bp+1] = 0x01;
				}
			}
			p += 16 * 80;
		}
	} else {
		for (audio_dif = 0; audio_dif < 9; audio_dif++) {
			for (bp = 8; bp < 80; bp += 2) {
				int i = dv_audio_unshuffle_60[dif_seg]
					[audio_dif] + (bp - 8)/2 * 45;
				p[bp] = wave_buf[
					audio->bytealignment * i
					+ 2*channel];
				p[bp + 1] = wave_buf[
					audio->bytealignment * i + 1
					+ 2*channel];
				if (p[bp] == 0x80 && p[bp+1] == 0x00) {
					p[bp+1] = 0x01;
				}
			}
			p += 16 * 80;
		}
	}
}


int _dv_raw_insert_audio(unsigned char * frame_buf, 
		     dv_enc_audio_info_t * audio, int isPAL)
{
	int dif_seg;
	int dif_seg_max = isPAL ? 12 : 10;
	int samplesperframe = audio->bytesperframe/(2*audio->channels);
	
	int bits_per_sample = 16;
	unsigned char* wave_buf = audio->data;

	unsigned char head_50[5];
	unsigned char head_51[5];
	unsigned char head_52[5];
	unsigned char head_53[5];

	head_50[0] = 0x50;

	if (isPAL) {
		head_50[3] = /* stype = */ 0 | (/* isPAL */ 1 << 5)
			| (/* ml */ 1 << 6) | (/* res */ 1 << 7);
		switch(audio->frequency) {
		case 32000:
			if (audio->channels == 2) {
				head_50[1] = (samplesperframe - 1264) 
					| (1 << 6) | (/* unlocked = */ 1 << 7);
				head_50[2] = /* audio_mode= */ 0 
					| (/* pa = */ 0 << 4) 
					| (/* chn = */ 0 << 5)
					| (/* sm = */ 0 << 7);
				head_50[4] = /* 16 Bits */0 
					| (/* 32000 kHz */ 2 << 3)
					| (/* tc = */ 1 << 6) 
					| (/* ef = */ 1 << 7);
			} else {
				head_50[1] = (samplesperframe - 1264) 
					| (1 << 6) | (/* unlocked = */ 1 << 7);
				head_50[2] = /* audio_mode= */ 0 
					| (/* pa = */ 1 << 4) 
					| (/* chn = */ 1 << 5)
					| (/* sm = */ 0 << 7);
				head_50[4] = /* 12 Bits */1 
					| (/* 32000 kHz */ 2 << 3)
					| (/* tc = */ 1 << 6) 
					| (/* ef = */ 1 << 7);
				bits_per_sample = 12;
			}
			break;
		case 44100:
			head_50[1] = (samplesperframe - 1742) | (1 << 6)
				| (/* unlocked = */ 1 << 7);
			head_50[2] = /* audio_mode= */ 0 | (/* pa = */ 0 << 4) 
				| (/* chn = */ 0 << 5)
				| (/* sm = */ 0 << 7);
			head_50[4] = /* 16 Bits */0 | (/* 44100 kHz */ 1 << 3)
				| (/* tc = */ 1 << 6) | (/* ef = */ 1 << 7);
			break;
		case 48000:
			head_50[1] = (samplesperframe - 1896) | (1 << 6)
				| (/* unlocked = */ 1 << 7);
			head_50[2] = /* audio_mode= */ 0 | (/* pa = */ 0 << 4) 
				| (/* chn = */ 0 << 5);
			head_50[4] = /* 16 Bits */0 | (/* 48000 kHz */ 0 << 3)
				| (/* tc = */ 1 << 6) | (/* ef = */ 1 << 7);
			break;
		default:
			fprintf(stderr, "Impossible frequency??\n");
			return(-1);
		}
	} else {
		head_50[3] = /* stype = */ 0 | (/* isPAL */ 0 << 5)
			| (/* ml */ 1 << 6) | (/* res */ 1 << 7);
		switch(audio->frequency) {
		case 32000:
			if (audio->channels == 2) {
				head_50[1] = (samplesperframe - 1053) 
					| (1 << 6) | (/* unlocked = */ 1 << 7);
				head_50[2] = /* audio_mode= */ 0 
					| (/* pa = */ 0 << 4) 
					| (/* chn = */ 0 << 5)
					| (/* sm = */ 0 << 7);
				head_50[4] = /* 16 Bits */0 
					| (/* 32000 kHz */ 2 << 3)
					| (/* tc = */ 1 << 6) 
					| (/* ef = */ 1 << 7);
			} else {
				head_50[1] = (samplesperframe - 1053) 
					| (1 << 6) | (/* unlocked = */ 1 << 7);
				head_50[2] = /* audio_mode= */ 0 
					| (/* pa = */ 1 << 4) 
					| (/* chn = */ 1 << 5)
					| (/* sm = */ 0 << 7);
				head_50[4] = /* 12 Bits */1 
					| (/* 32000 kHz */ 2 << 3)
					| (/* tc = */ 1 << 6) 
					| (/* ef = */ 1 << 7);
				bits_per_sample = 12;
			}
			break;
		case 44100:
			head_50[1] = (samplesperframe - 1452) | (1 << 6)
				| (/* unlocked = */ 1 << 7);
			head_50[2] = /* audio_mode= */ 0 | (/* pa = */ 0 << 4) 
				| (/* chn = */ 0 << 5)
				| (/* sm = */ 0 << 7);
			head_50[4] = /* 16 Bits */0 | (/* 44100 kHz */ 1 << 3)
				| (/* tc = */ 1 << 6) | (/* ef = */ 1 << 7);
			break;
		case 48000:
			head_50[1] = (samplesperframe - 1580) | (1 << 6)
				| (/* unlocked = */ 1 << 7);
			head_50[2] = /* audio_mode= */ 0 | (/* pa = */ 0 << 4) 
				| (/* chn = */ 0 << 5);
			head_50[4] = /* 16 Bits */0 | (/* 48000 kHz */ 0 << 3)
				| (/* tc = */ 1 << 6) | (/* ef = */ 1 << 7);
			break;
		default:
			fprintf(stderr, "Impossible frequency??\n");
			return(-1);
		}
	}

	head_51[0] = 0x51; /* FIXME: What's this? */ 
	head_51[1] = 0x33;
	head_51[2] = 0xcf;
	if ((frame_buf[4] & 0x7) == 0) /* IEC 61834? */
		head_51[3] = 0xa0; /* forward, normal speed */
	else if (isPAL) /* SMPTE 314M */
		head_51[3] = 0x80 /* forward */ | 0x64 /* normal speed */;
	else
		head_51[3] = 0x80 /* forward */ | 0x78 /* normal speed */;
	head_51[4] = 0xff;

	head_52[0] = 0x52;
	head_52[1] = frame_buf[5 * 80 + 48 + 2 * 5 + 1]; /* steal video */
	head_52[2] = frame_buf[5 * 80 + 48 + 2 * 5 + 2]; /* timestamp */
	head_52[3] = frame_buf[5 * 80 + 48 + 2 * 5 + 3]; /* this gets us an */
	head_52[4] = frame_buf[5 * 80 + 48 + 2 * 5 + 4]; /* off by one error!*/
	                                                   
	head_53[0] = 0x53; 
	head_53[1] = frame_buf[5 * 80 + 48 + 3 * 5 + 1];
	head_53[2] = frame_buf[5 * 80 + 48 + 3 * 5 + 2];
	head_53[3] = frame_buf[5 * 80 + 48 + 3 * 5 + 3];
	head_53[4] = frame_buf[5 * 80 + 48 + 3 * 5 + 4];

	for (dif_seg = 0; dif_seg < dif_seg_max; dif_seg++) {
		int audio_dif;
		unsigned char* target= frame_buf + dif_seg * 150 * 80 + 6 * 80;
		int channel;
		int ds;

		unsigned char* p = target + 3;
		for (audio_dif = 0; audio_dif < 9; audio_dif++) {
			memset(p, 0xff, 5);
			p += 16 * 80;
		}

		if ((dif_seg & 1) == 0) {
			p = target + 3 * 16 * 80 + 3;
		} else {
			p = target + 3;
		}

		/* Timestamp it! */
		memcpy(p + 0*16*80, head_50, 5);
		memcpy(p + 1*16*80, head_51, 5);
		memcpy(p + 2*16*80, head_52, 5);
		memcpy(p + 3*16*80, head_53, 5);

		if (dif_seg >= dif_seg_max/2) {
			p[2] |= 1;
		}

		switch(bits_per_sample) {
		case 12: {
			int pair;
			ds = dif_seg;
			if(ds < dif_seg_max/2) {
				pair = 0;
			} else {
				pair = 1;
				ds -= dif_seg_max / 2;
			}
			put_12_bit(target, wave_buf,
				   audio, ds, isPAL, pair);
			if (!isPAL && dif_seg == 6) {
				/* SMPTE 314M 525/60: the AAUX source and
				 * source control packs of the second
				 * channel pair live in audio blocks 0/1
				 * of DIF sequence 6. */
				unsigned char* q = target + 3;
				memcpy(q, head_50, 5);
				q[2] |= 1; /* second channel pair */
				memcpy(q + 16*80, head_51, 5);
			}
			break;
		}
		case 16:
			ds = dif_seg;
			if(ds < dif_seg_max/2) {
				channel = 0;
			} else {
				channel = 1;
				ds -= dif_seg_max / 2;
			} 
			put_16_bit(target, wave_buf,
				   audio, ds, isPAL, channel);
			break;
		}
	}
	return 0;
}


static int frame_counter = 0;

static int raw_store(unsigned char* encoded_data, 
		     dv_enc_audio_info_t* audio_data, 
		     int keep_meta_headers,
		     int isPAL, int is16x9, time_t now)
{
	if (!keep_meta_headers) {
		_dv_write_meta_data(encoded_data, frame_counter, 
				isPAL, is16x9, &now);
	}
	if (audio_data) {
		int rval = _dv_raw_insert_audio(encoded_data, audio_data, isPAL);
		if (rval) {
			return rval;
		}
	}
	fwrite(encoded_data, 1, isPAL ? 144000 : 120000, out_fp);
	frame_counter++;
	return 0;
}

static dv_enc_output_filter_t filters[DV_ENC_MAX_OUTPUT_FILTERS] = {
	{ raw_init, raw_finish, raw_store, "raw" },
	{ NULL, NULL, NULL, NULL }};

void dv_enc_register_output_filter(dv_enc_output_filter_t filter)
{
	dv_enc_output_filter_t * p = filters;
	while (p->filter_name) p++;
	*p = filter;
}

int dv_enc_get_output_filters(dv_enc_output_filter_t ** filters_,
			      int * count)
{
	dv_enc_output_filter_t * p = filters;

	*count = 0;
	while (p->filter_name) p++, (*count)++;

	*filters_ = filters;
	return 0;
}




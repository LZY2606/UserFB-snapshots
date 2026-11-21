/*
** Copyright (C) 2024
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU Lesser General Public License as published by
** the Free Software Foundation; either version 2.1 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU Lesser General Public License for more details.
**
** You should have received a copy of the GNU Lesser General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
*/

/*
** Contract tests for read-only LIST/wavl IMA-ADPCM support.
**
** The compressed blocks are assembled independently here and decoded by
** a local, nibble-by-nibble reference recursion; the library output is
** never used as its own reference. Three distinct "voices" are used on
** purpose: the block header predictor, the real final decoded frame of
** a block and explicit zero, so hold handling cannot pass by accident.
*/

#include "task_support/check.h"
#include "task_support/fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define T_BLOCKALIGN_ST	16
#define T_SPB			9
#define T_MAX_FRAMES	4096

/*--------------------------------------------------------------------------------
** Independent IMA-ADPCM reference.
*/

static const int t_step_size [89] =
{	7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
	50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
	253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
	1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
	3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
	11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
	32767
} ;

static const int t_indx_adjust [16] =
{	-1, -1, -1, -1, +2, +4, +6, +8, -1, -1, -1, -1, +2, +4, +6, +8
} ;

static int
t_clamp (int i)
{	if (i < 0)
		return 0 ;
	if (i > 88)
		return 88 ;
	return i ;
} /* t_clamp */

/* Signed 16 bit to 32 bit widening used by sf_readf_int(). The unsigned
** shift keeps negative inputs well defined while producing the same bit
** pattern as an arithmetic left shift.
*/
static int
t_shift16 (int x)
{	return (int) ((uint32_t) x << 16) ;
} /* t_shift16 */

/* Decode one block exactly the way the WAV layout is specified, nibble
** by nibble, independent of the library implementation.
*/
static void
t_ref_decode (const unsigned char *block, int ch, int blockalign, int spb, short *out)
{	int chan, k, predictor, blockindx, indx, indxstart, diff ;
	short step, bytecode, stepindx [2] = { 0, 0 } ;

	for (chan = 0 ; chan < ch ; chan++)
	{	predictor = block [chan * 4] | (block [chan * 4 + 1] << 8) ;
		if (predictor & 0x8000)
			predictor -= 0x10000 ;
		stepindx [chan] = t_clamp (block [chan * 4 + 2]) ;
		out [chan] = (short) predictor ;
		} ;

	blockindx = 4 * ch ;
	indxstart = ch ;
	while (blockindx < blockalign)
	{	for (chan = 0 ; chan < ch ; chan++)
		{	indx = indxstart + chan ;
			for (k = 0 ; k < 4 ; k++)
			{	bytecode = block [blockindx++] ;
				out [indx] = bytecode & 0x0F ;
				indx += ch ;
				out [indx] = (bytecode >> 4) & 0x0F ;
				indx += ch ;
				} ;
			} ;
		indxstart += 8 * ch ;
		} ;

	for (k = ch ; k < spb * ch ; k++)
	{	chan = (ch > 1) ? (k % 2) : 0 ;
		bytecode = out [k] & 0xF ;
		step = t_step_size [stepindx [chan]] ;
		predictor = out [k - ch] ;

		diff = step >> 3 ;
		if (bytecode & 1) diff += step >> 2 ;
		if (bytecode & 2) diff += step >> 1 ;
		if (bytecode & 4) diff += step ;
		if (bytecode & 8) diff = -diff ;

		predictor += diff ;
		if (predictor > 32767) predictor = 32767 ;
		if (predictor < -32768) predictor = -32768 ;

		stepindx [chan] = t_clamp (stepindx [chan] + t_indx_adjust [bytecode]) ;
		out [k] = (short) predictor ;
		} ;
} /* t_ref_decode */

/*--------------------------------------------------------------------------------
** Block and file construction.
*/

typedef struct
{	short			pred [2] ;
	unsigned char	idx [2] ;
	/* Eight nibble codes per channel (spb - 1 == 8 for the 9/16 layout). */
	unsigned char	code [2] [8] ;
} T_BLOCK ;

typedef struct
{	int				kind ;		/* 1 = data, 2 = slnt, 3 = unknown */
	int				nblocks ;
	const T_BLOCK	*blocks ;
	uint32_t		slnt ;
	int				unknown_len ;
} T_SEG ;

static void
t_put_block (task_memory *b, int ch, const T_BLOCK *d)
{	int chan, k, blockalign = ch == 2 ? T_BLOCKALIGN_ST : 8 ;

	for (chan = 0 ; chan < ch ; chan++)
	{	task_u16 (b, (uint16_t) d->pred [chan]) ;
		b->bytes [b->n++] = d->idx [chan] ;
		b->bytes [b->n++] = 0 ;
		} ;

	/* Nibble payload: eight codes per channel, low nibble first. */
	{	int used = 4 * ch ;
		while (used < blockalign)
		{	for (chan = 0 ; chan < ch ; chan++)
				for (k = 0 ; k < 4 ; k++)
				{	unsigned char lo = d->code [chan] [2 * k] ;
					unsigned char hi = d->code [chan] [2 * k + 1] ;
					b->bytes [b->n++] = (hi << 4) | (lo & 0x0F) ;
					used++ ;
					} ;
			} ;
		} ;
} /* t_put_block */

static void
t_fmt (task_memory *b, int ch)
{	task_tag (b, "fmt ") ;
	task_u32 (b, 20) ;
	task_u16 (b, 17) ;
	task_u16 (b, (unsigned) ch) ;
	task_u32 (b, 8000) ;
	task_u32 (b, ch == 2 ? 14222 : 7111) ;
	task_u16 (b, (unsigned) (ch == 2 ? T_BLOCKALIGN_ST : 8)) ;
	task_u16 (b, 4) ;
	task_u16 (b, 2) ;
	task_u16 (b, T_SPB) ;
} /* t_fmt */

static void
t_fact (task_memory *b, uint32_t frames)
{	task_tag (b, "fact") ;
	task_u32 (b, 4) ;
	task_u32 (b, frames) ;
} /* t_fact */

/*
** Build a wavl file from the segment description. When fact_frames is
** non-NULL a fact chunk is emitted. The segment stream may contain empty
** data segments (nblocks == 0), zero length slnt and odd unknown chunks.
*/
static void t_init_vocab (void) ;

static void
t_ensure_vocab (void)
{	static int done = 0 ;
	if (done)
		return ;
	done = 1 ;
	t_init_vocab () ;
} /* t_ensure_vocab */

static void
t_build (task_memory *b, int ch, const T_SEG *segs, int nseg, const uint32_t *fact_frames)
{	size_t riff_pos, list_pos, list_size_pos ;
	int i ;

	t_ensure_vocab () ;

	memset (b, 0, sizeof (*b)) ;
	task_tag (b, "RIFF") ;
	riff_pos = b->n ;
	task_u32 (b, 0) ;
	task_tag (b, "WAVE") ;

	t_fmt (b, ch) ;
	if (fact_frames != NULL)
		t_fact (b, *fact_frames) ;

	task_tag (b, "LIST") ;
	list_pos = b->n ;
	task_u32 (b, 0) ;
	task_tag (b, "wavl") ;

	for (i = 0 ; i < nseg ; i++)
	{	switch (segs [i].kind)
		{	case 1 :
			{	int j ;
				size_t payload = (size_t) segs [i].nblocks * (ch == 2 ? T_BLOCKALIGN_ST : 8) ;
				task_tag (b, "data") ;
				task_u32 (b, (uint32_t) payload) ;
				for (j = 0 ; j < segs [i].nblocks ; j++)
					t_put_block (b, ch, &(segs [i].blocks [j])) ;
				break ;
				} ;
			case 2 :
				task_tag (b, "slnt") ;
				task_u32 (b, 4) ;
				task_u32 (b, segs [i].slnt) ;
				break ;
			case 3 :
			{	int j ;
				task_tag (b, "zzzz") ;
				task_u32 (b, (uint32_t) segs [i].unknown_len) ;
				for (j = 0 ; j < segs [i].unknown_len ; j++)
					b->bytes [b->n++] = (unsigned char) (0x40 + j) ;
				if (segs [i].unknown_len & 1)
					b->bytes [b->n++] = 0xAB ;	/* odd padding */
				break ;
				} ;
			} ;
		} ;

	list_size_pos = list_pos ;
	task_patch32 (b, list_size_pos, (uint32_t) (b->n - list_pos - 4)) ;
	task_patch32 (b, riff_pos, (uint32_t) (b->n - 8)) ;
} /* t_build */

/* Build the expected expanded axis for a segment stream. Returns the
** number of logical frames; too-large slnt counts are rejected so the
** caller can handle the 64 bit case separately.
*/
static sf_count_t
t_expect (int ch, const T_SEG *segs, int nseg, short *out, sf_count_t cap)
{	short		hold [2] = { 0, 0 } ;
	int			have_hold = 0 ;
	sf_count_t	logical = 0 ;
	int			i, j ;

	/* Serial reference: each data block is re-encoded into a temporary
	** buffer, decoded with the independent recursion, and every slnt
	** frame repeats the last decoded frame (zero before any audio).
	*/
	{	unsigned char tmp [T_BLOCKALIGN_ST] ;
		task_memory tb ;

		have_hold = 0 ;
		hold [0] = hold [1] = 0 ;
		memset (&tb, 0, sizeof (tb)) ;
		(void) tmp ;

		for (i = 0 ; i < nseg ; i++)
		{	switch (segs [i].kind)
			{	case 1 :
					for (j = 0 ; j < segs [i].nblocks ; j++)
					{	short decoded [T_SPB * 2] ;
						int f, c ;

						tb.n = 0 ;
						t_put_block (&tb, ch, &(segs [i].blocks [j])) ;
						t_ref_decode (tb.bytes, ch, ch == 2 ? T_BLOCKALIGN_ST : 8, T_SPB, decoded) ;

						for (f = 0 ; f < T_SPB ; f++)
						{	if (logical >= cap)
								return -1 ;
							for (c = 0 ; c < ch ; c++)
								out [logical * ch + c] = decoded [f * ch + c] ;
							logical++ ;
							} ;

						for (c = 0 ; c < ch ; c++)
							hold [c] = decoded [(T_SPB - 1) * ch + c] ;
						have_hold = 1 ;
						} ;
					break ;

				case 2 :
				{	uint32_t k ;
					for (k = 0 ; k < segs [i].slnt ; k++)
					{	int c ;
						if (logical >= cap)
							return -1 ;
						for (c = 0 ; c < ch ; c++)
							out [logical * ch + c] = have_hold ? hold [c] : 0 ;
						logical++ ;
						} ;
					break ;
					} ;

				case 3 :
					break ;
				} ;
			} ;
		} ;

	return logical ;
} /* t_expect */

/*--------------------------------------------------------------------------------
** Counting / byte-budget virtual IO.
*/

typedef struct
{	task_memory	*mem ;
	size_t		cap ;		/* Maximum cumulative bytes readable. */
	size_t		denied ;
} T_LIM ;

static sf_count_t
t_lim_len (void *u)
{	return task_len (((T_LIM *) u)->mem) ;
}

static sf_count_t
t_lim_seek (sf_count_t n, int whence, void *u)
{	return task_seek (n, whence, ((T_LIM *) u)->mem) ;
}

static sf_count_t
t_lim_tell (void *u)
{	return ((T_LIM *) u)->mem->pos ;
}

static sf_count_t
t_lim_read (void *p, sf_count_t n, void *u)
{	T_LIM *l = u ;
	task_memory *m = l->mem ;
	sf_count_t room ;

	if (n <= 0)
		return 0 ;

	room = (sf_count_t) l->cap - (sf_count_t) m->bytes_read ;
	if (room <= 0)
	{	l->denied++ ;
		return 0 ;
		} ;
	if (n > room)
		n = room ;

	return task_read (p, n, m) ;
} /* t_lim_read */

static SNDFILE *
t_open_lim (T_LIM *l, SF_INFO *info)
{	SF_VIRTUAL_IO io = { t_lim_len, t_lim_seek, t_lim_read, task_write, t_lim_tell } ;

	memset (info, 0, sizeof (*info)) ;
	l->mem->pos = 0 ;
	return sf_open_virtual (&io, SFM_READ, info, l) ;
} /* t_open_lim */

/*--------------------------------------------------------------------------------
** Shared block vocabulary: non-zero step indices, positive and negative
** predictors and different left/right codes.
*/

static T_BLOCK
t_mkblock (short lpred, short rpred, unsigned char lidx, unsigned char ridx,
	unsigned char lcode, unsigned char rcode)
{	T_BLOCK d ;
	int k ;

	memset (&d, 0, sizeof (d)) ;
	d.pred [0] = lpred ;
	d.pred [1] = rpred ;
	d.idx [0] = lidx ;
	d.idx [1] = ridx ;
	for (k = 0 ; k < 8 ; k++)
	{	d.code [0] [k] = lcode ;
		d.code [1] [k] = rcode ;
		} ;
	return d ;
} /* t_mkblock */

static T_BLOCK t_blocks_stereo [4] ;
static T_BLOCK t_blocks_mono [3] ;

static void
t_init_vocab (void)
{	/* Stereo: positive left / negative right predictors, distinct codes
	** so the final frame differs from both the header value and zero.
	*/
	t_blocks_stereo [0] = t_mkblock (1000, -1000, 4, 4, 0x1, 0x9) ;
	t_blocks_stereo [1] = t_mkblock (5000, -5000, 7, 5, 0x7, 0x3) ;
	t_blocks_stereo [2] = t_mkblock (-3000, 3000, 2, 6, 0xA, 0x2) ;
	t_blocks_stereo [3] = t_mkblock (12000, -12000, 9, 3, 0x4, 0xC) ;

	t_blocks_mono [0] = t_mkblock (2000, 0, 5, 0, 0x5, 0) ;
	t_blocks_mono [1] = t_mkblock (-7000, 0, 8, 0, 0xB, 0) ;
	t_blocks_mono [2] = t_mkblock (9000, 0, 1, 0, 0x2, 0) ;
} /* t_init_vocab */

/*================================================================================
** decoding group
*/

static int
t_dec_stereo_full (void)
{	task_memory	mem ;
	SF_INFO		info ;
	SNDFILE		*sf ;
	short		got [T_MAX_FRAMES * 2] ;
	short		want [T_MAX_FRAMES * 2] ;
	sf_count_t	want_frames, got_frames ;

	T_SEG segs [] =
	{	{ 1, 2, &(t_blocks_stereo [0]), 0, 0 },
		{ 2, 0, NULL, 5, 0 },
		{ 1, 1, &(t_blocks_stereo [2]), 0, 0 },
		{ 2, 0, NULL, 3, 0 },
		{ 1, 1, &(t_blocks_stereo [3]), 0, 0 },
	} ;

	t_build (&mem, 2, segs, (int) (sizeof (segs) / sizeof (segs [0])), NULL) ;
	want_frames = t_expect (2, segs, (int) (sizeof (segs) / sizeof (segs [0])), want, T_MAX_FRAMES) ;
	TASK_CHECK (want_frames == 2 * 9 + 5 + 9 + 3 + 9) ;

	sf = task_open (&mem, &info) ;
	TASK_CHECK (sf != NULL) ;
	TASK_CHECK (info.frames == want_frames) ;
	TASK_CHECK (info.channels == 2) ;

	got_frames = sf_readf_short (sf, got, want_frames) ;
	TASK_CHECK (got_frames == want_frames) ;
	TASK_CHECK (memcmp (got, want, (size_t) want_frames * 2 * sizeof (short)) == 0) ;

	/* The held frame must be the decoded final frame, not the block
	** header predictor (1000/-1000) and not zero.
	*/
	TASK_CHECK (want [9 * 2] != 1000 && want [9 * 2] != 0) ;
	TASK_CHECK (want [9 * 2 + 1] != -1000 && want [9 * 2 + 1] != 0) ;

	TASK_CHECK (sf_close (sf) == 0) ;
	return 0 ;
} /* t_dec_stereo_full */

static int
t_dec_mono (void)
{	task_memory	mem ;
	SF_INFO		info ;
	SNDFILE		*sf ;
	short		got [T_MAX_FRAMES] ;
	short		want [T_MAX_FRAMES] ;
	sf_count_t	wf, gf ;

	T_SEG segs [] =
	{	{ 2, 0, NULL, 4, 0 },					/* leading silence */
		{ 1, 2, &(t_blocks_mono [0]), 0, 0 },
		{ 2, 0, NULL, 6, 0 },
		{ 1, 1, &(t_blocks_mono [2]), 0, 0 },
	} ;

	t_build (&mem, 1, segs, 4, NULL) ;
	wf = t_expect (1, segs, 4, want, T_MAX_FRAMES) ;
	TASK_CHECK (wf == 4 + 18 + 6 + 9) ;

	sf = task_open (&mem, &info) ;
	TASK_CHECK (sf != NULL) ;
	TASK_CHECK (info.channels == 1 && info.frames == wf) ;

	gf = sf_readf_short (sf, got, wf) ;
	TASK_CHECK (gf == wf) ;
	TASK_CHECK (memcmp (got, want, (size_t) wf * sizeof (short)) == 0) ;

	/* Leading slnt before any data is zero. */
	TASK_CHECK (got [0] == 0 && got [3] == 0) ;
	TASK_CHECK (got [4] != 0) ;

	TASK_CHECK (sf_close (sf) == 0) ;
	return 0 ;
} /* t_dec_mono */

static int
t_dec_four_readers (void)
{	task_memory	mem ;
	SF_INFO		info ;
	SNDFILE		*sf ;
	short		ss [64] ;
	int			ii [64] ;
	float		ff [64] ;
	double		dd [64] ;
	short		want [64] ;
	sf_count_t	wf ;
	int			k ;

	T_SEG segs [] =
	{	{ 1, 1, &(t_blocks_stereo [1]), 0, 0 },
		{ 2, 0, NULL, 4, 0 },
		{ 1, 1, &(t_blocks_stereo [0]), 0, 0 },
	} ;

	t_build (&mem, 2, segs, 3, NULL) ;
	wf = t_expect (2, segs, 3, want, 32) ;
	TASK_CHECK (wf == 22) ;

	sf = task_open (&mem, &info) ;
	TASK_CHECK (sf != NULL) ;

	/* Interleave the four readers on the same axis. */
	TASK_CHECK (sf_readf_short (sf, ss, 3) == 3) ;
	TASK_CHECK (sf_readf_int (sf, ii, 3) == 3) ;
	TASK_CHECK (sf_readf_float (sf, ff, 3) == 3) ;
	TASK_CHECK (sf_readf_double (sf, dd, 3) == 3) ;

	for (k = 0 ; k < 6 ; k++)
	{	TASK_CHECK (ss [k] == want [k]) ;
		TASK_CHECK (ii [k] == t_shift16 (want [3 * 2 + k])) ;
		TASK_CHECK (ff [k] == (float) want [6 * 2 + k] / (float) 0x8000) ;
		TASK_CHECK (dd [k] == (double) want [9 * 2 + k] / (double) 0x8000) ;
		} ;

	/* Continue with mixed readers to the end; every value must match. */
	TASK_CHECK (sf_seek (sf, 0, SEEK_SET) == 0) ;
	{	int frames = 0 ;
		while (frames < (int) wf)
		{	int n = wf - frames ;
			if (n > 2) n = 2 ;
			TASK_CHECK (sf_readf_float (sf, ff, n) == n) ;
			for (k = 0 ; k < n * 2 ; k++)
				TASK_CHECK (ff [k] == (float) want [frames * 2 + k] / (float) 0x8000) ;
			frames += n ;
			if (frames >= (int) wf)
				break ;
			n = wf - frames ;
			if (n > 3) n = 3 ;
			TASK_CHECK (sf_readf_double (sf, dd, n) == n) ;
			for (k = 0 ; k < n * 2 ; k++)
				TASK_CHECK (dd [k] == (double) want [frames * 2 + k] / (double) 0x8000) ;
			frames += n ;
			if (frames >= (int) wf)
				break ;
			n = wf - frames ;
			if (n > 1) n = 1 ;
			TASK_CHECK (sf_readf_int (sf, ii, n) == n) ;
			for (k = 0 ; k < n * 2 ; k++)
				TASK_CHECK (ii [k] == t_shift16 (want [frames * 2 + k])) ;
			frames += n ;
			if (frames >= (int) wf)
				break ;
			n = wf - frames ;
			if (n > 2) n = 2 ;
			TASK_CHECK (sf_readf_short (sf, ss, n) == n) ;
			for (k = 0 ; k < n * 2 ; k++)
				TASK_CHECK (ss [k] == want [frames * 2 + k]) ;
			frames += n ;
			} ;
		TASK_CHECK (frames == (int) wf) ;
		} ;

	TASK_CHECK (sf_close (sf) == 0) ;
	return 0 ;
} /* t_dec_four_readers */

static int
t_dec_slnt_only_and_empty (void)
{	task_memory	mem ;
	SF_INFO		info ;
	SNDFILE		*sf ;
	short		got [64] ;

	/* Only slnt entries and empty data: every frame is zero silence. */
	T_SEG only_slnt [] =
	{	{ 2, 0, NULL, 4, 0 },
		{ 1, 0, NULL, 0, 0 },				/* empty data, no hold reset */
		{ 2, 0, NULL, 3, 0 },
	} ;

	t_build (&mem, 2, only_slnt, 3, NULL) ;
	sf = task_open (&mem, &info) ;
	TASK_CHECK (sf != NULL) ;
	TASK_CHECK (info.frames == 7) ;
	memset (got, 0x77, sizeof (got)) ;
	TASK_CHECK (sf_readf_short (sf, got, 7) == 7) ;
	{	int k ;
		for (k = 0 ; k < 14 ; k++)
			TASK_CHECK (got [k] == 0) ;
		} ;
	/* A seek inside the hold still yields zero. */
	TASK_CHECK (sf_seek (sf, 5, SEEK_SET) == 5) ;
	TASK_CHECK (sf_readf_short (sf, got, 2) == 2) ;
	TASK_CHECK (got [0] == 0 && got [3] == 0) ;
	TASK_CHECK (sf_close (sf) == 0) ;

	/* A wavl body with no subchunks at all opens as zero frames. */
	{	unsigned char *p = mem.bytes ;
		size_t list_size_field = 12 + 8 + 20 + 4 ;
		/* Shrink the LIST to the four byte form tag and patch RIFF. */
		task_patch32 (&mem, list_size_field, 4) ;
		mem.n = (size_t) list_size_field + 4 + 4 ;
		task_patch32 (&mem, 4, (uint32_t) (mem.n - 8)) ;
		(void) p ;
		sf = task_open (&mem, &info) ;
		TASK_CHECK (sf != NULL) ;
		TASK_CHECK (info.frames == 0) ;
		TASK_CHECK (sf_seek (sf, 0, SEEK_SET) == 0) ;
		TASK_CHECK (sf_readf_short (sf, got, 4) == 0) ;
		TASK_CHECK (sf_close (sf) == 0) ;
		} ;

	return 0 ;
} /* t_dec_slnt_only_and_empty */

static int
t_dec_chunk_sizes (void)
{	task_memory	mem ;
	SF_INFO		info ;
	SNDFILE		*sf ;
	short		want [T_MAX_FRAMES * 2] ;
	short		got [T_MAX_FRAMES * 2] ;
	sf_count_t	wf ;
	int			chunk ;

	T_SEG segs [] =
	{	{ 1, 2, &(t_blocks_stereo [0]), 0, 0 },
		{ 2, 0, NULL, 7, 0 },
		{ 1, 2, &(t_blocks_stereo [1]), 0, 0 },
	} ;

	t_build (&mem, 2, segs, 3, NULL) ;
	wf = t_expect (2, segs, 3, want, T_MAX_FRAMES) ;
	TASK_CHECK (wf == 18 + 7 + 18) ;

	for (chunk = 0 ; chunk < 3 ; chunk++)
	{	int sizes [] = { 1, 7, 19 } ;
		int step = sizes [chunk] ;
		sf_count_t done = 0 ;

		sf = task_open (&mem, &info) ;
		TASK_CHECK (sf != NULL && info.frames == wf) ;

		while (done < wf)
		{	sf_count_t n = wf - done ;
			sf_count_t r ;
			if (n > step)
				n = step ;
			r = sf_readf_short (sf, got + done * 2, n) ;
			TASK_CHECK (r == n) ;
			done += r ;
			} ;

		TASK_CHECK (done == wf) ;
		TASK_CHECK (memcmp (got, want, (size_t) wf * 2 * sizeof (short)) == 0) ;
		TASK_CHECK (sf_close (sf) == 0) ;
		} ;

	return 0 ;
} /* t_dec_chunk_sizes */

static int
t_dec_hold_edge_cases (void)
{	task_memory	mem ;
	SF_INFO		info ;
	SNDFILE		*sf ;
	short		want [T_MAX_FRAMES * 2] ;
	short		got [T_MAX_FRAMES * 2] ;
	sf_count_t	wf ;

	/* Leading slnt, consecutive slnt, empty data and zero length slnt:
	** none of these may reset the hold value.
	*/
	T_SEG segs [] =
	{	{ 2, 0, NULL, 3, 0 },					/* leading: zero */
		{ 1, 1, &(t_blocks_stereo [2]), 0, 0 },
		{ 1, 0, NULL, 0, 0 },					/* empty data */
		{ 2, 0, NULL, 2, 0 },
		{ 2, 0, NULL, 4, 0 },					/* consecutive slnt */
		{ 2, 0, NULL, 0, 0 },					/* zero length slnt */
		{ 1, 1, &(t_blocks_stereo [3]), 0, 0 },
	} ;

	t_build (&mem, 2, segs, 7, NULL) ;
	wf = t_expect (2, segs, 7, want, T_MAX_FRAMES) ;
	TASK_CHECK (wf == 3 + 9 + 6 + 9) ;

	sf = task_open (&mem, &info) ;
	TASK_CHECK (sf != NULL && info.frames == wf) ;
	TASK_CHECK (sf_readf_short (sf, got, wf) == wf) ;
	TASK_CHECK (memcmp (got, want, (size_t) wf * 2 * sizeof (short)) == 0) ;

	/* Frames across the consecutive/zero slnt boundary all repeat the
	** same held frame.
	*/
	{	int f ;
		for (f = 12 ; f < 18 ; f++)
			TASK_CHECK (got [f * 2] == want [11 * 2] && got [f * 2 + 1] == want [11 * 2 + 1]) ;
		} ;

	TASK_CHECK (sf_close (sf) == 0) ;
	return 0 ;
} /* t_dec_hold_edge_cases */

static int
t_dec_odd_unknown (void)
{	task_memory	mem ;
	SF_INFO		info ;
	SNDFILE		*sf ;
	short		want [256 * 2] ;
	short		got [256 * 2] ;
	sf_count_t	wf ;

	T_SEG segs [] =
	{	{ 3, 0, NULL, 0, 5 },						/* odd unknown + pad */
		{ 1, 1, &(t_blocks_stereo [0]), 0, 0 },
		{ 3, 0, NULL, 0, 4 },						/* even unknown */
		{ 2, 0, NULL, 4, 0 },
		{ 3, 0, NULL, 0, 7 },						/* odd unknown + pad */
		{ 1, 1, &(t_blocks_stereo [1]), 0, 0 },
	} ;

	t_build (&mem, 2, segs, 6, NULL) ;
	wf = t_expect (2, segs, 6, want, 256) ;
	TASK_CHECK (wf == 9 + 4 + 9) ;

	sf = task_open (&mem, &info) ;
	TASK_CHECK (sf != NULL && info.frames == wf) ;
	TASK_CHECK (sf_readf_short (sf, got, wf) == wf) ;
	TASK_CHECK (memcmp (got, want, (size_t) wf * 2 * sizeof (short)) == 0) ;
	TASK_CHECK (sf_close (sf) == 0) ;
	return 0 ;
} /* t_dec_odd_unknown */

/*================================================================================
** seeking group
*/

static int
t_seek_vs_sequential (void)
{	task_memory	mem ;
	SF_INFO		info ;
	SNDFILE		*sf ;
	short		want [512 * 2] ;
	short		got [64 * 2] ;
	sf_count_t	wf ;
	sf_count_t	pos ;
	T_SEG s [] =
	{	{ 1, 2, &(t_blocks_stereo [0]), 0, 0 },
		{ 2, 0, NULL, 6, 0 },
		{ 1, 1, &(t_blocks_stereo [1]), 0, 0 },
		{ 2, 0, NULL, 5, 0 },
		{ 1, 2, &(t_blocks_stereo [2]), 0, 0 },
	} ;

	t_build (&mem, 2, s, 5, NULL) ;
	wf = t_expect (2, s, 5, want, 512) ;
	TASK_CHECK (wf == 18 + 6 + 9 + 5 + 18) ;

	sf = task_open (&mem, &info) ;
	TASK_CHECK (sf != NULL && info.frames == wf) ;

	/* Every frame position: seek then read, output must match the
	** sequential axis, including block-internal and slnt-middle points.
	*/
	for (pos = 0 ; pos < wf ; pos++)
	{	sf_count_t r, n = wf - pos ;
		if (n > 5)
			n = 5 ;
		TASK_CHECK (sf_seek (sf, pos, SEEK_SET) == pos) ;
		r = sf_readf_short (sf, got, n) ;
		TASK_CHECK (r == n) ;
		TASK_CHECK (memcmp (got, want + pos * 2, (size_t) n * 2 * sizeof (short)) == 0) ;
		} ;

	/* SEEK_CUR and SEEK_END produce the same axis. */
	TASK_CHECK (sf_seek (sf, 0, SEEK_SET) == 0) ;
	TASK_CHECK (sf_seek (sf, 7, SEEK_CUR) == 7) ;
	TASK_CHECK (sf_readf_short (sf, got, 4) == 4) ;
	TASK_CHECK (memcmp (got, want + 7 * 2, 4 * 2 * sizeof (short)) == 0) ;

	TASK_CHECK (sf_seek (sf, -10, SEEK_END) == wf - 10) ;
	TASK_CHECK (sf_readf_short (sf, got, 10) == 10) ;
	TASK_CHECK (memcmp (got, want + (wf - 10) * 2, 10 * 2 * sizeof (short)) == 0) ;

	TASK_CHECK (sf_close (sf) == 0) ;
	return 0 ;
} /* t_seek_vs_sequential */

static int
t_seek_far_slnt_budget (void)
{	task_memory	mem ;
	SF_INFO		info ;
	SNDFILE		*sf ;
	short		hold [8] ;
	short		decoded [T_SPB * 2] ;
	task_memory	tb ;
	size_t		before ;
	const uint32_t big = 200000 ;

	T_SEG segs [] =
	{	{ 1, 3, &(t_blocks_stereo [0]), 0, 0 },
		{ 2, 0, NULL, big, 0 },
		{ 1, 1, &(t_blocks_stereo [3]), 0, 0 },
	} ;

	t_build (&mem, 2, segs, 3, NULL) ;

	sf = task_open (&mem, &info) ;
	TASK_CHECK (sf != NULL) ;
	TASK_CHECK (info.frames == (sf_count_t) (27 + big + 9)) ;

	/* Reference final frame of the last block of the first data segment. */
	tb.n = 0 ;
	t_put_block (&tb, 2, &(t_blocks_stereo [2])) ;
	t_ref_decode (tb.bytes, 2, T_BLOCKALIGN_ST, T_SPB, decoded) ;

	/* Jump straight into a never-visited remote slnt region. */
	before = mem.bytes_read ;
	TASK_CHECK (sf_seek (sf, 27 + big / 2, SEEK_SET) == 27 + (sf_count_t) big / 2) ;
	TASK_CHECK (sf_readf_short (sf, hold, 4) == 4) ;

	/* Only one compressed block (plus a seek) may be touched; the IO
	** volume must not grow with the slnt count or recording length.
	*/
	TASK_CHECK (mem.bytes_read - before <= (size_t) (2 * T_BLOCKALIGN_ST + 16)) ;
	{	int k ;
		for (k = 0 ; k < 4 ; k++)
		{	TASK_CHECK (hold [2 * k] == decoded [(T_SPB - 1) * 2]) ;
			TASK_CHECK (hold [2 * k + 1] == decoded [(T_SPB - 1) * 2 + 1]) ;
			} ;
		} ;

	TASK_CHECK (sf_close (sf) == 0) ;
	return 0 ;
} /* t_seek_far_slnt_budget */

static int
t_seek_independent_instances (void)
{	task_memory	m1, m2 ;
	SF_INFO		i1, i2 ;
	SNDFILE		*f1, *f2 ;
	short		want1 [256 * 2], want2 [256 * 2] ;
	short		buf [16] ;
	sf_count_t	w1, w2 ;

	T_SEG s1 [] =
	{	{ 1, 1, &(t_blocks_stereo [0]), 0, 0 },
		{ 2, 0, NULL, 8, 0 },
		{ 1, 1, &(t_blocks_stereo [1]), 0, 0 },
	} ;
	T_SEG s2 [] =
	{	{ 2, 0, NULL, 3, 0 },
		{ 1, 2, &(t_blocks_stereo [2]), 0, 0 },
		{ 2, 0, NULL, 4, 0 },
	} ;

	t_build (&m1, 2, s1, 3, NULL) ;
	t_build (&m2, 2, s2, 3, NULL) ;
	w1 = t_expect (2, s1, 3, want1, 256) ;
	w2 = t_expect (2, s2, 3, want2, 256) ;

	f1 = task_open (&m1, &i1) ;
	f2 = task_open (&m2, &i2) ;
	TASK_CHECK (f1 != NULL && f2 != NULL) ;
	TASK_CHECK (i1.frames == w1 && i2.frames == w2) ;

	/* Interleave seeks and reads on two handles; neither may disturb the
	** other's position.
	*/
	TASK_CHECK (sf_seek (f1, 10, SEEK_SET) == 10) ;
	TASK_CHECK (sf_seek (f2, 2, SEEK_SET) == 2) ;
	TASK_CHECK (sf_readf_short (f1, buf, 3) == 3) ;
	TASK_CHECK (memcmp (buf, want1 + 20, 3 * 2 * sizeof (short)) == 0) ;
	TASK_CHECK (sf_readf_short (f2, buf, 3) == 3) ;
	TASK_CHECK (memcmp (buf, want2 + 4, 3 * 2 * sizeof (short)) == 0) ;

	TASK_CHECK (sf_seek (f1, 0, SEEK_SET) == 0) ;
	TASK_CHECK (sf_readf_short (f1, buf, 2) == 2) ;
	TASK_CHECK (memcmp (buf, want1, 2 * 2 * sizeof (short)) == 0) ;
	TASK_CHECK (sf_seek (f2, w2 - 1, SEEK_SET) == w2 - 1) ;
	TASK_CHECK (sf_readf_short (f2, buf, 1) == 1) ;
	TASK_CHECK (memcmp (buf, want2 + (w2 - 1) * 2, 2 * sizeof (short)) == 0) ;

	TASK_CHECK (sf_close (f1) == 0 && sf_close (f2) == 0) ;
	return 0 ;
} /* t_seek_independent_instances */

static int
t_seek_64bit_tail (void)
{	task_memory	mem ;
	SF_INFO		info ;
	SNDFILE		*sf ;
	short		tail [16] ;
	short		decoded [T_SPB * 2] ;
	task_memory	tb ;
	const uint32_t huge = 4000000000u ;
	sf_count_t total ;

	T_SEG segs [] =
	{	{ 1, 2, &(t_blocks_stereo [0]), 0, 0 },
		{ 2, 0, NULL, huge, 0 },
		{ 1, 2, &(t_blocks_stereo [1]), 0, 0 },
		{ 2, 0, NULL, 7, 0 },
	} ;

	t_build (&mem, 2, segs, 4, NULL) ;
	total = (sf_count_t) 18 + (sf_count_t) huge + 18 + 7 ;

	sf = task_open (&mem, &info) ;
	TASK_CHECK (sf != NULL) ;
	TASK_CHECK (info.frames == total) ;

	/* 64 bit end positioning across the giant slnt and later segments. */
	TASK_CHECK (sf_seek (sf, 0, SEEK_END) == total) ;
	TASK_CHECK (sf_seek (sf, -5, SEEK_END) == total - 5) ;
	TASK_CHECK (sf_readf_short (sf, tail, 5) == 5) ;

	tb.n = 0 ;
	t_put_block (&tb, 2, &(t_blocks_stereo [2])) ;
	t_ref_decode (tb.bytes, 2, T_BLOCKALIGN_ST, T_SPB, decoded) ;
	/* The final slnt repeats the last frame of the preceding data block. */
	TASK_CHECK (tail [0] == decoded [(T_SPB - 1) * 2]) ;
	TASK_CHECK (tail [1] == decoded [(T_SPB - 1) * 2 + 1]) ;
	TASK_CHECK (tail [8] == decoded [(T_SPB - 1) * 2]) ;

	/* Absolute 64 bit position inside the giant hold region. */
	TASK_CHECK (sf_seek (sf, total - 30, SEEK_SET) == total - 30) ;
	TASK_CHECK (sf_readf_short (sf, tail, 6) == 6) ;

	/* The legal tail position reads zero frames. */
	TASK_CHECK (sf_seek (sf, total, SEEK_SET) == total) ;
	TASK_CHECK (sf_readf_short (sf, tail, 2) == 0) ;

	TASK_CHECK (sf_close (sf) == 0) ;
	return 0 ;
} /* t_seek_64bit_tail */

static int
t_seek_boundary_in_block (void)
{	task_memory	mem ;
	SF_INFO		info ;
	SNDFILE		*sf ;
	short		want [256 * 2] ;
	short		buf [32] ;
	sf_count_t	wf ;

	/* One slnt starts and ends inside what would be a 9-frame decode
	** block window; the suffix after a seek must equal the sequential
	** read.
	*/
	T_SEG segs [] =
	{	{ 1, 2, &(t_blocks_stereo [0]), 0, 0 },
		{ 2, 0, NULL, 4, 0 },
		{ 1, 2, &(t_blocks_stereo [2]), 0, 0 },
	} ;

	t_build (&mem, 2, segs, 3, NULL) ;
	wf = t_expect (2, segs, 3, want, 256) ;
	TASK_CHECK (wf == 40) ;

	sf = task_open (&mem, &info) ;
	TASK_CHECK (sf != NULL) ;

	{	const int targets [] = { 8, 9, 12, 13, 17, 18, 22, 31, 39 } ;
		unsigned k ;
		for (k = 0 ; k < sizeof (targets) / sizeof (targets [0]) ; k++)
		{	int p = targets [k] ;
			TASK_CHECK (sf_seek (sf, p, SEEK_SET) == p) ;
			TASK_CHECK (sf_readf_short (sf, buf, 4) == (p + 4 <= wf ? 4 : wf - p)) ;
			TASK_CHECK (memcmp (buf, want + p * 2, (size_t) (p + 4 <= wf ? 4 : wf - p) * 2 * sizeof (short)) == 0) ;
			} ;
		} ;

	TASK_CHECK (sf_close (sf) == 0) ;
	return 0 ;
} /* t_seek_boundary_in_block */

/*================================================================================
** bounds group
*/

static int
t_bounds_disk_and_eof (void)
{	task_memory	mem ;
	SF_INFO		vinfo, dinfo ;
	SNDFILE		*vsf, *dsf ;
	char		path [512] ;
	FILE		*fp ;
	short		buf [32] ;
	sf_count_t	wf ;

	T_SEG segs [] =
	{	{ 1, 1, &(t_blocks_stereo [0]), 0, 0 },
		{ 2, 0, NULL, 5, 0 },
		{ 1, 1, &(t_blocks_stereo [1]), 0, 0 },
	} ;

	t_build (&mem, 2, segs, 3, NULL) ;
	wf = 9 + 5 + 9 ;

	/* Virtual entry. */
	vsf = task_open (&mem, &vinfo) ;
	TASK_CHECK (vsf != NULL && vinfo.frames == wf) ;

	/* Real file entry. */
	snprintf (path, sizeof (path), "%s/wavl_contract.wav", getenv ("TMPDIR")) ;
	fp = fopen (path, "wb") ;
	TASK_CHECK (fp != NULL) ;
	TASK_CHECK (fwrite (mem.bytes, 1, mem.n, fp) == mem.n) ;
	fclose (fp) ;

	memset (&dinfo, 0, sizeof (dinfo)) ;
	dsf = sf_open (path, SFM_READ, &dinfo) ;
	TASK_CHECK (dsf != NULL && dinfo.frames == wf) ;

	/* EOF: tail position is legal, a read yields zero and zero fills. */
	memset (buf, 0x55, sizeof (buf)) ;
	TASK_CHECK (sf_seek (dsf, wf, SEEK_SET) == wf) ;
	TASK_CHECK (sf_readf_short (dsf, buf, 3) == 0) ;
	TASK_CHECK (buf [0] == 0 && buf [5] == 0) ;

	/* Reading across EOF returns the available tail then stops. */
	TASK_CHECK (sf_seek (dsf, wf - 2, SEEK_SET) == wf - 2) ;
	TASK_CHECK (sf_readf_short (dsf, buf, 8) == 2) ;

	TASK_CHECK (sf_close (dsf) == 0) ;
	TASK_CHECK (sf_close (vsf) == 0) ;
	remove (path) ;
	return 0 ;
} /* t_bounds_disk_and_eof */

static int
t_bounds_bad_open (void)
{	task_memory	good ;
	SF_INFO		info ;
	T_SEG segs [] =
	{	{ 1, 1, &(t_blocks_stereo [0]), 0, 0 },
		{ 2, 0, NULL, 4, 0 },
		{ 1, 1, &(t_blocks_stereo [1]), 0, 0 },
	} ;

	t_build (&good, 2, segs, 3, NULL) ;

	/* Truncation inside the LIST body. */
	{	task_memory m = good ;
		m.n -= 6 ;
		TASK_CHECK (task_open (&m, &info) == NULL) ;
		} ;

	/* Data payload not a whole number of blocks. */
	{	task_memory m = good ;
		task_patch32 (&m, 0, 0) ; /* dummy to keep helper referenced */
		/* Locate the first data chunk size field (after RIFF/WAVE/fmt/LIST/wavl). */
		{	size_t p = 12 + 8 + 20 + 8 + 4 + 4 ;
			task_patch32 (&m, p, 15) ;	/* 15 is not a multiple of 16 */
			TASK_CHECK (task_open (&m, &info) == NULL) ;
			} ;
		} ;

	/* Subchunk declared beyond the LIST boundary. */
	{	task_memory m = good ;
		size_t list_size_field = 12 + 8 + 20 + 4 ;
		task_patch32 (&m, list_size_field, 1000000) ;
		TASK_CHECK (task_open (&m, &info) == NULL) ;
		} ;

	/* RIFF size beyond the actual file. */
	{	task_memory m = good ;
		task_patch32 (&m, 4, 1000000) ;
		TASK_CHECK (task_open (&m, &info) == NULL) ;
		} ;

	/* slnt with the wrong payload size. */
	{	task_memory m = good ;
		/* The first slnt header follows the first 16-byte data payload. */
		size_t slnt_size_field = 12 + 8 + 20 + 8 + 4 + 8 + 16 + 4 ;
		task_patch32 (&m, slnt_size_field, 8) ;
		TASK_CHECK (task_open (&m, &info) == NULL) ;
		} ;

	/* Invalid block header: step index out of range. */
	{	task_memory m = good ;
		size_t first_data = 12 + 8 + 20 + 8 + 4 + 8 ;
		m.bytes [first_data + 6] = 89 ;		/* left step index */
		TASK_CHECK (task_open (&m, &info) == NULL) ;
		} ;

	/* Invalid block header: synchronisation byte non-zero. */
	{	task_memory m = good ;
		size_t first_data = 12 + 8 + 20 + 8 + 4 + 8 ;
		m.bytes [first_data + 7] = 1 ;
		TASK_CHECK (task_open (&m, &info) == NULL) ;
		} ;

	/* fact mismatch. */
	{	uint32_t fact = 999 ;
		task_memory m ;
		t_build (&m, 2, segs, 3, &fact) ;
		TASK_CHECK (task_open (&m, &info) == NULL) ;
		} ;

	/* Correct fact value opens fine. */
	{	uint32_t fact = (uint32_t) (9 + 4 + 9) ;
		task_memory m ;
		t_build (&m, 2, segs, 3, &fact) ;
		SNDFILE *sf = task_open (&m, &info) ;
		TASK_CHECK (sf != NULL && info.frames == fact) ;
		TASK_CHECK (sf_close (sf) == 0) ;
		} ;

	return 0 ;
} /* t_bounds_bad_open */

static int
t_bounds_duplicate_and_nested (void)
{	task_memory	b ;
	SF_INFO		info ;
	size_t		list1_size ;

	t_ensure_vocab () ;

	/* Two audio LIST/wavl roots. */
	memset (&b, 0, sizeof (b)) ;
	task_tag (&b, "RIFF") ;
	task_u32 (&b, 0) ;
	task_tag (&b, "WAVE") ;
	t_fmt (&b, 2) ;

	task_tag (&b, "LIST") ;
	task_u32 (&b, 0) ;
	task_tag (&b, "wavl") ;
	task_tag (&b, "slnt") ;
	task_u32 (&b, 4) ;
	task_u32 (&b, 2) ;
	list1_size = b.n - (12 + 8 + 20) - 8 ;
	task_patch32 (&b, 12 + 8 + 20 + 4, (uint32_t) list1_size) ;

	task_tag (&b, "LIST") ;
	task_u32 (&b, 0) ;
	task_tag (&b, "wavl") ;
	task_tag (&b, "slnt") ;
	task_u32 (&b, 4) ;
	task_u32 (&b, 1) ;
	task_patch32 (&b, b.n - 8 - 4 - 8, (uint32_t) (4 + 8)) ;
	task_patch32 (&b, 4, (uint32_t) (b.n - 8)) ;
	TASK_CHECK (task_open (&b, &info) == NULL) ;

	/* Nested LIST inside wavl. */
	memset (&b, 0, sizeof (b)) ;
	task_tag (&b, "RIFF") ;
	task_u32 (&b, 0) ;
	task_tag (&b, "WAVE") ;
	t_fmt (&b, 2) ;
	task_tag (&b, "LIST") ;
	task_u32 (&b, 4 + 8 + 12) ;
	task_tag (&b, "wavl") ;
	task_tag (&b, "LIST") ;
	task_u32 (&b, 8) ;
	task_tag (&b, "INFO") ;
	task_u32 (&b, 0) ;
	task_patch32 (&b, 4, (uint32_t) (b.n - 8)) ;
	TASK_CHECK (task_open (&b, &info) == NULL) ;

	/* Top level data together with a wavl LIST. */
	memset (&b, 0, sizeof (b)) ;
	task_tag (&b, "RIFF") ;
	task_u32 (&b, 0) ;
	task_tag (&b, "WAVE") ;
	t_fmt (&b, 2) ;
	task_tag (&b, "data") ;
	task_u32 (&b, 16) ;
	{	T_BLOCK d = t_blocks_stereo [0] ;
		t_put_block (&b, 2, &d) ;
		} ;
	task_tag (&b, "LIST") ;
	task_u32 (&b, 4 + 8 + 4) ;
	task_tag (&b, "wavl") ;
	task_tag (&b, "slnt") ;
	task_u32 (&b, 4) ;
	task_u32 (&b, 1) ;
	task_patch32 (&b, 4, (uint32_t) (b.n - 8)) ;
	TASK_CHECK (task_open (&b, &info) == NULL) ;

	return 0 ;
} /* t_bounds_duplicate_and_nested */

static int
t_bounds_failed_seek_position (void)
{	task_memory	mem ;
	SF_INFO		info ;
	SNDFILE		*sf ;
	short		want [256 * 2] ;
	short		buf [16] ;
	sf_count_t	wf ;

	T_SEG segs [] =
	{	{ 1, 1, &(t_blocks_stereo [0]), 0, 0 },
		{ 2, 0, NULL, 6, 0 },
		{ 1, 1, &(t_blocks_stereo [2]), 0, 0 },
	} ;

	t_build (&mem, 2, segs, 3, NULL) ;
	wf = t_expect (2, segs, 3, want, 256) ;

	sf = task_open (&mem, &info) ;
	TASK_CHECK (sf != NULL && info.frames == wf) ;

	TASK_CHECK (sf_seek (sf, 5, SEEK_SET) == 5) ;

	/* Out of range seeks fail and leave the next frame unchanged. */
	TASK_CHECK (sf_seek (sf, wf + 1, SEEK_SET) == -1) ;
	TASK_CHECK (sf_seek (sf, -1, SEEK_SET) == -1) ;
	TASK_CHECK (sf_seek (sf, 100, SEEK_CUR) == -1) ;
	TASK_CHECK (sf_readf_short (sf, buf, 3) == 3) ;
	TASK_CHECK (memcmp (buf, want + 5 * 2, 3 * 2 * sizeof (short)) == 0) ;

	TASK_CHECK (sf_close (sf) == 0) ;
	return 0 ;
} /* t_bounds_failed_seek_position */

static int
t_bounds_io_budget (void)
{	task_memory	mem ;
	SF_INFO		info ;
	T_LIM		lim ;
	T_SEG segs [] =
	{	{ 1, 2, &(t_blocks_stereo [0]), 0, 0 },
		{ 2, 0, NULL, 4, 0 },
		{ 1, 1, &(t_blocks_stereo [1]), 0, 0 },
	} ;

	t_build (&mem, 2, segs, 3, NULL) ;

	/* A budget far below the header size must fail before audio is
	** handed out.
	*/
	lim.mem = &mem ;
	lim.cap = 24 ;
	lim.denied = 0 ;
	TASK_CHECK (t_open_lim (&lim, &info) == NULL) ;
	TASK_CHECK (lim.denied > 0) ;

	/* A generous budget opens normally and reads the full axis. */
	lim.cap = mem.n + 4096 ;
	lim.denied = 0 ;
	{	SNDFILE *sf = t_open_lim (&lim, &info) ;
		short got [128] ;
		TASK_CHECK (sf != NULL) ;
		TASK_CHECK (sf_readf_short (sf, got, info.frames) == info.frames) ;
		TASK_CHECK (sf_close (sf) == 0) ;
		} ;

	return 0 ;
} /* t_bounds_io_budget */

/*================================================================================
** Registration
*/

const struct task_case task_cases [] =
{
	{ "decoding", "stereo_full_axis",		t_dec_stereo_full },
	{ "decoding", "mono_leading_silence",	t_dec_mono },
	{ "decoding", "four_readers_interleaved", t_dec_four_readers },
	{ "decoding", "slnt_only_and_empty_list",	t_dec_slnt_only_and_empty },
	{ "decoding", "chunk_sizes_1_7_19",		t_dec_chunk_sizes },
	{ "decoding", "hold_edge_cases",		t_dec_hold_edge_cases },
	{ "decoding", "odd_unknown_chunks",		t_dec_odd_unknown },

	{ "seeking", "matches_sequential",		t_seek_vs_sequential },
	{ "seeking", "far_slnt_io_budget",		t_seek_far_slnt_budget },
	{ "seeking", "independent_instances",	t_seek_independent_instances },
	{ "seeking", "tail_64bit_huge_slnt",	t_seek_64bit_tail },
	{ "seeking", "boundaries_inside_block",	t_seek_boundary_in_block },

	{ "bounds", "disk_entry_and_eof",		t_bounds_disk_and_eof },
	{ "bounds", "malformed_open_failures",	t_bounds_bad_open },
	{ "bounds", "duplicate_root_and_nested", t_bounds_duplicate_and_nested },
	{ "bounds", "failed_seek_keeps_position", t_bounds_failed_seek_position },
	{ "bounds", "counting_vio_budget",		t_bounds_io_budget },
} ;

const size_t task_case_count = sizeof (task_cases) / sizeof (task_cases [0]) ;

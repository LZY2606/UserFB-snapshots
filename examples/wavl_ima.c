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
** examples/wavl_ima.c
**
** Build a short segmented IMA-ADPCM WAVE file (LIST/wavl with data and
** slnt subchunks) on disk, open it through the public libsndfile API,
** seek at random points of the expanded frame axis and check that the
** held frames inside 'slnt' regions repeat the last decoded frame of
** the preceding 'data' region. The program prints WAVL_IMA_DEMO_OK on
** success.
**
** Supported scope (see src/wavl.c): seekable little-endian RIFF/WAVE,
** mono/stereo IMA-ADPCM, one audio LIST/wavl, no top level data chunk,
** read-only access. Call sf_open()/sf_open_virtual() as for any other
** WAVE file; sf_seek() and the sf_readf_*() family operate on the
** expanded logical frame axis, so slnt silence is invisible to the
** caller except for the repeated sample values.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <sndfile.h>

#define BLOCKALIGN	16
#define SPB			9
#define CHANNELS	2

static void
put16 (unsigned char *p, uint16_t v)
{	p [0] = (unsigned char) v ;
	p [1] = (unsigned char) (v >> 8) ;
} /* put16 */

static void
put32 (unsigned char *p, uint32_t v)
{	put16 (p, (uint16_t) v) ;
	put16 (p + 2, (uint16_t) (v >> 16)) ;
} /* put32 */

static size_t
tag (unsigned char *b, size_t n, const char *s)
{	memcpy (b + n, s, 4) ;
	return n + 4 ;
} /* tag */

/* One stereo IMA block: 4 byte header per channel then 8 nibble bytes,
** four carrying the left codes and four the right codes.
*/
static size_t
ima_block (unsigned char *b, size_t n, int lp, int li, int lcode, int rp, int ri, int rcode)
{	int k ;

	put16 (b + n, (uint16_t) lp) ;
	put16 (b + n + 2, (uint16_t) li) ;
	n += 4 ;
	put16 (b + n, (uint16_t) rp) ;
	put16 (b + n + 2, (uint16_t) ri) ;
	n += 4 ;

	for (k = 0 ; k < 4 ; k++)
		b [n++] = (unsigned char) (lcode | (lcode << 4)) ;
	for (k = 0 ; k < 4 ; k++)
		b [n++] = (unsigned char) (rcode | (rcode << 4)) ;

	return n ;
} /* ima_block */

static size_t
data_chunk (unsigned char *b, size_t n, size_t payload)
{	n = tag (b, n, "data") ;
	put32 (b + n, (uint32_t) payload) ;
	return n + 4 ;
} /* data_chunk */

static size_t
slnt_chunk (unsigned char *b, size_t n, uint32_t frames)
{	n = tag (b, n, "slnt") ;
	put32 (b + n, 4) ;
	put32 (b + n + 4, frames) ;
	return n + 8 ;
} /* slnt_chunk */

int
main (void)
{
	const char		*dir = getenv ("TMPDIR") ;
	char			 path [512] ;
	FILE			*fp ;
	SF_INFO			 info ;
	SNDFILE			*sf ;
	unsigned char	 buf [512] ;
	size_t			 n = 0, list_size_pos, riff_size_pos ;
	short			 pcm [64] ;
	sf_count_t		 got ;
	int				 i ;

	snprintf (path, sizeof (path), "%s/wavl_ima_demo.wav", (dir != NULL && dir [0]) ? dir : "/tmp") ;

	/* RIFF/WAVE header. */
	n = tag (buf, n, "RIFF") ;
	riff_size_pos = n ;
	n += 4 ;
	n = tag (buf, n, "WAVE") ;

	/* fmt chunk: IMA-ADPCM stereo, blockAlign 16, 9 samples per block. */
	n = tag (buf, n, "fmt ") ;
	put32 (buf + n, 20) ;
	n += 4 ;
	put16 (buf + n, 0x0011) ; n += 2 ;		/* WAVE_FORMAT_IMA_ADPCM */
	put16 (buf + n, CHANNELS) ; n += 2 ;
	put32 (buf + n, 8000) ; n += 4 ;
	put32 (buf + n, 14222) ; n += 4 ;
	put16 (buf + n, BLOCKALIGN) ; n += 2 ;
	put16 (buf + n, 4) ; n += 2 ;			/* bits per sample */
	put16 (buf + n, 2) ; n += 2 ;			/* extra bytes */
	put16 (buf + n, SPB) ; n += 2 ;			/* samples per block */

	/* Audio LIST/wavl: data, slnt, data. */
	n = tag (buf, n, "LIST") ;
	n += 4 ;								/* LIST size, patched below */
	list_size_pos = n - 4 ;
	n = tag (buf, n, "wavl") ;

	n = data_chunk (buf, n, BLOCKALIGN) ;
	n = ima_block (buf, n, 1000, 4, 1, -1000, 4, 9) ;

	n = slnt_chunk (buf, n, 5) ;

	n = data_chunk (buf, n, BLOCKALIGN) ;
	n = ima_block (buf, n, -2000, 6, 7, 2000, 6, 3) ;

	put32 (buf + list_size_pos, (uint32_t) (n - list_size_pos - 4)) ;
	put32 (buf + riff_size_pos, (uint32_t) (n - 8)) ;

	fp = fopen (path, "wb") ;
	if (fp == NULL)
	{	fprintf (stderr, "cannot create %s\n", path) ;
		return 1 ;
		} ;
	if (fwrite (buf, 1, n, fp) != n)
	{	fprintf (stderr, "short write\n") ;
		fclose (fp) ;
		return 1 ;
		} ;
	fclose (fp) ;

	memset (&info, 0, sizeof (info)) ;
	if ((sf = sf_open (path, SFM_READ, &info)) == NULL)
	{	fprintf (stderr, "sf_open failed: %s\n", sf_strerror (NULL)) ;
		return 1 ;
		} ;

	/* 9 + 5 + 9 = 23 expanded frames. */
	if (info.frames != 23 || info.channels != CHANNELS)
	{	fprintf (stderr, "unexpected SF_INFO: frames=%lld channels=%d\n",
			(long long) info.frames, info.channels) ;
		sf_close (sf) ;
		return 1 ;
		} ;

	/* Full sequential read. */
	got = sf_readf_short (sf, pcm, 23) ;
	if (got != 23)
	{	fprintf (stderr, "sequential read returned %lld\n", (long long) got) ;
		sf_close (sf) ;
		return 1 ;
		} ;

	/* The five slnt frames (9..13) must repeat frame 8 of the first data
	** block, i.e. the real final decoded frame, not its block header
	** predictor (1000/-1000).
	*/
	for (i = 9 ; i < 14 ; i++)
		if (pcm [2 * i] != pcm [16] || pcm [2 * i + 1] != pcm [17])
		{	fprintf (stderr, "slnt frame %d does not hold frame 8\n", i) ;
			sf_close (sf) ;
			return 1 ;
			} ;

	/* Random seek straight into the never re-read slnt region. */
	if (sf_seek (sf, 11, SEEK_SET) != 11)
	{	fprintf (stderr, "seek into slnt failed\n") ;
		sf_close (sf) ;
		return 1 ;
		} ;
	{	short held [4] = { 0 } ;
		if (sf_readf_short (sf, held, 2) != 2 ||
			held [0] != pcm [16] || held [1] != pcm [17] ||
			held [2] != pcm [16] || held [3] != pcm [17])
		{	fprintf (stderr, "held frames after seek mismatch\n") ;
			sf_close (sf) ;
			return 1 ;
			} ;
		} ;

	/* SEEK_CUR and the legal tail position. */
	if (sf_seek (sf, -2, SEEK_END) != 21)
	{	fprintf (stderr, "SEEK_END failed\n") ;
		sf_close (sf) ;
		return 1 ;
		} ;
	if (sf_seek (sf, 23, SEEK_SET) != 23 || sf_readf_short (sf, pcm, 1) != 0)
	{	fprintf (stderr, "tail seek/read failed\n") ;
		sf_close (sf) ;
		return 1 ;
		} ;

	if (sf_close (sf) != 0)
	{	fprintf (stderr, "sf_close failed\n") ;
		return 1 ;
		} ;

	remove (path) ;
	puts ("WAVL_IMA_DEMO_OK") ;
	return 0 ;
} /* main */

/*
** Regression tests for RIFF/WAVE IMA-ADPCM short tail block handling.
**
** Fixtures are constructed byte by byte, independently of the library
** writer, and expected PCM is produced by a small reference decoder
** below. The reference decoder only recovers frames for which every
** channel actually owns codewords in the data chunk.
*/

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sndfile.h>

#include "check.h"

/*============================================================================
** Independent reference IMA decoder.
*/

static const int ref_steps [89] =
{	7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
	50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
	253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
	1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
	3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
	11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
	32767
} ;

static const signed char ref_adjust [16] =
{	-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8
} ;

static int
ref_step (int *pred, int *indx, int code)
{	int step = ref_steps [*indx] ;
	int diff = step >> 3 ;
	if (code & 1) diff += step >> 2 ;
	if (code & 2) diff += step >> 1 ;
	if (code & 4) diff += step ;
	if (code & 8) diff = -diff ;
	*pred += diff ;
	if (*pred > 32767) *pred = 32767 ;
	if (*pred < -32768) *pred = -32768 ;
	*indx += ref_adjust [code & 15] ;
	if (*indx < 0) *indx = 0 ;
	if (*indx > 88) *indx = 88 ;
	return *pred ;
} /* ref_step */

/* Frames per channel recoverable from a block of which only 'avail' bytes
** exist : one frame from each 4 byte channel header plus two frames per
** codeword byte, taking the common length over all channels. */
static int
ref_block_frames (int channels, int blockalign, int avail)
{	int c, frames, body, unit ;

	if (avail < 4 * channels)
		return 0 ;

	body = avail - 4 * channels ;
	unit = 4 * channels ;

	frames = 2 * (blockalign - 4 * channels) / channels + 1 ;
	for (c = 0 ; c < channels ; c++)
	{	int rem = body % unit - 4 * c ;
		int bytes ;
		if (rem < 0) rem = 0 ;
		if (rem > 4) rem = 4 ;
		bytes = 4 * (body / unit) + rem ;
		if (1 + 2 * bytes < frames)
			frames = 1 + 2 * bytes ;
		} ;

	return frames ;
} /* ref_block_frames */

/* Decode 'ndata' bytes of WAV IMA data into interleaved shorts, honouring
** block geometry. Returns the number of frames (per channel). */
static int
ref_decode (int channels, int blockalign, const unsigned char *data, int ndata, short *out)
{	int total = 0, pos = 0 ;

	while (pos < ndata)
	{	int avail = ndata - pos ;
		const unsigned char *blk ;
		short ch [2] [512] ;
		int bf, c, f ;

		if (avail > blockalign)
			avail = blockalign ;
		blk = data + pos ;
		pos += avail ;

		bf = ref_block_frames (channels, blockalign, avail) ;
		if (bf <= 0)
			continue ;

		for (c = 0 ; c < channels ; c++)
		{	int pred = (short) (blk [4*c] | (blk [4*c+1] << 8)) ;
			int indx = blk [4*c+2] ;
			int body = avail - 4 * channels, unit = 4 * channels ;
			int rem = body % unit - 4 * c ;
			int bytes, i, n = 1 ;

			if (indx < 0) indx = 0 ;
			if (indx > 88) indx = 88 ;
			if (rem < 0) rem = 0 ;
			if (rem > 4) rem = 4 ;
			bytes = 4 * (body / unit) + rem ;

			ch [c] [0] = (short) pred ;
			for (i = 0 ; i < bytes ; i++)
			{	unsigned byte = blk [4*channels + (i / 4) * unit + 4*c + (i % 4)] ;
				ch [c] [n++] = (short) ref_step (&pred, &indx, byte & 15) ;
				ch [c] [n++] = (short) ref_step (&pred, &indx, byte >> 4) ;
				} ;
			} ;

		for (f = 0 ; f < bf ; f++)
			for (c = 0 ; c < channels ; c++)
				out [(total + f) * channels + c] = ch [c] [f] ;
		total += bf ;
		} ;

	return total ;
} /* ref_decode */

/*============================================================================
** Fixture construction.
*/

typedef struct
{	const char	*path ;
	int			channels ;
	int			blockalign ;
	int			samplesperblock ;
	const unsigned char *data ;
	int			ndata ;
	long		fact ;			/* < 0 : no fact chunk */
	int			fact_after ;	/* place the fact chunk after the data chunk */
	int			junk_after ;	/* append a JUNK chunk after everything else */
	unsigned	pad ;			/* RIFF pad byte value for odd data sizes */
} fixture_t ;

static void
put_u16 (FILE *f, unsigned x)
{	fputc (x & 255, f) ;
	fputc ((x >> 8) & 255, f) ;
} /* put_u16 */

static void
put_u32 (FILE *f, unsigned long x)
{	put_u16 (f, x & 0xffff) ;
	put_u16 (f, (x >> 16) & 0xffff) ;
} /* put_u32 */

static void
write_fact (FILE *f, unsigned long frames)
{	fwrite ("fact", 1, 4, f) ;
	put_u32 (f, 4) ;
	put_u32 (f, frames) ;
} /* write_fact */

static void
write_wav (const fixture_t *fx)
{	FILE *f = fopen (fx->path, "wb") ;
	static const unsigned char junk [] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x77, 0x73 } ;
	long sizepos, end ;

	if (! f)
	{	perror ("fopen") ;
		exit (2) ;
		} ;

	fwrite ("RIFF", 1, 4, f) ;
	sizepos = ftell (f) ;
	put_u32 (f, 0) ;	/* patched below */
	fwrite ("WAVE", 1, 4, f) ;

	fwrite ("fmt ", 1, 4, f) ;
	put_u32 (f, 20) ;
	put_u16 (f, 0x11) ;					/* WAVE_FORMAT_IMA_ADPCM */
	put_u16 (f, (unsigned) fx->channels) ;
	put_u32 (f, 8000) ;
	put_u32 (f, 8000UL * (unsigned long) fx->blockalign / (unsigned long) fx->samplesperblock) ;
	put_u16 (f, (unsigned) fx->blockalign) ;
	put_u16 (f, 4) ;					/* bits per sample */
	put_u16 (f, 2) ;					/* extra bytes */
	put_u16 (f, (unsigned) fx->samplesperblock) ;

	if (fx->fact >= 0 && ! fx->fact_after)
		write_fact (f, (unsigned long) fx->fact) ;

	fwrite ("data", 1, 4, f) ;
	put_u32 (f, (unsigned long) fx->ndata) ;
	fwrite (fx->data, 1, (size_t) fx->ndata, f) ;
	if (fx->ndata & 1)
		fputc ((int) (fx->pad & 255), f) ;

	if (fx->fact >= 0 && fx->fact_after)
		write_fact (f, (unsigned long) fx->fact) ;

	if (fx->junk_after)
	{	fwrite ("JUNK", 1, 4, f) ;
		put_u32 (f, sizeof (junk)) ;
		fwrite (junk, 1, sizeof (junk), f) ;
		} ;

	end = ftell (f) ;
	fseek (f, sizepos, SEEK_SET) ;
	put_u32 (f, (unsigned long) (end - 8)) ;
	fclose (f) ;
} /* write_wav */

/*============================================================================
** Fixture data. Predictors are non-zero and the two channels carry
** different history ; body bytes are chosen so the decoded sequences move.
*/

/* Mono, blockalign 8, samplesperblock 9. */
static const unsigned char mono_tail5 [] =
{	0xE8, 0x03, 0x00, 0x00, 0x10, 0x32, 0x54, 0x76,	/* full block, pred 1000 */
	0x18, 0xFC, 0x03, 0x00, 0x27						/* 5 byte tail, pred -1000 */
} ;

static const unsigned char mono_tail6 [] =
{	0xE8, 0x03, 0x00, 0x00, 0x10, 0x32, 0x54, 0x76,
	0x18, 0xFC, 0x03, 0x00, 0x27, 0x05				/* 6 byte tail */
} ;

static const unsigned char mono_tail3 [] =
{	0xE8, 0x03, 0x00, 0x00, 0x10, 0x32, 0x54, 0x76,
	0x18, 0xFC, 0x03									/* 3 byte tail : no header */
} ;

static const unsigned char mono_tail4 [] =
{	0xE8, 0x03, 0x00, 0x00, 0x10, 0x32, 0x54, 0x76,
	0x18, 0xFC, 0x03, 0x00							/* 4 byte tail : header only */
} ;

static const unsigned char mono_two_full [] =
{	0xE8, 0x03, 0x00, 0x00, 0x10, 0x32, 0x54, 0x76,
	0x30, 0xF8, 0x05, 0x00, 0x77, 0x66, 0x55, 0x44	/* second full block, pred -2000 */
} ;

/* Stereo, blockalign 16, samplesperblock 9. Left and right differ. */
static const unsigned char stereo_tail12 [] =
{	0xD0, 0x07, 0x00, 0x00, 0x30, 0xF8, 0x01, 0x00,	/* full block : L pred 2000, R pred -2000 */
	0x01, 0x23, 0x45, 0x67, 0x76, 0x54, 0x32, 0x10,
	0xF4, 0x01, 0x02, 0x00, 0x0C, 0xFE, 0x04, 0x00,	/* 12 byte tail : L pred 500, R pred -500 */
	0x11, 0x22, 0x33, 0x44							/* left body only */
} ;

static const unsigned char stereo_tail14 [] =
{	0xD0, 0x07, 0x00, 0x00, 0x30, 0xF8, 0x01, 0x00,
	0x01, 0x23, 0x45, 0x67, 0x76, 0x54, 0x32, 0x10,
	0xF4, 0x01, 0x02, 0x00, 0x0C, 0xFE, 0x04, 0x00,
	0x11, 0x22, 0x33, 0x44, 0x99, 0x88				/* left 4 body bytes, right 2 */
} ;

static const unsigned char stereo_tail15 [] =
{	0xD0, 0x07, 0x00, 0x00, 0x30, 0xF8, 0x01, 0x00,
	0x01, 0x23, 0x45, 0x67, 0x76, 0x54, 0x32, 0x10,
	0xF4, 0x01, 0x02, 0x00, 0x0C, 0xFE, 0x04, 0x00,
	0x11, 0x22, 0x33, 0x44, 0x99, 0x88, 0x77		/* left 4 body bytes, right 3 */
} ;

static const unsigned char stereo_tail6 [] =
{	0xD0, 0x07, 0x00, 0x00, 0x30, 0xF8, 0x01, 0x00,
	0x01, 0x23, 0x45, 0x67, 0x76, 0x54, 0x32, 0x10,
	0xF4, 0x01, 0x02, 0x00, 0x0C, 0xFE				/* 6 byte tail : no full header set */
} ;

/*============================================================================
** Helpers.
*/

#define ARRAY_LENG(x)	((int) (sizeof (x) / sizeof ((x) [0])))

static int
open_and_check (const char *path, SNDFILE **snd, SF_INFO *info, sf_count_t frames, int channels)
{	memset (info, 0, sizeof (*info)) ;
	*snd = sf_open (path, SFM_READ, info) ;
	TASK_CHECK (*snd != NULL) ;
	if (! *snd)
		return 1 ;
	TASK_CHECK (info->frames == frames) ;
	TASK_CHECK (info->channels == channels) ;
	return 0 ;
} /* open_and_check */

/* Open 'path', read the whole signal with sf_readf_short and compare it
** against 'ref' (interleaved, 'frames' frames of 'channels' channels). */
static int
check_signal_short (const char *path, const short *ref, sf_count_t frames, int channels)
{	SNDFILE *snd ;
	SF_INFO info ;
	short buf [4096] ;
	sf_count_t n ;
	int i ;

	if (open_and_check (path, &snd, &info, frames, channels))
		return 1 ;

	memset (buf, 0x55, sizeof (buf)) ;
	n = sf_readf_short (snd, buf, ARRAY_LENG (buf) / channels) ;
	TASK_CHECK (n == frames) ;
	if (n > 0)
	{	int count = (int) n * channels ;
		if (count > ARRAY_LENG (buf)) count = ARRAY_LENG (buf) ;
		for (i = 0 ; i < count ; i++)
			TASK_CHECK (buf [i] == ref [i]) ;
		} ;
	TASK_CHECK (sf_readf_short (snd, buf, 16) == 0) ;
	TASK_CHECK (sf_close (snd) == 0) ;
	return 0 ;
} /* check_signal_short */

/*============================================================================
** geometry group.
*/

static int
geo_mono_tail (void)
{	static const struct
	{	const char			*name ;
		const unsigned char *data ;
		int					ndata ;
	} variants [] =
	{	{	".task-build/ima_geo_mono_tail5.wav",	mono_tail5,		ARRAY_LENG (mono_tail5)		},
		{	".task-build/ima_geo_mono_tail6.wav",	mono_tail6,		ARRAY_LENG (mono_tail6)		},
		{	".task-build/ima_geo_mono_tail3.wav",	mono_tail3,		ARRAY_LENG (mono_tail3)		},
		{	".task-build/ima_geo_mono_tail4.wav",	mono_tail4,		ARRAY_LENG (mono_tail4)		},
		{	".task-build/ima_geo_mono_two.wav",		mono_two_full,	ARRAY_LENG (mono_two_full)	}
	} ;
	short ref [512] ;
	int k ;

	for (k = 0 ; k < ARRAY_LENG (variants) ; k++)
	{	fixture_t fx ;
		int nref ;

		memset (&fx, 0, sizeof (fx)) ;
		fx.path = variants [k].name ;
		fx.channels = 1 ;
		fx.blockalign = 8 ;
		fx.samplesperblock = 9 ;
		fx.data = variants [k].data ;
		fx.ndata = variants [k].ndata ;
		fx.fact = -1 ;
		fx.junk_after = 1 ;
		fx.pad = 0x5A ;	/* non-zero pad byte must not be decoded */
		write_wav (&fx) ;

		nref = ref_decode (1, 8, variants [k].data, variants [k].ndata, ref) ;
		TASK_CHECK (nref == 9 + ref_block_frames (1, 8, variants [k].ndata - 8)) ;

		if (check_signal_short (fx.path, ref, nref, 1))
			return 1 ;
		} ;

	return 0 ;
} /* geo_mono_tail */

static int
geo_stereo_tail (void)
{	static const struct
	{	const char			*name ;
		const unsigned char *data ;
		int					ndata ;
	} variants [] =
	{	{	".task-build/ima_geo_st_tail12.wav",	stereo_tail12,	ARRAY_LENG (stereo_tail12)	},
		{	".task-build/ima_geo_st_tail14.wav",	stereo_tail14,	ARRAY_LENG (stereo_tail14)	},
		{	".task-build/ima_geo_st_tail15.wav",	stereo_tail15,	ARRAY_LENG (stereo_tail15)	},
		{	".task-build/ima_geo_st_tail6.wav",		stereo_tail6,	ARRAY_LENG (stereo_tail6)	}
	} ;
	short ref [512] ;
	int k ;

	for (k = 0 ; k < ARRAY_LENG (variants) ; k++)
	{	fixture_t fx ;
		int nref ;

		memset (&fx, 0, sizeof (fx)) ;
		fx.path = variants [k].name ;
		fx.channels = 2 ;
		fx.blockalign = 16 ;
		fx.samplesperblock = 9 ;
		fx.data = variants [k].data ;
		fx.ndata = variants [k].ndata ;
		fx.fact = -1 ;
		fx.junk_after = 1 ;
		fx.pad = 0xA5 ;
		write_wav (&fx) ;

		nref = ref_decode (2, 16, variants [k].data, variants [k].ndata, ref) ;
		TASK_CHECK (nref == 9 + ref_block_frames (2, 16, variants [k].ndata - 16)) ;

		if (check_signal_short (fx.path, ref, nref, 2))
			return 1 ;
		} ;

	return 0 ;
} /* geo_stereo_tail */

static int
geo_read_types (void)
{	fixture_t fx ;
	SNDFILE *snd ;
	SF_INFO info ;
	short ref [64], sbuf [8] ;
	int ibuf [64] ;
	float fbuf [64] ;
	double dbuf [64] ;
	sf_count_t n, got ;
	int i, step ;

	memset (&fx, 0, sizeof (fx)) ;
	fx.path = ".task-build/ima_geo_types.wav" ;
	fx.channels = 1 ;
	fx.blockalign = 8 ;
	fx.samplesperblock = 9 ;
	fx.data = mono_tail5 ;
	fx.ndata = ARRAY_LENG (mono_tail5) ;
	fx.fact = -1 ;
	fx.junk_after = 1 ;
	fx.pad = 0x5A ;
	write_wav (&fx) ;

	TASK_CHECK (ref_decode (1, 8, mono_tail5, ARRAY_LENG (mono_tail5), ref) == 12) ;

	/* sf_readf_int : signal << 16. */
	if (open_and_check (fx.path, &snd, &info, 12, 1))
		return 1 ;
	n = sf_readf_int (snd, ibuf, ARRAY_LENG (ibuf)) ;
	TASK_CHECK (n == 12) ;
	for (i = 0 ; i < 12 ; i++)
		TASK_CHECK (ibuf [i] == ref [i] * 65536) ;
	TASK_CHECK (sf_readf_int (snd, ibuf, 8) == 0) ;
	TASK_CHECK (sf_close (snd) == 0) ;

	/* sf_readf_float : normalised by 1/0x8000. */
	if (open_and_check (fx.path, &snd, &info, 12, 1))
		return 1 ;
	n = sf_readf_float (snd, fbuf, ARRAY_LENG (fbuf)) ;
	TASK_CHECK (n == 12) ;
	for (i = 0 ; i < 12 ; i++)
		TASK_CHECK (fabsf (fbuf [i] - ref [i] / 32768.0f) <= 1e-9f) ;
	TASK_CHECK (sf_readf_float (snd, fbuf, 8) == 0) ;
	TASK_CHECK (sf_close (snd) == 0) ;

	/* sf_readf_double : normalised by 1/0x8000. */
	if (open_and_check (fx.path, &snd, &info, 12, 1))
		return 1 ;
	n = sf_readf_double (snd, dbuf, ARRAY_LENG (dbuf)) ;
	TASK_CHECK (n == 12) ;
	for (i = 0 ; i < 12 ; i++)
		TASK_CHECK (fabs (dbuf [i] - ref [i] / 32768.0) <= 1e-12) ;
	TASK_CHECK (sf_readf_double (snd, dbuf, 8) == 0) ;
	TASK_CHECK (sf_close (snd) == 0) ;

	/* Repeated short reads must deliver the same signal as one full read. */
	if (open_and_check (fx.path, &snd, &info, 12, 1))
		return 1 ;
	got = 0 ;
	step = 1 ;
	while ((n = sf_readf_short (snd, sbuf, step)) > 0)
	{	TASK_CHECK (n <= step) ;
		for (i = 0 ; i < (int) n ; i++)
			TASK_CHECK (sbuf [i] == ref [got + i]) ;
		got += n ;
		step = step % 3 + 1 ;
		} ;
	TASK_CHECK (got == 12) ;
	TASK_CHECK (sf_close (snd) == 0) ;

	/* Same again through sf_readf_float. */
	if (open_and_check (fx.path, &snd, &info, 12, 1))
		return 1 ;
	got = 0 ;
	step = 2 ;
	while ((n = sf_readf_float (snd, fbuf, step)) > 0)
	{	TASK_CHECK (n <= step) ;
		for (i = 0 ; i < (int) n ; i++)
			TASK_CHECK (fabsf (fbuf [i] - ref [got + i] / 32768.0f) <= 1e-9f) ;
		got += n ;
		step = step % 3 + 1 ;
		} ;
	TASK_CHECK (got == 12) ;
	TASK_CHECK (sf_close (snd) == 0) ;

	return 0 ;
} /* geo_read_types */

/*============================================================================
** fact group.
*/

static int
fact_trim (void)
{	short ref [64] ;
	SNDFILE *snd ;
	SF_INFO info ;
	short buf [64] ;
	sf_count_t n ;
	int i, after ;

	TASK_CHECK (ref_decode (1, 8, mono_tail5, ARRAY_LENG (mono_tail5), ref) == 12) ;

	/* A fact count below the physical capacity ends the audio there,
	** whether the fact chunk sits before or after the data chunk. */
	for (after = 0 ; after < 2 ; after++)
	{	fixture_t fx ;

		memset (&fx, 0, sizeof (fx)) ;
		fx.path = after ? ".task-build/ima_fact_after.wav" : ".task-build/ima_fact_before.wav" ;
		fx.channels = 1 ;
		fx.blockalign = 8 ;
		fx.samplesperblock = 9 ;
		fx.data = mono_tail5 ;
		fx.ndata = ARRAY_LENG (mono_tail5) ;
		fx.fact = 10 ;
		fx.fact_after = after ;
		fx.junk_after = 1 ;
		fx.pad = 0x5A ;
		write_wav (&fx) ;

		if (open_and_check (fx.path, &snd, &info, 10, 1))
			return 1 ;
		n = sf_readf_short (snd, buf, ARRAY_LENG (buf)) ;
		TASK_CHECK (n == 10) ;
		for (i = 0 ; i < 10 ; i++)
			TASK_CHECK (buf [i] == ref [i]) ;
		TASK_CHECK (sf_readf_short (snd, buf, 8) == 0) ;
		TASK_CHECK (sf_close (snd) == 0) ;
		} ;

	return 0 ;
} /* fact_trim */

static int
fact_limits (void)
{	static const struct
	{	const char	*name ;
		long		fact ;
		sf_count_t	expect ;
	} variants [] =
	{	{	".task-build/ima_fact_zero.wav",	0,	12	},	/* zero fact : physical capacity */
		{	".task-build/ima_fact_none.wav",	-1,	12	},	/* no fact chunk at all */
		{	".task-build/ima_fact_excess.wav",	30,	12	},	/* beyond capacity : no extra sound */
		{	".task-build/ima_fact_equal.wav",	12,	12	}
	} ;
	short ref [64], buf [64] ;
	SNDFILE *snd ;
	SF_INFO info ;
	sf_count_t n ;
	int i, k ;

	TASK_CHECK (ref_decode (1, 8, mono_tail5, ARRAY_LENG (mono_tail5), ref) == 12) ;

	for (k = 0 ; k < ARRAY_LENG (variants) ; k++)
	{	fixture_t fx ;

		memset (&fx, 0, sizeof (fx)) ;
		fx.path = variants [k].name ;
		fx.channels = 1 ;
		fx.blockalign = 8 ;
		fx.samplesperblock = 9 ;
		fx.data = mono_tail5 ;
		fx.ndata = ARRAY_LENG (mono_tail5) ;
		fx.fact = variants [k].fact ;
		fx.junk_after = 1 ;
		fx.pad = 0x5A ;
		write_wav (&fx) ;

		if (open_and_check (fx.path, &snd, &info, variants [k].expect, 1))
			return 1 ;
		n = sf_readf_short (snd, buf, ARRAY_LENG (buf)) ;
		TASK_CHECK (n == variants [k].expect) ;
		for (i = 0 ; i < (int) n ; i++)
			TASK_CHECK (buf [i] == ref [i]) ;
		TASK_CHECK (sf_readf_short (snd, buf, 8) == 0) ;
		TASK_CHECK (sf_close (snd) == 0) ;
		} ;

	return 0 ;
} /* fact_limits */

/*============================================================================
** seek group.
*/

static int
seek_tail_suffix (void)
{	fixture_t fx ;
	SNDFILE *snd ;
	SF_INFO info ;
	short ref [64], buf [64] ;
	sf_count_t n ;
	int i ;

	memset (&fx, 0, sizeof (fx)) ;
	fx.path = ".task-build/ima_seek_suffix.wav" ;
	fx.channels = 1 ;
	fx.blockalign = 8 ;
	fx.samplesperblock = 9 ;
	fx.data = mono_tail5 ;
	fx.ndata = ARRAY_LENG (mono_tail5) ;
	fx.fact = -1 ;
	fx.junk_after = 1 ;
	fx.pad = 0x5A ;
	write_wav (&fx) ;

	TASK_CHECK (ref_decode (1, 8, mono_tail5, ARRAY_LENG (mono_tail5), ref) == 12) ;

	if (open_and_check (fx.path, &snd, &info, 12, 1))
		return 1 ;

	/* Seek into the tail block : the suffix must match the sequential read. */
	TASK_CHECK (sf_seek (snd, 10, SEEK_SET) == 10) ;
	n = sf_readf_short (snd, buf, ARRAY_LENG (buf)) ;
	TASK_CHECK (n == 2) ;
	TASK_CHECK (buf [0] == ref [10]) ;
	TASK_CHECK (buf [1] == ref [11]) ;
	TASK_CHECK (sf_readf_short (snd, buf, 8) == 0) ;

	/* Seek to the first frame of the tail block. */
	TASK_CHECK (sf_seek (snd, 9, SEEK_SET) == 9) ;
	n = sf_readf_short (snd, buf, ARRAY_LENG (buf)) ;
	TASK_CHECK (n == 3) ;
	for (i = 0 ; i < 3 ; i++)
		TASK_CHECK (buf [i] == ref [9 + i]) ;

	/* Seek inside a full block. */
	TASK_CHECK (sf_seek (snd, 5, SEEK_SET) == 5) ;
	n = sf_readf_short (snd, buf, ARRAY_LENG (buf)) ;
	TASK_CHECK (n == 7) ;
	for (i = 0 ; i < 7 ; i++)
		TASK_CHECK (buf [i] == ref [5 + i]) ;

	/* Back to the start : the whole signal again. */
	TASK_CHECK (sf_seek (snd, 0, SEEK_SET) == 0) ;
	n = sf_readf_short (snd, buf, ARRAY_LENG (buf)) ;
	TASK_CHECK (n == 12) ;
	for (i = 0 ; i < 12 ; i++)
		TASK_CHECK (buf [i] == ref [i]) ;

	TASK_CHECK (sf_close (snd) == 0) ;
	return 0 ;
} /* seek_tail_suffix */

static int
seek_end_and_beyond (void)
{	fixture_t fx ;
	SNDFILE *snd ;
	SF_INFO info ;
	short ref [64], buf [64] ;
	sf_count_t n ;

	TASK_CHECK (ref_decode (1, 8, mono_tail5, ARRAY_LENG (mono_tail5), ref) == 12) ;

	memset (&fx, 0, sizeof (fx)) ;
	fx.path = ".task-build/ima_seek_end.wav" ;
	fx.channels = 1 ;
	fx.blockalign = 8 ;
	fx.samplesperblock = 9 ;
	fx.data = mono_tail5 ;
	fx.ndata = ARRAY_LENG (mono_tail5) ;
	fx.fact = -1 ;
	fx.junk_after = 1 ;
	fx.pad = 0x5A ;
	write_wav (&fx) ;

	if (open_and_check (fx.path, &snd, &info, 12, 1))
		return 1 ;

	/* The logical end is a valid position and reads return zero there. */
	TASK_CHECK (sf_seek (snd, 12, SEEK_SET) == 12) ;
	TASK_CHECK (sf_readf_short (snd, buf, 8) == 0) ;

	/* Past the end keeps the existing failure convention. */
	TASK_CHECK (sf_seek (snd, 13, SEEK_SET) == -1) ;

	/* The file is still usable after the failed seek. */
	TASK_CHECK (sf_seek (snd, 11, SEEK_SET) == 11) ;
	n = sf_readf_short (snd, buf, ARRAY_LENG (buf)) ;
	TASK_CHECK (n == 1) ;
	TASK_CHECK (buf [0] == ref [11]) ;
	TASK_CHECK (sf_close (snd) == 0) ;

	/* Same rules when a fact chunk trims the file to 10 frames. */
	memset (&fx, 0, sizeof (fx)) ;
	fx.path = ".task-build/ima_seek_fact.wav" ;
	fx.channels = 1 ;
	fx.blockalign = 8 ;
	fx.samplesperblock = 9 ;
	fx.data = mono_tail5 ;
	fx.ndata = ARRAY_LENG (mono_tail5) ;
	fx.fact = 10 ;
	fx.junk_after = 1 ;
	fx.pad = 0x5A ;
	write_wav (&fx) ;

	if (open_and_check (fx.path, &snd, &info, 10, 1))
		return 1 ;

	TASK_CHECK (sf_seek (snd, 10, SEEK_SET) == 10) ;
	TASK_CHECK (sf_readf_short (snd, buf, 8) == 0) ;
	TASK_CHECK (sf_seek (snd, 11, SEEK_SET) == -1) ;

	TASK_CHECK (sf_seek (snd, 9, SEEK_SET) == 9) ;
	n = sf_readf_short (snd, buf, ARRAY_LENG (buf)) ;
	TASK_CHECK (n == 1) ;
	TASK_CHECK (buf [0] == ref [9]) ;
	TASK_CHECK (sf_readf_short (snd, buf, 8) == 0) ;
	TASK_CHECK (sf_close (snd) == 0) ;

	return 0 ;
} /* seek_end_and_beyond */

/*============================================================================
*/

const struct task_case task_cases [] =
{	{ "geometry",	"mono_tail_block_geometry",		geo_mono_tail		},
	{ "geometry",	"stereo_tail_common_length",	geo_stereo_tail		},
	{ "geometry",	"read_types_and_short_reads",	geo_read_types		},
	{ "fact",		"fact_trims_tail",				fact_trim			},
	{ "fact",		"fact_zero_missing_excess",		fact_limits			},
	{ "seek",		"seek_tail_suffix",				seek_tail_suffix	},
	{ "seek",		"seek_end_and_beyond",			seek_end_and_beyond	}
} ;

const size_t task_case_count = sizeof (task_cases) / sizeof (task_cases [0]) ;

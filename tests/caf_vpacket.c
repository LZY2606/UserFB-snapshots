/*
** Regression tests for CAF/ALAC files whose packets have individual
** durations (desc frames_per_packet == 0, pakt entries carry both a byte
** count and a frame count).
**
** The ALAC payloads are produced with the in-tree encoder, and the original
** PCM is kept as the independent reference the library output is compared
** against.
*/

#include "sfconfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>

#include <sndfile.h>

#include "task_support/check.h"
#include "src/ALAC/alac_codec.h"
#include "src/ALAC/ALACBitUtilities.h"

#define MAX_PACKETS		64
#define SAMPLE_RATE		48000
#define MAX_PACKET_FRAMES	4096

typedef struct
{	unsigned	npackets ;
	uint32_t	counts [MAX_PACKETS] ;
	sf_count_t	total ;

	uint8_t		*data ;
	size_t		len, cap ;

	uint32_t	sizes [MAX_PACKETS] ;

	size_t		pakt_valid_off ;
	size_t		table_off ;
	size_t		size_off [MAX_PACKETS] ;
	unsigned	size_w [MAX_PACKETS] ;
	size_t		frame_off [MAX_PACKETS] ;
	unsigned	frame_w [MAX_PACKETS] ;
	size_t		table_len ;
	size_t		data_size_off ;
} VFILE ;

typedef struct
{	int64_t		count_delta, valid_delta ;
	int32_t		priming, remainder ;
	unsigned	extra_entries ;
	int			skip_last_table_byte ;
	int			long_varint_index ;
} BUILD_OPTS ;

/*--------------------------------------------------------------------------*/

static int
vf_append (VFILE *vf, const void *bytes, size_t count)
{	size_t need ;
	uint8_t *tmp ;

	if (count == 0)
		return 1 ;

	need = vf->len + count ;
	if (need < vf->len)
		return 0 ;

	if (need > vf->cap)
	{	size_t newcap = vf->cap ? vf->cap : 4096 ;

		while (newcap < need)
		{	if (newcap > SIZE_MAX / 2)
				return 0 ;
			newcap *= 2 ;
			} ;

		if ((tmp = realloc (vf->data, newcap)) == NULL)
			return 0 ;
		vf->data = tmp ;
		vf->cap = newcap ;
		} ;

	memcpy (vf->data + vf->len, bytes, count) ;
	vf->len = need ;

	return 1 ;
} /* vf_append */

static void
be32_put (uint8_t *p, uint32_t v)
{	p [0] = (uint8_t) (v >> 24) ;
	p [1] = (uint8_t) (v >> 16) ;
	p [2] = (uint8_t) (v >> 8) ;
	p [3] = (uint8_t) v ;
} /* be32_put */

static void
be64_put (uint8_t *p, uint64_t v)
{	int k ;

	for (k = 0 ; k < 8 ; k++)
		p [k] = (uint8_t) (v >> (8 * (7 - k))) ;
} /* be64_put */

static unsigned
varint_width (uint64_t v)
{	unsigned w = 1 ;
	while (v >= 0x80)
	{	v >>= 7 ;
		w++ ;
		} ;
	return w ;
} /* varint_width */

/* Write v in exactly `width` seven-bit continuation bytes. */
static void
varint_put_fixed (uint8_t *p, unsigned width, uint64_t v)
{	unsigned k ;

	for (k = 0 ; k < width ; k++)
	{	unsigned shift = 7 * (width - 1 - k) ;
		uint8_t b = (shift >= 64) ? 0 : (uint8_t) ((v >> shift) & 0x7f) ;

		if (k + 1 < width)
			b |= 0x80 ;
		p [k] = b ;
		} ;
} /* varint_put_fixed */

/*--------------------------------------------------------------------------
** Signal generation. 16-bit material spans nearly the full range. The two
** channels of 24-bit material are deliberately asymmetric in both amplitude
** and phase, and stay well inside full scale so the stereo matrix keeps its
** extra head bit.
**
** Samples are left justified in 32 bits, matching the integer interface;
** the bottom byte is zero, because the 24-bit codec handles the top 24 bits
** of the 32-bit word.
*/

static int32_t
sample_value (int bits, int channel, uint32_t i, uint32_t seed)
{	int32_t v ;

	if (bits == 16)
	{	if (channel == 0)
			v = (int32_t) ((i * 211u + seed) % 65001u) - 32500 ;
		else
			v = (int32_t) ((i * 97u + seed * 3u) % 12001u) - 6000 ;
		return v * 65536 ;
		} ;

	if (channel == 0)
		v = ((int32_t) ((i + seed) % 2001u) - 1000) * 2048 ;
	else
		v = ((int32_t) ((i / 7u + seed) % 401u) - 150) * 2048 ;

	return v * 256 ;
} /* sample_value */

static int32_t *
make_pcm (int bits, int channels, const uint32_t *counts, unsigned npackets, sf_count_t *total_out, uint32_t seed)
{	int32_t *pcm ;
	sf_count_t total = 0, frame, pos ;
	unsigned p ;
	int ch ;

	for (p = 0 ; p < npackets ; p++)
		total += counts [p] ;

	pcm = malloc ((size_t) total * channels * sizeof (int32_t)) ;
	if (pcm == NULL)
		return NULL ;

	pos = 0 ;
	for (p = 0 ; p < npackets ; p++)
	{	for (frame = 0 ; frame < (sf_count_t) counts [p] ; frame++)
			for (ch = 0 ; ch < channels ; ch++)
				pcm [(pos + frame) * channels + ch] = sample_value (bits, ch, (uint32_t) (pos + frame + seed), seed) ;
		pos += counts [p] ;
		} ;

	*total_out = total ;
	return pcm ;
} /* make_pcm */

/*--------------------------------------------------------------------------
** CAF construction.
*/

static int
build_vfile (VFILE *vf, int bits, int channels, const uint32_t *counts, unsigned npackets,
				const int32_t *pcm, const BUILD_OPTS *opt)
{	static const BUILD_OPTS defaults = { 0, 0, 0, 0, 0, 0, -1 } ;
	ALAC_ENCODER *enc = NULL ;
	uint8_t *packets = NULL, cookie [512] ;
	uint8_t tmp [32] ;
	uint32_t cookie_len = sizeof (cookie), flags, psize, total_bytes = 0, k ;
	size_t per_packet ;
	sf_count_t pos = 0 ;
	unsigned p ;
	int ok = 0 ;

	if (opt == NULL)
		opt = &defaults ;

	memset (vf, 0, sizeof (*vf)) ;
	vf->npackets = npackets ;

	for (p = 0 ; p < npackets ; p++)
		vf->counts [p] = counts [p] ;
	vf->total = 0 ;
	for (p = 0 ; p < npackets ; p++)
		vf->total += counts [p] ;

	flags = (bits == 16) ? 1u : 3u ;
	per_packet = (size_t) MAX_PACKET_FRAMES * channels * 6 + 1024 ;

	enc = calloc (1, sizeof (*enc)) ;
	packets = malloc (per_packet * npackets) ;
	if (enc == NULL || packets == NULL)
		goto Cleanup ;

	if (alac_encoder_init (enc, SAMPLE_RATE, (uint32_t) channels, flags, MAX_PACKET_FRAMES) != ALAC_noErr)
		goto Cleanup ;

	alac_get_magic_cookie (enc, cookie, &cookie_len) ;
	if (cookie_len == 0)
		goto Cleanup ;

	for (p = 0 ; p < npackets ; p++)
	{	/* Encode every packet from a fresh encoder state so the payloads
		** do not depend on the preceding packets in the test material. */
		free (enc) ;
		if ((enc = calloc (1, sizeof (*enc))) == NULL)
			goto Cleanup ;
		if (alac_encoder_init (enc, SAMPLE_RATE, (uint32_t) channels, flags, MAX_PACKET_FRAMES) != ALAC_noErr)
			goto Cleanup ;

		psize = (uint32_t) per_packet ;
		if (alac_encode (enc, counts [p], pcm + pos * channels, packets + p * per_packet, &psize) != ALAC_noErr)
			goto Cleanup ;
		if (psize == 0 || psize > per_packet)
			goto Cleanup ;
		vf->sizes [p] = psize ;
		total_bytes += psize ;
		pos += counts [p] ;
		} ;

	/* 'caff' header. */
	if (! vf_append (vf, "caff", 4) || ! vf_append (vf, "\x00\x01\x00\x00", 4))
		goto Cleanup ;

	/* 'desc' chunk. */
	if (! vf_append (vf, "desc", 4))
		goto Cleanup ;
	be64_put (tmp, 32) ;
	if (! vf_append (vf, tmp, 8))
		goto Cleanup ;
	{	double rate = SAMPLE_RATE ;
		uint64_t bits64 ;

		memcpy (&bits64, &rate, sizeof (bits64)) ;
		be64_put (tmp, bits64) ;
		}
	if (! vf_append (vf, tmp, 8))
		goto Cleanup ;
	be32_put (tmp, 0x616c6163u) ;	/* 'alac' */
	be32_put (tmp + 4, flags) ;
	be32_put (tmp + 8, 0) ;			/* bytes per packet */
	be32_put (tmp + 12, 0) ;		/* frames per packet */
	be32_put (tmp + 16, (uint32_t) channels) ;
	be32_put (tmp + 20, 0) ;
	if (! vf_append (vf, tmp, 24))
		goto Cleanup ;

	/* 'kuki' chunk. */
	if (! vf_append (vf, "kuki", 4))
		goto Cleanup ;
	be64_put (tmp, cookie_len) ;
	if (! vf_append (vf, tmp, 8) || ! vf_append (vf, cookie, cookie_len))
		goto Cleanup ;

	/* 'pakt' chunk: header, then one byte/frame pair per packet. */
	if (! vf_append (vf, "pakt", 4))
		goto Cleanup ;
	if (! vf_append (vf, "\x00\x00\x00\x00\x00\x00\x00\x00", 8))
		goto Cleanup ;

	/* The chunk size field is written below, once the table length is known. */
	vf->pakt_valid_off = vf->len + 8 ;
	vf->table_off = vf->len + 24 ;

	be64_put (tmp, (uint64_t) ((int64_t) npackets + opt->count_delta)) ;
	be64_put (tmp + 8, (uint64_t) ((int64_t) vf->total + opt->valid_delta)) ;
	be32_put (tmp + 16, (uint32_t) opt->priming) ;
	be32_put (tmp + 20, (uint32_t) opt->remainder) ;
	if (! vf_append (vf, tmp, 24))
		goto Cleanup ;

	for (p = 0 ; p < npackets ; p++)
	{	unsigned width ;

		if ((int) p == opt->long_varint_index)
		{	vf->size_off [p] = vf->len ;
			vf->size_w [p] = 10 ;
			for (k = 0 ; k < 10 ; k++)
			if (! vf_append (vf, "\x81", 1))
				goto Cleanup ;
			}
		else
		{	width = varint_width (vf->sizes [p]) ;
			vf->size_off [p] = vf->len ;
			vf->size_w [p] = width ;
			{	uint8_t vb [10] ;
				varint_put_fixed (vb, width, vf->sizes [p]) ;
				if (! vf_append (vf, vb, width))
					goto Cleanup ;
				} ;
			} ;

		width = varint_width (counts [p]) ;
		vf->frame_off [p] = vf->len ;
		vf->frame_w [p] = width ;

		if (opt->skip_last_table_byte && p + 1 == npackets)
		{	/* Leave the declared packet without a complete entry. */
			if (width > 1)
			{	uint8_t vb [10] ;
				varint_put_fixed (vb, width - 1, counts [p]) ;
				if (! vf_append (vf, vb, width - 1))
					goto Cleanup ;
				} ;
			}
		else
		{	uint8_t vb [10] ;
			varint_put_fixed (vb, width, counts [p]) ;
			if (! vf_append (vf, vb, width))
				goto Cleanup ;
			} ;
		} ;

	/* Optional junk entries after the declared packets. */
	for (k = 0 ; k < opt->extra_entries ; k++)
	{	uint8_t vb [10] ;
		varint_put_fixed (vb, 1, 1) ;
		if (! vf_append (vf, vb, 1) || ! vf_append (vf, vb, 1))
			goto Cleanup ;
		} ;

	vf->table_len = vf->len - vf->table_off ;

	/* Fix up the 'pakt' chunk size. The table start is 36 bytes past the
	** chunk header: marker (4) + size (8) + the 24 byte table header. */
	be64_put (vf->data + vf->table_off - 32, (uint64_t) (24 + vf->table_len)) ;

	/* 'data' chunk. */
	if (! vf_append (vf, "data", 4))
		goto Cleanup ;
	vf->data_size_off = vf->len ;
	be64_put (tmp, (uint64_t) (4 + total_bytes)) ;
	if (! vf_append (vf, tmp, 8))
		goto Cleanup ;
	if (! vf_append (vf, "\x00\x00\x00\x00", 4))
		goto Cleanup ;

	for (p = 0 ; p < npackets ; p++)
		if (! vf_append (vf, packets + p * per_packet, vf->sizes [p]))
			goto Cleanup ;

	ok = 1 ;

Cleanup :
	free (enc) ;
	free (packets) ;
	return ok ;
} /* build_vfile */

static void
vf_free (VFILE *vf)
{	free (vf->data) ;
	vf->data = NULL ;
	vf->len = vf->cap = 0 ;
} /* vf_free */

/*--------------------------------------------------------------------------
** In-memory virtual IO.
*/

typedef struct
{	const uint8_t *data ;
	size_t len, pos ;
} VIO ;

static sf_count_t
vio_get_filelen (void *user_data)
{	VIO *vio = user_data ;
	return (sf_count_t) vio->len ;
} /* vio_get_filelen */

static sf_count_t
vio_seek (sf_count_t offset, int whence, void *user_data)
{	VIO *vio = user_data ;
	sf_count_t target ;

	switch (whence)
	{	case SEEK_SET : target = offset ; break ;
		case SEEK_CUR : target = (sf_count_t) vio->pos + offset ; break ;
		case SEEK_END : target = (sf_count_t) vio->len + offset ; break ;
		default : return -1 ;
		} ;

	if (target < 0 || (uint64_t) target > vio->len)
		return -1 ;

	vio->pos = (size_t) target ;
	return target ;
} /* vio_seek */

static sf_count_t
vio_read (void *ptr, sf_count_t count, void *user_data)
{	VIO *vio = user_data ;
	size_t n ;

	if (count <= 0)
		return 0 ;

	n = (size_t) count ;
	if (n > vio->len - vio->pos)
		n = vio->len - vio->pos ;

	memcpy (ptr, vio->data + vio->pos, n) ;
	vio->pos += n ;
	return (sf_count_t) n ;
} /* vio_read */

static sf_count_t
vio_tell (void *user_data)
{	VIO *vio = user_data ;
	return (sf_count_t) vio->pos ;
} /* vio_tell */

static SNDFILE *
open_virtual (VFILE *vf, SF_INFO *info, VIO *vio)
{	SF_VIRTUAL_IO vio_api ;

	memset (&vio_api, 0, sizeof (vio_api)) ;
	vio_api.get_filelen = vio_get_filelen ;
	vio_api.seek = vio_seek ;
	vio_api.read = vio_read ;
	vio_api.tell = vio_tell ;

	vio->data = vf->data ;
	vio->len = vf->len ;
	vio->pos = 0 ;

	memset (info, 0, sizeof (*info)) ;
	return sf_open_virtual (&vio_api, SFM_READ, info, vio) ;
} /* open_virtual */

static SNDFILE *
open_plain (VFILE *vf, SF_INFO *info, const char *path)
{	FILE *file ;
	SNDFILE *sndfile ;

	file = fopen (path, "wb") ;
	if (file == NULL)
		return NULL ;
	if (fwrite (vf->data, 1, vf->len, file) != vf->len)
	{	fclose (file) ;
		return NULL ;
		} ;
	fclose (file) ;

	memset (info, 0, sizeof (*info)) ;
	sndfile = sf_open (path, SFM_READ, info) ;
	remove (path) ;

	return sndfile ;
} /* open_plain */

/*--------------------------------------------------------------------------
** Content comparison helpers.  Expected values follow the library's own
** integer -> short/float/double conversions.
*/


static float
float_scale (void)
{	return 1.0f / ((float) 0x80000000u) ;
} /* float_scale */

static int
check_ints (const int32_t *expected, const int *got, sf_count_t count)
{	sf_count_t k ;

	for (k = 0 ; k < count ; k++)
		if (got [k] != expected [k])
			return 0 ;
	return 1 ;
} /* check_ints */

static int
check_shorts (const int32_t *expected, const short *got, sf_count_t count)
{	sf_count_t k ;

	for (k = 0 ; k < count ; k++)
		if (got [k] != (short) (expected [k] >> 16))
			return 0 ;
	return 1 ;
} /* check_shorts */

static int
check_floats (const int32_t *expected, const float *got, sf_count_t count)
{	sf_count_t k ;
	float normfact = float_scale () ;

	for (k = 0 ; k < count ; k++)
		if (got [k] != normfact * expected [k])
			return 0 ;
	return 1 ;
} /* check_floats */

static int
check_doubles (const int32_t *expected, const double *got, sf_count_t count)
{	sf_count_t k ;
	double normfact = float_scale () ;

	for (k = 0 ; k < count ; k++)
		if (got [k] != normfact * expected [k])
			return 0 ;
	return 1 ;
} /* check_doubles */

/*--------------------------------------------------------------------------
** Shared material.
*/

static const uint32_t mono16_counts [] =
{	127, 128, 129, 255, 256, 1, 1025, MAX_PACKET_FRAMES, 257, 63
} ;

static const uint32_t stereo16_counts [] =
{	MAX_PACKET_FRAMES, 1, 127, 257, 1025, 255, 128, 63
} ;

static const uint32_t mono24_counts [] =
{	MAX_PACKET_FRAMES, 1, 128, 257, 1023, 255, 127, 129
} ;

/* The in-tree ALAC encoder cannot round-trip one- or two-frame 24-bit
** stereo packets (the shared codec fails for those lengths). One-frame
** packets are covered by the mono 24-bit material.
*/
static const uint32_t stereo24_counts [] =
{	MAX_PACKET_FRAMES, 127, 256, 1025, 129, 255, 128, 512, 300, 384, 63, 257
} ;

/*==========================================================================
** Group: packets
*/

static int
case_mono16 (void)
{	int32_t *pcm ;
	sf_count_t total = 0, read_frames ;
	VFILE vf ;
	VIO vio ;
	SF_INFO info ;
	SNDFILE *s ;
	int *got ;
	unsigned p = sizeof (mono16_counts) / sizeof (mono16_counts [0]) ;

	pcm = make_pcm (16, 1, mono16_counts, p, &total, 11) ;
	TASK_CHECK (pcm != NULL) ;
	TASK_CHECK (build_vfile (&vf, 16, 1, mono16_counts, p, pcm, NULL)) ;

	s = open_virtual (&vf, &info, &vio) ;
	if (s == NULL)
	{	fprintf (stderr, "open failed: %s\n", sf_strerror (NULL)) ;
		{	static char logbuf [8192] ;
			sf_command (NULL, SFC_GET_LOG_INFO, logbuf, sizeof (logbuf)) ;
			fprintf (stderr, "parse log:\n%s\n", logbuf) ;
			}
		fflush (stderr) ;
		}
	TASK_CHECK (s != NULL) ;
	TASK_CHECK (info.frames == total) ;
	TASK_CHECK (info.channels == 1) ;
	TASK_CHECK (info.samplerate == SAMPLE_RATE) ;
	TASK_CHECK (info.format == (SF_FORMAT_CAF | SF_FORMAT_ALAC_16)) ;

	got = malloc ((size_t) total * sizeof (int)) ;
	TASK_CHECK (got != NULL) ;
	read_frames = sf_readf_int (s, got, total) ;
	TASK_CHECK (read_frames == total) ;
	TASK_CHECK (check_ints (pcm, got, total)) ;
	TASK_CHECK (sf_error (s) == 0) ;

	sf_close (s) ;
	free (got) ;
	vf_free (&vf) ;
	free (pcm) ;
	return 0 ;
} /* case_mono16 */



static int
case_stereo16_plain_file (void)
{	int32_t *pcm ;
	sf_count_t total = 0, done = 0, want ;
	VFILE vf ;
	SF_INFO info ;
	SNDFILE *s ;
	short *got ;
	unsigned p = sizeof (stereo16_counts) / sizeof (stereo16_counts [0]) ;
	unsigned block ;

	pcm = make_pcm (16, 2, stereo16_counts, p, &total, 23) ;
	TASK_CHECK (pcm != NULL) ;
	TASK_CHECK (build_vfile (&vf, 16, 2, stereo16_counts, p, pcm, NULL)) ;

	s = open_plain (&vf, &info, "caf_vpacket_tmp.caf") ;
	TASK_CHECK (s != NULL) ;
	TASK_CHECK (info.frames == total) ;
	TASK_CHECK (info.channels == 2) ;

	got = malloc ((size_t) total * 2 * sizeof (short)) ;
	TASK_CHECK (got != NULL) ;

	/* Deliberately ragged block sizes so packet and block boundaries differ. */
	for (block = 0 ; done < total ; )
	{	static const unsigned sizes [] = { 1, 3, 7, 64, 129, 2, 255 } ;

		want = (sf_count_t) sizes [block % (sizeof (sizes) / sizeof (sizes [0]))] ;
		if (want > total - done)
			want = total - done ;
		block++ ;

		TASK_CHECK (sf_readf_short (s, got + done * 2, want) == want) ;
		done += want ;
		} ;

	TASK_CHECK (check_shorts (pcm, got, total * 2)) ;
	TASK_CHECK (sf_readf_short (s, got, 1) == 0) ;
	TASK_CHECK (sf_error (s) == 0) ;

	sf_close (s) ;
	free (got) ;
	vf_free (&vf) ;
	free (pcm) ;
	return 0 ;
} /* case_stereo16_plain_file */

static int
case_mono24 (void)
{	int32_t *pcm ;
	sf_count_t total = 0, read_frames ;
	VFILE vf ;
	VIO vio ;
	SF_INFO info ;
	SNDFILE *s ;
	int *got ;
	unsigned p = sizeof (mono24_counts) / sizeof (mono24_counts [0]) ;

	pcm = make_pcm (24, 1, mono24_counts, p, &total, 37) ;
	TASK_CHECK (pcm != NULL) ;
	TASK_CHECK (build_vfile (&vf, 24, 1, mono24_counts, p, pcm, NULL)) ;

	s = open_virtual (&vf, &info, &vio) ;
	TASK_CHECK (s != NULL) ;
	TASK_CHECK (info.frames == total) ;
	TASK_CHECK (info.format == (SF_FORMAT_CAF | SF_FORMAT_ALAC_24)) ;

	got = malloc ((size_t) total * sizeof (int)) ;
	TASK_CHECK (got != NULL) ;
	read_frames = sf_readf_int (s, got, total) ;
	TASK_CHECK (read_frames == total) ;
	TASK_CHECK (check_ints (pcm, got, total)) ;
	TASK_CHECK (sf_error (s) == 0) ;

	sf_close (s) ;
	free (got) ;
	vf_free (&vf) ;
	free (pcm) ;
	return 0 ;
} /* case_mono24 */

static int
case_stereo24_blocks (void)
{	int32_t *pcm ;
	sf_count_t total = 0, done = 0, want ;
	VFILE vf ;
	VIO vio ;
	SF_INFO info ;
	SNDFILE *s ;
	int *got ;
	unsigned p = sizeof (stereo24_counts) / sizeof (stereo24_counts [0]) ;
	unsigned block ;

	pcm = make_pcm (24, 2, stereo24_counts, p, &total, 53) ;
	TASK_CHECK (pcm != NULL) ;
	TASK_CHECK (build_vfile (&vf, 24, 2, stereo24_counts, p, pcm, NULL)) ;

	s = open_virtual (&vf, &info, &vio) ;
	TASK_CHECK (s != NULL) ;
	TASK_CHECK (info.frames == total) ;
	TASK_CHECK (info.channels == 2) ;

	got = malloc ((size_t) total * 2 * sizeof (int)) ;
	TASK_CHECK (got != NULL) ;

	for (block = 0 ; done < total ; block++)
	{	static const unsigned sizes [] = { 5, 1, 17, 100, 4096, 33 } ;

		want = (sf_count_t) sizes [block % (sizeof (sizes) / sizeof (sizes [0]))] ;
		if (want > total - done)
			want = total - done ;

		TASK_CHECK (sf_readf_int (s, got + done * 2, want) == want) ;
		done += want ;
		} ;

	TASK_CHECK (check_ints (pcm, got, total * 2)) ;
	TASK_CHECK (sf_readf_int (s, got, 10) == 0) ;

	sf_close (s) ;
	free (got) ;
	vf_free (&vf) ;
	free (pcm) ;
	return 0 ;
} /* case_stereo24_blocks */

static int
case_four_output_types (void)
{	int32_t *pcm ;
	sf_count_t total = 0, k ;
	VFILE vf ;
	VIO vio ;
	SF_INFO info ;
	SNDFILE *s ;
	int *ints ;
	short *shorts ;
	float *floats ;
	double *doubles ;
	unsigned p = sizeof (stereo16_counts) / sizeof (stereo16_counts [0]) ;

	pcm = make_pcm (16, 2, stereo16_counts, p, &total, 71) ;
	TASK_CHECK (pcm != NULL) ;
	TASK_CHECK (build_vfile (&vf, 16, 2, stereo16_counts, p, pcm, NULL)) ;

	ints = malloc ((size_t) total * 2 * sizeof (int)) ;
	shorts = malloc ((size_t) total * 2 * sizeof (short)) ;
	floats = malloc ((size_t) total * 2 * sizeof (float)) ;
	doubles = malloc ((size_t) total * 2 * sizeof (double)) ;
	TASK_CHECK (ints && shorts && floats && doubles) ;

	s = open_virtual (&vf, &info, &vio) ;
	TASK_CHECK (s != NULL) ;
	TASK_CHECK (sf_readf_int (s, ints, total) == total) ;
	sf_close (s) ;

	s = open_virtual (&vf, &info, &vio) ;
	TASK_CHECK (s != NULL) ;
	TASK_CHECK (sf_readf_short (s, shorts, total) == total) ;
	sf_close (s) ;

	s = open_virtual (&vf, &info, &vio) ;
	TASK_CHECK (s != NULL) ;
	/* Read in uneven pieces for this output type. */
	for (k = 0 ; k < total ; )
	{	sf_count_t want = (k % 3 == 0) ? 1 : (k % 5 == 0 ? 97 : 11) ;

		if (want > total - k)
			want = total - k ;
		TASK_CHECK (sf_readf_float (s, floats + k * 2, want) == want) ;
		k += want ;
		} ;
	sf_close (s) ;

	s = open_virtual (&vf, &info, &vio) ;
	TASK_CHECK (s != NULL) ;
	TASK_CHECK (sf_readf_double (s, doubles, total) == total) ;
	sf_close (s) ;

	TASK_CHECK (check_ints (pcm, ints, total * 2)) ;
	TASK_CHECK (check_shorts (pcm, shorts, total * 2)) ;
	TASK_CHECK (check_floats (pcm, floats, total * 2)) ;
	TASK_CHECK (check_doubles (pcm, doubles, total * 2)) ;

	free (ints) ;
	free (shorts) ;
	free (floats) ;
	free (doubles) ;
	vf_free (&vf) ;
	free (pcm) ;
	return 0 ;
} /* case_four_output_types */

/*==========================================================================
** Group: seeking
*/

static int
seek_check_ints (SNDFILE *s, const int32_t *pcm, sf_count_t frame, sf_count_t frames, int channels, int *scratch)
{	sf_count_t k ;

	TASK_CHECK (sf_seek (s, frame, SEEK_SET) == frame) ;
	TASK_CHECK (sf_readf_int (s, scratch, frames) == frames) ;
	for (k = 0 ; k < frames * channels ; k++)
		TASK_CHECK (scratch [k] == pcm [frame * channels + k]) ;

	return 0 ;
} /* seek_check_ints */

static int
case_seek_mono16 (void)
{	int32_t *pcm ;
	sf_count_t total = 0, base = 0, k ;
	VFILE vf ;
	VIO vio ;
	SF_INFO info ;
	SNDFILE *s ;
	int scratch [64] ;
	unsigned p = sizeof (mono16_counts) / sizeof (mono16_counts [0]), i ;

	pcm = make_pcm (16, 1, mono16_counts, p, &total, 89) ;
	TASK_CHECK (pcm != NULL) ;
	TASK_CHECK (build_vfile (&vf, 16, 1, mono16_counts, p, pcm, NULL)) ;

	s = open_virtual (&vf, &info, &vio) ;
	TASK_CHECK (s != NULL) ;
	TASK_CHECK (info.frames == total) ;

	/* Every packet boundary, and a few positions inside each packet. */
	for (i = 0 ; i < p ; i++)
	{	sf_count_t inside [] = { 0, 1, (sf_count_t) mono16_counts [i] / 2, (sf_count_t) mono16_counts [i] - 1 } ;
		unsigned j ;

		for (j = 0 ; j < sizeof (inside) / sizeof (inside [0]) ; j++)
		{	sf_count_t frame = base + inside [j] ;
			sf_count_t want = 17 ;

			if (want > total - frame)
				want = total - frame ;
			if (want <= 0)
				continue ;
			TASK_CHECK (seek_check_ints (s, pcm, frame, want, 1, scratch) == 0) ;
			} ;

		base += mono16_counts [i] ;
		} ;

	/* Repeated forward and backward jumps. */
	for (k = 0 ; k < 10 ; k++)
	{	sf_count_t frame = ((sf_count_t []) { total / 3, total - 5, 0, total / 2, 17 }) [k % 5] ;
		sf_count_t want = 9 ;

		if (want > total - frame)
			want = total - frame ;
		TASK_CHECK (seek_check_ints (s, pcm, frame, want, 1, scratch) == 0) ;
		} ;

	/* SEEK_SET to the end is legal, and a read there returns nothing. */
	TASK_CHECK (sf_seek (s, total, SEEK_SET) == total) ;
	TASK_CHECK (sf_readf_int (s, scratch, 1) == 0) ;
	TASK_CHECK (sf_error (s) == 0) ;

	/* SEEK_END with an offset that lands on a packet boundary. */
	base = total ;
	for (i = p ; i-- > 0 ; )
	{	base -= mono16_counts [i] ;
		TASK_CHECK (sf_seek (s, base - total, SEEK_END) == base) ;
		TASK_CHECK (sf_readf_int (s, scratch, 7) == 7) ;
		for (k = 0 ; k < 7 ; k++)
			TASK_CHECK (scratch [k] == pcm [base + k]) ;
		break ;
		} ;

	/* Out of range seeks must fail. */
	TASK_CHECK (sf_seek (s, -1, SEEK_SET) == -1) ;
	TASK_CHECK (sf_seek (s, total + 1, SEEK_SET) == -1) ;
	TASK_CHECK (sf_seek (s, 1, SEEK_END) == -1) ;

	sf_close (s) ;
	vf_free (&vf) ;
	free (pcm) ;
	return 0 ;
} /* case_seek_mono16 */

static int
case_seek_stereo24 (void)
{	int32_t *pcm ;
	sf_count_t total = 0, base = 0, k ;
	VFILE vf ;
	SF_INFO info ;
	SNDFILE *s ;
	int scratch [128] ;
	unsigned p = sizeof (stereo24_counts) / sizeof (stereo24_counts [0]), i ;

	pcm = make_pcm (24, 2, stereo24_counts, p, &total, 97) ;
	TASK_CHECK (pcm != NULL) ;
	TASK_CHECK (build_vfile (&vf, 24, 2, stereo24_counts, p, pcm, NULL)) ;

	/* Seek the same way through a plain file as through virtual IO. */
	s = open_plain (&vf, &info, "caf_vpacket_seek.caf") ;
	TASK_CHECK (s != NULL) ;
	TASK_CHECK (info.frames == total) ;

	/* Seek to each packet boundary, then read across the next two packets. */
	for (i = 0 ; i < p ; i++)
	{	sf_count_t want = 3 ;

		TASK_CHECK (seek_check_ints (s, pcm, base, want, 2, scratch) == 0) ;
		if (base > 0)
		{	/* One frame before the boundary: previous packet, last frame. */
			TASK_CHECK (sf_seek (s, base - 1, SEEK_SET) == base - 1) ;
			TASK_CHECK (sf_readf_int (s, scratch, 1) == 1) ;
			for (k = 0 ; k < 2 ; k++)
				TASK_CHECK (scratch [k] == pcm [(base - 1) * 2 + k]) ;
			} ;
		base += stereo24_counts [i] ;
		} ;

	/* Backward/forward pairs around a mid-file position. */
	k = total / 2 ;
	TASK_CHECK (seek_check_ints (s, pcm, k, 11, 2, scratch) == 0) ;
	TASK_CHECK (seek_check_ints (s, pcm, 3, 25, 2, scratch) == 0) ;
	TASK_CHECK (seek_check_ints (s, pcm, k, 11, 2, scratch) == 0) ;
	TASK_CHECK (seek_check_ints (s, pcm, total - 4, 4, 2, scratch) == 0) ;

	/* SEEK_CUR works on the same zero based sample axis. */
	TASK_CHECK (sf_seek (s, 7, SEEK_SET) == 7) ;
	TASK_CHECK (sf_seek (s, 5, SEEK_CUR) == 12) ;
	TASK_CHECK (sf_readf_int (s, scratch, 4) == 4) ;
	for (k = 0 ; k < 8 ; k++)
		TASK_CHECK (scratch [k] == pcm [12 * 2 + k]) ;
	TASK_CHECK (sf_seek (s, 0, SEEK_CUR) == 16) ;

	/* SEEK_END from zero, then slightly backwards. */
	TASK_CHECK (sf_seek (s, 0, SEEK_END) == total) ;
	TASK_CHECK (sf_seek (s, -3, SEEK_END) == total - 3) ;
	TASK_CHECK (sf_readf_int (s, scratch, 3) == 3) ;
	for (k = 0 ; k < 6 ; k++)
		TASK_CHECK (scratch [k] == pcm [(total - 3) * 2 + k]) ;

	sf_close (s) ;
	vf_free (&vf) ;
	free (pcm) ;
	return 0 ;
} /* case_seek_stereo24 */

/*==========================================================================
** Group: invalid
*/

static int
expect_open_failure (VFILE *vf, const char *name)
{	SF_INFO info ;
	VIO vio ;
	SNDFILE *s ;

	s = open_virtual (vf, &info, &vio) ;
	if (s != NULL)
	{	fprintf (stderr, "expected rejection but opened: %s\n", name) ;
		sf_close (s) ;
		return 0 ;
		} ;

	/* A malformed file must surface an error, not a quiet EOF-shaped open. */
	return sf_error (NULL) != 0 ;
} /* expect_open_failure */

/* Build a baseline variable-packet file and confirm it opens and reads
** correctly. Every invalid case starts from this fixture, so a rejection
** below is caused by the mutation and not by a broken fixture.
*/
static int
invalid_base (VFILE *vf, int32_t **pcm_out, sf_count_t *total_out)
{	int32_t *pcm ;
	sf_count_t total = 0 ;
	unsigned p = sizeof (stereo16_counts) / sizeof (stereo16_counts [0]) ;
	VIO vio ;
	SF_INFO info ;
	SNDFILE *s ;
	int *got ;

	pcm = make_pcm (16, 2, stereo16_counts, p, &total, 131) ;
	if (pcm == NULL)
		return 0 ;
	if (! build_vfile (vf, 16, 2, stereo16_counts, p, pcm, NULL))
	{	free (pcm) ;
		return 0 ;
		} ;

	if ((got = malloc ((size_t) total * 2 * sizeof (int))) == NULL)
	{	free (pcm) ;
		vf_free (vf) ;
		return 0 ;
		} ;

	s = open_virtual (vf, &info, &vio) ;
	if (s == NULL || info.frames != total || sf_readf_int (s, got, total) != total)
	{	if (s)
			sf_close (s) ;
		free (got) ;
		free (pcm) ;
		vf_free (vf) ;
		return 0 ;
		} ;
	sf_close (s) ;

	*pcm_out = pcm ;
	*total_out = total ;
	free (got) ;
	return 1 ;
} /* invalid_base */

static int
case_invalid_zero_size (void)
{	VFILE vf ;
	int32_t *pcm ;
	sf_count_t total ;
	TASK_CHECK (invalid_base (&vf, &pcm, &total)) ;
	(void) total ;
	/* Packet 2 is a small packet; zero its byte count in place. */
	varint_put_fixed (vf.data + vf.size_off [2], vf.size_w [2], 0) ;
	TASK_CHECK (expect_open_failure (&vf, "zero_size")) ;

	vf_free (&vf) ;
	free (pcm) ;
	return 0 ;
} /* case_invalid_zero_size */

static int
case_invalid_zero_frames (void)
{	VFILE vf ;
	int32_t *pcm ;
	sf_count_t total ;
	TASK_CHECK (invalid_base (&vf, &pcm, &total)) ;
	(void) total ;
	varint_put_fixed (vf.data + vf.frame_off [2], vf.frame_w [2], 0) ;
	TASK_CHECK (expect_open_failure (&vf, "zero_frames")) ;

	vf_free (&vf) ;
	free (pcm) ;
	return 0 ;
} /* case_invalid_zero_frames */

static int
case_invalid_truncated_table (void)
{	VFILE vf ;
	int32_t *pcm ;
	sf_count_t total ;
	BUILD_OPTS opt = { 0, 0, 0, 0, 0, 1, -1 } ;

	pcm = make_pcm (16, 2, stereo16_counts, sizeof (stereo16_counts) / sizeof (stereo16_counts [0]), &total, 131) ;
	TASK_CHECK (pcm != NULL) ;
	TASK_CHECK (build_vfile (&vf, 16, 2, stereo16_counts,
				sizeof (stereo16_counts) / sizeof (stereo16_counts [0]), pcm, &opt)) ;
	TASK_CHECK (expect_open_failure (&vf, "truncated_table")) ;

	vf_free (&vf) ;
	free (pcm) ;
	return 0 ;
} /* case_invalid_truncated_table */

static int
case_invalid_varint_overflow (void)
{	VFILE vf ;
	int32_t *pcm ;
	sf_count_t total ;
	BUILD_OPTS opt = { 0, 0, 0, 0, 0, 0, 1 } ;

	pcm = make_pcm (16, 2, stereo16_counts, sizeof (stereo16_counts) / sizeof (stereo16_counts [0]), &total, 131) ;
	TASK_CHECK (pcm != NULL) ;
	TASK_CHECK (build_vfile (&vf, 16, 2, stereo16_counts,
				sizeof (stereo16_counts) / sizeof (stereo16_counts [0]), pcm, &opt)) ;
	TASK_CHECK (expect_open_failure (&vf, "varint_overflow")) ;

	vf_free (&vf) ;
	free (pcm) ;
	return 0 ;
} /* case_invalid_varint_overflow */

static int
case_invalid_count_mismatch (void)
{	VFILE vf ;
	int32_t *pcm ;
	sf_count_t total ;
	BUILD_OPTS low = { -1, 0, 0, 0, 0, 0, -1 } ;
	BUILD_OPTS high = { 1, 0, 0, 0, 0, 0, -1 } ;
	BUILD_OPTS extra = { 0, 0, 0, 0, 1, 0, -1 } ;
	unsigned p = sizeof (stereo16_counts) / sizeof (stereo16_counts [0]) ;

	pcm = make_pcm (16, 2, stereo16_counts, p, &total, 131) ;
	TASK_CHECK (pcm != NULL) ;

	TASK_CHECK (build_vfile (&vf, 16, 2, stereo16_counts, p, pcm, &low)) ;
	TASK_CHECK (expect_open_failure (&vf, "count_low")) ;
	vf_free (&vf) ;

	TASK_CHECK (build_vfile (&vf, 16, 2, stereo16_counts, p, pcm, &high)) ;
	TASK_CHECK (expect_open_failure (&vf, "count_high")) ;
	vf_free (&vf) ;

	TASK_CHECK (build_vfile (&vf, 16, 2, stereo16_counts, p, pcm, &extra)) ;
	TASK_CHECK (expect_open_failure (&vf, "count_extra")) ;
	vf_free (&vf) ;

	free (pcm) ;
	return 0 ;
} /* case_invalid_count_mismatch */

static int
case_invalid_frame_total (void)
{	VFILE vf ;
	int32_t *pcm ;
	sf_count_t total ;
	TASK_CHECK (invalid_base (&vf, &pcm, &total)) ;
	(void) total ;

	/* Patch the valid-frame field directly so the build remains otherwise
	** valid. */
	be64_put (vf.data + vf.pakt_valid_off, (uint64_t) (vf.total - 1)) ;
	TASK_CHECK (expect_open_failure (&vf, "valid_low")) ;
	vf_free (&vf) ;

	TASK_CHECK (invalid_base (&vf, &pcm, &total)) ;
	be64_put (vf.data + vf.pakt_valid_off, (uint64_t) (vf.total + 1)) ;
	TASK_CHECK (expect_open_failure (&vf, "valid_high")) ;
	vf_free (&vf) ;

	free (pcm) ;
	return 0 ;
} /* case_invalid_frame_total */

static int
case_invalid_data_range (void)
{	VFILE vf ;
	int32_t *pcm ;
	sf_count_t total ;

	TASK_CHECK (invalid_base (&vf, &pcm, &total)) ;
	(void) total ;
	{	uint64_t sum = 0 ;
		unsigned p ;
		for (p = 0 ; p < vf.npackets ; p++)
			sum += vf.sizes [p] ;
		/* One byte short: the table accounts for more payload than the
		** data chunk holds. */
		be64_put (vf.data + vf.data_size_off, sum + 4 - 1) ;
		}
	TASK_CHECK (expect_open_failure (&vf, "data_short")) ;
	vf_free (&vf) ;

	TASK_CHECK (invalid_base (&vf, &pcm, &total)) ;
	{	uint64_t sum = 0 ;
		unsigned p ;
		for (p = 0 ; p < vf.npackets ; p++)
			sum += vf.sizes [p] ;
		/* One byte extra: the table accounts for less payload than the
		** data chunk holds. */
		be64_put (vf.data + vf.data_size_off, sum + 4 + 1) ;
		}
	TASK_CHECK (expect_open_failure (&vf, "data_long")) ;
	vf_free (&vf) ;

	free (pcm) ;
	return 0 ;
} /* case_invalid_data_range */

static int
case_invalid_frame_too_large (void)
{	VFILE vf ;
	int32_t *pcm ;
	sf_count_t total ;
	unsigned p ;

	TASK_CHECK (invalid_base (&vf, &pcm, &total)) ;
	(void) total ;
	/* Packet 4 has 1025 frames, encoded in two bytes. */
	p = 4 ;
	if (vf.frame_w [p] < 2)
	{	vf_free (&vf) ;
		free (pcm) ;
		TASK_CHECK (0) ;
		} ;
	varint_put_fixed (vf.data + vf.frame_off [p], vf.frame_w [p], MAX_PACKET_FRAMES + 1) ;
	TASK_CHECK (expect_open_failure (&vf, "frame_too_large")) ;

	vf_free (&vf) ;
	free (pcm) ;
	return 0 ;
} /* case_invalid_frame_too_large */

static int
case_invalid_priming (void)
{	VFILE vf ;
	int32_t *pcm ;
	sf_count_t total ;
	BUILD_OPTS opt = { 0, 0, 1, 0, 0, 0, -1 } ;

	pcm = make_pcm (16, 2, stereo16_counts, sizeof (stereo16_counts) / sizeof (stereo16_counts [0]), &total, 131) ;
	TASK_CHECK (pcm != NULL) ;
	TASK_CHECK (build_vfile (&vf, 16, 2, stereo16_counts,
				sizeof (stereo16_counts) / sizeof (stereo16_counts [0]), pcm, &opt)) ;
	TASK_CHECK (expect_open_failure (&vf, "priming")) ;
	vf_free (&vf) ;
	free (pcm) ;

	opt.priming = 0 ;
	opt.remainder = 3 ;
	pcm = make_pcm (16, 2, stereo16_counts, sizeof (stereo16_counts) / sizeof (stereo16_counts [0]), &total, 131) ;
	TASK_CHECK (pcm != NULL) ;
	TASK_CHECK (build_vfile (&vf, 16, 2, stereo16_counts,
				sizeof (stereo16_counts) / sizeof (stereo16_counts [0]), pcm, &opt)) ;
	TASK_CHECK (expect_open_failure (&vf, "remainder")) ;
	vf_free (&vf) ;
	free (pcm) ;

	return 0 ;
} /* case_invalid_priming */

static int
case_invalid_declared_frames (void)
{	int32_t *pcm ;
	sf_count_t total = 0, prefix = 0 ;
	VFILE vf ;
	VIO vio ;
	SF_INFO info ;
	SNDFILE *s ;
	int got [16] ;
	uint32_t first_bad ;
	unsigned p = sizeof (stereo16_counts) / sizeof (stereo16_counts [0]) ;

	pcm = make_pcm (16, 2, stereo16_counts, p, &total, 131) ;
	TASK_CHECK (pcm != NULL) ;
	TASK_CHECK (build_vfile (&vf, 16, 2, stereo16_counts, p, pcm, NULL)) ;

	/* Packet 3 holds 257 frames, encoded in a two byte varint. Claim 258
	** instead: the table total and the declared valid-frame count both move
	** by one so opening still succeeds, but the packet itself still holds
	** 257 frames and cannot decode to the declared length.
	*/
	first_bad = 3 ;
	TASK_CHECK (stereo16_counts [first_bad] >= 128) ;
	TASK_CHECK (vf.frame_w [first_bad] == 2) ;
	varint_put_fixed (vf.data + vf.frame_off [first_bad], vf.frame_w [first_bad], stereo16_counts [first_bad] + 1) ;
	be64_put (vf.data + vf.pakt_valid_off, (uint64_t) (vf.total + 1)) ;
	/* The packet byte total is unchanged, so the data range check passes. */

	prefix = 0 ;
	{	uint32_t k ;
		for (k = 0 ; k < first_bad ; k++)
			prefix += stereo16_counts [k] ;
		}

	s = open_virtual (&vf, &info, &vio) ;
	TASK_CHECK (s != NULL) ;
	TASK_CHECK (info.frames == total + 1) ;

	/* Asking for the whole stream must publish exactly the correct prefix. */
	TASK_CHECK (sf_readf_int (s, got, 5) == 5) ;
	{
		int k ;
		for (k = 0 ; k < 10 ; k++)
			TASK_CHECK (got [k] == pcm [k]) ;
		} ;
	TASK_CHECK (sf_seek (s, 0, SEEK_SET) == 0) ;
	{
		int32_t *whole = malloc ((size_t) (total + 1) * 2 * sizeof (int)) ;
		sf_count_t n ;
		TASK_CHECK (whole != NULL) ;
		n = sf_readf_int (s, whole, total + 1) ;
		TASK_CHECK (n == prefix) ;
		{
			sf_count_t k ;
			for (k = 0 ; k < prefix * 2 ; k++)
				TASK_CHECK (whole [k] == pcm [k]) ;
			} ;
		TASK_CHECK (sf_error (s) != 0) ;
		/* A retry must fail again rather than skip the bad packet. */
		TASK_CHECK (sf_readf_int (s, whole, 4) == 0) ;
		free (whole) ;
		} ;

	sf_close (s) ;
	vf_free (&vf) ;
	free (pcm) ;
	return 0 ;
} /* case_invalid_declared_frames */

/*==========================================================================*/

const struct task_case task_cases [] =
{	{ "packets", "mono16_virtual", case_mono16 },
	{ "packets", "stereo16_plain_file", case_stereo16_plain_file },
	{ "packets", "mono24_virtual", case_mono24 },
	{ "packets", "stereo24_blocks", case_stereo24_blocks },
	{ "packets", "four_output_types", case_four_output_types },

	{ "seeking", "mono16_boundaries", case_seek_mono16 },
	{ "seeking", "stereo24_boundaries", case_seek_stereo24 },

	{ "invalid", "zero_size", case_invalid_zero_size },
	{ "invalid", "zero_frames", case_invalid_zero_frames },
	{ "invalid", "truncated_table", case_invalid_truncated_table },
	{ "invalid", "varint_overflow", case_invalid_varint_overflow },
	{ "invalid", "count_mismatch", case_invalid_count_mismatch },
	{ "invalid", "frame_total", case_invalid_frame_total },
	{ "invalid", "data_range", case_invalid_data_range },
	{ "invalid", "frame_too_large", case_invalid_frame_too_large },
	{ "invalid", "priming_remainder", case_invalid_priming },
	{ "invalid", "declared_frames", case_invalid_declared_frames }
} ;

const size_t task_case_count = sizeof (task_cases) / sizeof (task_cases [0]) ;

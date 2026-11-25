/*
** Regression tests for variable duration CAF/ALAC packets.
**
** The pakt chunk of these files carries alternating (byte size, frame count)
** vints per packet while the desc chunk has bytes_per_packet and
** frames_per_packet both set to zero.  Every fixture is generated with the
** in tree ALAC encoder and compared against the original PCM independently.
*/

#include "task_support/check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include "sndfile.h"
#include "src/ALAC/alac_codec.h"

/*----------------------------------------------------------------------------
** Small dynamic byte buffer / CAF builder.
*/

typedef struct
{	unsigned char	*b ;
	size_t			len, cap ;
} bytes_t ;

static int
bytes_reserve (bytes_t *x, size_t extra)
{	if (x->len + extra + 1 <= x->cap)
		return 0 ;
	size_t cap = x->cap ? x->cap * 2 : 1024 ;
	while (cap < x->len + extra + 1)
		cap *= 2 ;
	unsigned char *nb = realloc (x->b, cap) ;
	if (nb == NULL)
		return 1 ;
	x->b = nb ;
	x->cap = cap ;
	return 0 ;
} /* bytes_reserve */

static int
bytes_put (bytes_t *x, const void *p, size_t n)
{	if (bytes_reserve (x, n))
		return 1 ;
	memcpy (x->b + x->len, p, n) ;
	x->len += n ;
	return 0 ;
} /* bytes_put */

static void
put_be (bytes_t *x, uint64_t value, int nbytes)
{	int i ;
	for (i = nbytes - 1 ; i >= 0 ; i --)
	{	unsigned char c = (unsigned char) ((value >> (8 * i)) & 0xff) ;
		bytes_put (x, &c, 1) ;
		} ;
} /* put_be */

/* Encode a high bit continuation, big endian 7 bit integer. */
static unsigned
put_vint (bytes_t *x, uint32_t value)
{	unsigned char tmp [5] ;
	unsigned n = 0, k = 0, written = 0 ;

	do
	{	tmp [n ++] = value & 0x7f ;
		value >>= 7 ;
		}
	while (value) ;

	while (n)
	{	n -- ;
		unsigned char c = tmp [n] | (n ? 0x80 : 0) ;
		bytes_put (x, &c, 1) ;
		written ++ ;
		} ;

	(void) k ;
	return written ;
} /* put_vint */

static unsigned
vint_encoded_len (uint32_t value)
{	unsigned n = 1 ;
	while (value >= 128)
	{	n ++ ;
		value >>= 7 ;
		} ;
	return n ;
} /* vint_encoded_len */

static void
emit_chunk (bytes_t *out, const char id [4], const bytes_t *body)
{	bytes_put (out, id, 4) ;
	put_be (out, body->len, 8) ;
	bytes_put (out, body->b, body->len) ;
} /* emit_chunk */

/*----------------------------------------------------------------------------
** Real ALAC packet scene and its independent PCM reference.
*/

#define		SCENE_MAX_PACKETS	32
#define		SCENE_MAX_FRAMES	4096

typedef struct
{	int				channels ;
	int				bits ;
	int				format_flag ;
	uint32_t		counts [SCENE_MAX_PACKETS] ;
	int				packet_count ;
	uint32_t		total_frames ;
	int32_t			*pcm ;						/* total_frames * channels */
	unsigned char	*packet_data [SCENE_MAX_PACKETS] ;
	uint32_t		packet_size [SCENE_MAX_PACKETS] ;
	unsigned char	cookie [256] ;
	uint32_t		cookie_size ;
} scene_t ;

static uint32_t
rng_next (uint32_t *state)
{	*state = *state * 1664525u + 1013904223u ;
	return *state ;
} /* rng_next */

/* Left aligned int32 sample, exactly the convention sf_readf_int exposes.
** The structured branch follows the same deterministic modulo ramp family as
** the task baseline and stays losslessly round trippable by the in tree ALAC
** codec ; the wild branch is only used on configurations whose escape packets
** are known to round trip exactly. */
static int32_t
gen_sample (uint32_t frame, int ch, int bits, int wild, uint32_t seed)
{	int64_t		top = 1LL << (bits - 1) ;
	int64_t		raw ;

	if (wild)
	{	uint32_t state = seed + frame * 2246822519u + (uint32_t) ch * 3266489917u + 1u ;
		raw = (int64_t) (rng_next (&state) % (uint32_t) (2 * top)) - top ;
		}
	else
	{	/* Channel asymmetric modulo ramps with a bounded noise component. */
		int64_t mod = 2 * top - 1 ;
		int64_t ramp = (int64_t) ((frame * (149 + 61 * ch) + 31 + 997 * ch + seed) % (uint64_t) mod) - top ;
		uint32_t state = seed ^ (frame * 2654435761u) ^ ((uint32_t) ch * 0x9e3779b9u) ;
		int64_t noise = (int64_t) (rng_next (&state) % (uint32_t) (top / 6)) - top / 12 ;
		raw = ramp + noise ;
		} ;

	if (raw < -top)
		raw = -top ;
	if (raw > top - 1)
		raw = top - 1 ;

	return (int32_t) (raw * (1LL << (32 - bits))) ;
} /* gen_sample */

static int
scene_init (scene_t *sc, int channels, int bits, const uint32_t *counts, int packet_count,
			int wild, uint32_t seed)
{	ALAC_ENCODER	*enc ;
	int				p, ch_index ;
	uint32_t		frame, total = 0 ;

	memset (sc, 0, sizeof (*sc)) ;
	sc->channels = channels ;
	sc->bits = bits ;
	sc->format_flag = (bits == 24) ? 3 : 1 ;
	sc->packet_count = packet_count ;

	for (p = 0 ; p < packet_count ; p ++)
	{	sc->counts [p] = counts [p] ;
		total += counts [p] ;
		} ;
	sc->total_frames = total ;

	sc->pcm = calloc ((size_t) total * channels, sizeof (int32_t)) ;
	if (sc->pcm == NULL)
		return 1 ;

	for (frame = 0 ; frame < total ; frame ++)
		for (ch_index = 0 ; ch_index < channels ; ch_index ++)
			sc->pcm [(size_t) frame * channels + ch_index] =
				gen_sample (frame, ch_index, bits, wild, seed) ;

	enc = calloc (1, sizeof (*enc)) ;
	if (enc == NULL)
		return 1 ;

	if (alac_encoder_init (enc, 48000, channels, sc->format_flag, SCENE_MAX_FRAMES))
	{	free (enc) ;
		return 1 ;
		} ;

	for (p = 0 ; p < packet_count ; p ++)
	{	uint32_t first = 0, q ;
		uint32_t io_bytes ;

		for (q = 0 ; q < (uint32_t) p ; q ++)
			first += sc->counts [q] ;

		sc->packet_data [p] = malloc (256 * 1024) ;
		if (sc->packet_data [p] == NULL)
		{	free (enc) ;
			return 1 ;
			} ;

		io_bytes = 256 * 1024 ;
		if (alac_encode (enc, sc->counts [p], sc->pcm + (size_t) first * channels,
				sc->packet_data [p], &io_bytes))
		{	free (enc) ;
			return 1 ;
			} ;
		sc->packet_size [p] = io_bytes ;
		} ;

	sc->cookie_size = sizeof (sc->cookie) ;
	alac_get_magic_cookie (enc, sc->cookie, &sc->cookie_size) ;
	free (enc) ;

	return sc->cookie_size == 0 ;
} /* scene_init */

static void
scene_free (scene_t *sc)
{	int p ;
	free (sc->pcm) ;
	for (p = 0 ; p < sc->packet_count ; p ++)
		free (sc->packet_data [p]) ;
	memset (sc, 0, sizeof (*sc)) ;
} /* scene_free */

/*----------------------------------------------------------------------------
** CAF construction with optional declaration corruptions.
*/

typedef struct
{	int				set_packets ;
	uint64_t		packets ;
	int				set_valid ;
	uint64_t		valid_frames ;
	int				set_pr ;
	uint32_t		priming, remainder ;

	int				size_override_index ;
	uint32_t		size_override_value ;
	int				frames_override_index ;
	uint32_t		frames_override_value ;

	const uint8_t	*raw_table ;
	size_t			raw_table_len ;
} caf_opts_t ;

static void
caf_opts_default (caf_opts_t *o)
{	memset (o, 0, sizeof (*o)) ;
	o->size_override_index = -1 ;
	o->frames_override_index = -1 ;
} /* caf_opts_default */

static int
build_caf (const scene_t *sc, const caf_opts_t *opts, bytes_t *out)
{	bytes_t		desc, kuki, pakt, data ;
	caf_opts_t	def ;
	int			p ;
	uint64_t	hdr_packets, hdr_valid ;
	uint32_t	priming = 0, remainder = 0 ;

	if (opts == NULL)
	{	caf_opts_default (&def) ;
		opts = &def ;
		} ;

	memset (out, 0, sizeof (*out)) ;
	memset (&desc, 0, sizeof (desc)) ;
	memset (&kuki, 0, sizeof (kuki)) ;
	memset (&pakt, 0, sizeof (pakt)) ;
	memset (&data, 0, sizeof (data)) ;

	/* caff file header. */
	if (bytes_put (out, "caff", 4))
		return 1 ;
	put_be (out, 1, 2) ;
	put_be (out, 0, 2) ;

	/* desc : ALAC, variable packets (both fixed sizes zero). */
	put_be (&desc, UINT64_C (0x40e7700000000000), 8) ;	/* 48000.0 double */
	bytes_put (&desc, "alac", 4) ;
	put_be (&desc, sc->format_flag, 4) ;
	put_be (&desc, 0, 4) ;							/* bytes per packet */
	put_be (&desc, 0, 4) ;							/* frames per packet */
	put_be (&desc, sc->channels, 4) ;
	put_be (&desc, 0, 4) ;							/* bits per channel */
	emit_chunk (out, "desc", &desc) ;

	/* kuki : ALAC magic cookie. */
	bytes_put (&kuki, sc->cookie, sc->cookie_size) ;
	emit_chunk (out, "kuki", &kuki) ;

	/* pakt. */
	hdr_packets = opts->set_packets ? opts->packets : (uint64_t) sc->packet_count ;
	hdr_valid = opts->set_valid ? opts->valid_frames : (uint64_t) sc->total_frames ;
	if (opts->set_pr)
	{	priming = opts->priming ;
		remainder = opts->remainder ;
		} ;

	put_be (&pakt, hdr_packets, 8) ;
	put_be (&pakt, hdr_valid, 8) ;
	put_be (&pakt, priming, 4) ;
	put_be (&pakt, remainder, 4) ;

	if (opts->raw_table != NULL)
	{	if (bytes_put (&pakt, opts->raw_table, opts->raw_table_len))
			return 1 ;
		}
	else
	{	for (p = 0 ; p < sc->packet_count ; p ++)
		{	uint32_t byte_count = sc->packet_size [p] ;
			uint32_t frame_count = sc->counts [p] ;

			if (opts->size_override_index == p)
				byte_count = opts->size_override_value ;
			if (opts->frames_override_index == p)
				frame_count = opts->frames_override_value ;

			put_vint (&pakt, byte_count) ;
			put_vint (&pakt, frame_count) ;
			} ;
		} ;

	emit_chunk (out, "pakt", &pakt) ;

	/* data : edit field plus the real encoded packets. */
	put_be (&data, 0, 4) ;
	for (p = 0 ; p < sc->packet_count ; p ++)
		bytes_put (&data, sc->packet_data [p], sc->packet_size [p]) ;
	emit_chunk (out, "data", &data) ;

	free (desc.b) ;
	free (kuki.b) ;
	free (pakt.b) ;
	free (data.b) ;
	return 0 ;
} /* build_caf */

static const char *
write_caf_file (const bytes_t *caf, const char *name)
{	static char	path [320] ;
	FILE		*f ;

	snprintf (path, sizeof (path), ".task-build/vp_%s.caf", name) ;
	f = fopen (path, "wb") ;
	if (f == NULL)
		return NULL ;
	if (fwrite (caf->b, 1, caf->len, f) != caf->len)
	{	fclose (f) ;
		return NULL ;
		} ;
	if (fclose (f))
		return NULL ;
	return path ;
} /* write_caf_file */

/*----------------------------------------------------------------------------
** Memory virtual IO.
*/

typedef struct
{	const unsigned char	*data ;
	sf_count_t			length, position ;
} mem_vio_t ;

static sf_count_t
mem_vio_filelen (void *user_data)
{	mem_vio_t *mv = user_data ;
	return mv->length ;
} /* mem_vio_filelen */

static sf_count_t
mem_vio_seek (sf_count_t offset, int whence, void *user_data)
{	mem_vio_t *mv = user_data ;

	switch (whence)
	{	case SEEK_SET :
			mv->position = offset ;
			break ;
		case SEEK_CUR :
			mv->position += offset ;
			break ;
		case SEEK_END :
			mv->position = mv->length + offset ;
			break ;
		default :
			break ;
		} ;
	return mv->position ;
} /* mem_vio_seek */

static sf_count_t
mem_vio_read (void *ptr, sf_count_t count, void *user_data)
{	mem_vio_t *mv = user_data ;

	if (mv->position < 0 || mv->position >= mv->length)
		return 0 ;
	if (mv->position + count > mv->length)
		count = mv->length - mv->position ;
	memcpy (ptr, mv->data + mv->position, (size_t) count) ;
	mv->position += count ;
	return count ;
} /* mem_vio_read */

static sf_count_t
mem_vio_write (const void *ptr, sf_count_t count, void *user_data)
{	(void) ptr ;
	(void) count ;
	(void) user_data ;
	return 0 ;
} /* mem_vio_write */

static sf_count_t
mem_vio_tell (void *user_data)
{	mem_vio_t *mv = user_data ;
	return mv->position ;
} /* mem_vio_tell */

static SNDFILE *
open_virtual (const bytes_t *caf, SF_INFO *info, mem_vio_t *mv, SF_VIRTUAL_IO *vio)
{
	memset (mv, 0, sizeof (*mv)) ;
	mv->data = caf->b ;
	mv->length = caf->len ;

	memset (vio, 0, sizeof (*vio)) ;
	vio->get_filelen = mem_vio_filelen ;
	vio->seek = mem_vio_seek ;
	vio->read = mem_vio_read ;
	vio->write = mem_vio_write ;
	vio->tell = mem_vio_tell ;

	memset (info, 0, sizeof (*info)) ;
	info->seekable = SF_TRUE ;
	return sf_open_virtual (vio, SFM_READ, info, mv) ;
} /* open_virtual */

/*----------------------------------------------------------------------------
** Sample comparison for the four output types.
*/

typedef enum
{	KIND_INT = 0, KIND_SHORT, KIND_FLOAT, KIND_DOUBLE
} output_kind_t ;

/* Arithmetic right shift by 16, without relying on implementation defined shift. */
static int
asr16 (int32_t value)
{	int q = value / 65536 ;
	int r = value % 65536 ;
	if (r != 0 && r < 0)
		q -- ;
	return q ;
} /* asr16 */

static int
check_samples (const scene_t *sc, output_kind_t kind, sf_count_t frame_pos,
		sf_count_t frames, const void *buffer)
{	int ch = sc->channels ;
	sf_count_t i ;
	int k ;

	for (i = 0 ; i < frames ; i ++)
	{	for (k = 0 ; k < ch ; k ++)
		{	size_t idx = (size_t) (frame_pos + i) * ch + k ;
			int32_t ref = sc->pcm [idx] ;

			switch (kind)
			{	case KIND_INT :
				{	const int *p = buffer ;
					TASK_CHECK (p [(size_t) i * ch + k] == ref) ;
					break ;
					} ;
				case KIND_SHORT :
				{	const short *p = buffer ;
					TASK_CHECK ((int) p [(size_t) i * ch + k] == asr16 (ref)) ;
					break ;
					} ;
				case KIND_FLOAT :
				{	const float *p = buffer ;
					float expected = (float) ref / 2147483648.0f ;
					TASK_CHECK (fabsf (p [(size_t) i * ch + k] - expected) <= 1e-6f) ;
					break ;
					} ;
				case KIND_DOUBLE :
				{	const double *p = buffer ;
					double expected = (double) ref / 2147483648.0 ;
					TASK_CHECK (fabs (p [(size_t) i * ch + k] - expected) <= 1e-12) ;
					break ;
					} ;
				} ;
			} ;
		} ;

	return 0 ;
} /* check_samples */

static sf_count_t
readf_frames (SNDFILE *f, output_kind_t kind, void *buffer, sf_count_t frames)
{	switch (kind)
	{	case KIND_INT :	return sf_readf_int (f, buffer, frames) ;
		case KIND_SHORT : return sf_readf_short (f, buffer, frames) ;
		case KIND_FLOAT : return sf_readf_float (f, buffer, frames) ;
		default :		   return sf_readf_double (f, buffer, frames) ;
		} ;
} /* readf_frames */

/*
** Read the whole file back in fixed chunks and compare every sample.
*/
static int
verify_full_read (const scene_t *sc, const bytes_t *caf, const char *path,
		int use_virtual, output_kind_t kind, sf_count_t chunk_frames)
{	SNDFILE		*f ;
	SF_INFO		info ;
	mem_vio_t	mv ;
	SF_VIRTUAL_IO vio ;
	void		*buffer ;
	sf_count_t	done = 0 ;
	size_t		elem_size ;

	if (use_virtual)
		f = open_virtual (caf, &info, &mv, &vio) ;
	else
	{	memset (&info, 0, sizeof (info)) ;
		f = sf_open (path, SFM_READ, &info) ;
		} ;

	TASK_CHECK (f != NULL) ;
	TASK_CHECK (info.frames == (sf_count_t) sc->total_frames) ;
	TASK_CHECK (info.channels == sc->channels) ;
	TASK_CHECK ((info.format & SF_FORMAT_TYPEMASK) == SF_FORMAT_CAF) ;
	if (sc->bits == 16)
		TASK_CHECK ((info.format & SF_FORMAT_SUBMASK) == SF_FORMAT_ALAC_16) ;
	else
		TASK_CHECK ((info.format & SF_FORMAT_SUBMASK) == SF_FORMAT_ALAC_24) ;

	switch (kind)
	{	case KIND_SHORT :	elem_size = sizeof (short) ; break ;
		case KIND_FLOAT :	elem_size = sizeof (float) ; break ;
		case KIND_DOUBLE :	elem_size = sizeof (double) ; break ;
		default :			elem_size = sizeof (int) ; break ;
		} ;

	buffer = malloc ((size_t) (chunk_frames > 16 ? chunk_frames : 16) *
			sc->channels * elem_size) ;
	TASK_CHECK (buffer != NULL) ;

	while (done < sc->total_frames)
	{	sf_count_t want = chunk_frames ;
		sf_count_t got ;

		if (want > sc->total_frames - done)
			want = sc->total_frames - done ;

		got = readf_frames (f, kind, buffer, want) ;
		TASK_CHECK (got == want) ;
		TASK_CHECK (check_samples (sc, kind, done, got, buffer) == 0) ;
		done += got ;
		} ;

	TASK_CHECK (done == sc->total_frames) ;

	/* Clean tail end : one more read returns zero with no error latched. */
	TASK_CHECK (readf_frames (f, kind, buffer, 16) == 0) ;
	TASK_CHECK (sf_error (f) == 0) ;

	free (buffer) ;
	sf_close (f) ;
	return 0 ;
} /* verify_full_read */

/* Seek to pos, read frames samples and compare against the reference axis. */
static int
seek_read_check (SNDFILE *f, const scene_t *sc, output_kind_t kind,
		sf_count_t pos, sf_count_t frames, void *buffer)
{	sf_count_t sought, got ;

	sought = sf_seek (f, pos, SEEK_SET) ;
	TASK_CHECK (sought == pos) ;

	got = readf_frames (f, kind, buffer, frames) ;
	TASK_CHECK (got == frames) ;
	TASK_CHECK (check_samples (sc, kind, pos, frames, buffer) == 0) ;
	return 0 ;
} /* seek_read_check */

static int
verify_seeking (const scene_t *sc, const bytes_t *caf, const char *path, int use_virtual)
{	SNDFILE		*f ;
	SF_INFO		info ;
	mem_vio_t	mv ;
	SF_VIRTUAL_IO vio ;
	int			p, j ;
	void		*buffer ;
	sf_count_t	total = sc->total_frames ;
	output_kind_t kinds [4] = { KIND_INT, KIND_SHORT, KIND_FLOAT, KIND_DOUBLE } ;
	int			ki ;

	if (use_virtual)
		f = open_virtual (caf, &info, &mv, &vio) ;
	else
	{	memset (&info, 0, sizeof (info)) ;
		f = sf_open (path, SFM_READ, &info) ;
		} ;

	TASK_CHECK (f != NULL) ;
	TASK_CHECK (info.frames == total) ;

	buffer = malloc (64 * sc->channels * sizeof (double)) ;
	TASK_CHECK (buffer != NULL) ;

	for (ki = 0 ; ki < 4 ; ki ++)
	{	output_kind_t kind = kinds [ki] ;

		/* Start, plus every packet boundary and two interior points. */
		sf_count_t boundary = 0 ;
		TASK_CHECK (seek_read_check (f, sc, kind, 0, 31, buffer) == 0) ;

		for (p = 0 ; p < sc->packet_count ; p ++)
		{	uint32_t n = sc->counts [p] ;
			sf_count_t inside [] = { 0, 1, (sf_count_t) n / 2, (sf_count_t) n - 1 } ;
			int z ;

			for (z = 0 ; z < 4 ; z ++)
			{	sf_count_t pos = boundary + inside [z] ;
				sf_count_t frames = 17 ;
				if (pos >= total)
					continue ;
				if (frames > total - pos)
					frames = total - pos ;
				if (frames <= 0)
					continue ;
				TASK_CHECK (seek_read_check (f, sc, kind, pos, frames, buffer) == 0) ;
				} ;

			boundary += n ;
			} ;

		/* Pseudo random forward/backward sequence on the same axis. */
		uint32_t rng = 0x1234abcd ;
		for (j = 0 ; j < 24 ; j ++)
		{	sf_count_t pos ;
			sf_count_t frames = 13 ;
			rng = rng_next (&rng) ;
			pos = rng % (uint32_t) total ;
			if (frames > total - pos)
				frames = total - pos ;
			TASK_CHECK (seek_read_check (f, sc, kind, pos, frames, buffer) == 0) ;
			} ;

		/* Relative SEEK_CUR moves composed with partial reads. */
		TASK_CHECK (sf_seek (f, 100, SEEK_SET) == 100) ;
		TASK_CHECK (readf_frames (f, kind, buffer, 10) == 10) ;
		TASK_CHECK (check_samples (sc, kind, 100, 10, buffer) == 0) ;
		TASK_CHECK (sf_seek (f, 37, SEEK_CUR) == 147) ;
		TASK_CHECK (readf_frames (f, kind, buffer, 13) == 13) ;
		TASK_CHECK (check_samples (sc, kind, 147, 13, buffer) == 0) ;
		TASK_CHECK (sf_seek (f, -50, SEEK_CUR) == 110) ;
		TASK_CHECK (readf_frames (f, kind, buffer, 9) == 9) ;
		TASK_CHECK (check_samples (sc, kind, 110, 9, buffer) == 0) ;

		/* Interior seek close to a boundary, then cross it over two reads. */
		{	sf_count_t near = (sf_count_t) sc->counts [0] - 6 ;
			TASK_CHECK (near > 0) ;
			TASK_CHECK (sf_seek (f, near, SEEK_SET) == near) ;
			TASK_CHECK (readf_frames (f, kind, buffer, 10) == 10) ;
			TASK_CHECK (check_samples (sc, kind, near, 10, buffer) == 0) ;
			TASK_CHECK (readf_frames (f, kind, buffer, 25) == 25) ;
			TASK_CHECK (check_samples (sc, kind, near + 10, 25, buffer) == 0) ;
			} ;

		/* Exact tail position is legal and reads zero frames. */
		TASK_CHECK (sf_seek (f, total, SEEK_SET) == total) ;
		TASK_CHECK (readf_frames (f, kind, buffer, 8) == 0) ;
		TASK_CHECK (sf_error (f) == 0) ;

		/* SEEK_END with negative offsets. */
		sf_count_t end_offsets [] = { 0, 1, 2, 7, 64, 311 } ;
		for (j = 0 ; j < (int) (sizeof (end_offsets) / sizeof (end_offsets [0])) ; j ++)
		{	sf_count_t off = end_offsets [j] ;
			sf_count_t pos, frames ;
			if (off > total)
				continue ;
			pos = total - off ;
			TASK_CHECK (sf_seek (f, -off, SEEK_END) == pos) ;
			if (off == 0)
			{	TASK_CHECK (readf_frames (f, kind, buffer, 8) == 0) ;
				continue ;
				} ;
			frames = off < 19 ? off : 19 ;
			TASK_CHECK (check_samples (sc, kind, pos,
					readf_frames (f, kind, buffer, frames), buffer) == 0) ;
			/* The read count itself must match too. */
			TASK_CHECK (sf_seek (f, -off, SEEK_END) == pos) ;
			TASK_CHECK (readf_frames (f, kind, buffer, frames) == frames) ;
			TASK_CHECK (check_samples (sc, kind, pos, frames, buffer) == 0) ;
			} ;

		/* Out of range seeks fail rather than clipping. */
		TASK_CHECK (sf_seek (f, total + 1, SEEK_SET) == -1) ;
		TASK_CHECK (sf_error (f) != 0) ;
		TASK_CHECK (sf_seek (f, -1, SEEK_SET) == -1) ;
		TASK_CHECK (sf_error (f) != 0) ;

		/* A failed seek does not wedge the reader. */
		TASK_CHECK (seek_read_check (f, sc, kind, 0, 40, buffer) == 0) ;
		} ;

	free (buffer) ;
	sf_close (f) ;
	return 0 ;
} /* verify_seeking */

static int
expect_open_failure (const bytes_t *caf)
{	SNDFILE		*f ;
	SF_INFO		info ;
	mem_vio_t	mv ;
	SF_VIRTUAL_IO vio ;

	f = open_virtual (caf, &info, &mv, &vio) ;
	TASK_CHECK (f == NULL) ;
	TASK_CHECK (sf_error (NULL) != 0) ;
	if (f != NULL)
		sf_close (f) ;
	return 0 ;
} /* expect_open_failure */

/*----------------------------------------------------------------------------
** Packets group.
*/

static const uint32_t counts_basic [] = { 127, 1025, 257 } ;
/* All comfortably above the in tree ALAC codec's tiny stereo 24 bit region,
** while still straddling the 127/128, 255/256 and 511/512 vint boundaries. */
static const uint32_t counts_edges [] =
	{ 127, 128, 129, 255, 256, 257, 511, 512, 513, 1025, 301 } ;
static const uint32_t counts_wide [] = { 4096, 1, 128, 127, 257, 4095 } ;
/* Minimum duration packets are exercised on the configurations whose codec
** round trip is exact at those lengths. */
static const uint32_t counts_tiny [] = { 1, 2, 3, 7, 16, 33 } ;

static int
case_packets_int16_mono (void)
{	scene_t sc ;
	bytes_t caf ;
	const char *path ;

	TASK_CHECK (scene_init (&sc, 1, 16, counts_basic, 3, 0, 11) == 0) ;
	TASK_CHECK (build_caf (&sc, NULL, &caf) == 0) ;
	path = write_caf_file (&caf, "i16m") ;
	TASK_CHECK (path != NULL) ;
	TASK_CHECK (verify_full_read (&sc, &caf, path, 0, KIND_INT, 37) == 0) ;
	free (caf.b) ;
	scene_free (&sc) ;
	return 0 ;
} /* case_packets_int16_mono */

static int
case_packets_tiny16_stereo (void)
{	scene_t sc ;
	bytes_t caf ;
	const char *path ;

	TASK_CHECK (scene_init (&sc, 2, 16, counts_tiny, 6, 0, 149) == 0) ;
	TASK_CHECK (build_caf (&sc, NULL, &caf) == 0) ;
	path = write_caf_file (&caf, "tiny16st") ;
	TASK_CHECK (path != NULL) ;
	TASK_CHECK (verify_full_read (&sc, &caf, path, 0, KIND_INT, 5) == 0) ;
	TASK_CHECK (verify_full_read (&sc, &caf, path, 0, KIND_SHORT, 4) == 0) ;
	free (caf.b) ;
	scene_free (&sc) ;
	return 0 ;
} /* case_packets_tiny16_stereo */

static int
case_packets_short24_stereo (void)
{	scene_t sc ;
	bytes_t caf ;
	const char *path ;

	TASK_CHECK (scene_init (&sc, 2, 24, counts_edges, 11, 0, 23) == 0) ;
	TASK_CHECK (build_caf (&sc, NULL, &caf) == 0) ;
	path = write_caf_file (&caf, "s24st") ;
	TASK_CHECK (path != NULL) ;
	TASK_CHECK (verify_full_read (&sc, &caf, path, 0, KIND_SHORT, 31) == 0) ;
	free (caf.b) ;
	scene_free (&sc) ;
	return 0 ;
} /* case_packets_short24_stereo */

static int
case_packets_float16_stereo (void)
{	scene_t sc ;
	bytes_t caf ;
	const char *path ;

	TASK_CHECK (scene_init (&sc, 2, 16, counts_edges, 11, 0, 37) == 0) ;
	TASK_CHECK (build_caf (&sc, NULL, &caf) == 0) ;
	path = write_caf_file (&caf, "f16st") ;
	TASK_CHECK (path != NULL) ;
	TASK_CHECK (verify_full_read (&sc, &caf, path, 0, KIND_FLOAT, 100) == 0) ;
	free (caf.b) ;
	scene_free (&sc) ;
	return 0 ;
} /* case_packets_float16_stereo */

static int
case_packets_double24_mono (void)
{	scene_t sc ;
	bytes_t caf ;
	const char *path ;

	TASK_CHECK (scene_init (&sc, 1, 24, counts_basic, 3, 0, 51) == 0) ;
	TASK_CHECK (build_caf (&sc, NULL, &caf) == 0) ;
	path = write_caf_file (&caf, "d24m") ;
	TASK_CHECK (path != NULL) ;
	TASK_CHECK (verify_full_read (&sc, &caf, path, 0, KIND_DOUBLE, 53) == 0) ;
	free (caf.b) ;
	scene_free (&sc) ;
	return 0 ;
} /* case_packets_double24_mono */

static int
case_packets_wide_vints (void)
{	scene_t sc ;
	bytes_t caf ;
	const char *path ;
	int p, saw_size_width [6] = { 0 }, saw_count_width [6] = { 0 } ;

	/* Wild stereo 16 bit full blocks push a size vint past the 16384
	** boundary ; the 16 bit escape path round trips exactly. */
	TASK_CHECK (scene_init (&sc, 2, 16, counts_wide, 5, 1, 71) == 0) ;
	TASK_CHECK (build_caf (&sc, NULL, &caf) == 0) ;

	/* The fixtures really do cross the one/two/three byte vint boundaries. */
	for (p = 0 ; p < sc.packet_count ; p ++)
	{	unsigned wb = vint_encoded_len (sc.packet_size [p]) ;
		unsigned wf = vint_encoded_len (sc.counts [p]) ;
		TASK_CHECK (wb <= 5 && wf <= 5) ;
		saw_size_width [wb] = 1 ;
		saw_count_width [wf] = 1 ;
		} ;
	TASK_CHECK (saw_size_width [1] && saw_size_width [2] && saw_size_width [3]) ;
	TASK_CHECK (saw_count_width [1] && saw_count_width [2]) ;

	path = write_caf_file (&caf, "wide") ;
	TASK_CHECK (path != NULL) ;
	TASK_CHECK (verify_full_read (&sc, &caf, path, 0, KIND_INT, 211) == 0) ;
	TASK_CHECK (verify_full_read (&sc, &caf, path, 0, KIND_FLOAT, 400) == 0) ;
	TASK_CHECK (verify_full_read (&sc, &caf, path, 1, KIND_INT, 333) == 0) ;
	free (caf.b) ;
	scene_free (&sc) ;
	return 0 ;
} /* case_packets_wide_vints */

static int
case_packets_virtual_io (void)
{	scene_t sc ;
	bytes_t caf ;
	int kind ;

	TASK_CHECK (scene_init (&sc, 2, 16, counts_basic, 3, 0, 97) == 0) ;
	TASK_CHECK (build_caf (&sc, NULL, &caf) == 0) ;

	for (kind = 0 ; kind < 4 ; kind ++)
	{	sf_count_t chunks [4] = { 64, 40, 77, 123 } ;
		TASK_CHECK (verify_full_read (&sc, &caf, NULL, 1,
				(output_kind_t) kind, chunks [kind]) == 0) ;
		} ;

	free (caf.b) ;
	scene_free (&sc) ;
	return 0 ;
} /* case_packets_virtual_io */

/*----------------------------------------------------------------------------
** Seeking group.
*/

static int
case_seeking_file (void)
{	scene_t sc ;
	bytes_t caf ;
	const char *path ;

	TASK_CHECK (scene_init (&sc, 1, 16, counts_basic, 3, 0, 131) == 0) ;
	TASK_CHECK (build_caf (&sc, NULL, &caf) == 0) ;
	path = write_caf_file (&caf, "seekf") ;
	TASK_CHECK (path != NULL) ;
	TASK_CHECK (verify_seeking (&sc, &caf, path, 0) == 0) ;
	free (caf.b) ;
	scene_free (&sc) ;
	return 0 ;
} /* case_seeking_file */

static int
case_seeking_boundaries24 (void)
{	scene_t sc ;
	bytes_t caf ;
	const char *path ;

	TASK_CHECK (scene_init (&sc, 2, 24, counts_edges, 11, 0, 157) == 0) ;
	TASK_CHECK (build_caf (&sc, NULL, &caf) == 0) ;
	path = write_caf_file (&caf, "seek24") ;
	TASK_CHECK (path != NULL) ;
	TASK_CHECK (verify_seeking (&sc, &caf, path, 0) == 0) ;
	free (caf.b) ;
	scene_free (&sc) ;
	return 0 ;
} /* case_seeking_boundaries24 */

static int
case_seeking_virtual_end (void)
{	scene_t sc ;
	bytes_t caf ;

	TASK_CHECK (scene_init (&sc, 1, 24, counts_wide, 6, 0, 173) == 0) ;
	TASK_CHECK (build_caf (&sc, NULL, &caf) == 0) ;
	TASK_CHECK (verify_seeking (&sc, &caf, NULL, 1) == 0) ;
	free (caf.b) ;
	scene_free (&sc) ;
	return 0 ;
} /* case_seeking_virtual_end */

/*----------------------------------------------------------------------------
** Invalid group.
*/

static int
case_invalid_truncated_vint (void)
{	scene_t sc ;
	bytes_t good, bad ;
	unsigned char table [128] ;
	unsigned table_len = 0 ;
	int p ;
	caf_opts_t opts ;

	TASK_CHECK (scene_init (&sc, 1, 16, counts_basic, 3, 0, 181) == 0) ;
	TASK_CHECK (build_caf (&sc, NULL, &good) == 0) ;

	/* Rebuild the well formed table, then leave its last vint dangling. */
	for (p = 0 ; p < sc.packet_count ; p ++)
	{	/* Use a temporary bytes buffer to capture each vint pair. */
		bytes_t tb ;
		memset (&tb, 0, sizeof (tb)) ;
		table_len += put_vint (&tb, sc.packet_size [p]) ;
		table_len += put_vint (&tb, sc.counts [p]) ;
		TASK_CHECK (table_len <= sizeof (table)) ;
		memcpy (table + table_len - tb.len, tb.b, tb.len) ;
		free (tb.b) ;
		} ;
	TASK_CHECK (table_len > 0) ;
	table [table_len - 1] |= 0x80 ;

	caf_opts_default (&opts) ;
	opts.raw_table = table ;
	opts.raw_table_len = table_len ;
	TASK_CHECK (build_caf (&sc, &opts, &bad) == 0) ;
	TASK_CHECK (expect_open_failure (&bad) == 0) ;

	free (bad.b) ;
	free (good.b) ;
	scene_free (&sc) ;
	return 0 ;
} /* case_invalid_truncated_vint */

static int
case_invalid_overflow_vint (void)
{	scene_t sc ;
	bytes_t bad ;
	static const unsigned char table [] =
		{ 0xff, 0xff, 0xff, 0xff, 0x7f, 0x01 } ;
	caf_opts_t opts ;

	TASK_CHECK (scene_init (&sc, 1, 16, counts_basic, 3, 0, 191) == 0) ;

	caf_opts_default (&opts) ;
	opts.set_packets = 1 ;
	opts.packets = 1 ;
	opts.raw_table = table ;
	opts.raw_table_len = sizeof (table) ;
	TASK_CHECK (build_caf (&sc, &opts, &bad) == 0) ;
	TASK_CHECK (expect_open_failure (&bad) == 0) ;

	free (bad.b) ;
	scene_free (&sc) ;
	return 0 ;
} /* case_invalid_overflow_vint */

static int
case_invalid_zero_byte_length (void)
{	scene_t sc ;
	bytes_t bad ;
	caf_opts_t opts ;

	TASK_CHECK (scene_init (&sc, 1, 16, counts_basic, 3, 0, 193) == 0) ;
	caf_opts_default (&opts) ;
	opts.size_override_index = 1 ;
	opts.size_override_value = 0 ;
	TASK_CHECK (build_caf (&sc, &opts, &bad) == 0) ;
	TASK_CHECK (expect_open_failure (&bad) == 0) ;
	free (bad.b) ;
	scene_free (&sc) ;
	return 0 ;
} /* case_invalid_zero_byte_length */

static int
case_invalid_zero_frame_count (void)
{	scene_t sc ;
	bytes_t bad ;
	caf_opts_t opts ;

	TASK_CHECK (scene_init (&sc, 2, 24, counts_edges, 11, 0, 197) == 0) ;
	caf_opts_default (&opts) ;
	opts.frames_override_index = 2 ;
	opts.frames_override_value = 0 ;
	TASK_CHECK (build_caf (&sc, &opts, &bad) == 0) ;
	TASK_CHECK (expect_open_failure (&bad) == 0) ;
	free (bad.b) ;
	scene_free (&sc) ;
	return 0 ;
} /* case_invalid_zero_frame_count */

static int
case_invalid_packet_count_more (void)
{	scene_t sc ;
	bytes_t bad ;
	caf_opts_t opts ;

	TASK_CHECK (scene_init (&sc, 1, 16, counts_basic, 3, 0, 199) == 0) ;
	caf_opts_default (&opts) ;
	opts.set_packets = 1 ;
	opts.packets = (uint64_t) sc.packet_count + 2 ;
	TASK_CHECK (build_caf (&sc, &opts, &bad) == 0) ;
	TASK_CHECK (expect_open_failure (&bad) == 0) ;
	free (bad.b) ;
	scene_free (&sc) ;
	return 0 ;
} /* case_invalid_packet_count_more */

static int
case_invalid_packet_count_less (void)
{	scene_t sc ;
	bytes_t bad ;
	caf_opts_t opts ;

	TASK_CHECK (scene_init (&sc, 1, 24, counts_basic, 3, 0, 211) == 0) ;
	caf_opts_default (&opts) ;
	opts.set_packets = 1 ;
	opts.packets = (uint64_t) sc.packet_count - 1 ;
	TASK_CHECK (build_caf (&sc, &opts, &bad) == 0) ;
	TASK_CHECK (expect_open_failure (&bad) == 0) ;
	free (bad.b) ;
	scene_free (&sc) ;
	return 0 ;
} /* case_invalid_packet_count_less */

static int
case_invalid_valid_frames_mismatch (void)
{	scene_t sc ;
	bytes_t bad ;
	caf_opts_t opts ;

	TASK_CHECK (scene_init (&sc, 1, 16, counts_basic, 3, 0, 223) == 0) ;
	caf_opts_default (&opts) ;
	opts.set_valid = 1 ;
	opts.valid_frames = sc.total_frames + 7 ;
	TASK_CHECK (build_caf (&sc, &opts, &bad) == 0) ;
	TASK_CHECK (expect_open_failure (&bad) == 0) ;
	free (bad.b) ;
	scene_free (&sc) ;
	return 0 ;
} /* case_invalid_valid_frames_mismatch */

static int
case_invalid_cumulative_frames (void)
{	scene_t sc ;
	bytes_t bad ;
	caf_opts_t opts ;

	TASK_CHECK (scene_init (&sc, 2, 16, counts_basic, 3, 0, 227) == 0) ;
	caf_opts_default (&opts) ;
	/* Entry says two extra frames but the pakt header keeps the old total. */
	opts.frames_override_index = 0 ;
	opts.frames_override_value = sc.counts [0] + 2 ;
	TASK_CHECK (build_caf (&sc, &opts, &bad) == 0) ;
	TASK_CHECK (expect_open_failure (&bad) == 0) ;
	free (bad.b) ;
	scene_free (&sc) ;
	return 0 ;
} /* case_invalid_cumulative_frames */

static int
case_invalid_byte_sum (void)
{	scene_t sc ;
	bytes_t bad ;
	caf_opts_t opts ;

	TASK_CHECK (scene_init (&sc, 1, 24, counts_basic, 3, 0, 229) == 0) ;
	caf_opts_default (&opts) ;
	opts.size_override_index = 1 ;
	opts.size_override_value = sc.packet_size [1] + 37 ;
	TASK_CHECK (build_caf (&sc, &opts, &bad) == 0) ;
	TASK_CHECK (expect_open_failure (&bad) == 0) ;
	free (bad.b) ;
	scene_free (&sc) ;
	return 0 ;
} /* case_invalid_byte_sum */

/*
** Metadata is internally consistent (pakt valid frames follows the lie), but
** the declared duration of one packet disagrees with the ALAC decoder output.
** Open succeeds ; the bad packet is withheld with an observable sf_error and
** the correctly decoded prefix survives sample for sample.
*/
static int
case_invalid_decode_output_mismatch (void)
{	scene_t sc ;
	bytes_t bad ;
	caf_opts_t opts ;
	SNDFILE *f ;
	SF_INFO info ;
	mem_vio_t mv ;
	SF_VIRTUAL_IO vio ;
	int bad_packet = 1 ;
	sf_count_t bad_start = 0, p, total_declared ;
	int *buffer ;
	sf_count_t done = 0 ;
	int error_seen = 0 ;

	TASK_CHECK (scene_init (&sc, 1, 16, counts_basic, 3, 0, 233) == 0) ;

	for (p = 0 ; p < bad_packet ; p ++)
		bad_start += sc.counts [p] ;

	caf_opts_default (&opts) ;
	opts.frames_override_index = bad_packet ;
	opts.frames_override_value = sc.counts [bad_packet] + 1 ;
	opts.set_valid = 1 ;
	total_declared = sc.total_frames + 1 ;
	opts.valid_frames = (uint64_t) total_declared ;
	TASK_CHECK (build_caf (&sc, &opts, &bad) == 0) ;

	f = open_virtual (&bad, &info, &mv, &vio) ;
	TASK_CHECK (f != NULL) ;
	TASK_CHECK (info.frames == total_declared) ;

	buffer = malloc ((size_t) bad_start * sc.channels * sizeof (int)) ;
	TASK_CHECK (buffer != NULL) ;

	/* Read in 50 frame chunks until the failure surfaces. */
	while (done < total_declared && sf_error (f) == 0)
	{	sf_count_t got = sf_readf_int (f, buffer, 50) ;
		sf_count_t comparable = got ;

		if (done + comparable > bad_start)
			comparable = bad_start > done ? bad_start - done : 0 ;

		TASK_CHECK (check_samples (&sc, KIND_INT, done, comparable, buffer) == 0) ;
		done += got ;

		if (sf_error (f) != 0)
		{	error_seen = 1 ;
			break ;
			} ;
		if (got == 0)
			break ;
		} ;

	TASK_CHECK (error_seen) ;
	TASK_CHECK (sf_error (f) != 0) ;
	/* Exactly the prefix before the bad packet may have been published. */
	TASK_CHECK (done == bad_start) ;

	/* Repeated reads keep reporting the failure, no silence is inserted. */
	TASK_CHECK (sf_readf_int (f, buffer, 50) == 0) ;
	TASK_CHECK (sf_error (f) != 0) ;

	/* Seeking back to the start gives the good prefix again. */
	TASK_CHECK (sf_seek (f, 0, SEEK_SET) == 0) ;
	TASK_CHECK (sf_readf_int (f, buffer, (sf_count_t) bad_start) == bad_start) ;
	TASK_CHECK (check_samples (&sc, KIND_INT, 0, bad_start, buffer) == 0) ;

	free (buffer) ;
	sf_close (f) ;
	free (bad.b) ;
	scene_free (&sc) ;
	return 0 ;
} /* case_invalid_decode_output_mismatch */

/*----------------------------------------------------------------------------
** Registration.
*/

const struct task_case task_cases [] =
{
	{ "packets", "int16_mono",				case_packets_int16_mono },
	{ "packets", "tiny16_stereo",			case_packets_tiny16_stereo },
	{ "packets", "short24_stereo",			case_packets_short24_stereo },
	{ "packets", "float16_stereo",			case_packets_float16_stereo },
	{ "packets", "double24_mono",			case_packets_double24_mono },
	{ "packets", "wide_vints",				case_packets_wide_vints },
	{ "packets", "virtual_io",				case_packets_virtual_io },

	{ "seeking", "file_scripted",			case_seeking_file },
	{ "seeking", "boundaries24_all_kinds",	case_seeking_boundaries24 },
	{ "seeking", "virtual_end",				case_seeking_virtual_end },

	{ "invalid", "truncated_vint",			case_invalid_truncated_vint },
	{ "invalid", "overflow_vint",			case_invalid_overflow_vint },
	{ "invalid", "zero_byte_length",		case_invalid_zero_byte_length },
	{ "invalid", "zero_frame_count",		case_invalid_zero_frame_count },
	{ "invalid", "packet_count_more",		case_invalid_packet_count_more },
	{ "invalid", "packet_count_less",		case_invalid_packet_count_less },
	{ "invalid", "valid_frames_mismatch",	case_invalid_valid_frames_mismatch },
	{ "invalid", "cumulative_frames",		case_invalid_cumulative_frames },
	{ "invalid", "byte_sum",				case_invalid_byte_sum },
	{ "invalid", "decode_output_mismatch",	case_invalid_decode_output_mismatch },
} ;

const size_t task_case_count = sizeof (task_cases) / sizeof (task_cases [0]) ;

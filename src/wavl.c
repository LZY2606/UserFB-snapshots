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
** Read-only LIST/wavl segment management for IMA-ADPCM WAVE files.
** See wavl.h for the supported layout and the design constraints.
*/

#include "sfconfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "sndfile.h"
#include "sfendian.h"
#include "common.h"
#include "wavlike.h"
#include "wavl.h"

#define WAVL_TAG_DATA		MAKE_MARKER ('d', 'a', 't', 'a')
#define WAVL_TAG_SLNT		MAKE_MARKER ('s', 'l', 'n', 't')
#define WAVL_TAG_LIST		MAKE_MARKER ('L', 'I', 'S', 'T')

/*
** Skip count bytes of subchunk payload. Parsing is strictly sequential, so
** the binheader cache is always fully consumed at a skip point. Small
** skips go through the binheader machinery; anything larger than INT_MAX
** (only plausible for an unknown chunk) seeks the underlying IO directly.
** The caller verifies the resulting file position afterwards.
*/
static void
wavl_skip_bytes (SF_PRIVATE *psf, sf_count_t count)
{	if (count <= 0)
		return ;

	if (count <= 0x7fffffff)
	{	psf_binheader_readf (psf, "j", (int) count) ;
		return ;
		} ;

	/* Drop the binheader cache so the underlying position is current. */
	psf_binheader_readf (psf, "!") ;
	psf_fseek (psf, count, SEEK_CUR) ;
} /* wavl_skip_bytes */

static WAVL_PRIVATE *
wavl_alloc (int channels, int blockalign, int samplesperblock, sf_count_t list_start, sf_count_t list_end)
{	WAVL_PRIVATE *wavl ;

	if ((wavl = calloc (1, sizeof (WAVL_PRIVATE))) == NULL)
		return NULL ;

	wavl->channels = channels ;
	wavl->blockalign = blockalign ;
	wavl->samplesperblock = samplesperblock ;
	wavl->list_start = list_start ;
	wavl->list_end = list_end ;
	wavl->cache_seg = -1 ;
	wavl->cache_phys = -1 ;

	/* Allocated up front so header scanning reads real bytes (a seek
	** skip would hide truncation past the last cached byte).
	*/
	if ((wavl->block = malloc ((size_t) blockalign)) == NULL)
	{	free (wavl) ;
		return NULL ;
		} ;

	return wavl ;
} /* wavl_alloc */

void
wavl_free (WAVL_PRIVATE *wavl)
{	if (wavl == NULL)
		return ;

	free (wavl->segs) ;
	free (wavl->block) ;
	free (wavl->samples) ;
	free (wavl) ;
} /* wavl_free */

static int
wavl_add_segment (WAVL_PRIVATE *wavl, int kind)
{	WAVL_SEGMENT *grown ;

	if (wavl->seg_count == wavl->seg_cap)
	{	int newcap = wavl->seg_cap ? wavl->seg_cap * 2 : 16 ;

		if (newcap <= wavl->seg_cap)
			return SFE_WAV_BAD_LIST ;

		grown = realloc (wavl->segs, (size_t) newcap * sizeof (WAVL_SEGMENT)) ;
		if (grown == NULL)
			return SFE_MALLOC_FAILED ;

		wavl->segs = grown ;
		wavl->seg_cap = newcap ;
		} ;

	memset (&(wavl->segs [wavl->seg_count]), 0, sizeof (WAVL_SEGMENT)) ;
	wavl->segs [wavl->seg_count].kind = kind ;
	wavl->segs [wavl->seg_count].prev_data = -1 ;
	wavl->seg_count ++ ;

	return 0 ;
} /* wavl_add_segment */

/*
** The WAV IMA block decoder is provided by ima_adpcm.c so that ordinary
** and segmented files use byte-identical decode math.
*/

int
wavl_parse_list (SF_PRIVATE *psf, WAVL_PRIVATE **pwavl, uint32_t list_chunk_size, sf_count_t form_tag_pos)
{	WAVL_PRIVATE		*wavl ;
	WAVLIKE_PRIVATE		*wpriv ;
	sf_count_t			body_start, body_end, cursor, filelength ;
	sf_count_t			logical = 0 ;
	int					last_data = -1, error ;

	if (pwavl == NULL || *pwavl != NULL)
		return SFE_INTERNAL ;

	if ((wpriv = psf->container_data) == NULL)
		return SFE_INTERNAL ;

	if (psf->rwf_endian != SF_ENDIAN_LITTLE || ! psf->sf.seekable || psf->is_pipe)
		return SFE_WAV_BAD_LIST ;

	if (wpriv->wav_fmt.ima.blockalign <= 0 || wpriv->wav_fmt.ima.samplesperblock <= 0)
		return SFE_WAV_BAD_LIST ;

	if (list_chunk_size < 4)
		return SFE_WAV_BAD_LIST ;

	/* form_tag_pos is the offset of the four byte 'wavl' form tag, the
	** first bytes of the LIST payload; the tag itself has already been
	** read on entry, so parsing starts at form_tag_pos + 4.
	*/
	body_start = form_tag_pos + 4 ;
	body_end = form_tag_pos + (sf_count_t) list_chunk_size ;
	filelength = psf->filelength ;

	if (body_end > filelength || body_start > body_end)
		return SFE_WAV_BAD_LIST ;

	wavl = wavl_alloc (psf->sf.channels, wpriv->wav_fmt.ima.blockalign,
					wpriv->wav_fmt.ima.samplesperblock, body_start, body_end) ;
	if (wavl == NULL)
		return SFE_MALLOC_FAILED ;

	cursor = body_start ;

	while (cursor < body_end)
	{	uint32_t marker = 0, chunk_size = 0 ;
		sf_count_t payload_start, next ;
		int bytesread ;

		/* Every subchunk needs its full 8 byte header. */
		if (body_end - cursor < 8)
		{	error = SFE_WAV_BAD_LIST ;
			goto fail ;
			} ;

		if (psf_ftell (psf) != cursor)
		{	error = SFE_WAV_BAD_LIST ;
			goto fail ;
			} ;

		bytesread = psf_binheader_readf (psf, "m4", &marker, &chunk_size) ;
		if (bytesread != 8)
		{	error = SFE_BAD_FILE_READ ;
			goto fail ;
			} ;

		payload_start = cursor + 8 ;
		next = payload_start + (sf_count_t) chunk_size + (chunk_size & 1) ;

		if (next > body_end || payload_start + (sf_count_t) chunk_size > filelength)
		{	error = SFE_WAV_BAD_LIST ;
			goto fail ;
			} ;

		switch (marker)
		{	case WAVL_TAG_DATA :
			{	sf_count_t nblocks, b ;

				if (chunk_size == 0)
				{	/* Empty data: no segment, no hold reset. */
					break ;
					} ;

				if (((sf_count_t) chunk_size) % wavl->blockalign != 0)
				{	error = SFE_WAV_BAD_LIST ;
					goto fail ;
					} ;

				nblocks = (sf_count_t) chunk_size / wavl->blockalign ;
				if (nblocks > (sf_count_t) (INT_MAX - wavl->block_count))
				{	error = SFE_WAV_BAD_LIST ;
					goto fail ;
					} ;

				/* Scan the block headers only; every block byte is read
				** for real so truncation cannot hide behind a seek.
				*/
				for (b = 0 ; b < nblocks ; b++)
				{	int chan ;

					if (psf_binheader_readf (psf, "b", wavl->block, wavl->blockalign) != wavl->blockalign)
					{	error = SFE_BAD_FILE_READ ;
						goto fail ;
						} ;

					for (chan = 0 ; chan < wavl->channels ; chan++)
					{	if (wavl->block [chan * 4 + 2] > 88 || wavl->block [chan * 4 + 3] != 0)
						{	error = SFE_WAV_BAD_LIST ;
							goto fail ;
							} ;
						} ;
					} ;

				if ((error = wavl_add_segment (wavl, WAVL_SEG_DATA)) != 0)
					goto fail ;

				{	WAVL_SEGMENT *seg = &(wavl->segs [wavl->seg_count - 1]) ;
					sf_count_t frames = nblocks * wavl->samplesperblock ;

					if (frames > SF_COUNT_MAX - logical)
					{	error = SFE_WAV_BAD_LIST ;
						goto fail ;
						} ;

					seg->offset = payload_start ;
					seg->length = chunk_size ;
					seg->start = logical ;
					seg->end = logical + frames ;
					seg->first_block = wavl->block_count ;
					seg->last_block = wavl->block_count + (int) nblocks - 1 ;
					seg->prev_data = last_data ;

					wavl->block_count += (int) nblocks ;
					wavl->data_frames += frames ;
					logical += frames ;
					last_data = wavl->seg_count - 1 ;
					} ;
				break ;
				} ;

			case WAVL_TAG_SLNT :
			{	uint32_t count = 0 ;

				if (chunk_size != 4)
				{	error = SFE_WAV_BAD_LIST ;
					goto fail ;
					} ;

				if (psf_binheader_readf (psf, "4", &count) != 4)
				{	error = SFE_BAD_FILE_READ ;
					goto fail ;
					} ;

				if (count == 0)
				{	/* Zero length slnt: no segment, hold value untouched. */
					break ;
					} ;

				if ((sf_count_t) count > SF_COUNT_MAX - logical)
				{	error = SFE_WAV_BAD_LIST ;
					goto fail ;
					} ;

				if ((error = wavl_add_segment (wavl, WAVL_SEG_SLNT)) != 0)
					goto fail ;

				{	WAVL_SEGMENT *seg = &(wavl->segs [wavl->seg_count - 1]) ;

					seg->frames = (sf_count_t) count ;
					seg->start = logical ;
					seg->end = logical + (sf_count_t) count ;
					seg->prev_data = last_data ;

					wavl->slnt_frames += count ;
					logical += count ;
					} ;
				break ;
				} ;

			case WAVL_TAG_LIST :
				/* Nested lists (including nested wavl) are not supported. */
				error = SFE_WAV_BAD_LIST ;
				goto fail ;

			default :
			{	sf_count_t skip = (sf_count_t) chunk_size + (chunk_size & 1) ;

				wavl_skip_bytes (psf, skip) ;
				break ;
				} ;
			} ;

		/* Every handler must leave the file position exactly at the first
		** byte after the (possibly padded) subchunk; metadata must never be
		** fed to the ADPCM decoder.
		*/
		if (psf_ftell (psf) != next)
		{	error = SFE_WAV_BAD_LIST ;
			goto fail ;
			} ;

		cursor = next ;
		} ;

	if (cursor != body_end)
	{	error = SFE_WAV_BAD_LIST ;
		goto fail ;
		} ;

	wavl->total_frames = logical ;
	*pwavl = wavl ;

	return 0 ;

fail :
	wavl_free (wavl) ;
	return error ;
} /* wavl_parse_list */

int
wavl_finalize (SF_PRIVATE *psf, WAVL_PRIVATE *wavl, uint32_t fact_frames, int have_fact)
{	WAVLIKE_PRIVATE	*wpriv ;
	int				spb, ch ;

	if (wavl == NULL || (wpriv = psf->container_data) == NULL)
		return SFE_INTERNAL ;

	spb = wavl->samplesperblock ;
	ch = wavl->channels ;

	if (spb <= 0 || ch <= 0 || wavl->blockalign <= 0)
		return SFE_WAV_BAD_LIST ;

	/* The WAV IMA layout must match the standard complete-block geometry
	** the shared decoder expects: one 4-byte header per channel plus
	** nibble-packed samples giving spb samples per block.
	*/
	if (spb != 2 * (wavl->blockalign - 4 * ch) / ch + 1)
		return SFE_WAV_ADPCM_SAMPLES ;

	if (wavl->total_frames < 0 || wavl->data_frames < 0 || wavl->slnt_frames < 0)
		return SFE_WAV_BAD_LIST ;

	if (have_fact && (sf_count_t) fact_frames != wavl->total_frames)
		return SFE_WAV_BAD_FACT ;

	if (wavl->total_frames > (SF_COUNT_MAX - spb + 1) / spb)
		return SFE_WAV_BAD_LIST ;

	if (wavl->block == NULL && (wavl->block = malloc ((size_t) wavl->blockalign)) == NULL)
		return SFE_MALLOC_FAILED ;

	if ((wavl->samples = calloc ((size_t) spb * (size_t) ch, sizeof (short))) == NULL)
		return SFE_MALLOC_FAILED ;

	/* Publish the expanded axis. dataoffset/datalength only have to stay
	** non-negative for the generic validation; all audio IO goes through
	** the segment directory.
	*/
	psf->sf.frames = wavl->total_frames ;
	psf->dataend = 0 ;
	if (wavl->block_count > 0)
	{	int k ;
		psf->dataoffset = wavl->segs [0].offset ;
		psf->datalength = 0 ;
		for (k = 0 ; k < wavl->seg_count ; k++)
			if (wavl->segs [k].kind == WAVL_SEG_DATA)
				psf->datalength += wavl->segs [k].length ;
		}
	else
	{	psf->dataoffset = wavl->list_start ;
		psf->datalength = 0 ;
		} ;

	wavl->cache_seg = -1 ;
	wavl->cache_phys = -1 ;
	wpriv->wavl_data = wavl ;

	return 0 ;
} /* wavl_finalize */

int
wavl_active (SF_PRIVATE *psf)
{	WAVLIKE_PRIVATE *wpriv ;

	if (psf == NULL || (wpriv = psf->container_data) == NULL)
		return 0 ;

	return wpriv->wavl_data != NULL ;
} /* wavl_active */

/*==============================================================================
** Physical block loading and logical block materialisation.
*/

/* Largest segment index whose start is <= frame. All frames handed here
** are below total_frames, so a segment always exists.
*/
static int
wavl_find_segment (const WAVL_PRIVATE *wavl, sf_count_t frame)
{	int lo = 0, hi = wavl->seg_count - 1 ;

	while (lo < hi)
	{	int mid = lo + (hi - lo + 1) / 2 ;

		if (wavl->segs [mid].start <= frame)
			lo = mid ;
		else
			hi = mid - 1 ;
		} ;

	return lo ;
} /* wavl_find_segment */

/* Decode physical block phys of data segment segidx into wavl->samples,
** reusing the cache when the requested block is already loaded. Every
** decode starts from the block header, so it never depends on history.
*/
static void
wavl_load_physical (SF_PRIVATE *psf, WAVL_PRIVATE *wavl, int segidx, int phys)
{	const WAVL_SEGMENT *seg ;
	sf_count_t offset ;
	sf_count_t got ;

	if (wavl->cache_seg == segidx && wavl->cache_phys == phys)
		return ;

	seg = &(wavl->segs [segidx]) ;
	offset = seg->offset + (sf_count_t) phys * wavl->blockalign ;

	if (offset + wavl->blockalign > seg->offset + seg->length)
	{	psf->error = SFE_BAD_FILE_READ ;
		memset (wavl->samples, 0, (size_t) wavl->samplesperblock * wavl->channels * sizeof (short)) ;
		return ;
		} ;

	if (psf_fseek (psf, offset, SEEK_SET) != offset)
	{	psf->error = SFE_SEEK_FAILED ;
		memset (wavl->samples, 0, (size_t) wavl->samplesperblock * wavl->channels * sizeof (short)) ;
		return ;
		} ;

	got = psf_fread (wavl->block, 1, wavl->blockalign, psf) ;
	if (got != wavl->blockalign)
	{	psf->error = SFE_BAD_FILE_READ ;
		memset (wavl->samples, 0, (size_t) wavl->samplesperblock * wavl->channels * sizeof (short)) ;
		return ;
		} ;

	wavlike_ima_decode_block_data (wavl->block, wavl->samples, wavl->channels,
		wavl->blockalign, wavl->samplesperblock) ;

	wavl->cache_seg = segidx ;
	wavl->cache_phys = phys ;
} /* wavl_load_physical */

void
wavl_fill_block (SF_PRIVATE *psf, int logical_block, short *out)
{	WAVLIKE_PRIVATE	*wpriv = psf->container_data ;
	WAVL_PRIVATE	*wavl = wpriv->wavl_data ;
	int				spb = wavl->samplesperblock ;
	int				ch = wavl->channels ;
	sf_count_t		lo = (sf_count_t) logical_block * spb ;
	sf_count_t		hi = lo + spb ;
	sf_count_t		frame ;

	memset (out, 0, (size_t) spb * (size_t) ch * sizeof (short)) ;

	if (lo >= wavl->total_frames)
		return ;

	if (hi > wavl->total_frames)
		hi = wavl->total_frames ;

	for (frame = lo ; frame < hi ; frame++)
	{	int segidx = wavl_find_segment (wavl, frame) ;
		const WAVL_SEGMENT *seg = &(wavl->segs [segidx]) ;
		int outidx = (int) (frame - lo) * ch ;

		if (seg->kind == WAVL_SEG_DATA)
		{	sf_count_t rel = frame - seg->start ;
			int phys = (int) (rel / spb) ;
			int inblk = (int) (rel % spb) ;

			wavl_load_physical (psf, wavl, segidx, phys) ;
			memcpy (out + outidx, wavl->samples + inblk * ch, (size_t) ch * sizeof (short)) ;
			}
		else
		{	int pred = seg->prev_data ;

			if (pred >= 0)
			{	const WAVL_SEGMENT *prev = &(wavl->segs [pred]) ;
				int last_phys = prev->last_block - prev->first_block ;

				/* Recover the real final frame from the compressed block;
				** the block header predictor is not used as the hold value.
				*/
				wavl_load_physical (psf, wavl, pred, last_phys) ;
				memcpy (out + outidx, wavl->samples + (spb - 1) * ch, (size_t) ch * sizeof (short)) ;
				} ;
			/* No preceding audio: leave the zeroed frame in place. */
			} ;
		} ;
} /* wavl_fill_block */

/*==============================================================================
** Codec wrapper. It presents the expanded frame axis through the ordinary
** libsndfile read hooks, one logical block at a time. The compressed block
** workspace stays one block large; no segment is ever expanded to PCM.
*/

typedef struct
{	WAVL_PRIVATE	*wavl ;

	short			*blockbuf ;
	int				block_loaded ;
	int				block_idx ;
	sf_count_t		pos ;
} WAVL_CODEC ;

static WAVL_CODEC *
wavl_codec_get (SF_PRIVATE *psf)
{	WAVLIKE_PRIVATE *wpriv = psf->container_data ;

	if (wpriv == NULL || wpriv->wavl_data == NULL)
		return NULL ;

	return (WAVL_CODEC *) psf->codec_data ;
} /* wavl_codec_get */

/* Ensure the logical block covering c->pos is decoded into blockbuf. */
static int
wavl_ensure_block (SF_PRIVATE *psf, WAVL_CODEC *c)
{	WAVL_PRIVATE *wavl = c->wavl ;
	int block_idx = (int) (c->pos / wavl->samplesperblock) ;

	if (c->block_loaded && c->block_idx == block_idx)
		return 1 ;

	wavl_fill_block (psf, block_idx, c->blockbuf) ;
	if (psf->error)
		return 0 ;

	c->block_idx = block_idx ;
	c->block_loaded = 1 ;

	return 1 ;
} /* wavl_ensure_block */

static sf_count_t
wavl_read_short (SF_PRIVATE *psf, short *ptr, sf_count_t len)
{	WAVL_CODEC	*c ;
	WAVL_PRIVATE *wavl ;
	sf_count_t	total = 0 ;

	if ((c = wavl_codec_get (psf)) == NULL)
		return 0 ;
	wavl = c->wavl ;

	while (total < len)
	{	int ch = wavl->channels ;
		int spb = wavl->samplesperblock ;
		int inblk, take, nframes ;

		if (c->pos >= wavl->total_frames)
			break ;

		if (! wavl_ensure_block (psf, c))
			break ;

		inblk = (int) (c->pos % spb) ;
		nframes = spb - inblk ;
		if ((sf_count_t) nframes > wavl->total_frames - c->pos)
			nframes = (int) (wavl->total_frames - c->pos) ;
		if ((sf_count_t) nframes > (len - total) / ch)
			nframes = (int) ((len - total) / ch) ;

		take = nframes * ch ;
		memcpy (ptr + total, c->blockbuf + inblk * ch, (size_t) take * sizeof (short)) ;

		total += take ;
		c->pos += nframes ;
		} ;

	return total ;
} /* wavl_read_short */

static sf_count_t
wavl_read_int (SF_PRIVATE *psf, int *ptr, sf_count_t len)
{	WAVL_CODEC	*c ;
	WAVL_PRIVATE *wavl ;
	sf_count_t	total = 0 ;

	if ((c = wavl_codec_get (psf)) == NULL)
		return 0 ;
	wavl = c->wavl ;

	while (total < len)
	{	int ch = wavl->channels ;
		int spb = wavl->samplesperblock ;
		int inblk, nframes, k ;

		if (c->pos >= wavl->total_frames)
			break ;

		if (! wavl_ensure_block (psf, c))
			break ;

		inblk = (int) (c->pos % spb) ;
		nframes = spb - inblk ;
		if ((sf_count_t) nframes > wavl->total_frames - c->pos)
			nframes = (int) (wavl->total_frames - c->pos) ;
		if ((sf_count_t) nframes > (len - total) / ch)
			nframes = (int) ((len - total) / ch) ;

		for (k = 0 ; k < nframes * ch ; k++)
			ptr [total + k] = arith_shift_left (c->blockbuf [inblk * ch + k], 16) ;

		total += nframes * ch ;
		c->pos += nframes ;
		} ;

	return total ;
} /* wavl_read_int */

static sf_count_t
wavl_read_float (SF_PRIVATE *psf, float *ptr, sf_count_t len)
{	WAVL_CODEC	*c ;
	WAVL_PRIVATE *wavl ;
	sf_count_t	total = 0 ;
	float		normfact ;

	if ((c = wavl_codec_get (psf)) == NULL)
		return 0 ;
	wavl = c->wavl ;

	normfact = (psf->norm_float == SF_TRUE) ? 1.0f / ((float) 0x8000) : 1.0f ;

	while (total < len)
	{	int ch = wavl->channels ;
		int spb = wavl->samplesperblock ;
		int inblk, nframes, k ;

		if (c->pos >= wavl->total_frames)
			break ;

		if (! wavl_ensure_block (psf, c))
			break ;

		inblk = (int) (c->pos % spb) ;
		nframes = spb - inblk ;
		if ((sf_count_t) nframes > wavl->total_frames - c->pos)
			nframes = (int) (wavl->total_frames - c->pos) ;
		if ((sf_count_t) nframes > (len - total) / ch)
			nframes = (int) ((len - total) / ch) ;

		for (k = 0 ; k < nframes * ch ; k++)
			ptr [total + k] = normfact * (float) (c->blockbuf [inblk * ch + k]) ;

		total += nframes * ch ;
		c->pos += nframes ;
		} ;

	return total ;
} /* wavl_read_float */

static sf_count_t
wavl_read_double (SF_PRIVATE *psf, double *ptr, sf_count_t len)
{	WAVL_CODEC	*c ;
	WAVL_PRIVATE *wavl ;
	sf_count_t	total = 0 ;
	double		normfact ;

	if ((c = wavl_codec_get (psf)) == NULL)
		return 0 ;
	wavl = c->wavl ;

	normfact = (psf->norm_double == SF_TRUE) ? 1.0 / ((double) 0x8000) : 1.0 ;

	while (total < len)
	{	int ch = wavl->channels ;
		int spb = wavl->samplesperblock ;
		int inblk, nframes, k ;

		if (c->pos >= wavl->total_frames)
			break ;

		if (! wavl_ensure_block (psf, c))
			break ;

		inblk = (int) (c->pos % spb) ;
		nframes = spb - inblk ;
		if ((sf_count_t) nframes > wavl->total_frames - c->pos)
			nframes = (int) (wavl->total_frames - c->pos) ;
		if ((sf_count_t) nframes > (len - total) / ch)
			nframes = (int) ((len - total) / ch) ;

		for (k = 0 ; k < nframes * ch ; k++)
			ptr [total + k] = normfact * (double) (c->blockbuf [inblk * ch + k]) ;

		total += nframes * ch ;
		c->pos += nframes ;
		} ;

	return total ;
} /* wavl_read_double */

static sf_count_t
wavl_seek_hook (SF_PRIVATE *psf, int mode, sf_count_t offset)
{	WAVL_CODEC	*c ;
	WAVL_PRIVATE *wavl ;

	if (mode != SFM_READ || (c = wavl_codec_get (psf)) == NULL)
	{	psf->error = SFE_BAD_SEEK ;
		return PSF_SEEK_ERROR ;
		} ;
	wavl = c->wavl ;

	if (offset < 0 || offset > wavl->total_frames)
	{	psf->error = SFE_BAD_SEEK ;
		return PSF_SEEK_ERROR ;
		} ;

	c->pos = offset ;
	c->block_loaded = 0 ;

	return offset ;
} /* wavl_seek_hook */

static int
wavl_codec_close (SF_PRIVATE *psf)
{	WAVL_CODEC *c = (WAVL_CODEC *) psf->codec_data ;

	if (c == NULL)
		return 0 ;

	free (c->blockbuf) ;
	free (c) ;
	psf->codec_data = NULL ;

	return 0 ;
} /* wavl_codec_close */

int
wavl_codec_init (SF_PRIVATE *psf)
{	WAVLIKE_PRIVATE	*wpriv = psf->container_data ;
	WAVL_PRIVATE	*wavl ;
	WAVL_CODEC		*c ;
	int				spbch ;

	if (wpriv == NULL || (wavl = wpriv->wavl_data) == NULL)
		return SFE_INTERNAL ;

	if (psf->codec_data != NULL)
		return SFE_INTERNAL ;

	spbch = wavl->samplesperblock * wavl->channels ;

	if ((c = calloc (1, sizeof (WAVL_CODEC))) == NULL)
		return SFE_MALLOC_FAILED ;

	if ((c->blockbuf = malloc ((size_t) spbch * sizeof (short))) == NULL)
	{	free (c) ;
		return SFE_MALLOC_FAILED ;
		} ;

	c->wavl = wavl ;
	c->block_loaded = 0 ;
	c->block_idx = -1 ;
	c->pos = 0 ;

	/* The expanded (data + slnt) frame axis was published in
	** wavl_finalize(); no audio is decoded here.
	*/
	psf->sf.frames = wavl->total_frames ;

	/* Install the read/seek/close hooks. */
	psf->codec_data = c ;
	psf->codec_close = wavl_codec_close ;
	psf->seek = wavl_seek_hook ;
	psf->read_short = wavl_read_short ;
	psf->read_int = wavl_read_int ;
	psf->read_float = wavl_read_float ;
	psf->read_double = wavl_read_double ;

	return 0 ;
} /* wavl_codec_init */

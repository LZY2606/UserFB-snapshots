/*
** Copyright (C) 2026 Erik de Castro Lopo <erikd@mega-nerd.com>
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
*/

/*
** CAF channel layout ('chan' chunk) tests.
**
** Hand crafted bitmap and channel-description files are fed through the
** public sf_open () / sf_open_virtual () read interface. The write side is
** verified by independently walking the big-endian chunks of the produced
** file: chunk lengths, on-disk description order and the raw audio bytes.
**
** No audio is ever reordered, mixed or resampled; the PCM bytes on disk must
** match the interleaved bytes handed to the write calls.
*/

#include "check.h"

#include <sndfile.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/*------------------------------------------------------------------------------
** Big-endian byte buffer builders.
*/

typedef struct
{	unsigned char	data [64 * 1024] ;
	size_t			len ;
} BYTE_BUF ;

static void
put_bytes (BYTE_BUF * buf, const void * src, size_t n)
{	TASK_CHECK (buf->len + n <= sizeof (buf->data)) ;
	memcpy (buf->data + buf->len, src, n) ;
	buf->len += n ;
} /* put_bytes */

static void
put_be (BYTE_BUF * buf, uint64_t value, int nbytes)
{	unsigned char tmp [8] ;
	int k ;

	for (k = 0 ; k < nbytes ; k++)
		tmp [nbytes - 1 - k] = (unsigned char) ((value >> (8 * k)) & 0xff) ;

	put_bytes (buf, tmp, (size_t) nbytes) ;
} /* put_be */

static void
put_be32 (BYTE_BUF * buf, uint32_t value)
{	put_be (buf, value, 4) ;
} /* put_be32 */

static void
put_be16 (BYTE_BUF * buf, uint16_t value)
{	put_be (buf, value, 2) ;
} /* put_be16 */

static void
put_be16_array (BYTE_BUF * buf, const short * pcm, int count)
{	int k ;

	for (k = 0 ; k < count ; k++)
		put_be16 (buf, (uint16_t) pcm [k]) ;
} /* put_be16_array */

static void
put_marker (BYTE_BUF * buf, const char * marker)
{	put_bytes (buf, marker, 4) ;
} /* put_marker */

static void
put_chunk_head (BYTE_BUF * buf, const char * marker, uint64_t chunk_size)
{	put_marker (buf, marker) ;
	put_be (buf, chunk_size, 8) ;
} /* put_chunk_head */

static uint32_t
get_be32 (const unsigned char * p)
{	return ((uint32_t) p [0] << 24) | ((uint32_t) p [1] << 16) |
			((uint32_t) p [2] << 8) | (uint32_t) p [3] ;
} /* get_be32 */

static void
put_be_float (BYTE_BUF * buf, float value)
{	uint32_t raw ;

	memcpy (&raw, &value, sizeof (raw)) ;
	put_be32 (buf, raw) ;
} /* put_be_float */

/*------------------------------------------------------------------------------
** Hand crafted CAF files.
*/

static void
put_desc_chunk (BYTE_BUF * buf, int channels, int bits_per_chan, int is_float)
{	double rate = 8000.0 ;
	uint64_t rate_bits ;

	put_chunk_head (buf, "desc", 32) ;

	memcpy (&rate_bits, &rate, sizeof (rate_bits)) ;
	put_be (buf, rate_bits, 8) ;

	put_marker (buf, "lpcm") ;
	put_be32 (buf, is_float ? 1u : 0u) ;					/* fmt flags */
	put_be32 (buf, (uint32_t) (bits_per_chan / 8 * channels)) ;	/* bytes per packet */
	put_be32 (buf, 1) ;									/* frames per packet */
	put_be32 (buf, (uint32_t) channels) ;
	put_be32 (buf, (uint32_t) bits_per_chan) ;
} /* put_desc_chunk */

static void
put_data_chunk (BYTE_BUF * buf, const void * audio, size_t audio_len)
{	put_chunk_head (buf, "data", (uint64_t) (4 + audio_len)) ;
	put_be32 (buf, 0) ;									/* edit count */
	put_bytes (buf, audio, audio_len) ;
} /* put_data_chunk */

/*
** Build a complete CAF file in memory. chan_payload/chan_len are the raw
** contents of an optional 'chan' chunk (header fields plus descriptions).
*/
static void
build_caf (BYTE_BUF * buf, int channels, int bits_per_chan, int is_float,
		const unsigned char * chan_payload, size_t chan_len,
		const unsigned char * audio, size_t audio_len)
{	buf->len = 0 ;

	put_marker (buf, "caff") ;
	put_be (buf, 1, 2) ;								/* version */
	put_be (buf, 0, 2) ;								/* flags */

	put_desc_chunk (buf, channels, bits_per_chan, is_float) ;

	if (chan_len > 0)
	{	put_chunk_head (buf, "chan", (uint64_t) chan_len) ;
		put_bytes (buf, chan_payload, chan_len) ;
		} ;

	put_data_chunk (buf, audio, audio_len) ;
} /* build_caf */

/* Header of an AudioChannelLayout chunk: tag, bitmap, description count. */
static void
put_chan_header (BYTE_BUF * buf, uint32_t tag, uint32_t bitmap, uint32_t descriptions)
{	put_be32 (buf, tag) ;
	put_be32 (buf, bitmap) ;
	put_be32 (buf, descriptions) ;
} /* put_chan_header */

/* One 20 byte AudioChannelDescription: label, flags and three zero coords. */
static void
put_chan_description (BYTE_BUF * buf, uint32_t label, uint32_t flags)
{	put_be32 (buf, label) ;
	put_be32 (buf, flags) ;
	put_be32 (buf, 0) ;
	put_be32 (buf, 0) ;
	put_be32 (buf, 0) ;
} /* put_chan_description */

/*------------------------------------------------------------------------------
** Seekable in-memory virtual IO.
*/

#define VIO_CAPACITY		(64 * 1024)

typedef struct
{	sf_count_t		offset, length ;
	unsigned char	data [VIO_CAPACITY] ;
} VIO_DATA ;

static sf_count_t
vio_get_filelen (void * user_data)
{	VIO_DATA * vf = (VIO_DATA *) user_data ;

	return vf->length ;
} /* vio_get_filelen */

static sf_count_t
vio_seek (sf_count_t offset, int whence, void * user_data)
{	VIO_DATA * vf = (VIO_DATA *) user_data ;

	switch (whence)
	{	case SEEK_SET :
			vf->offset = offset ;
			break ;
		case SEEK_CUR :
			vf->offset += offset ;
			break ;
		case SEEK_END :
			vf->offset = vf->length + offset ;
			break ;
		default :
			break ;
		} ;

	return vf->offset ;
} /* vio_seek */

static sf_count_t
vio_read (void * ptr, sf_count_t count, void * user_data)
{	VIO_DATA * vf = (VIO_DATA *) user_data ;

	if (vf->offset + count > vf->length)
		count = vf->length - vf->offset ;

	memcpy (ptr, vf->data + vf->offset, (size_t) count) ;
	vf->offset += count ;
	return count ;
} /* vio_read */

static sf_count_t
vio_write (const void * ptr, sf_count_t count, void * user_data)
{	VIO_DATA * vf = (VIO_DATA *) user_data ;

	TASK_CHECK (vf->offset + count <= (sf_count_t) sizeof (vf->data)) ;
	memcpy (vf->data + vf->offset, ptr, (size_t) count) ;
	vf->offset += count ;
	if (vf->offset > vf->length)
		vf->length = vf->offset ;
	return count ;
} /* vio_write */

static sf_count_t
vio_tell (void * user_data)
{	VIO_DATA * vf = (VIO_DATA *) user_data ;

	return vf->offset ;
} /* vio_tell */

static SF_VIRTUAL_IO virtual_io =
{	vio_get_filelen, vio_seek, vio_read, vio_write, vio_tell
} ;

/*------------------------------------------------------------------------------
** Output sinks: a regular file or a seekable virtual IO blob.
*/

typedef struct
{	int			is_virtual ;
	const char	* path ;
	VIO_DATA	vio ;
} SINK ;

static SNDFILE *
sink_open_write (SINK * sink, SF_INFO * sfinfo)
{
	if (sink->is_virtual)
	{	memset (&sink->vio, 0, sizeof (sink->vio)) ;
		return sf_open_virtual (&virtual_io, SFM_WRITE, sfinfo, &sink->vio) ;
		} ;

	remove (sink->path) ;
	return sf_open (sink->path, SFM_WRITE, sfinfo) ;
} /* sink_open_write */

/* After close, hand the whole produced blob back for independent parsing. */
static size_t
sink_contents (const SINK * sink, unsigned char * out, size_t capacity)
{	size_t len ;

	if (sink->is_virtual)
	{	len = (size_t) sink->vio.length ;
		TASK_CHECK (len <= capacity) ;
		memcpy (out, sink->vio.data, len) ;
		return len ;
		} ;

	{	FILE * f = fopen (sink->path, "rb") ;
		long flen ;

		TASK_CHECK (f != NULL) ;
		TASK_CHECK (fseek (f, 0, SEEK_END) == 0) ;
		flen = ftell (f) ;
		TASK_CHECK (flen > 0 && (size_t) flen <= capacity) ;
		TASK_CHECK (fseek (f, 0, SEEK_SET) == 0) ;
		TASK_CHECK (fread (out, 1, (size_t) flen, f) == (size_t) flen) ;
		fclose (f) ;
		return (size_t) flen ;
		} ;
} /* sink_contents */

static SNDFILE *
open_blob_read (const BYTE_BUF * blob, int use_virtual, VIO_DATA * vio, SF_INFO * sfinfo)
{
	if (use_virtual)
	{	memset (vio, 0, sizeof (*vio)) ;
		TASK_CHECK (blob->len <= sizeof (vio->data)) ;
		memcpy (vio->data, blob->data, blob->len) ;
		vio->length = (sf_count_t) blob->len ;
		return sf_open_virtual (&virtual_io, SFM_READ, sfinfo, vio) ;
		} ;

	{	const char * path = "caf_layout_handcrafted.caf" ;
		FILE * f = fopen (path, "wb") ;

		TASK_CHECK (f != NULL) ;
		TASK_CHECK (fwrite (blob->data, 1, blob->len, f) == blob->len) ;
		fclose (f) ;
		return sf_open (path, SFM_READ, sfinfo) ;
		} ;
} /* open_blob_read */

/*------------------------------------------------------------------------------
** Independent big-endian chunk walker.
*/

typedef struct
{	uint32_t		marker ;
	uint64_t		size ;
	const unsigned char	* content ;
} PARSED_CHUNK ;

typedef struct
{	PARSED_CHUNK		chunks [32] ;
	int					count ;
	const unsigned char	* data_payload ;
	uint64_t			data_payload_len ;
	int					chan_count ;
	const unsigned char	* chan_content ;
	uint64_t			chan_size ;
} PARSED_CAF ;

static void
parse_caf (const unsigned char * p, size_t len, PARSED_CAF * out)
{	size_t pos = 8 ;		/* skip 'caff', version and flags. */

	memset (out, 0, sizeof (*out)) ;
	TASK_CHECK (len >= 8 && memcmp (p, "caff", 4) == 0) ;

	while (pos + 12 <= len)
	{	uint32_t marker = get_be32 (p + pos) ;
		uint64_t size = 0 ;
		int k ;
		PARSED_CHUNK * ch ;

		for (k = 0 ; k < 8 ; k++)
			size = (size << 8) | p [pos + 4 + k] ;

		pos += 12 ;
		TASK_CHECK (pos + (size_t) size <= len) ;
		TASK_CHECK (out->count < (int) (sizeof (out->chunks) / sizeof (out->chunks [0]))) ;

		ch = &out->chunks [out->count++] ;
		ch->marker = marker ;
		ch->size = size ;
		ch->content = p + pos ;

		if (memcmp (p + pos - 12, "chan", 4) == 0)
		{	out->chan_count ++ ;
			out->chan_content = ch->content ;
			out->chan_size = size ;
			} ;

		if (memcmp (p + pos - 12, "data", 4) == 0)
		{	TASK_CHECK (size >= 4) ;
			out->data_payload = ch->content + 4 ;		/* skip edit count */
			out->data_payload_len = size - 4 ;
			} ;

		pos += (size_t) size ;
		} ;

	TASK_CHECK (pos == len) ;
} /* parse_caf */

/*------------------------------------------------------------------------------
** Read-side cases.
*/

static void
case_read_descriptions (int use_virtual)
{	BYTE_BUF blob, chan ;
	VIO_DATA vio ;
	SF_INFO sfinfo ;
	SNDFILE * file ;
	short pcm [6] = { 10, 20, 30, -40, 50, -60 } ;
	short got [6] ;
	int map [3] ;
	BYTE_BUF audio ;
	static const int expected [3] =
		{ SF_CHANNEL_MAP_RIGHT, SF_CHANNEL_MAP_LEFT, SF_CHANNEL_MAP_CENTER } ;

	/* Three channels in disk order Right, Left, Center. */
	chan.len = 0 ;
	put_chan_header (&chan, 0, 0, 3) ;			/* UseChannelDescriptions */
	put_chan_description (&chan, 2, 0) ;		/* Right */
	put_chan_description (&chan, 1, 0) ;		/* Left */
	put_chan_description (&chan, 3, 0) ;		/* Center */

	/* CAF integer PCM is big-endian on disk. */
	audio.len = 0 ;
	put_be16_array (&audio, pcm, 6) ;
	build_caf (&blob, 3, 16, 0, chan.data, chan.len, audio.data, audio.len) ;

	memset (&sfinfo, 0, sizeof (sfinfo)) ;
	file = open_blob_read (&blob, use_virtual, &vio, &sfinfo) ;
	TASK_CHECK (file != NULL) ;
	TASK_CHECK (sfinfo.channels == 3) ;
	TASK_CHECK (sfinfo.frames == 2) ;

	TASK_CHECK (sf_command (file, SFC_GET_CHANNEL_MAP_INFO, map, sizeof (map)) == SF_TRUE) ;
	TASK_CHECK (memcmp (map, expected, sizeof (expected)) == 0) ;

	memset (got, 0, sizeof (got)) ;
	TASK_CHECK (sf_readf_short (file, got, 2) == 2) ;
	TASK_CHECK (memcmp (got, pcm, sizeof (pcm)) == 0) ;

	TASK_CHECK (sf_close (file) == 0) ;
	remove ("caf_layout_handcrafted.caf") ;
} /* case_read_descriptions */

static void
case_read_bitmap (int use_virtual)
{	BYTE_BUF blob, chan ;
	VIO_DATA vio ;
	SF_INFO sfinfo ;
	SNDFILE * file ;
	short pcm [4] = { 1, 2, 3, 4 } ;
	BYTE_BUF audio ;
	int map [2] ;
	static const int expected [2] =
		{ SF_CHANNEL_MAP_RIGHT, SF_CHANNEL_MAP_LFE } ;

	/*
	** Bits correspond to labels (bit position + 1). Set bits, low to high,
	** give the channel order: bit 1 is Right (label 2), bit 3 is LFE (label 4).
	*/
	chan.len = 0 ;
	put_chan_header (&chan, (1u << 16), (1u << 1) | (1u << 3), 0) ;	/* UseChannelBitmap */

	audio.len = 0 ;
	put_be16_array (&audio, pcm, 4) ;
	build_caf (&blob, 2, 16, 0, chan.data, chan.len, audio.data, audio.len) ;

	memset (&sfinfo, 0, sizeof (sfinfo)) ;
	file = open_blob_read (&blob, use_virtual, &vio, &sfinfo) ;
	TASK_CHECK (file != NULL) ;

	TASK_CHECK (sf_command (file, SFC_GET_CHANNEL_MAP_INFO, map, sizeof (map)) == SF_TRUE) ;
	TASK_CHECK (memcmp (map, expected, sizeof (expected)) == 0) ;

	TASK_CHECK (sf_close (file) == 0) ;
	remove ("caf_layout_handcrafted.caf") ;
} /* case_read_bitmap */

static void
case_read_bitmap_canonical_order (int use_virtual)
{	BYTE_BUF blob, chan ;
	VIO_DATA vio ;
	SF_INFO sfinfo ;
	SNDFILE * file ;
	short pcm [6] = { 0 } ;
	int map [3] ;
	BYTE_BUF audio ;
	static const int expected [3] =
		{ SF_CHANNEL_MAP_LEFT, SF_CHANNEL_MAP_RIGHT, SF_CHANNEL_MAP_CENTER } ;

	chan.len = 0 ;
	put_chan_header (&chan, (1u << 16), 0x7u, 0) ;	/* labels 1, 2, 3 */

	audio.len = 0 ;
	put_be16_array (&audio, pcm, 6) ;
	build_caf (&blob, 3, 16, 0, chan.data, chan.len, audio.data, audio.len) ;

	memset (&sfinfo, 0, sizeof (sfinfo)) ;
	file = open_blob_read (&blob, use_virtual, &vio, &sfinfo) ;
	TASK_CHECK (file != NULL) ;

	TASK_CHECK (sf_command (file, SFC_GET_CHANNEL_MAP_INFO, map, sizeof (map)) == SF_TRUE) ;
	TASK_CHECK (memcmp (map, expected, sizeof (expected)) == 0) ;

	TASK_CHECK (sf_close (file) == 0) ;
	remove ("caf_layout_handcrafted.caf") ;
} /* case_read_bitmap_canonical_order */

/*
** Every one of these malformed layouts must make sf_open () fail; a partial
** channel table must never be exposed.
*/
static void
case_bad_layouts (int use_virtual)
{
	static const short pcm [6] = { 10, 20, 30, -40, 50, -60 } ;
	VIO_DATA vio ;
	SF_INFO sfinfo ;
	SNDFILE * file ;
	BYTE_BUF blob, audio ;
	int k ;

	/* CAF integer PCM is big-endian on disk. */
	audio.len = 0 ;
	put_be16_array (&audio, pcm, 6) ;

	/* Case 1: bitmap containing an unsupported bit (label 10, bit 9). */
	{	BYTE_BUF local ;

		local.len = 0 ;
		put_chan_header (&local, (1u << 16), (1u << 0) | (1u << 1) | (1u << 9), 0) ;
		build_caf (&blob, 3, 16, 0, local.data, local.len, audio.data, audio.len) ;
		memset (&sfinfo, 0, sizeof (sfinfo)) ;
		file = open_blob_read (&blob, use_virtual, &vio, &sfinfo) ;
		TASK_CHECK (file == NULL) ;
		remove ("caf_layout_handcrafted.caf") ;
		} ;

	/* Case 2: bitmap set-bit count does not match the audio channel count. */
	{	BYTE_BUF local ;

		local.len = 0 ;
		put_chan_header (&local, (1u << 16), 0x3u, 0) ;		/* only two roles */
		build_caf (&blob, 3, 16, 0, local.data, local.len, audio.data, audio.len) ;
		memset (&sfinfo, 0, sizeof (sfinfo)) ;
		file = open_blob_read (&blob, use_virtual, &vio, &sfinfo) ;
		TASK_CHECK (file == NULL) ;
		remove ("caf_layout_handcrafted.caf") ;
		} ;

	/* Case 3: duplicate role in the descriptions. */
	{	BYTE_BUF local ;

		local.len = 0 ;
		put_chan_header (&local, 0, 0, 3) ;
		put_chan_description (&local, 2, 0) ;		/* Right */
		put_chan_description (&local, 2, 0) ;		/* Right again */
		put_chan_description (&local, 3, 0) ;		/* Center */
		build_caf (&blob, 3, 16, 0, local.data, local.len, audio.data, audio.len) ;
		memset (&sfinfo, 0, sizeof (sfinfo)) ;
		file = open_blob_read (&blob, use_virtual, &vio, &sfinfo) ;
		TASK_CHECK (file == NULL) ;
		remove ("caf_layout_handcrafted.caf") ;
		} ;

	/* Case 4: description count does not match the audio channel count. */
	{	BYTE_BUF local ;

		local.len = 0 ;
		put_chan_header (&local, 0, 0, 2) ;
		put_chan_description (&local, 2, 0) ;
		put_chan_description (&local, 1, 0) ;
		build_caf (&blob, 3, 16, 0, local.data, local.len, audio.data, audio.len) ;
		memset (&sfinfo, 0, sizeof (sfinfo)) ;
		file = open_blob_read (&blob, use_virtual, &vio, &sfinfo) ;
		TASK_CHECK (file == NULL) ;
		remove ("caf_layout_handcrafted.caf") ;
		} ;

	/* Case 5: descriptions truncated by the parent chunk. */
	{	BYTE_BUF local ;

		local.len = 0 ;
		put_chan_header (&local, 0, 0, 3) ;
		put_chan_description (&local, 2, 0) ;
		put_chan_description (&local, 1, 0) ;
		/* The chunk claims three descriptions but only two fit in the size. */
		blob.len = 0 ;
		put_marker (&blob, "caff") ;
		put_be (&blob, 1, 2) ;
		put_be (&blob, 0, 2) ;
		put_desc_chunk (&blob, 3, 16, 0) ;
		put_chunk_head (&blob, "chan", (uint64_t) (12 + 2 * 20)) ;
		put_bytes (&blob, local.data, local.len) ;
		put_data_chunk (&blob, audio.data, audio.len) ;
		memset (&sfinfo, 0, sizeof (sfinfo)) ;
		file = open_blob_read (&blob, use_virtual, &vio, &sfinfo) ;
		TASK_CHECK (file == NULL) ;
		remove ("caf_layout_handcrafted.caf") ;
		} ;

	/* Case 6: non-zero flags, then a non-zero coordinate, in a description. */
	{	BYTE_BUF local ;

		local.len = 0 ;
		put_chan_header (&local, 0, 0, 3) ;
		put_chan_description (&local, 2, 1) ;		/* flags must be zero */
		put_chan_description (&local, 1, 0) ;
		put_chan_description (&local, 3, 0) ;
		build_caf (&blob, 3, 16, 0, local.data, local.len, audio.data, audio.len) ;
		memset (&sfinfo, 0, sizeof (sfinfo)) ;
		file = open_blob_read (&blob, use_virtual, &vio, &sfinfo) ;
		TASK_CHECK (file == NULL) ;
		remove ("caf_layout_handcrafted.caf") ;
		} ;

	{	BYTE_BUF local ;

		local.len = 0 ;
		put_chan_header (&local, 0, 0, 1) ;
		put_be32 (&local, 1) ;					/* label Left */
		put_be32 (&local, 0) ;					/* flags */
		put_be32 (&local, 0x3f800000) ;			/* coordinate 1 = 1.0f, must be zero */
		put_be32 (&local, 0) ;
		put_be32 (&local, 0) ;
		build_caf (&blob, 1, 16, 0, local.data, local.len, audio.data, 2) ;
		memset (&sfinfo, 0, sizeof (sfinfo)) ;
		file = open_blob_read (&blob, use_virtual, &vio, &sfinfo) ;
		TASK_CHECK (file == NULL) ;
		remove ("caf_layout_handcrafted.caf") ;
		} ;

	/* Case 7: unsupported label, plus an undersized chunk. */
	for (k = 0 ; k < 2 ; k++)
	{	BYTE_BUF local ;

		local.len = 0 ;
		if (k == 0)
		{	put_chan_header (&local, 0, 0, 3) ;
			put_chan_description (&local, 10, 0) ;	/* label 10 is not handled */
			put_chan_description (&local, 1, 0) ;
			put_chan_description (&local, 3, 0) ;
			build_caf (&blob, 3, 16, 0, local.data, local.len, audio.data, audio.len) ;
			}
		else
		{	/* Chunk smaller than the 12 byte layout header. */
			blob.len = 0 ;
			put_marker (&blob, "caff") ;
			put_be (&blob, 1, 2) ;
			put_be (&blob, 0, 2) ;
			put_desc_chunk (&blob, 3, 16, 0) ;
			put_chunk_head (&blob, "chan", 8) ;
			put_be32 (&blob, 0) ;
			put_be32 (&blob, 0) ;
			put_data_chunk (&blob, audio.data, audio.len) ;
			} ;

		memset (&sfinfo, 0, sizeof (sfinfo)) ;
		file = open_blob_read (&blob, use_virtual, &vio, &sfinfo) ;
		TASK_CHECK (file == NULL) ;
		remove ("caf_layout_handcrafted.caf") ;
		} ;
} /* case_bad_layouts */

static void
case_no_chan_and_unknown_tag (int use_virtual)
{	BYTE_BUF blob, chan ;
	VIO_DATA vio ;
	SF_INFO sfinfo ;
	SNDFILE * file ;
	short pcm [6] = { 10, 20, 30, -40, 50, -60 } ;
	short got [6] ;
	int map [3] ;
	BYTE_BUF audio ;

	/* CAF integer PCM is big-endian on disk. */
	audio.len = 0 ;
	put_be16_array (&audio, pcm, 6) ;

	/* Old file without a 'chan' chunk reads normally and exposes no map. */
	build_caf (&blob, 3, 16, 0, NULL, 0, audio.data, audio.len) ;
	memset (&sfinfo, 0, sizeof (sfinfo)) ;
	file = open_blob_read (&blob, use_virtual, &vio, &sfinfo) ;
	TASK_CHECK (file != NULL) ;
	TASK_CHECK (sf_command (file, SFC_GET_CHANNEL_MAP_INFO, map, sizeof (map)) == SF_FALSE) ;
	TASK_CHECK (sf_readf_short (file, got, 2) == 2) ;
	TASK_CHECK (memcmp (got, pcm, sizeof (pcm)) == 0) ;
	TASK_CHECK (sf_close (file) == 0) ;
	remove ("caf_layout_handcrafted.caf") ;

	/* An unknown predefined tag keeps the old no-mapping behaviour. */
	chan.len = 0 ;
	put_chan_header (&chan, (200u << 16) | 3, 0, 0) ;
	build_caf (&blob, 3, 16, 0, chan.data, chan.len, audio.data, audio.len) ;
	memset (&sfinfo, 0, sizeof (sfinfo)) ;
	file = open_blob_read (&blob, use_virtual, &vio, &sfinfo) ;
	TASK_CHECK (file != NULL) ;
	TASK_CHECK (sf_command (file, SFC_GET_CHANNEL_MAP_INFO, map, sizeof (map)) == SF_FALSE) ;
	TASK_CHECK (sf_close (file) == 0) ;
	remove ("caf_layout_handcrafted.caf") ;
} /* case_no_chan_and_unknown_tag */

/*------------------------------------------------------------------------------
** Write-side cases.
*/

static unsigned char expected_pcm_bytes [12] ;

static void
build_expected_pcm_bytes (const short * pcm, int count)
{	BYTE_BUF tmp ;
	int k ;

	tmp.len = 0 ;
	for (k = 0 ; k < count ; k++)
		put_be16 (&tmp, (uint16_t) pcm [k]) ;
	TASK_CHECK (tmp.len == sizeof (expected_pcm_bytes)) ;
	memcpy (expected_pcm_bytes, tmp.data, tmp.len) ;
} /* build_expected_pcm_bytes */

static void
check_description_chan (const PARSED_CAF * parsed, int channels, const int * labels)
{	const unsigned char * c ;
	int k ;

	TASK_CHECK (parsed->chan_count == 1) ;
	TASK_CHECK (parsed->chan_size == (uint64_t) (12 + 20 * channels)) ;

	c = parsed->chan_content ;
	TASK_CHECK (get_be32 (c) == 0) ;						/* UseChannelDescriptions */
	TASK_CHECK (get_be32 (c + 4) == 0) ;					/* bitmap */
	TASK_CHECK (get_be32 (c + 8) == (uint32_t) channels) ;

	for (k = 0 ; k < channels ; k++)
	{	const unsigned char * d = c + 12 + 20 * k ;

		TASK_CHECK ((int) get_be32 (d) == labels [k]) ;	/* disk order preserved */
		TASK_CHECK (get_be32 (d + 4) == 0) ;				/* flags */
		TASK_CHECK (get_be32 (d + 8) == 0) ;				/* coordinates ... */
		TASK_CHECK (get_be32 (d + 12) == 0) ;
		TASK_CHECK (get_be32 (d + 16) == 0) ;
		} ;
} /* check_description_chan */

static void
check_tag_chan (const PARSED_CAF * parsed, uint32_t tag)
{	const unsigned char * c ;

	TASK_CHECK (parsed->chan_count == 1) ;
	TASK_CHECK (parsed->chan_size == 12) ;

	c = parsed->chan_content ;
	TASK_CHECK (get_be32 (c) == tag) ;
	TASK_CHECK (get_be32 (c + 4) == 0) ;
	TASK_CHECK (get_be32 (c + 8) == 0) ;
} /* check_tag_chan */

/*
** The headline case: Right, Left, Center written in two chunks with a header
** update in between. The same bytes must come back, in the same order, from
** both a regular file and a seekable virtual IO blob.
*/
static void
case_write_rlc (int use_virtual)
{	SINK sink = { use_virtual, "caf_layout_rlc.caf", { 0, 0, { 0 } } } ;
	SF_INFO sfinfo ;
	SNDFILE * file ;
	static int write_map [3] =
		{ SF_CHANNEL_MAP_RIGHT, SF_CHANNEL_MAP_LEFT, SF_CHANNEL_MAP_CENTER } ;
	static const int labels [3] = { 2, 1, 3 } ;
	short frame1 [3] = { 10, 20, 30 } ;
	short frame2 [3] = { -40, 50, -60 } ;
	short pcm [6], got [6] ;
	int read_map [3] ;
	BYTE_BUF out ;
	PARSED_CAF parsed ;

	memcpy (pcm, frame1, sizeof (frame1)) ;
	memcpy (pcm + 3, frame2, sizeof (frame2)) ;
	build_expected_pcm_bytes (pcm, 6) ;

	memset (&sfinfo, 0, sizeof (sfinfo)) ;
	sfinfo.samplerate = 8000 ;
	sfinfo.channels = 3 ;
	sfinfo.format = SF_FORMAT_CAF | SF_FORMAT_PCM_16 ;

	file = sink_open_write (&sink, &sfinfo) ;
	TASK_CHECK (file != NULL) ;
	TASK_CHECK (sf_command (file, SFC_SET_CHANNEL_MAP_INFO, write_map, sizeof (write_map)) == SF_TRUE) ;

	TASK_CHECK (sf_writef_short (file, frame1, 1) == 1) ;
	sf_command (file, SFC_UPDATE_HEADER_NOW, NULL, 0) ;
	TASK_CHECK (sf_error (file) == SF_ERR_NO_ERROR) ;
	TASK_CHECK (sf_writef_short (file, frame2, 1) == 1) ;
	TASK_CHECK (sf_close (file) == 0) ;

	/* Independently walk the big-endian chunks on disk. */
	out.len = sink_contents (&sink, out.data, sizeof (out.data)) ;
	parse_caf (out.data, out.len, &parsed) ;
	check_description_chan (&parsed, 3, labels) ;
	TASK_CHECK (parsed.data_payload_len == sizeof (expected_pcm_bytes)) ;
	TASK_CHECK (memcmp (parsed.data_payload, expected_pcm_bytes, sizeof (expected_pcm_bytes)) == 0) ;

	/* The public read interface must preserve both the map and the PCM order. */
	if (use_virtual)
	{	/* The library does not rewind a virtual blob; reset it like the
		** existing virtual IO tests do. */
		sink.vio.offset = 0 ;
		memset (&sfinfo, 0, sizeof (sfinfo)) ;
		file = sf_open_virtual (&virtual_io, SFM_READ, &sfinfo, &sink.vio) ;
		}
	else
	{	memset (&sfinfo, 0, sizeof (sfinfo)) ;
		file = sf_open (sink.path, SFM_READ, &sfinfo) ;
		} ;

	TASK_CHECK (file != NULL) ;
	TASK_CHECK (sfinfo.frames == 2) ;
	TASK_CHECK (sf_command (file, SFC_GET_CHANNEL_MAP_INFO, read_map, sizeof (read_map)) == SF_TRUE) ;
	TASK_CHECK (memcmp (read_map, write_map, sizeof (write_map)) == 0) ;
	TASK_CHECK (sf_readf_short (file, got, 2) == 2) ;
	TASK_CHECK (memcmp (got, pcm, sizeof (pcm)) == 0) ;
	TASK_CHECK (sf_close (file) == 0) ;

	remove (sink.path) ;
} /* case_write_rlc */

/* Two layouts before the first frame: the last one set is what lands on disk. */
static void
case_switch_layout_before_first_frame (void)
{	const char * path = "caf_layout_switch.caf" ;
	SF_INFO sfinfo ;
	SNDFILE * file ;
	BYTE_BUF out ;
	PARSED_CAF parsed ;
	static int map_lrc [3] =
		{ SF_CHANNEL_MAP_LEFT, SF_CHANNEL_MAP_RIGHT, SF_CHANNEL_MAP_CENTER } ;
	static int map_rlc [3] =
		{ SF_CHANNEL_MAP_RIGHT, SF_CHANNEL_MAP_LEFT, SF_CHANNEL_MAP_CENTER } ;
	static const int labels_rlc [3] = { 2, 1, 3 } ;
	short frame [3] = { 1, 2, 3 } ;

	/* Predefined layout first, custom layout second: descriptions win. */
	memset (&sfinfo, 0, sizeof (sfinfo)) ;
	sfinfo.samplerate = 8000 ;
	sfinfo.channels = 3 ;
	sfinfo.format = SF_FORMAT_CAF | SF_FORMAT_PCM_16 ;

	file = sf_open (path, SFM_WRITE, &sfinfo) ;
	TASK_CHECK (file != NULL) ;
	TASK_CHECK (sf_command (file, SFC_SET_CHANNEL_MAP_INFO, map_lrc, sizeof (map_lrc)) == SF_TRUE) ;
	TASK_CHECK (sf_command (file, SFC_SET_CHANNEL_MAP_INFO, map_rlc, sizeof (map_rlc)) == SF_TRUE) ;
	TASK_CHECK (sf_writef_short (file, frame, 1) == 1) ;
	TASK_CHECK (sf_close (file) == 0) ;

	out.len = sink_contents (&(SINK) { 0, path, { 0, 0, { 0 } } }, out.data, sizeof (out.data)) ;
	parse_caf (out.data, out.len, &parsed) ;
	check_description_chan (&parsed, 3, labels_rlc) ;

	/* Custom layout first, predefined layout second: the tag wins. */
	file = sf_open (path, SFM_WRITE, &sfinfo) ;
	TASK_CHECK (file != NULL) ;
	TASK_CHECK (sf_command (file, SFC_SET_CHANNEL_MAP_INFO, map_rlc, sizeof (map_rlc)) == SF_TRUE) ;
	TASK_CHECK (sf_command (file, SFC_SET_CHANNEL_MAP_INFO, map_lrc, sizeof (map_lrc)) == SF_TRUE) ;
	TASK_CHECK (sf_writef_short (file, frame, 1) == 1) ;
	TASK_CHECK (sf_close (file) == 0) ;

	out.len = sink_contents (&(SINK) { 0, path, { 0, 0, { 0 } } }, out.data, sizeof (out.data)) ;
	parse_caf (out.data, out.len, &parsed) ;
	check_tag_chan (&parsed, (113u << 16) | 3) ;		/* MPEG 3 0 A (L, R, C) */

	remove (path) ;
} /* case_switch_layout_before_first_frame */

/* A layout matching a predefined tag always uses the compact 12 byte form. */
static void
case_standard_tag_preferred (void)
{	const char * path = "caf_layout_tag.caf" ;
	SF_INFO sfinfo ;
	SNDFILE * file ;
	BYTE_BUF out ;
	PARSED_CAF parsed ;
	static int map_stereo [2] =
		{ SF_CHANNEL_MAP_LEFT, SF_CHANNEL_MAP_RIGHT } ;
	short frame [2] = { 7, -8 } ;

	memset (&sfinfo, 0, sizeof (sfinfo)) ;
	sfinfo.samplerate = 8000 ;
	sfinfo.channels = 2 ;
	sfinfo.format = SF_FORMAT_CAF | SF_FORMAT_PCM_16 ;

	file = sf_open (path, SFM_WRITE, &sfinfo) ;
	TASK_CHECK (file != NULL) ;
	TASK_CHECK (sf_command (file, SFC_SET_CHANNEL_MAP_INFO, map_stereo, sizeof (map_stereo)) == SF_TRUE) ;
	TASK_CHECK (sf_writef_short (file, frame, 1) == 1) ;
	TASK_CHECK (sf_close (file) == 0) ;

	out.len = sink_contents (&(SINK) { 0, path, { 0, 0, { 0 } } }, out.data, sizeof (out.data)) ;
	parse_caf (out.data, out.len, &parsed) ;
	check_tag_chan (&parsed, (101u << 16) | 2) ;		/* stereo (L, R) */

	remove (path) ;
} /* case_standard_tag_preferred */

/* After the first frame, changing the layout has to be rejected. */
static void
case_reject_change_after_write (void)
{	const char * path = "caf_layout_locked.caf" ;
	SF_INFO sfinfo ;
	SNDFILE * file ;
	BYTE_BUF out ;
	PARSED_CAF parsed ;
	static int map_rlc [3] =
		{ SF_CHANNEL_MAP_RIGHT, SF_CHANNEL_MAP_LEFT, SF_CHANNEL_MAP_CENTER } ;
	static int map_lrc [3] =
		{ SF_CHANNEL_MAP_LEFT, SF_CHANNEL_MAP_RIGHT, SF_CHANNEL_MAP_CENTER } ;
	static const int labels_rlc [3] = { 2, 1, 3 } ;
	short frame [3] = { 11, 12, 13 } ;
	int read_map [3] ;

	memset (&sfinfo, 0, sizeof (sfinfo)) ;
	sfinfo.samplerate = 8000 ;
	sfinfo.channels = 3 ;
	sfinfo.format = SF_FORMAT_CAF | SF_FORMAT_PCM_16 ;

	file = sf_open (path, SFM_WRITE, &sfinfo) ;
	TASK_CHECK (file != NULL) ;
	TASK_CHECK (sf_command (file, SFC_SET_CHANNEL_MAP_INFO, map_rlc, sizeof (map_rlc)) == SF_TRUE) ;
	TASK_CHECK (sf_writef_short (file, frame, 1) == 1) ;
	TASK_CHECK (sf_command (file, SFC_SET_CHANNEL_MAP_INFO, map_lrc, sizeof (map_lrc)) == SF_FALSE) ;
	TASK_CHECK (sf_close (file) == 0) ;

	out.len = sink_contents (&(SINK) { 0, path, { 0, 0, { 0 } } }, out.data, sizeof (out.data)) ;
	parse_caf (out.data, out.len, &parsed) ;
	check_description_chan (&parsed, 3, labels_rlc) ;

	memset (&sfinfo, 0, sizeof (sfinfo)) ;
	file = sf_open (path, SFM_READ, &sfinfo) ;
	TASK_CHECK (file != NULL) ;
	TASK_CHECK (sf_command (file, SFC_GET_CHANNEL_MAP_INFO, read_map, sizeof (read_map)) == SF_TRUE) ;
	TASK_CHECK (memcmp (read_map, map_rlc, sizeof (map_rlc)) == 0) ;
	TASK_CHECK (sf_close (file) == 0) ;

	remove (path) ;
} /* case_reject_change_after_write */

/* Roles outside the nine discrete speakers cannot be stored in a CAF file. */
static void
case_reject_unsupported_set (void)
{	const char * path = "caf_layout_unsupported.caf" ;
	SF_INFO sfinfo ;
	SNDFILE * file ;
	static int map_ambisonic [2] =
		{ SF_CHANNEL_MAP_AMBISONIC_B_W, SF_CHANNEL_MAP_AMBISONIC_B_X } ;

	memset (&sfinfo, 0, sizeof (sfinfo)) ;
	sfinfo.samplerate = 8000 ;
	sfinfo.channels = 2 ;
	sfinfo.format = SF_FORMAT_CAF | SF_FORMAT_PCM_16 ;

	file = sf_open (path, SFM_WRITE, &sfinfo) ;
	TASK_CHECK (file != NULL) ;
	TASK_CHECK (sf_command (file, SFC_SET_CHANNEL_MAP_INFO, map_ambisonic, sizeof (map_ambisonic)) == SF_FALSE) ;
	TASK_CHECK (sf_close (file) == 0) ;

	/* No 'chan' chunk may have been written. */
	{	BYTE_BUF out ;
		PARSED_CAF parsed ;

		out.len = sink_contents (&(SINK) { 0, path, { 0, 0, { 0 } } }, out.data, sizeof (out.data)) ;
		parse_caf (out.data, out.len, &parsed) ;
		TASK_CHECK (parsed.chan_count == 0) ;
		} ;

	remove (path) ;
} /* case_reject_unsupported_set */

/* Floating point output keeps the raw big-endian float payload intact. */
static void
case_write_float (void)
{	const char * path = "caf_layout_float.caf" ;
	SF_INFO sfinfo ;
	SNDFILE * file ;
	BYTE_BUF out, expected ;
	PARSED_CAF parsed ;
	static int map_rl [2] =
		{ SF_CHANNEL_MAP_RIGHT, SF_CHANNEL_MAP_LEFT } ;
	static const int labels_rl [2] = { 2, 1 } ;
	float frame1 [2] = { 0.25f, -0.5f } ;
	float frame2 [2] = { 1.0f, -1.0f } ;
	float floats [4], got [4] ;
	int read_map [2] ;

	memcpy (floats, frame1, sizeof (frame1)) ;
	memcpy (floats + 2, frame2, sizeof (frame2)) ;

	expected.len = 0 ;
	put_be_float (&expected, 0.25f) ;
	put_be_float (&expected, -0.5f) ;
	put_be_float (&expected, 1.0f) ;
	put_be_float (&expected, -1.0f) ;

	memset (&sfinfo, 0, sizeof (sfinfo)) ;
	sfinfo.samplerate = 8000 ;
	sfinfo.channels = 2 ;
	sfinfo.format = SF_FORMAT_CAF | SF_FORMAT_FLOAT ;

	file = sf_open (path, SFM_WRITE, &sfinfo) ;
	TASK_CHECK (file != NULL) ;
	TASK_CHECK (sf_command (file, SFC_SET_CHANNEL_MAP_INFO, map_rl, sizeof (map_rl)) == SF_TRUE) ;
	TASK_CHECK (sf_writef_float (file, frame1, 1) == 1) ;
	sf_command (file, SFC_UPDATE_HEADER_NOW, NULL, 0) ;
	TASK_CHECK (sf_error (file) == SF_ERR_NO_ERROR) ;
	TASK_CHECK (sf_writef_float (file, frame2, 1) == 1) ;
	TASK_CHECK (sf_close (file) == 0) ;

	out.len = sink_contents (&(SINK) { 0, path, { 0, 0, { 0 } } }, out.data, sizeof (out.data)) ;
	parse_caf (out.data, out.len, &parsed) ;
	check_description_chan (&parsed, 2, labels_rl) ;
	TASK_CHECK (parsed.data_payload_len == expected.len) ;
	TASK_CHECK (memcmp (parsed.data_payload, expected.data, expected.len) == 0) ;

	memset (&sfinfo, 0, sizeof (sfinfo)) ;
	file = sf_open (path, SFM_READ, &sfinfo) ;
	TASK_CHECK (file != NULL) ;
	TASK_CHECK (sf_command (file, SFC_GET_CHANNEL_MAP_INFO, read_map, sizeof (read_map)) == SF_TRUE) ;
	TASK_CHECK (memcmp (read_map, map_rl, sizeof (map_rl)) == 0) ;
	TASK_CHECK (sf_readf_float (file, got, 2) == 2) ;
	TASK_CHECK (memcmp (got, floats, sizeof (floats)) == 0) ;
	TASK_CHECK (sf_close (file) == 0) ;

	remove (path) ;
} /* case_write_float */

/* Integer PCM beyond 16 bit: the 32 bit path keeps the same ordering rules. */
static void
case_write_pcm32 (void)
{	const char * path = "caf_layout_pcm32.caf" ;
	SF_INFO sfinfo ;
	SNDFILE * file ;
	BYTE_BUF out, expected ;
	PARSED_CAF parsed ;
	static int map_rlc [3] =
		{ SF_CHANNEL_MAP_RIGHT, SF_CHANNEL_MAP_LEFT, SF_CHANNEL_MAP_CENTER } ;
	static const int labels_rlc [3] = { 2, 1, 3 } ;
	int frame1 [3] = { 1 << 20, -(1 << 20), 1234567 } ;
	int frame2 [3] = { -7654321, 0, 2147483647 - 8 } ;
	int pcm [6], got [6] ;
	int read_map [3], k ;

	memcpy (pcm, frame1, sizeof (frame1)) ;
	memcpy (pcm + 3, frame2, sizeof (frame2)) ;

	expected.len = 0 ;
	for (k = 0 ; k < 6 ; k++)
		put_be32 (&expected, (uint32_t) pcm [k]) ;

	memset (&sfinfo, 0, sizeof (sfinfo)) ;
	sfinfo.samplerate = 8000 ;
	sfinfo.channels = 3 ;
	sfinfo.format = SF_FORMAT_CAF | SF_FORMAT_PCM_32 ;

	file = sf_open (path, SFM_WRITE, &sfinfo) ;
	TASK_CHECK (file != NULL) ;
	TASK_CHECK (sf_command (file, SFC_SET_CHANNEL_MAP_INFO, map_rlc, sizeof (map_rlc)) == SF_TRUE) ;
	TASK_CHECK (sf_writef_int (file, frame1, 1) == 1) ;
	sf_command (file, SFC_UPDATE_HEADER_NOW, NULL, 0) ;
	TASK_CHECK (sf_error (file) == SF_ERR_NO_ERROR) ;
	TASK_CHECK (sf_writef_int (file, frame2, 1) == 1) ;
	TASK_CHECK (sf_close (file) == 0) ;

	out.len = sink_contents (&(SINK) { 0, path, { 0, 0, { 0 } } }, out.data, sizeof (out.data)) ;
	parse_caf (out.data, out.len, &parsed) ;
	check_description_chan (&parsed, 3, labels_rlc) ;
	TASK_CHECK (parsed.data_payload_len == expected.len) ;
	TASK_CHECK (memcmp (parsed.data_payload, expected.data, expected.len) == 0) ;

	memset (&sfinfo, 0, sizeof (sfinfo)) ;
	file = sf_open (path, SFM_READ, &sfinfo) ;
	TASK_CHECK (file != NULL) ;
	TASK_CHECK (sf_command (file, SFC_GET_CHANNEL_MAP_INFO, read_map, sizeof (read_map)) == SF_TRUE) ;
	TASK_CHECK (memcmp (read_map, map_rlc, sizeof (map_rlc)) == 0) ;
	TASK_CHECK (sf_readf_int (file, got, 2) == 2) ;
	TASK_CHECK (memcmp (got, pcm, sizeof (pcm)) == 0) ;
	TASK_CHECK (sf_close (file) == 0) ;

	remove (path) ;
} /* case_write_pcm32 */

/*
** A second header update (at close, after the explicit one) must not duplicate
** the 'chan' chunk or grow the file: the header is rewritten in place.
*/
static void
case_single_chan_after_repeated_updates (void)
{	const char * path = "caf_layout_updates.caf" ;
	SF_INFO sfinfo ;
	SNDFILE * file ;
	BYTE_BUF out ;
	PARSED_CAF parsed ;
	static int map_rlc [3] =
		{ SF_CHANNEL_MAP_RIGHT, SF_CHANNEL_MAP_LEFT, SF_CHANNEL_MAP_CENTER } ;
	short frame [3] = { 4, 5, 6 } ;
	size_t first_len ;

	memset (&sfinfo, 0, sizeof (sfinfo)) ;
	sfinfo.samplerate = 8000 ;
	sfinfo.channels = 3 ;
	sfinfo.format = SF_FORMAT_CAF | SF_FORMAT_PCM_16 ;

	file = sf_open (path, SFM_WRITE, &sfinfo) ;
	TASK_CHECK (file != NULL) ;
	TASK_CHECK (sf_command (file, SFC_SET_CHANNEL_MAP_INFO, map_rlc, sizeof (map_rlc)) == SF_TRUE) ;
	TASK_CHECK (sf_writef_short (file, frame, 1) == 1) ;
	sf_command (file, SFC_UPDATE_HEADER_NOW, NULL, 0) ;
	TASK_CHECK (sf_error (file) == SF_ERR_NO_ERROR) ;

	first_len = sink_contents (&(SINK) { 0, path, { 0, 0, { 0 } } }, out.data, sizeof (out.data)) ;
	parse_caf (out.data, first_len, &parsed) ;
	TASK_CHECK (parsed.chan_count == 1) ;

	TASK_CHECK (sf_writef_short (file, frame, 1) == 1) ;
	TASK_CHECK (sf_close (file) == 0) ;

	first_len = sink_contents (&(SINK) { 0, path, { 0, 0, { 0 } } }, out.data, sizeof (out.data)) ;
	parse_caf (out.data, first_len, &parsed) ;
	TASK_CHECK (parsed.chan_count == 1) ;
	TASK_CHECK (parsed.data_payload_len == 12) ;

	remove (path) ;
} /* case_single_chan_after_repeated_updates */

void
task_cases (void)
{
	int use_virtual ;

	for (use_virtual = 0 ; use_virtual <= 1 ; use_virtual++)
	{	case_read_descriptions (use_virtual) ;
		case_read_bitmap (use_virtual) ;
		case_read_bitmap_canonical_order (use_virtual) ;
		case_bad_layouts (use_virtual) ;
		case_no_chan_and_unknown_tag (use_virtual) ;
		case_write_rlc (use_virtual) ;
		} ;

	case_switch_layout_before_first_frame () ;
	case_standard_tag_preferred () ;
	case_reject_change_after_write () ;
	case_reject_unsupported_set () ;
	case_write_float () ;
	case_write_pcm32 () ;
	case_single_chan_after_repeated_updates () ;
} /* task_cases */

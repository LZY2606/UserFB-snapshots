/*
** Copyright (C) 2026 Erik de Castro Lopo <erikd@mega-nerd.com>
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
** A packet-level cutter for CAF/ALAC files.
**
** The function keeps the original compressed packets that intersect the
** requested window and writes a new CAF file with fresh 'pakt' framing. The
** packet payloads are copied byte for byte; nothing is decoded and re-encoded.
*/

#include	"sfconfig.h"

#include	<stdio.h>
#include	<stdlib.h>
#include	<string.h>
#include	<stdint.h>
#include	<inttypes.h>
#include	<errno.h>
#include	<sys/stat.h>

#include	"sndfile.h"
#include	"sfendian.h"
#include	"ALAC/alac_codec.h"
#include	"ALAC/ALACBitUtilities.h"

#define		CAF_CUT_MAX_FRAME_LENGTH	4096
#define		CAF_CUT_DECODE_BUFFER		(1 << 20)
#define		CAF_CUT_COPY_BUFFER			(64 * 1024)

/* Four character markers as they read in big-endian file order. */
#define		BE_MARKER(a, b, c, d)	(((uint32_t) (a) << 24) | ((uint32_t) (b) << 16) | ((uint32_t) (c) << 8) | (uint32_t) (d))

#define		alac_MARKER		BE_MARKER ('a', 'l', 'a', 'c')
#define		caff_MARKER		BE_MARKER ('c', 'a', 'f', 'f')
#define		chan_MARKER		BE_MARKER ('c', 'h', 'a', 'n')
#define		data_MARKER		BE_MARKER ('d', 'a', 't', 'a')
#define		desc_MARKER		BE_MARKER ('d', 'e', 's', 'c')
#define		free_MARKER		BE_MARKER ('f', 'r', 'e', 'e')
#define		info_MARKER		BE_MARKER ('i', 'n', 'f', 'o')
#define		kuki_MARKER		BE_MARKER ('k', 'u', 'k', 'i')
#define		pakt_MARKER		BE_MARKER ('p', 'a', 'k', 't')

typedef struct
{	int		present ;
	uint64_t	offset ;	/* Offset of the chunk body. */
	uint64_t	size ;		/* Size of the chunk body. */
} CAF_CHUNK ;

typedef struct
{	const char		*input ;
	const char		*output ;
	FILE			*file ;
	sf_count_t		filelength ;

	CAF_CHUNK		desc, kuki, pakt, data, chan, info ;

	uint32_t		fmt_flags ;
	uint32_t		frames_per_packet ;
	uint32_t		channels ;

	uint8_t			*pakt_data ;
	uint64_t		pakt_size ;

	uint64_t		packets ;
	uint64_t		valid_frames ;
	uint32_t		priming_frames ;
	uint32_t		remainder_frames ;

	uint32_t		*packet_size ;
	uint64_t		data_payload_offset ;
	uint64_t		data_payload_size ;
	uint64_t		decoded_frames ;
} CAF_CUT_FILE ;

/*------------------------------------------------------------------------------
** Small big-endian helpers.
*/

static uint32_t
be32_read (const uint8_t *ptr)
{	return ((uint32_t) ptr [0] << 24) | ((uint32_t) ptr [1] << 16) | ((uint32_t) ptr [2] << 8) | ptr [3] ;
} /* be32_read */

static uint64_t
be64_read (const uint8_t *ptr)
{	uint64_t value = 0 ;
	int k ;

	for (k = 0 ; k < 8 ; k++)
		value = (value << 8) | ptr [k] ;

	return value ;
} /* be64_read */

static void
be32_write (uint8_t *ptr, uint32_t value)
{	ptr [0] = (uint8_t) (value >> 24) ;
	ptr [1] = (uint8_t) (value >> 16) ;
	ptr [2] = (uint8_t) (value >> 8) ;
	ptr [3] = (uint8_t) value ;
} /* be32_write */

static void
be64_write (uint8_t *ptr, uint64_t value)
{	int k ;

	for (k = 7 ; k >= 0 ; k--)
	{	ptr [k] = (uint8_t) value ;
		value >>= 8 ;
		} ;
} /* be64_write */

static int
read_at (FILE *file, sf_count_t offset, void *data, size_t bytes)
{	if (fseek (file, (long) offset, SEEK_SET) != 0)
		return 0 ;
	if (bytes == 0)
		return 1 ;
	return fread (data, 1, bytes, file) == bytes ;
} /* read_at */

/*------------------------------------------------------------------------------
** CAF parsing.
*/

static int
chunk_claim (CAF_CHUNK *chunk, uint64_t offset, uint64_t size)
{	if (chunk->present)
		return 0 ;
	chunk->present = 1 ;
	chunk->offset = offset ;
	chunk->size = size ;
	return 1 ;
} /* chunk_claim */

static int
caf_read_chunks (CAF_CUT_FILE *caf)
{	uint8_t header [12] ;
	uint64_t pos = 8 ;
	int first_chunk = 1 ;
	struct stat st ;

	if (fstat (fileno (caf->file), &st) != 0 || ! S_ISREG (st.st_mode))
		return 0 ;

	caf->filelength = (sf_count_t) st.st_size ;
	if (caf->filelength < 8)
		return 0 ;

	if (! read_at (caf->file, 0, header, 8) || be32_read (header) != caff_MARKER)
		return 0 ;

	while (pos < (uint64_t) caf->filelength)
	{	uint32_t marker ;
		uint64_t body, size ;
		int64_t ssize ;

		if ((uint64_t) caf->filelength - pos < 12)
			break ;		/* Trailing pad byte. */
		if (! read_at (caf->file, (sf_count_t) pos, header, 12))
			return 0 ;

		marker = be32_read (header) ;
		ssize = (int64_t) be64_read (header + 4) ;

		if (marker == 0)
			return 0 ;

		body = pos + 12 ;

		if (ssize < 0)
		{	if (marker != data_MARKER || ssize != -1)
				return 0 ;
			size = (uint64_t) caf->filelength - body ;
			}
		else
		{	size = (uint64_t) ssize ;
			if (size > (uint64_t) caf->filelength - body)
				return 0 ;
			} ;

		if (first_chunk && marker != desc_MARKER)
			return 0 ;
		first_chunk = 0 ;

		switch (marker)
		{	case desc_MARKER :
				if (! chunk_claim (&caf->desc, body, size))
					return 0 ;
				break ;

			case kuki_MARKER :
				if (! chunk_claim (&caf->kuki, body, size))
					return 0 ;
				break ;

			case pakt_MARKER :
				if (! chunk_claim (&caf->pakt, body, size))
					return 0 ;
				break ;

			case data_MARKER :
				if (! chunk_claim (&caf->data, body, size))
					return 0 ;
				break ;

			case chan_MARKER :
				if (! chunk_claim (&caf->chan, body, size))
					return 0 ;
				break ;

			case info_MARKER :
				if (! chunk_claim (&caf->info, body, size))
					return 0 ;
				break ;

			case free_MARKER :
				/* Dropped by the cutter. */
				break ;

			default :
				/* Anything else is not something this cutter understands. */
				return 0 ;
			} ;

		pos = body + size ;
		} ;

	/* A single trailing pad byte is allowed because the writer aligns odd
	** sized 'data' payloads. Anything more is a malformed file. */
	if (pos != (uint64_t) caf->filelength && pos + 1 != (uint64_t) caf->filelength)
		return 0 ;

	return caf->desc.present && caf->kuki.present && caf->pakt.present && caf->data.present ;
} /* caf_read_chunks */

static int
caf_parse_desc (CAF_CUT_FILE *caf)
{	uint8_t desc [32] ;

	if (caf->desc.size < 32)
		return 0 ;
	if (! read_at (caf->file, (sf_count_t) caf->desc.offset, desc, 32))
		return 0 ;

	if (be32_read (desc + 8) != alac_MARKER)
		return 0 ;

	caf->fmt_flags = be32_read (desc + 12) ;
	caf->channels = be32_read (desc + 24) ;

	if (be32_read (desc + 16) != 0)
		return 0 ;

	caf->frames_per_packet = be32_read (desc + 20) ;
	if (caf->frames_per_packet == 0 || caf->frames_per_packet > CAF_CUT_MAX_FRAME_LENGTH)
		return 0 ;

	if (! ((caf->fmt_flags == 1 || caf->fmt_flags == 3) && (caf->channels == 1 || caf->channels == 2)))
		return 0 ;

	return 1 ;
} /* caf_parse_desc */

static int
caf_decode_varint (const uint8_t *data, uint64_t size, uint64_t *pos, uint32_t *value)
{	uint64_t v = 0 ;
	int count = 0 ;
	uint8_t byte ;

	do
	{	if (*pos >= size || count >= 5)
			return 0 ;
		byte = data [(*pos) ++] ;
		v = (v << 7) | (byte & 0x7F) ;
		count ++ ;
		if (v > 0xFFFFFFFFu)
			return 0 ;
		}
	while (byte & 0x80) ;

	*value = (uint32_t) v ;
	return 1 ;
} /* caf_decode_varint */

static int
caf_parse_pakt (CAF_CUT_FILE *caf)
{	uint64_t pos = 24, total = 0 ;
	uint64_t k ;

	if (caf->pakt.size < 24)
		return 0 ;

	caf->pakt_size = caf->pakt.size ;
	if ((caf->pakt_data = malloc ((size_t) caf->pakt_size)) == NULL)
		return 0 ;
	if (! read_at (caf->file, (sf_count_t) caf->pakt.offset, caf->pakt_data, (size_t) caf->pakt_size))
		return 0 ;

	caf->packets = be64_read (caf->pakt_data) ;
	caf->valid_frames = be64_read (caf->pakt_data + 8) ;
	caf->priming_frames = be32_read (caf->pakt_data + 16) ;
	caf->remainder_frames = be32_read (caf->pakt_data + 20) ;

	if (caf->packets == 0 || caf->packets > 0xFFFFFFFFu)
		return 0 ;
	/* Every packet needs at least one varint byte. */
	if (caf->packets > caf->pakt_size - 24)
		return 0 ;
	if (caf->valid_frames > (uint64_t) INT64_MAX)
		return 0 ;

	if ((caf->packet_size = calloc ((size_t) caf->packets, sizeof (caf->packet_size [0]))) == NULL)
		return 0 ;

	for (k = 0 ; k < caf->packets ; k++)
	{	if (! caf_decode_varint (caf->pakt_data, caf->pakt_size, &pos, &caf->packet_size [k]))
			return 0 ;
		if (caf->packet_size [k] == 0)
			return 0 ;
		if (total > UINT64_MAX - caf->packet_size [k])
			return 0 ;
		total += caf->packet_size [k] ;
		} ;

	/* The packet table must account for every byte of the 'pakt' chunk. The
	** writer pads the chunk body to a four byte boundary, so allow up to three
	** trailing zero pad bytes but nothing more. */
	if (caf->pakt_size - pos > 3)
		return 0 ;
	for (k = pos ; k < caf->pakt_size ; k++)
		if (caf->pakt_data [k] != 0)
			return 0 ;

	if (caf->data.size < 4)
		return 0 ;
	caf->data_payload_offset = caf->data.offset + 4 ;
	caf->data_payload_size = caf->data.size - 4 ;

	if (total != caf->data_payload_size)
		return 0 ;

	return 1 ;
} /* caf_parse_pakt */

static int
caf_validate_packets (CAF_CUT_FILE *caf)
{	ALAC_DECODER decoder ;
	uint8_t *cookie = NULL ;
	uint8_t *packet = NULL ;
	int32_t *samples = NULL ;
	uint64_t total_frames = 0 ;
	uint64_t pos = 0 ;
	uint64_t k ;
	int result = 0 ;
	int32_t error ;
	uint32_t expected_flags = 0 ;
	int closed, legacy ;

	if (caf->kuki.size == 0 || caf->kuki.size > (uint64_t) INT32_MAX)
		return 0 ;

	if ((cookie = malloc ((size_t) caf->kuki.size)) == NULL)
		goto cleanup ;
	if (! read_at (caf->file, (sf_count_t) caf->kuki.offset, cookie, (size_t) caf->kuki.size))
		goto cleanup ;

	memset (&decoder, 0, sizeof (decoder)) ;
	if ((error = alac_decoder_init (&decoder, cookie, (uint32_t) caf->kuki.size)) != ALAC_noErr)
		goto cleanup ;

	if (decoder.mConfig.frameLength != caf->frames_per_packet
			|| decoder.mConfig.numChannels != caf->channels)
		goto cleanup ;

	switch (caf->fmt_flags)
	{	case 1 : expected_flags = 16 ; break ;
		case 3 : expected_flags = 24 ; break ;
		default : goto cleanup ;
		} ;
	if (decoder.mConfig.bitDepth != expected_flags)
		goto cleanup ;

	if ((packet = malloc (CAF_CUT_DECODE_BUFFER)) == NULL)
		goto cleanup ;
	if ((samples = malloc ((size_t) caf->channels * caf->frames_per_packet * sizeof (int32_t))) == NULL)
		goto cleanup ;

	for (k = 0 ; k < caf->packets ; k++)
	{	BitBuffer bits ;
		uint32_t produced = 0 ;

		if (caf->packet_size [k] > CAF_CUT_DECODE_BUFFER)
			goto cleanup ;
		if (! read_at (caf->file, (sf_count_t) (caf->data_payload_offset + pos), packet, caf->packet_size [k]))
			goto cleanup ;

		BitBufferInit (&bits, packet, caf->packet_size [k]) ;
		error = alac_decode (&decoder, &bits, samples, caf->frames_per_packet, &produced) ;
		if (error != ALAC_noErr || produced == 0 || produced > caf->frames_per_packet)
			goto cleanup ;

		if (k + 1 < caf->packets && produced != caf->frames_per_packet)
			goto cleanup ;

		total_frames += produced ;
		pos += caf->packet_size [k] ;
		} ;

	/* Signature of the totals is kept in the cut file as 32 bit values. */
	if (total_frames > (uint64_t) INT64_MAX)
		goto cleanup ;
	if (caf->priming_frames > UINT32_MAX || caf->remainder_frames > UINT32_MAX)
		goto cleanup ;

	/* The audible window must sit inside the frames the packets really decode.
	** The frame counts come in one of two shapes :
	**   - closed : priming + valid + remainder == the decoded packet total ;
	**   - legacy : priming == 0 and valid + remainder covers the padded packet
	**     range (packets * frames_per_packet), which is what libsndfile's own
	**     ALAC writer emits for a partial final packet.
	** Everything else is an inconsistent table. */
	if (caf->priming_frames > INT32_MAX || caf->remainder_frames > INT32_MAX)
		goto cleanup ;
	if ((uint64_t) caf->priming_frames + caf->valid_frames > total_frames)
		goto cleanup ;

	closed = (uint64_t) caf->priming_frames + caf->valid_frames + caf->remainder_frames == total_frames ;
	legacy = caf->priming_frames == 0
			&& (uint64_t) caf->valid_frames + caf->remainder_frames
					== (uint64_t) caf->packets * caf->frames_per_packet ;
	if (! closed && ! legacy)
		goto cleanup ;

	caf->decoded_frames = total_frames ;
	result = 1 ;

cleanup :
	free (cookie) ;
	free (packet) ;
	free (samples) ;
	return result ;
} /* caf_validate_packets */

static int
caf_open_and_validate (CAF_CUT_FILE *caf, const char *input)
{	struct stat st ;

	memset (caf, 0, sizeof (*caf)) ;
	caf->input = input ;

	if (stat (input, &st) != 0 || ! S_ISREG (st.st_mode))
		return 0 ;

	if ((caf->file = fopen (input, "rb")) == NULL)
		return 0 ;

	if (! caf_read_chunks (caf) || ! caf_parse_desc (caf) || ! caf_parse_pakt (caf))
		return 0 ;

	if (! caf_validate_packets (caf))
		return 0 ;

	return 1 ;
} /* caf_open_and_validate */

/*------------------------------------------------------------------------------
** Output.
*/

static int
write_all (FILE *file, const void *data, size_t bytes)
{	if (bytes == 0)
		return 1 ;
	return fwrite (data, 1, bytes, file) == bytes ;
} /* write_all */

static int
write_chunk_header (FILE *file, uint32_t marker, uint64_t size)
{	uint8_t header [12] ;

	be32_write (header, marker) ;
	be64_write (header + 4, size) ;

	return write_all (file, header, sizeof (header)) ;
} /* write_chunk_header */

static int
copy_range (FILE *input, sf_count_t offset, FILE *output, uint64_t bytes)
{	uint8_t buffer [CAF_CUT_COPY_BUFFER] ;

	if (fseek (input, (long) offset, SEEK_SET) != 0)
		return 0 ;

	while (bytes > 0)
	{	size_t count = bytes > sizeof (buffer) ? sizeof (buffer) : (size_t) bytes ;

		if (fread (buffer, 1, count, input) != count)
			return 0 ;
		if (! write_all (output, buffer, count))
			return 0 ;
		bytes -= count ;
		} ;

	return 1 ;
} /* copy_range */

static uint32_t
varint_size (uint32_t value)
{	if (value < 0x80)
		return 1 ;
	if (value < 0x4000)
		return 2 ;
	if (value < 0x200000)
		return 3 ;
	if (value < 0x10000000)
		return 4 ;
	return 5 ;
} /* varint_size */

static uint32_t
varint_write (uint8_t *ptr, uint32_t value)
{	uint8_t bytes [5] ;
	uint32_t count = 0, k ;

	if (value < 0x80)
		bytes [count ++] = (uint8_t) value ;
	else
	{	bytes [count ++] = (uint8_t) (value & 0x7F) ;
		value >>= 7 ;
		while (value)
		{	bytes [count ++] = (uint8_t) ((value & 0x7F) | 0x80) ;
			value >>= 7 ;
			} ;
		} ;

	for (k = 0 ; k < count ; k++)
		ptr [k] = bytes [count - 1 - k] ;

	return count ;
} /* varint_write */

static int
write_pakt_chunk (FILE *output, const CAF_CUT_FILE *caf, uint64_t first, uint64_t last,
		uint64_t new_valid, uint32_t new_priming, uint32_t new_remainder)
{	uint64_t count = last - first + 1, k, size ;
	uint8_t *data ;

	size = 24 ;
	for (k = first ; k <= last ; k++)
		size += varint_size (caf->packet_size [k]) ;

	if (size > 0xFFFFFFFFu)
		return 0 ;

	if ((data = calloc (1, (size_t) size)) == NULL)
		return 0 ;

	be64_write (data, count) ;
	be64_write (data + 8, new_valid) ;
	be32_write (data + 16, new_priming) ;
	be32_write (data + 20, new_remainder) ;

	size = 24 ;
	for (k = first ; k <= last ; k++)
		size += varint_write (data + size, caf->packet_size [k]) ;

	if (! write_chunk_header (output, pakt_MARKER, size) || ! write_all (output, data, (size_t) size))
	{	free (data) ;
		return 0 ;
		} ;

	free (data) ;
	return 1 ;
} /* write_pakt_chunk */

static int
copy_optional_chunk (const CAF_CUT_FILE *caf, FILE *output, const CAF_CHUNK *chunk, uint32_t marker)
{	if (! chunk->present)
		return 1 ;
	if (! write_chunk_header (output, marker, chunk->size))
		return 0 ;
	return copy_range (caf->file, (sf_count_t) chunk->offset, output, chunk->size) ;
} /* copy_optional_chunk */

static int
caf_write_cut (CAF_CUT_FILE *caf, uint64_t first, uint64_t last, uint64_t start_in_first,
		uint64_t window_frames, uint32_t new_remainder)
{	FILE *output = NULL ;
	uint8_t header [8] ;
	uint64_t k, data_size = 0, data_offset ;

	if ((output = fopen (caf->output, "wb")) == NULL)
		return 0 ;

	/* 'caff' header. */
	be32_write (header, caff_MARKER) ;
	be32_write (header + 4, 0x00010000) ;	/* version 1, flags 0 */
	if (! write_all (output, header, sizeof (header)))
		goto fail ;

	/* 'desc' is preserved verbatim. */
	if (! write_chunk_header (output, desc_MARKER, caf->desc.size)
			|| ! copy_range (caf->file, (sf_count_t) caf->desc.offset, output, caf->desc.size))
		goto fail ;

	/* 'kuki' is preserved verbatim. */
	if (! write_chunk_header (output, kuki_MARKER, caf->kuki.size)
			|| ! copy_range (caf->file, (sf_count_t) caf->kuki.offset, output, caf->kuki.size))
		goto fail ;

	if (! copy_optional_chunk (caf, output, &caf->chan, chan_MARKER))
		goto fail ;
	if (! copy_optional_chunk (caf, output, &caf->info, info_MARKER))
		goto fail ;

	if (! write_pakt_chunk (output, caf, first, last, window_frames,
			(uint32_t) start_in_first, new_remainder))
		goto fail ;

	for (k = first ; k <= last ; k++)
		data_size += caf->packet_size [k] ;

	if (! write_chunk_header (output, data_MARKER, data_size + 4))
		goto fail ;

	be32_write (header, 0) ;	/* Edit count. */
	if (! write_all (output, header, 4))
		goto fail ;

	for (k = 0, data_offset = 0 ; k < first ; k++)
		data_offset += caf->packet_size [k] ;

	if (! copy_range (caf->file, (sf_count_t) (caf->data_payload_offset + data_offset),
			output, data_size))
		goto fail ;

	if (fflush (output) != 0 || fclose (output) != 0)
	{	output = NULL ;
		goto fail ;
		} ;

	return 1 ;

fail :
	if (output != NULL)
		fclose (output) ;
	remove (caf->output) ;
	return 0 ;
} /* caf_write_cut */

/*------------------------------------------------------------------------------
** Path checks.
*/

static int
output_directory_ok (const char *path)
{	char dir [1024] ;
	const char *slash ;
	struct stat st ;
	size_t len ;

	if ((slash = strrchr (path, '/')) == NULL)
	{	strcpy (dir, ".") ;
		}
	else
	{	len = (size_t) (slash - path) ;
		if (len == 0)
			strcpy (dir, "/") ;
		else
		{	if (len >= sizeof (dir))
				return 0 ;
			memcpy (dir, path, len) ;
			dir [len] = 0 ;
			} ;
		} ;

	return stat (dir, &st) == 0 && S_ISDIR (st.st_mode) ;
} /* output_directory_ok */

static void
caf_cut_free (CAF_CUT_FILE *caf)
{	if (caf->file != NULL)
		fclose (caf->file) ;
	free (caf->pakt_data) ;
	free (caf->packet_size) ;
	caf->file = NULL ;
	caf->pakt_data = NULL ;
	caf->packet_size = NULL ;
} /* caf_cut_free */

/*------------------------------------------------------------------------------
** Public API.
*/

int
sf_caf_alac_cut (const char *input, const char *output, sf_count_t start, sf_count_t frames)
{	CAF_CUT_FILE caf ;
	uint64_t audible_start, audible_end, first, last ;
	uint64_t last_packet_end, window_end, start_in_first, new_remainder, last_frames ;
	struct stat st ;

	if (input == NULL || output == NULL || input [0] == 0 || output [0] == 0)
		return 1 ;
	if (start < 0 || frames <= 0)
		return 1 ;
	if (strcmp (input, output) == 0)
		return 1 ;

	/* These checks happen before anything is created. */
	if (stat (output, &st) == 0)
		return 1 ;
	if (! output_directory_ok (output))
		return 1 ;

	if (! caf_open_and_validate (&caf, input))
	{	caf_cut_free (&caf) ;
		return 1 ;
		} ;

	caf.output = output ;

	if ((uint64_t) start > caf.valid_frames
			|| (uint64_t) frames > caf.valid_frames - (uint64_t) start)
		goto fail ;

	audible_start = (uint64_t) start ;
	audible_end = audible_start + (uint64_t) frames ;

	/* Convert from the audible sample axis to the decoded frame axis. */
	first = (caf.priming_frames + audible_start) / caf.frames_per_packet ;
	last = (caf.priming_frames + audible_end - 1) / caf.frames_per_packet ;

	if (first > last || last >= caf.packets)
		goto fail ;

	/* The last packet may be short. The cut file only describes what the
	** decoder really produced, so derive its length from the validated total. */
	last_frames = caf.decoded_frames - (uint64_t) (caf.packets - 1) * caf.frames_per_packet ;
	window_end = (uint64_t) caf.priming_frames + audible_end ;
	last_packet_end = (uint64_t) last * caf.frames_per_packet
			+ (last + 1 == caf.packets ? last_frames : (uint64_t) caf.frames_per_packet) ;

	start_in_first = caf.priming_frames + audible_start - (uint64_t) first * caf.frames_per_packet ;
	new_remainder = last_packet_end - window_end ;

	if (start_in_first >= (first + 1 == caf.packets ? last_frames : (uint64_t) caf.frames_per_packet))
		goto fail ;
	if (new_remainder >= last_packet_end - (uint64_t) last * caf.frames_per_packet)
		goto fail ;
	if (start_in_first > UINT32_MAX || new_remainder > UINT32_MAX)
		goto fail ;

	if (! caf_write_cut (&caf, first, last, start_in_first, (uint64_t) frames, (uint32_t) new_remainder))
		goto fail ;

	caf_cut_free (&caf) ;
	return 0 ;

fail :
	caf_cut_free (&caf) ;
	return 1 ;
} /* sf_caf_alac_cut */

/*
** Copyright (C) 2026 The libsndfile authors
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
**	CAF channel layout tests. The fixtures are built byte by byte here so no
**	binary sample files are needed. The write-side checks walk the resulting
**	file as big-endian CAF chunks instead of trusting the library to read back
**	what it just wrote.
*/

#include "check.h"

#include <sndfile.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CAF_FRAMES		2

#define CHAN_TAG_BITMAP			0x00010000
#define CHAN_TAG_DESCRIPTIONS	0x00000000

#define MAX_DESCRIPTIONS	16

static const char *pfx = "caf_layout_test_" ;

/*------------------------------------------------------------------------------
** Byte level helpers for hand-built files and for parsing written files.
*/

static void
write_be (FILE *f, uint64_t value, int bytes)
{	int k ;

	for (k = bytes - 1 ; k >= 0 ; k--)
		fputc ((int) ((value >> (k * 8)) & 0xff), f) ;
} /* write_be */

static void
write_marker (FILE *f, const char *marker)
{	fwrite (marker, 1, 4, f) ;
} /* write_marker */

static void
write_chunk_header (FILE *f, const char *marker, uint64_t size)
{	write_marker (f, marker) ;
	write_be (f, size, 8) ;
} /* write_chunk_header */

static void
write_double_be (FILE *f, double value)
{	uint64_t bits ;

	memcpy (&bits, &value, sizeof (bits)) ;
	write_be (f, bits, 8) ;
} /* write_double_be */

static void
write_float_be (FILE *f, float value)
{	uint32_t bits ;

	memcpy (&bits, &value, sizeof (bits)) ;
	write_be (f, bits, 4) ;
} /* write_float_be */

static uint32_t
get_be32 (const unsigned char *ptr)
{	return ((uint32_t) ptr [0] << 24) | ((uint32_t) ptr [1] << 16) |
			((uint32_t) ptr [2] << 8) | (uint32_t) ptr [3] ;
} /* get_be32 */

static uint64_t
get_be64 (const unsigned char *ptr)
{	uint64_t value = 0 ;
	int k ;

	for (k = 0 ; k < 8 ; k++)
		value = (value << 8) | ptr [k] ;

	return value ;
} /* get_be64 */

static float
get_float_be (const unsigned char *ptr)
{	uint32_t bits = get_be32 (ptr) ;
	float value ;

	memcpy (&value, &bits, sizeof (value)) ;
	return value ;
} /* get_float_be */

/*------------------------------------------------------------------------------
** Builders for hand crafted CAF fixtures.
*/

static FILE *
open_fixture (const char *path)
{	FILE *f = fopen (path, "wb") ;

	TASK_CHECK (f != NULL) ;
	write_marker (f, "caff") ;
	write_be (f, 1, 2) ;	/* version */
	write_be (f, 0, 2) ;	/* flags */

	return f ;
} /* open_fixture */

static void
write_desc (FILE *f, int channels, int bits, int is_float)
{	write_chunk_header (f, "desc", 32) ;
	write_double_be (f, 8000.0) ;
	write_marker (f, "lpcm") ;
	write_be (f, is_float ? 1 : 0, 4) ;		/* big endian, possibly float */
	write_be (f, (uint64_t) channels * (bits / 8), 4) ;
	write_be (f, 1, 4) ;					/* frames per packet */
	write_be (f, (uint64_t) channels, 4) ;
	write_be (f, (uint64_t) bits, 4) ;
} /* write_desc */

static void
write_chan_header (FILE *f, uint64_t chunk_size, uint32_t tag, uint32_t bitmap, uint32_t count)
{	write_chunk_header (f, "chan", chunk_size) ;
	write_be (f, tag, 4) ;
	write_be (f, bitmap, 4) ;
	write_be (f, count, 4) ;
} /* write_chan_header */

static void
write_bitmap_chan (FILE *f, uint32_t bitmap, uint64_t chunk_size)
{	write_chan_header (f, chunk_size, CHAN_TAG_BITMAP, bitmap, 0) ;
} /* write_bitmap_chan */

static void
write_desc_chan (FILE *f, uint64_t chunk_size, uint32_t tag, uint32_t count,
				 const uint32_t *labels, uint32_t flags, float flag_coord)
{	uint32_t k ;

	write_chan_header (f, chunk_size, tag, 0, count) ;
	for (k = 0 ; k < count ; k++)
	{	write_be (f, labels [k], 4) ;
		write_be (f, flags, 4) ;
		write_float_be (f, flag_coord) ;
		write_float_be (f, 0.0f) ;
		write_float_be (f, 0.0f) ;
		} ;
} /* write_desc_chan */

static void
write_short_data (FILE *f, const short *pcm, int frames, int channels)
{	int k, count = frames * channels ;

	write_chunk_header (f, "data", 4 + (uint64_t) count * 2) ;
	write_be (f, 0, 4) ;	/* edit count */
	for (k = 0 ; k < count ; k++)
		write_be (f, (uint16_t) pcm [k], 2) ;
} /* write_short_data */

static void
write_float_data (FILE *f, const float *pcm, int frames, int channels)
{	int k, count = frames * channels ;

	write_chunk_header (f, "data", 4 + (uint64_t) count * 4) ;
	write_be (f, 0, 4) ;	/* edit count */
	for (k = 0 ; k < count ; k++)
		write_float_be (f, pcm [k]) ;
} /* write_float_data */

/* Hand built float file used to check float PCM order on the read side. */
static void
case_read_float_descriptions (void)
{	SF_INFO info = { 0 } ;
	SNDFILE *s ;
	FILE *f ;
	int back [3] = { 0, 0, 0 } ;
	uint32_t labels [3] = { 2, 1, 3 } ;
	float pcm [6] = { 10.0f, 20.0f, 30.0f, -40.0f, 50.0f, -60.0f } ;
	float out [6] = { 0 } ;
	char path [256] ;

	snprintf (path, sizeof (path), "%sdiskfloat.caf", pfx) ;

	f = open_fixture (path) ;
	write_desc (f, 3, 32, 1) ;
	write_desc_chan (f, 12 + 3 * 20, CHAN_TAG_DESCRIPTIONS, 3, labels, 0, 0.0f) ;
	write_float_data (f, pcm, CAF_FRAMES, 3) ;
	TASK_CHECK (fclose (f) == 0) ;

	memset (&info, 0, sizeof (info)) ;
	s = sf_open (path, SFM_READ, &info) ;
	TASK_CHECK (s != NULL) ;
	TASK_CHECK (sf_readf_float (s, out, CAF_FRAMES) == CAF_FRAMES) ;
	TASK_CHECK (memcmp (pcm, out, sizeof (pcm)) == 0) ;
	TASK_CHECK (sf_command (s, SFC_GET_CHANNEL_MAP_INFO, back, sizeof (back)) == SF_TRUE) ;
	TASK_CHECK (back [0] == SF_CHANNEL_MAP_RIGHT) ;
	TASK_CHECK (back [1] == SF_CHANNEL_MAP_LEFT) ;
	TASK_CHECK (back [2] == SF_CHANNEL_MAP_CENTER) ;
	TASK_CHECK (sf_close (s) == 0) ;

	remove (path) ;
} /* case_read_float_descriptions */

/*------------------------------------------------------------------------------
** Independent big-endian walker for the files produced by libsndfile.
*/

typedef struct
{	int				chan_count ;
	uint64_t		chan_len ;
	uint32_t		tag ;
	uint32_t		bitmap ;
	uint32_t		desc_count ;
	uint32_t		labels [MAX_DESCRIPTIONS] ;
	uint32_t		flags [MAX_DESCRIPTIONS] ;
	float			coords [MAX_DESCRIPTIONS][3] ;
	uint64_t		data_len ;
	size_t			data_offset ;	/* Offset of the chunk size field, points at its payload. */
} CAF_WINFO ;

static void
parse_caf_bytes (const unsigned char *buf, size_t len, CAF_WINFO *info)
{	size_t pos = 8 ;
	int guard = 0 ;

	memset (info, 0, sizeof (*info)) ;

	if (len < 8 || memcmp (buf, "caff", 4) != 0)
		return ;

	while (pos + 12 <= len && guard++ < 4096)
	{	uint32_t marker = get_be32 (buf + pos) ;
		uint64_t chunk_size = get_be64 (buf + pos + 4) ;
		size_t payload = pos + 12 ;

		if (chunk_size > len - payload)
			return ;

		if (marker == 0x6368616e) /* 'chan' */
		{	uint32_t k ;

			info->chan_count ++ ;
			info->chan_len = chunk_size ;
			if (chunk_size >= 12)
			{	info->tag = get_be32 (buf + payload) ;
				info->bitmap = get_be32 (buf + payload + 4) ;
				info->desc_count = get_be32 (buf + payload + 8) ;

				for (k = 0 ; k < info->desc_count && k < MAX_DESCRIPTIONS ; k++)
				{	size_t off = payload + 12 + (size_t) k * 20 ;

					if (off + 20 > payload + chunk_size || off + 20 > len)
						break ;
					info->labels [k] = get_be32 (buf + off) ;
					info->flags [k] = get_be32 (buf + off + 4) ;
					info->coords [k][0] = get_float_be (buf + off + 8) ;
					info->coords [k][1] = get_float_be (buf + off + 12) ;
					info->coords [k][2] = get_float_be (buf + off + 16) ;
					} ;
				} ;
			}
		else if (marker == 0x64617461) /* 'data' */
		{	info->data_len = chunk_size ;
			info->data_offset = payload ;
			} ;

		pos = payload + (size_t) chunk_size ;
		} ;
} /* parse_caf_bytes */

static void
parse_caf_file (const char *path, CAF_WINFO *info)
{	FILE *f = fopen (path, "rb") ;
	unsigned char *buf ;
	long len ;

	TASK_CHECK (f != NULL) ;
	fseek (f, 0, SEEK_END) ;
	len = ftell (f) ;
	fseek (f, 0, SEEK_SET) ;
	TASK_CHECK (len > 0) ;

	buf = malloc ((size_t) len) ;
	TASK_CHECK (buf != NULL) ;
	TASK_CHECK (fread (buf, 1, (size_t) len, f) == (size_t) len) ;
	TASK_CHECK (fclose (f) == 0) ;

	parse_caf_bytes (buf, (size_t) len, info) ;
	free (buf) ;
} /* parse_caf_file */

/* Check the raw data payload against the samples the caller wrote (PCM 16 BE). */
static void
check_short_data (const char *path, const CAF_WINFO *info, const short *pcm, int frames, int channels)
{	FILE *f = fopen (path, "rb") ;
	int count = frames * channels, k ;

	TASK_CHECK (f != NULL) ;
	TASK_CHECK (info->data_len == (uint64_t) (4 + count * 2)) ;
	TASK_CHECK (fseek (f, (long) info->data_offset + 4, SEEK_SET) == 0) ;
	for (k = 0 ; k < count ; k++)
	{	unsigned char bytes [2] ;
		uint16_t expected = (uint16_t) pcm [k] ;

		TASK_CHECK (fread (bytes, 1, 2, f) == 2) ;
		TASK_CHECK (bytes [0] == (unsigned char) (expected >> 8)) ;
		TASK_CHECK (bytes [1] == (unsigned char) (expected & 0xff)) ;
		} ;
	TASK_CHECK (fclose (f) == 0) ;
} /* check_short_data */

static void
check_float_data (const char *path, const CAF_WINFO *info, const float *pcm, int frames, int channels)
{	FILE *f = fopen (path, "rb") ;
	int count = frames * channels, k ;

	TASK_CHECK (f != NULL) ;
	TASK_CHECK (info->data_len == (uint64_t) (4 + count * 4)) ;
	TASK_CHECK (fseek (f, (long) info->data_offset + 4, SEEK_SET) == 0) ;
	for (k = 0 ; k < count ; k++)
	{	unsigned char bytes [4] ;
		uint32_t expected ;

		memcpy (&expected, &pcm [k], sizeof (expected)) ;
		TASK_CHECK (fread (bytes, 1, 4, f) == 4) ;
		TASK_CHECK (get_be32 (bytes) == expected) ;
		} ;
	TASK_CHECK (fclose (f) == 0) ;
} /* check_float_data */

/*------------------------------------------------------------------------------
** Small in-memory seekable virtual file used by sf_open_virtual.
*/

typedef struct
{	unsigned char	*buf ;
	size_t			len ;
	size_t			cap ;
	size_t			pos ;
} MEM_FILE ;

static sf_count_t
mf_getlen (void *user)
{	MEM_FILE *mf = user ;

	return (sf_count_t) mf->len ;
} /* mf_getlen */

static sf_count_t
mf_seek (sf_count_t offset, int whence, void *user)
{	MEM_FILE *mf = user ;
	sf_count_t newpos ;

	switch (whence)
	{	case SEEK_SET : newpos = offset ; break ;
		case SEEK_CUR : newpos = (sf_count_t) mf->pos + offset ; break ;
		case SEEK_END : newpos = (sf_count_t) mf->len + offset ; break ;
		default : return -1 ;
		} ;

	if (newpos < 0)
		return -1 ;

	mf->pos = (size_t) newpos ;
	return newpos ;
} /* mf_seek */

static sf_count_t
mf_read (void *ptr, sf_count_t count, void *user)
{	MEM_FILE *mf = user ;
	size_t avail = mf->len - mf->pos ;

	if (count > (sf_count_t) avail)
		count = (sf_count_t) avail ;
	memcpy (ptr, mf->buf + mf->pos, (size_t) count) ;
	mf->pos += (size_t) count ;

	return count ;
} /* mf_read */

static sf_count_t
mf_write (const void *ptr, sf_count_t count, void *user)
{	MEM_FILE *mf = user ;
	size_t needed = mf->pos + (size_t) count ;

	if (needed > mf->cap)
	{	size_t newcap = mf->cap ? mf->cap : 1024 ;
		unsigned char *newbuf ;

		while (newcap < needed)
			newcap *= 2 ;
		newbuf = realloc (mf->buf, newcap) ;
		TASK_CHECK (newbuf != NULL) ;
		mf->buf = newbuf ;
		mf->cap = newcap ;
		} ;
	if (mf->pos > mf->len)
		memset (mf->buf + mf->len, 0, mf->pos - mf->len) ;
	memcpy (mf->buf + mf->pos, ptr, (size_t) count) ;
	mf->pos += (size_t) count ;
	if (mf->pos > mf->len)
		mf->len = mf->pos ;

	return count ;
} /* mf_write */

static sf_count_t
mf_tell (void *user)
{	MEM_FILE *mf = user ;

	return (sf_count_t) mf->pos ;
} /* mf_tell */

/*------------------------------------------------------------------------------
** Test cases.
*/

/* Write with a channel map that has a predefined layout tag. */
static void
case_predefined_stereo (void)
{	SF_INFO info = { 0 } ;
	SNDFILE *s ;
	CAF_WINFO winfo ;
	int map [2] = { SF_CHANNEL_MAP_LEFT, SF_CHANNEL_MAP_RIGHT } ;
	int back [2] = { 0, 0 } ;
	short pcm [4] = { 10, 20, -40, 50 } ;
	short out [4] = { 0 } ;
	char path [256] ;

	snprintf (path, sizeof (path), "%sstereo.caf", pfx) ;

	info.samplerate = 8000 ;
	info.channels = 2 ;
	info.format = SF_FORMAT_CAF | SF_FORMAT_PCM_16 ;

	s = sf_open (path, SFM_WRITE, &info) ;
	TASK_CHECK (s != NULL) ;
	TASK_CHECK (sf_command (s, SFC_SET_CHANNEL_MAP_INFO, map, sizeof (map)) == SF_TRUE) ;
	TASK_CHECK (sf_writef_short (s, pcm, CAF_FRAMES) == CAF_FRAMES) ;
	TASK_CHECK (sf_close (s) == 0) ;

	parse_caf_file (path, &winfo) ;
	TASK_CHECK (winfo.chan_count == 1) ;
	TASK_CHECK (winfo.chan_len == 12) ;
	TASK_CHECK (winfo.tag == ((101u << 16) | 2u)) ;
	check_short_data (path, &winfo, pcm, CAF_FRAMES, 2) ;

	memset (&info, 0, sizeof (info)) ;
	s = sf_open (path, SFM_READ, &info) ;
	TASK_CHECK (s != NULL) ;
	TASK_CHECK (sf_readf_short (s, out, CAF_FRAMES) == CAF_FRAMES) ;
	TASK_CHECK (memcmp (pcm, out, sizeof (pcm)) == 0) ;
	TASK_CHECK (sf_command (s, SFC_GET_CHANNEL_MAP_INFO, back, sizeof (back)) == SF_TRUE) ;
	TASK_CHECK (memcmp (pcm, out, sizeof (pcm)) == 0) ;
	TASK_CHECK (sf_command (s, SFC_GET_CHANNEL_MAP_INFO, back, sizeof (back)) == SF_TRUE) ;
	TASK_CHECK (back [0] == SF_CHANNEL_MAP_LEFT && back [1] == SF_CHANNEL_MAP_RIGHT) ;
	TASK_CHECK (sf_close (s) == 0) ;

	remove (path) ;
} /* case_predefined_stereo */

/* A layout that matches a predefined tag must keep using the tag, even though
** it could also be written as channel descriptions. */
static void
case_predefined_priority (void)
{	SF_INFO info = { 0 } ;
	SNDFILE *s ;
	CAF_WINFO winfo ;
	int map [3] = { SF_CHANNEL_MAP_LEFT, SF_CHANNEL_MAP_RIGHT, SF_CHANNEL_MAP_CENTER } ;
	short pcm [6] = { 1, 2, 3, 4, 5, 6 } ;
	char path [256] ;

	snprintf (path, sizeof (path), "%spriority.caf", pfx) ;

	info.samplerate = 8000 ;
	info.channels = 3 ;
	info.format = SF_FORMAT_CAF | SF_FORMAT_PCM_16 ;

	s = sf_open (path, SFM_WRITE, &info) ;
	TASK_CHECK (s != NULL) ;
	TASK_CHECK (sf_command (s, SFC_SET_CHANNEL_MAP_INFO, map, sizeof (map)) == SF_TRUE) ;
	TASK_CHECK (sf_writef_short (s, pcm, CAF_FRAMES) == CAF_FRAMES) ;
	TASK_CHECK (sf_close (s) == 0) ;

	parse_caf_file (path, &winfo) ;
	TASK_CHECK (winfo.chan_count == 1) ;
	TASK_CHECK (winfo.chan_len == 12) ;
	TASK_CHECK (winfo.desc_count == 0) ;
	TASK_CHECK (winfo.tag == ((113u << 16) | 3u)) ;
	check_short_data (path, &winfo, pcm, CAF_FRAMES, 3) ;

	remove (path) ;
} /* case_predefined_priority */

/* Right, left, center has no matching predefined tag: it must be written as
** three channel descriptions in the caller's order and the PCM must stay in
** that same order. */
static void
case_descriptions_pcm16 (void)
{	SF_INFO info = { 0 } ;
	SNDFILE *s ;
	CAF_WINFO winfo ;
	int map [3] = { SF_CHANNEL_MAP_RIGHT, SF_CHANNEL_MAP_LEFT, SF_CHANNEL_MAP_CENTER } ;
	int back [3] = { 0, 0, 0 } ;
	short pcm [6] = { 10, 20, 30, -40, 50, -60 } ;
	short out [6] = { 0 } ;
	char path [256] ;
	int k ;

	snprintf (path, sizeof (path), "%sdescribed.caf", pfx) ;

	info.samplerate = 8000 ;
	info.channels = 3 ;
	info.format = SF_FORMAT_CAF | SF_FORMAT_PCM_16 ;

	s = sf_open (path, SFM_WRITE, &info) ;
	TASK_CHECK (s != NULL) ;
	TASK_CHECK (sf_command (s, SFC_SET_CHANNEL_MAP_INFO, map, sizeof (map)) == SF_TRUE) ;
	TASK_CHECK (sf_writef_short (s, pcm, CAF_FRAMES) == CAF_FRAMES) ;
	TASK_CHECK (sf_close (s) == 0) ;

	parse_caf_file (path, &winfo) ;
	TASK_CHECK (winfo.chan_count == 1) ;
	TASK_CHECK (winfo.chan_len == 12 + 3 * 20) ;
	TASK_CHECK (winfo.tag == CHAN_TAG_DESCRIPTIONS) ;
	TASK_CHECK (winfo.bitmap == 0) ;
	TASK_CHECK (winfo.desc_count == 3) ;
	for (k = 0 ; k < 3 ; k++)
	{	TASK_CHECK (winfo.labels [k] == (uint32_t) (map [k] == SF_CHANNEL_MAP_LEFT ? 1 :
					map [k] == SF_CHANNEL_MAP_RIGHT ? 2 : 3)) ;
		TASK_CHECK (winfo.flags [k] == 0) ;
		TASK_CHECK (winfo.coords [k][0] == 0.0f && winfo.coords [k][1] == 0.0f && winfo.coords [k][2] == 0.0f) ;
		} ;
	check_short_data (path, &winfo, pcm, CAF_FRAMES, 3) ;

	memset (&info, 0, sizeof (info)) ;
	s = sf_open (path, SFM_READ, &info) ;
	TASK_CHECK (s != NULL) ;
	TASK_CHECK (sf_readf_short (s, out, CAF_FRAMES) == CAF_FRAMES) ;
	TASK_CHECK (memcmp (pcm, out, sizeof (pcm)) == 0) ;
	TASK_CHECK (sf_command (s, SFC_GET_CHANNEL_MAP_INFO, back, sizeof (back)) == SF_TRUE) ;
	TASK_CHECK (memcmp (map, back, sizeof (map)) == 0) ;
	TASK_CHECK (sf_close (s) == 0) ;

	remove (path) ;
} /* case_descriptions_pcm16 */

/* Same custom layout with floating point audio. */
static void
case_descriptions_float (void)
{	SF_INFO info = { 0 } ;
	SNDFILE *s ;
	CAF_WINFO winfo ;
	int map [3] = { SF_CHANNEL_MAP_RIGHT, SF_CHANNEL_MAP_LEFT, SF_CHANNEL_MAP_CENTER } ;
	int back [3] = { 0, 0, 0 } ;
	float pcm [6] = { 10.0f, 20.0f, 30.0f, -40.0f, 50.0f, -60.0f } ;
	float out [6] = { 0 } ;
	char path [256] ;

	snprintf (path, sizeof (path), "%sfloat.caf", pfx) ;

	info.samplerate = 8000 ;
	info.channels = 3 ;
	info.format = SF_FORMAT_CAF | SF_FORMAT_FLOAT ;

	s = sf_open (path, SFM_WRITE, &info) ;
	TASK_CHECK (s != NULL) ;
	TASK_CHECK (sf_command (s, SFC_SET_CHANNEL_MAP_INFO, map, sizeof (map)) == SF_TRUE) ;
	TASK_CHECK (sf_writef_float (s, pcm, CAF_FRAMES) == CAF_FRAMES) ;
	TASK_CHECK (sf_close (s) == 0) ;

	parse_caf_file (path, &winfo) ;
	TASK_CHECK (winfo.chan_count == 1) ;
	TASK_CHECK (winfo.chan_len == 12 + 3 * 20) ;
	TASK_CHECK (winfo.desc_count == 3) ;
	TASK_CHECK (winfo.labels [0] == 2 && winfo.labels [1] == 1 && winfo.labels [2] == 3) ;
	check_float_data (path, &winfo, pcm, CAF_FRAMES, 3) ;

	memset (&info, 0, sizeof (info)) ;
	s = sf_open (path, SFM_READ, &info) ;
	TASK_CHECK (s != NULL) ;
	TASK_CHECK (sf_readf_float (s, out, CAF_FRAMES) == CAF_FRAMES) ;
	TASK_CHECK (memcmp (pcm, out, sizeof (pcm)) == 0) ;
	TASK_CHECK (sf_command (s, SFC_GET_CHANNEL_MAP_INFO, back, sizeof (back)) == SF_TRUE) ;
	TASK_CHECK (memcmp (map, back, sizeof (map)) == 0) ;
	TASK_CHECK (sf_close (s) == 0) ;

	remove (path) ;
} /* case_descriptions_float */

/* Hand built UseChannelBitmap file: bits are channel order, low to high. */
static void
case_read_bitmap (void)
{	SF_INFO info = { 0 } ;
	SNDFILE *s ;
	FILE *f ;
	int back [3] = { 0, 0, 0 } ;
	short pcm [6] = { 10, 20, 30, -40, 50, -60 } ;
	short out [6] = { 0 } ;
	char path [256] ;
	/* bit 0 = label 1 (left), bit 2 = label 3 (center), bit 4 = label 5 (rear left). */
	uint32_t bitmap = (1u << 0) | (1u << 2) | (1u << 4) ;

	snprintf (path, sizeof (path), "%sbitmap.caf", pfx) ;

	f = open_fixture (path) ;
	write_desc (f, 3, 16, 0) ;
	write_bitmap_chan (f, bitmap, 12) ;
	write_short_data (f, pcm, CAF_FRAMES, 3) ;
	TASK_CHECK (fclose (f) == 0) ;

	memset (&info, 0, sizeof (info)) ;
	s = sf_open (path, SFM_READ, &info) ;
	TASK_CHECK (s != NULL) ;
	TASK_CHECK (sf_readf_short (s, out, CAF_FRAMES) == CAF_FRAMES) ;
	TASK_CHECK (memcmp (pcm, out, sizeof (pcm)) == 0) ;
	TASK_CHECK (sf_command (s, SFC_GET_CHANNEL_MAP_INFO, back, sizeof (back)) == SF_TRUE) ;
	TASK_CHECK (back [0] == SF_CHANNEL_MAP_LEFT) ;
	TASK_CHECK (back [1] == SF_CHANNEL_MAP_CENTER) ;
	TASK_CHECK (back [2] == SF_CHANNEL_MAP_REAR_LEFT) ;
	TASK_CHECK (sf_close (s) == 0) ;

	remove (path) ;
} /* case_read_bitmap */

/* Hand built UseChannelDescriptions file. Descriptions are used in disk order,
** not sorted by label. */
static void
case_read_descriptions (void)
{	SF_INFO info = { 0 } ;
	SNDFILE *s ;
	FILE *f ;
	int back [3] = { 0, 0, 0 } ;
	short pcm [6] = { 10, 20, 30, -40, 50, -60 } ;
	short out [6] = { 0 } ;
	uint32_t labels [3] = { 2, 1, 3 } ;
	char path [256] ;

	snprintf (path, sizeof (path), "%sdiskdesc.caf", pfx) ;

	f = open_fixture (path) ;
	write_desc (f, 3, 16, 0) ;
	write_desc_chan (f, 12 + 3 * 20, CHAN_TAG_DESCRIPTIONS, 3, labels, 0, 0.0f) ;
	write_short_data (f, pcm, CAF_FRAMES, 3) ;
	TASK_CHECK (fclose (f) == 0) ;

	memset (&info, 0, sizeof (info)) ;
	s = sf_open (path, SFM_READ, &info) ;
	TASK_CHECK (s != NULL) ;
	TASK_CHECK (sf_readf_short (s, out, CAF_FRAMES) == CAF_FRAMES) ;
	TASK_CHECK (memcmp (pcm, out, sizeof (pcm)) == 0) ;
	TASK_CHECK (sf_command (s, SFC_GET_CHANNEL_MAP_INFO, back, sizeof (back)) == SF_TRUE) ;
	TASK_CHECK (back [0] == SF_CHANNEL_MAP_RIGHT) ;
	TASK_CHECK (back [1] == SF_CHANNEL_MAP_LEFT) ;
	TASK_CHECK (back [2] == SF_CHANNEL_MAP_CENTER) ;
	TASK_CHECK (sf_close (s) == 0) ;

	remove (path) ;
} /* case_read_descriptions */

/* All nine supported labels map onto the documented roles. */
static void
case_read_all_labels (void)
{	SF_INFO info = { 0 } ;
	SNDFILE *s ;
	FILE *f ;
	uint32_t labels [9] = { 1, 2, 3, 4, 5, 6, 7, 8, 9 } ;
	int expected [9] =
	{	SF_CHANNEL_MAP_LEFT, SF_CHANNEL_MAP_RIGHT, SF_CHANNEL_MAP_CENTER,
		SF_CHANNEL_MAP_LFE, SF_CHANNEL_MAP_REAR_LEFT, SF_CHANNEL_MAP_REAR_RIGHT,
		SF_CHANNEL_MAP_FRONT_LEFT_OF_CENTER, SF_CHANNEL_MAP_FRONT_RIGHT_OF_CENTER,
		SF_CHANNEL_MAP_REAR_CENTER
		} ;
	int back [9] = { 0 } ;
	short pcm [9] = { 1, 2, 3, 4, 5, 6, 7, 8, 9 } ;
	short out [9] = { 0 } ;
	char path [256] ;

	snprintf (path, sizeof (path), "%snine.caf", pfx) ;

	f = open_fixture (path) ;
	write_desc (f, 9, 16, 0) ;
	write_desc_chan (f, 12 + 9 * 20, CHAN_TAG_DESCRIPTIONS, 9, labels, 0, 0.0f) ;
	write_short_data (f, pcm, 1, 9) ;
	TASK_CHECK (fclose (f) == 0) ;

	memset (&info, 0, sizeof (info)) ;
	s = sf_open (path, SFM_READ, &info) ;
	TASK_CHECK (s != NULL) ;
	TASK_CHECK (sf_readf_short (s, out, 1) == 1) ;
	TASK_CHECK (memcmp (pcm, out, sizeof (pcm)) == 0) ;
	TASK_CHECK (sf_command (s, SFC_GET_CHANNEL_MAP_INFO, back, sizeof (back)) == SF_TRUE) ;
	TASK_CHECK (memcmp (expected, back, sizeof (back)) == 0) ;
	TASK_CHECK (sf_close (s) == 0) ;

	remove (path) ;
} /* case_read_all_labels */

/* A file that has no 'chan' chunk keeps working and reports no map. */
static void
case_read_no_chan (void)
{	SF_INFO info = { 0 } ;
	SNDFILE *s ;
	FILE *f ;
	short pcm [6] = { 10, 20, 30, -40, 50, -60 } ;
	short out [6] = { 0 } ;
	int back [3] = { 0 } ;
	char path [256] ;

	snprintf (path, sizeof (path), "%snochan.caf", pfx) ;

	f = open_fixture (path) ;
	write_desc (f, 3, 16, 0) ;
	write_short_data (f, pcm, CAF_FRAMES, 3) ;
	TASK_CHECK (fclose (f) == 0) ;

	memset (&info, 0, sizeof (info)) ;
	s = sf_open (path, SFM_READ, &info) ;
	TASK_CHECK (s != NULL) ;
	TASK_CHECK (sf_readf_short (s, out, CAF_FRAMES) == CAF_FRAMES) ;
	TASK_CHECK (memcmp (pcm, out, sizeof (pcm)) == 0) ;
	TASK_CHECK (sf_command (s, SFC_GET_CHANNEL_MAP_INFO, back, sizeof (back)) == SF_FALSE) ;
	TASK_CHECK (sf_close (s) == 0) ;

	remove (path) ;
} /* case_read_no_chan */

/* Unknown predefined layout tags keep the previous behaviour: no map. */
static void
case_read_unknown_tag (void)
{	SF_INFO info = { 0 } ;
	SNDFILE *s ;
	FILE *f ;
	short pcm [6] = { 10, 20, 30, -40, 50, -60 } ;
	short out [6] = { 0 } ;
	int back [3] = { 0 } ;
	char path [256] ;

	snprintf (path, sizeof (path), "%sunknown.caf", pfx) ;

	f = open_fixture (path) ;
	write_desc (f, 3, 16, 0) ;
	write_chan_header (f, 12, (99u << 16) | 3u, 0, 0) ;
	write_short_data (f, pcm, CAF_FRAMES, 3) ;
	TASK_CHECK (fclose (f) == 0) ;

	memset (&info, 0, sizeof (info)) ;
	s = sf_open (path, SFM_READ, &info) ;
	TASK_CHECK (s != NULL) ;
	TASK_CHECK (sf_readf_short (s, out, CAF_FRAMES) == CAF_FRAMES) ;
	TASK_CHECK (memcmp (pcm, out, sizeof (pcm)) == 0) ;
	TASK_CHECK (sf_command (s, SFC_GET_CHANNEL_MAP_INFO, back, sizeof (back)) == SF_FALSE) ;
	TASK_CHECK (sf_close (s) == 0) ;

	remove (path) ;
} /* case_read_unknown_tag */

/*------------------------------------------------------------------------------
** Broken layouts must make sf_open fail, not return a partial channel table.
*/

static void
expect_open_failure (const char *path, int line)
{	SF_INFO info = { 0 } ;
	SNDFILE *s ;

	s = sf_open (path, SFM_READ, &info) ;
	if (s != NULL)
	{	fprintf (stderr, "FAIL %s:%d: malformed CAF was accepted\n", __FILE__, line) ;
		sf_close (s) ;
		exit (42) ;
		} ;
	task_checks ++ ;
} /* expect_open_failure */

#define EXPECT_FAILURE(path)	expect_open_failure ((path), __LINE__)

static void
case_bad_bitmap (void)
{	FILE *f ;
	short pcm [6] = { 10, 20, 30, -40, 50, -60 } ;
	char path [256] ;

	/* Bit 9 is label 10, which is not one of the nine supported roles. */
	snprintf (path, sizeof (path), "%sbadbit.caf", pfx) ;
	f = open_fixture (path) ;
	write_desc (f, 3, 16, 0) ;
	write_bitmap_chan (f, (1u << 0) | (1u << 1) | (1u << 9), 12) ;
	write_short_data (f, pcm, CAF_FRAMES, 3) ;
	TASK_CHECK (fclose (f) == 0) ;
	EXPECT_FAILURE (path) ;
	remove (path) ;

	/* Two bits set for three channels. */
	snprintf (path, sizeof (path), "%sbitmiss.caf", pfx) ;
	f = open_fixture (path) ;
	write_desc (f, 3, 16, 0) ;
	write_bitmap_chan (f, (1u << 0) | (1u << 1), 12) ;
	write_short_data (f, pcm, CAF_FRAMES, 3) ;
	TASK_CHECK (fclose (f) == 0) ;
	EXPECT_FAILURE (path) ;
	remove (path) ;

	/* Four bits set for three channels. */
	snprintf (path, sizeof (path), "%sbitmore.caf", pfx) ;
	f = open_fixture (path) ;
	write_desc (f, 3, 16, 0) ;
	write_bitmap_chan (f, (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3), 12) ;
	write_short_data (f, pcm, CAF_FRAMES, 3) ;
	TASK_CHECK (fclose (f) == 0) ;
	EXPECT_FAILURE (path) ;
	remove (path) ;
} /* case_bad_bitmap */

static void
case_bad_descriptions (void)
{	FILE *f ;
	short pcm [6] = { 10, 20, 30, -40, 50, -60 } ;
	uint32_t duplicate [3] = { 1, 1, 2 } ;
	uint32_t two_only [3] = { 0, 0, 0 } ;
	uint32_t bad_label [3] = { 1, 2, 10 } ;
	char path [256] ;

	/* Duplicate speaker role. */
	snprintf (path, sizeof (path), "%sdup.caf", pfx) ;
	f = open_fixture (path) ;
	write_desc (f, 3, 16, 0) ;
	write_desc_chan (f, 12 + 3 * 20, CHAN_TAG_DESCRIPTIONS, 3, duplicate, 0, 0.0f) ;
	write_short_data (f, pcm, CAF_FRAMES, 3) ;
	TASK_CHECK (fclose (f) == 0) ;
	EXPECT_FAILURE (path) ;
	remove (path) ;

	/* Description count does not match the audio channel count. */
	snprintf (path, sizeof (path), "%scount.caf", pfx) ;
	f = open_fixture (path) ;
	write_desc (f, 3, 16, 0) ;
	write_desc_chan (f, 12 + 2 * 20, CHAN_TAG_DESCRIPTIONS, 2, two_only, 0, 0.0f) ;
	write_short_data (f, pcm, CAF_FRAMES, 3) ;
	TASK_CHECK (fclose (f) == 0) ;
	EXPECT_FAILURE (path) ;
	remove (path) ;

	/* Unsupported description label. */
	snprintf (path, sizeof (path), "%slabel.caf", pfx) ;
	f = open_fixture (path) ;
	write_desc (f, 3, 16, 0) ;
	write_desc_chan (f, 12 + 3 * 20, CHAN_TAG_DESCRIPTIONS, 3, bad_label, 0, 0.0f) ;
	write_short_data (f, pcm, CAF_FRAMES, 3) ;
	TASK_CHECK (fclose (f) == 0) ;
	EXPECT_FAILURE (path) ;
	remove (path) ;

	/* Non-zero flags or coordinates are not supported. */
	snprintf (path, sizeof (path), "%sflags.caf", pfx) ;
	f = open_fixture (path) ;
	write_desc (f, 3, 16, 0) ;
	write_desc_chan (f, 12 + 3 * 20, CHAN_TAG_DESCRIPTIONS, 3, two_only, 1, 0.0f) ;
	write_short_data (f, pcm, CAF_FRAMES, 3) ;
	TASK_CHECK (fclose (f) == 0) ;
	EXPECT_FAILURE (path) ;
	remove (path) ;

	snprintf (path, sizeof (path), "%scoords.caf", pfx) ;
	f = open_fixture (path) ;
	write_desc (f, 3, 16, 0) ;
	write_desc_chan (f, 12 + 3 * 20, CHAN_TAG_DESCRIPTIONS, 3, two_only, 0, 0.5f) ;
	write_short_data (f, pcm, CAF_FRAMES, 3) ;
	TASK_CHECK (fclose (f) == 0) ;
	EXPECT_FAILURE (path) ;
	remove (path) ;

	/* Parent 'chan' chunk claims three descriptions but only has room for two. */
	snprintf (path, sizeof (path), "%scut.caf", pfx) ;
	f = open_fixture (path) ;
	write_desc (f, 3, 16, 0) ;
	write_desc_chan (f, 12 + 2 * 20, CHAN_TAG_DESCRIPTIONS, 3, two_only, 0, 0.0f) ;
	write_short_data (f, pcm, CAF_FRAMES, 3) ;
	TASK_CHECK (fclose (f) == 0) ;
	EXPECT_FAILURE (path) ;

	/* The file ends in the middle of the description array. */
	{	FILE *g = fopen (path, "wb") ;

		TASK_CHECK (g != NULL) ;
		write_marker (g, "caff") ;
		write_be (g, 1, 2) ;
		write_be (g, 0, 2) ;
		write_desc (g, 3, 16, 0) ;
		/* Three descriptions claimed, room for three, but only two written. */
		write_chan_header (g, 12 + 3 * 20, CHAN_TAG_DESCRIPTIONS, 0, 3) ;
		write_be (g, 1, 4) ;
		write_be (g, 0, 4) ;
		write_float_be (g, 0.0f) ;
		write_float_be (g, 0.0f) ;
		write_float_be (g, 0.0f) ;
		write_be (g, 2, 4) ;
		write_be (g, 0, 4) ;
		write_float_be (g, 0.0f) ;
		write_float_be (g, 0.0f) ;
		write_float_be (g, 0.0f) ;
		TASK_CHECK (fclose (g) == 0) ;
		} ;
	EXPECT_FAILURE (path) ;
	remove (path) ;
} /* case_bad_descriptions */

/*------------------------------------------------------------------------------
** Write side behaviour: metadata timing, header updates and virtual I/O.
*/

/* Switching the layout twice before the first frame: the last one wins. */
static void
case_switch_layout (void)
{	SF_INFO info = { 0 } ;
	SNDFILE *s ;
	CAF_WINFO winfo ;
	int first [3] = { SF_CHANNEL_MAP_RIGHT, SF_CHANNEL_MAP_LEFT, SF_CHANNEL_MAP_CENTER } ;
	int second [3] = { SF_CHANNEL_MAP_LEFT, SF_CHANNEL_MAP_RIGHT, SF_CHANNEL_MAP_CENTER } ;
	short pcm [6] = { 10, 20, 30, -40, 50, -60 } ;
	char path [256] ;

	snprintf (path, sizeof (path), "%sswitch.caf", pfx) ;

	info.samplerate = 8000 ;
	info.channels = 3 ;
	info.format = SF_FORMAT_CAF | SF_FORMAT_PCM_16 ;

	s = sf_open (path, SFM_WRITE, &info) ;
	TASK_CHECK (s != NULL) ;
	TASK_CHECK (sf_command (s, SFC_SET_CHANNEL_MAP_INFO, first, sizeof (first)) == SF_TRUE) ;
	TASK_CHECK (sf_command (s, SFC_SET_CHANNEL_MAP_INFO, second, sizeof (second)) == SF_TRUE) ;
	TASK_CHECK (sf_writef_short (s, pcm, CAF_FRAMES) == CAF_FRAMES) ;
	TASK_CHECK (sf_close (s) == 0) ;

	parse_caf_file (path, &winfo) ;
	TASK_CHECK (winfo.chan_count == 1) ;
	TASK_CHECK (winfo.chan_len == 12) ;
	TASK_CHECK (winfo.tag == ((113u << 16) | 3u)) ;
	check_short_data (path, &winfo, pcm, CAF_FRAMES, 3) ;

	remove (path) ;
} /* case_switch_layout */

/* Two writes with an explicit header update in between, then the update on
** close. The 'chan' chunk must appear exactly once and must not eat the audio. */
static void
case_split_write_update (void)
{	SF_INFO info = { 0 } ;
	SNDFILE *s ;
	CAF_WINFO winfo, mid ;
	int map [3] = { SF_CHANNEL_MAP_RIGHT, SF_CHANNEL_MAP_LEFT, SF_CHANNEL_MAP_CENTER } ;
	short pcm [6] = { 10, 20, 30, -40, 50, -60 } ;
	short out [6] = { 0 } ;
	int back [3] = { 0 } ;
	char path [256] ;

	snprintf (path, sizeof (path), "%ssplit.caf", pfx) ;

	info.samplerate = 8000 ;
	info.channels = 3 ;
	info.format = SF_FORMAT_CAF | SF_FORMAT_PCM_16 ;

	s = sf_open (path, SFM_WRITE, &info) ;
	TASK_CHECK (s != NULL) ;
	TASK_CHECK (sf_command (s, SFC_SET_CHANNEL_MAP_INFO, map, sizeof (map)) == SF_TRUE) ;
	TASK_CHECK (sf_writef_short (s, pcm, 1) == 1) ;
	TASK_CHECK (sf_command (s, SFC_UPDATE_HEADER_NOW, NULL, 0) == 0) ;

	/* Mid-stream the header has been rewritten once; still one chan chunk and
	** the first frame untouched. */
	parse_caf_file (path, &mid) ;
	TASK_CHECK (mid.chan_count == 1) ;
	TASK_CHECK (mid.chan_len == 12 + 3 * 20) ;
	check_short_data (path, &mid, pcm, 1, 3) ;

	TASK_CHECK (sf_writef_short (s, pcm + 3, 1) == 1) ;
	TASK_CHECK (sf_close (s) == 0) ;

	parse_caf_file (path, &winfo) ;
	TASK_CHECK (winfo.chan_count == 1) ;
	TASK_CHECK (winfo.desc_count == 3) ;
	TASK_CHECK (winfo.labels [0] == 2 && winfo.labels [1] == 1 && winfo.labels [2] == 3) ;
	check_short_data (path, &winfo, pcm, CAF_FRAMES, 3) ;

	memset (&info, 0, sizeof (info)) ;
	s = sf_open (path, SFM_READ, &info) ;
	TASK_CHECK (s != NULL) ;
	TASK_CHECK (sf_readf_short (s, out, CAF_FRAMES) == CAF_FRAMES) ;
	TASK_CHECK (memcmp (pcm, out, sizeof (pcm)) == 0) ;
	TASK_CHECK (sf_command (s, SFC_GET_CHANNEL_MAP_INFO, back, sizeof (back)) == SF_TRUE) ;
	TASK_CHECK (memcmp (map, back, sizeof (map)) == 0) ;
	TASK_CHECK (sf_close (s) == 0) ;

	remove (path) ;
} /* case_split_write_update */

/* Once audio has been written the channel map can no longer be changed. */
static void
case_reject_after_write (void)
{	SF_INFO info = { 0 } ;
	SNDFILE *s ;
	int map [3] = { SF_CHANNEL_MAP_RIGHT, SF_CHANNEL_MAP_LEFT, SF_CHANNEL_MAP_CENTER } ;
	int other [3] = { SF_CHANNEL_MAP_LEFT, SF_CHANNEL_MAP_RIGHT, SF_CHANNEL_MAP_CENTER } ;
	short pcm [3] = { 10, 20, 30 } ;
	char path [256] ;

	snprintf (path, sizeof (path), "%slate.caf", pfx) ;

	info.samplerate = 8000 ;
	info.channels = 3 ;
	info.format = SF_FORMAT_CAF | SF_FORMAT_PCM_16 ;

	s = sf_open (path, SFM_WRITE, &info) ;
	TASK_CHECK (s != NULL) ;
	TASK_CHECK (sf_command (s, SFC_SET_CHANNEL_MAP_INFO, map, sizeof (map)) == SF_TRUE) ;
	TASK_CHECK (sf_writef_short (s, pcm, 1) == 1) ;
	TASK_CHECK (sf_command (s, SFC_SET_CHANNEL_MAP_INFO, other, sizeof (other)) == SF_FALSE) ;
	TASK_CHECK (sf_close (s) == 0) ;

	remove (path) ;
} /* case_reject_after_write */

/* The same write/read result through a seekable sf_open_virtual stream. */
static void
case_virtual_io (void)
{	SF_INFO info = { 0 } ;
	SF_VIRTUAL_IO vio ;
	MEM_FILE mf = { NULL, 0, 0, 0 } ;
	SNDFILE *s ;
	CAF_WINFO winfo ;
	int map [3] = { SF_CHANNEL_MAP_RIGHT, SF_CHANNEL_MAP_LEFT, SF_CHANNEL_MAP_CENTER } ;
	int back [3] = { 0, 0, 0 } ;
	short pcm [6] = { 10, 20, 30, -40, 50, -60 } ;
	short out [6] = { 0 } ;
	double rate = 8000 ;

	vio.get_filelen = mf_getlen ;
	vio.seek = mf_seek ;
	vio.read = mf_read ;
	vio.write = mf_write ;
	vio.tell = mf_tell ;

	info.samplerate = 8000 ;
	info.channels = 3 ;
	info.format = SF_FORMAT_CAF | SF_FORMAT_PCM_16 ;

	s = sf_open_virtual (&vio, SFM_WRITE, &info, &mf) ;
	TASK_CHECK (s != NULL) ;
	TASK_CHECK (sf_command (s, SFC_SET_CHANNEL_MAP_INFO, map, sizeof (map)) == SF_TRUE) ;
	TASK_CHECK (sf_writef_short (s, pcm, 1) == 1) ;
	TASK_CHECK (sf_command (s, SFC_UPDATE_HEADER_NOW, NULL, 0) == 0) ;
	TASK_CHECK (sf_writef_short (s, pcm + 3, 1) == 1) ;
	TASK_CHECK (sf_close (s) == 0) ;

	parse_caf_bytes (mf.buf, mf.len, &winfo) ;
	TASK_CHECK (winfo.chan_count == 1) ;
	TASK_CHECK (winfo.chan_len == 12 + 3 * 20) ;
	TASK_CHECK (winfo.desc_count == 3) ;
	TASK_CHECK (winfo.labels [0] == 2 && winfo.labels [1] == 1 && winfo.labels [2] == 3) ;
	TASK_CHECK (winfo.data_len == 4 + 6 * 2) ;

	mf.pos = 0 ;
	memset (&info, 0, sizeof (info)) ;
	s = sf_open_virtual (&vio, SFM_READ, &info, &mf) ;
	TASK_CHECK (s != NULL) ;
	TASK_CHECK (info.samplerate == (int) rate) ;
	TASK_CHECK (sf_readf_short (s, out, CAF_FRAMES) == CAF_FRAMES) ;
	TASK_CHECK (memcmp (pcm, out, sizeof (pcm)) == 0) ;
	TASK_CHECK (sf_command (s, SFC_GET_CHANNEL_MAP_INFO, back, sizeof (back)) == SF_TRUE) ;
	TASK_CHECK (memcmp (map, back, sizeof (map)) == 0) ;
	TASK_CHECK (sf_close (s) == 0) ;

	free (mf.buf) ;
} /* case_virtual_io */

/*------------------------------------------------------------------------------
*/

void
task_cases (void)
{	case_predefined_stereo () ;
	case_predefined_priority () ;
	case_descriptions_pcm16 () ;
	case_descriptions_float () ;
	case_read_bitmap () ;
	case_read_descriptions () ;
	case_read_float_descriptions () ;
	case_read_all_labels () ;
	case_read_no_chan () ;
	case_read_unknown_tag () ;
	case_bad_bitmap () ;
	case_bad_descriptions () ;
	case_switch_layout () ;
	case_split_write_update () ;
	case_reject_after_write () ;
	case_virtual_io () ;
} /* task_cases */

/*
** Copyright (C) 2026 Erik de Castro Lopo <erikd@mega-nerd.com>
**
** This program is free software ; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation ; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY ; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program ; if not, write to the Free Software
** Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
*/

/*
** Tests for sf_caf_alac_cut().
**
** The fixtures are produced by the in-tree ALAC encoder. The source PCM, the
** source packet bytes and the source 'pakt' counts are kept separately so the
** cutter can be checked for byte exact packet copies and for closed frame
** accounting.
*/

#include "../task_support/check.h"
#include "sndfile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define	SRATE		44100

/*------------------------------------------------------------------------------
** Small helpers.
*/

typedef struct
{	unsigned char *data ;
	size_t size ;
} BYTES ;

static uint32_t
rd32 (const unsigned char *p)
{	return ((uint32_t) p [0] << 24) | ((uint32_t) p [1] << 16) | ((uint32_t) p [2] << 8) | p [3] ;
} /* rd32 */

static uint64_t
rd64 (const unsigned char *p)
{	uint64_t v = 0 ;
	int k ;

	for (k = 0 ; k < 8 ; k++)
		v = (v << 8) | p [k] ;

	return v ;
} /* rd64 */

static void
wr32 (unsigned char *p, uint32_t v)
{	p [0] = (unsigned char) (v >> 24) ;
	p [1] = (unsigned char) (v >> 16) ;
	p [2] = (unsigned char) (v >> 8) ;
	p [3] = (unsigned char) v ;
} /* wr32 */

static void
wr64 (unsigned char *p, uint64_t v)
{	int k ;

	for (k = 7 ; k >= 0 ; k--)
	{	p [k] = (unsigned char) v ;
		v >>= 8 ;
		} ;
} /* wr64 */

static int
read_file (const char *path, BYTES *out)
{	FILE *file ;
	long len ;

	memset (out, 0, sizeof (*out)) ;
	if ((file = fopen (path, "rb")) == NULL)
		return 0 ;
	if (fseek (file, 0, SEEK_END) != 0)
	{	fclose (file) ;
		return 0 ;
		} ;
	len = ftell (file) ;
	if (len < 0 || fseek (file, 0, SEEK_SET) != 0)
	{	fclose (file) ;
		return 0 ;
		} ;
	out->data = malloc ((size_t) len) ;
	if (out->data == NULL)
	{	fclose (file) ;
		return 0 ;
		} ;
	out->size = (size_t) len ;
	if (fread (out->data, 1, out->size, file) != out->size)
	{	free (out->data) ;
		out->data = NULL ;
		fclose (file) ;
		return 0 ;
		} ;
	fclose (file) ;
	return 1 ;
} /* read_file */

static int
write_file (const char *path, const unsigned char *data, size_t size)
{	FILE *file ;

	if ((file = fopen (path, "wb")) == NULL)
		return 0 ;
	if (fwrite (data, 1, size, file) != size)
	{	fclose (file) ;
		return 0 ;
		} ;
	return fclose (file) == 0 ;
} /* write_file */

static void
free_bytes (BYTES *b)
{	free (b->data) ;
	memset (b, 0, sizeof (*b)) ;
} /* free_bytes */

static int
file_exists (const char *path)
{	FILE *file = fopen (path, "rb") ;

	if (file == NULL)
		return 0 ;
	fclose (file) ;
	return 1 ;
} /* file_exists */

/*------------------------------------------------------------------------------
** Chunk lookup over raw CAF bytes.
*/

typedef struct
{	const unsigned char *body ;
	uint64_t size ;
	long offset ;
} CHUNK ;

static int
find_chunk (const BYTES *file, const char *name, CHUNK *chunk)
{	size_t pos = 8 ;

	memset (chunk, 0, sizeof (*chunk)) ;

	while (pos + 12 <= file->size)
	{	uint64_t size = rd64 (file->data + pos + 4) ;
		uint64_t body = pos + 12 ;

		if (memcmp (file->data + pos, name, 4) == 0)
		{	chunk->body = file->data + body ;
			chunk->size = size ;
			chunk->offset = (long) pos ;
			return 1 ;
			} ;
		if (size > file->size - body)
			return 0 ;
		pos = (size_t) (body + size) ;
		} ;

	return 0 ;
} /* find_chunk */

static int
count_chunk (const BYTES *file, const char *name)
{	size_t pos = 8 ;
	int count = 0 ;

	while (pos + 12 <= file->size)
	{	uint64_t size = rd64 (file->data + pos + 4) ;
		uint64_t body = pos + 12 ;

		if (memcmp (file->data + pos, name, 4) == 0)
			count ++ ;
		if (size > file->size - body)
			break ;
		pos = (size_t) (body + size) ;
		} ;

	return count ;
} /* count_chunk */

static unsigned
decode_varint (const unsigned char *data, uint64_t size, uint64_t *pos)
{	unsigned value = 0 ;
	unsigned char byte ;
	int count = 0 ;

	do
	{	if (*pos >= size || count >= 5)
			return 0 ;
		byte = data [(*pos) ++] ;
		value = (value << 7) | (byte & 0x7F) ;
		count ++ ;
		}
	while (byte & 0x80) ;

	return value ;
} /* decode_varint */

typedef struct
{	uint64_t packets, valid ;
	uint32_t priming, remainder ;
	unsigned sizes [64] ;
} PAKT ;

static int
parse_pakt (const BYTES *file, PAKT *pakt)
{	CHUNK chunk ;
	uint64_t pos = 24 ;
	uint64_t k ;

	memset (pakt, 0, sizeof (*pakt)) ;

	if (! find_chunk (file, "pakt", &chunk) || chunk.size < 24 || chunk.size > 24 + 64 * 5)
		return 0 ;

	pakt->packets = rd64 (chunk.body) ;
	pakt->valid = rd64 (chunk.body + 8) ;
	pakt->priming = rd32 (chunk.body + 16) ;
	pakt->remainder = rd32 (chunk.body + 20) ;

	if (pakt->packets > 64)
		return 0 ;

	for (k = 0 ; k < pakt->packets ; k++)
	{	pakt->sizes [k] = decode_varint (chunk.body, chunk.size, &pos) ;
		if (pakt->sizes [k] == 0)
			return 0 ;
		} ;

	return 1 ;
} /* parse_pakt */

/* Return a pointer to the raw packet area (after the 4 byte edit count). */
static const unsigned char *
data_payload (const BYTES *file, uint64_t *size)
{	CHUNK chunk ;

	if (! find_chunk (file, "data", &chunk) || chunk.size < 4)
		return NULL ;
	*size = chunk.size - 4 ;
	return chunk.body + 4 ;
} /* data_payload */

/*------------------------------------------------------------------------------
** Audio generation.
*/

static void
fill_pcm (short *pcm, int channels, int frames)
{	int k, ch ;

	for (k = 0 ; k < frames ; k++)
		for (ch = 0 ; ch < channels ; ch++)
		{	int v = (ch == 0) ? ((k * 37) % 30011) - 15000 : 12000 - ((k * 53) % 24001) ;
			pcm [k * channels + ch] = (short) v ;
			} ;
} /* fill_pcm */

static int
make_alac (const char *path, int format, int channels, const short *pcm, int frames)
{	SF_INFO info ;
	SNDFILE *file ;

	memset (&info, 0, sizeof (info)) ;
	info.samplerate = SRATE ;
	info.channels = channels ;
	info.format = SF_FORMAT_CAF | format ;

	if ((file = sf_open (path, SFM_WRITE, &info)) == NULL)
		return 0 ;

	if (sf_writef_short (file, pcm, frames) != frames)
	{	sf_close (file) ;
		return 0 ;
		} ;

	return sf_close (file) == 0 ;
} /* make_alac */

static int
patch_pakt_counts (const char *path, uint64_t packets, uint64_t valid, uint32_t priming, uint32_t remainder)
{	BYTES file ;
	CHUNK chunk ;
	int ok = 0 ;

	if (! read_file (path, &file))
		return 0 ;

	if (find_chunk (&file, "pakt", &chunk) && chunk.size >= 24)
	{	unsigned char *body = (unsigned char *) chunk.body ;
		wr64 (body, packets) ;
		wr64 (body + 8, valid) ;
		wr32 (body + 16, priming) ;
		wr32 (body + 20, remainder) ;
		ok = write_file (path, file.data, file.size) ;
		} ;

	free_bytes (&file) ;
	return ok ;
} /* patch_pakt_counts */

static int
patch_pakt_size (const char *path, unsigned index, unsigned value)
{	BYTES file ;
	CHUNK chunk ;
	int ok = 0 ;
	uint64_t pos = 24 ;
	unsigned k ;

	if (! read_file (path, &file))
		return 0 ;

	if (find_chunk (&file, "pakt", &chunk) && chunk.size >= 24)
	{	unsigned char *body = (unsigned char *) chunk.body ;

		for (k = 0 ; k < index ; k++)
		{	unsigned char byte ;
			do
			{	if (pos >= chunk.size)
					break ;
				byte = body [pos ++] ;
				}
			while (byte & 0x80) ;
			} ;

		if (pos < chunk.size)
		{	/* Only one and two byte encodings are needed by these fixtures. */
			if (value < 0x80)
			{	body [pos] = (unsigned char) value ;
				ok = write_file (path, file.data, file.size) ;
				}
			else if (pos + 1 < chunk.size && (body [pos] & 0x80) == 0)
			{	/* Widen in place only when the following byte is free. Pass. */
				ok = 0 ;
				}
			} ;
		} ;

	free_bytes (&file) ;
	return ok ;
} /* patch_pakt_size */

/* Insert a chunk (with its header) just before the 'data' chunk. */
static int
insert_chunk (const char *path, const char *name, const unsigned char *data, size_t size)
{	BYTES file ;
	CHUNK chunk ;
	unsigned char *out ;
	size_t pos, new_size ;
	int ok = 0 ;

	if (! read_file (path, &file))
		return 0 ;
	if (! find_chunk (&file, "data", &chunk))
	{	free_bytes (&file) ;
		return 0 ;
		} ;

	pos = (size_t) chunk.offset ;
	new_size = file.size + 12 + size ;
	if ((out = malloc (new_size)) == NULL)
	{	free_bytes (&file) ;
		return 0 ;
		} ;

	memcpy (out, file.data, pos) ;
	memcpy (out + pos, name, 4) ;
	wr64 (out + pos + 4, size) ;
	memcpy (out + pos + 12, data, size) ;
	memcpy (out + pos + 12 + size, file.data + pos, file.size - pos) ;

	ok = write_file (path, out, new_size) ;
	free (out) ;
	free_bytes (&file) ;
	return ok ;
} /* insert_chunk */

static short *
read_short (const char *path, sf_count_t *frames, int *channels)
{	SF_INFO info ;
	SNDFILE *file ;
	short *pcm ;
	sf_count_t count ;

	memset (&info, 0, sizeof (info)) ;
	if ((file = sf_open (path, SFM_READ, &info)) == NULL)
		return NULL ;

	pcm = malloc ((size_t) (info.frames > 0 ? info.frames : 1) * info.channels * sizeof (short)) ;
	if (pcm == NULL)
	{	sf_close (file) ;
		return NULL ;
		} ;

	count = sf_readf_short (file, pcm, info.frames) ;
	sf_close (file) ;

	*frames = count ;
	*channels = info.channels ;

	if (count != info.frames)
	{	free (pcm) ;
		return NULL ;
		} ;

	return pcm ;
} /* read_short */

static int
compare_window (const short *source, int channels, sf_count_t start, const short *cut, sf_count_t count)
{	sf_count_t k ;

	for (k = 0 ; k < count * channels ; k++)
		if (source [start * channels + k] != cut [k])
			return 0 ;

	return 1 ;
} /* compare_window */

/*------------------------------------------------------------------------------
** Virtual I/O.
*/

typedef struct
{	const unsigned char *data ;
	sf_count_t size, pos ;
	sf_count_t reads ;
} MEM_FILE ;

static sf_count_t
mem_getlen (void *u)
{	return ((MEM_FILE *) u)->size ;
} /* mem_getlen */

static sf_count_t
mem_seek (sf_count_t offset, int whence, void *u)
{	MEM_FILE *m = u ;
	sf_count_t pos ;

	if (whence == SEEK_SET)
		pos = offset ;
	else if (whence == SEEK_CUR)
		pos = m->pos + offset ;
	else if (whence == SEEK_END)
		pos = m->size + offset ;
	else
		return -1 ;

	if (pos < 0 || pos > m->size)
		return -1 ;

	m->pos = pos ;
	return pos ;
} /* mem_seek */

static sf_count_t
mem_read (void *ptr, sf_count_t count, void *u)
{	MEM_FILE *m = u ;

	if (m->pos + count > m->size)
		count = m->size - m->pos ;

	memcpy (ptr, m->data + m->pos, (size_t) count) ;
	m->pos += count ;
	m->reads ++ ;

	return count ;
} /* mem_read */

static sf_count_t
mem_tell (void *u)
{	return ((MEM_FILE *) u)->pos ;
} /* mem_tell */

/*------------------------------------------------------------------------------
** Fixture.
*/

#define	MAX_PACKETS	16

typedef struct
{	char path [128] ;
	int format, channels, frames, priming ;
	short *pcm ;				/* Source PCM, `frames` frames. */
	sf_count_t audible_frames ;
	unsigned char *packets [MAX_PACKETS] ;
	unsigned packet_sizes [MAX_PACKETS] ;
	unsigned packets_count ;
	unsigned decoded_total ;
	uint32_t frames_per_packet ;
} FIXTURE ;

static int
fixture_build (FIXTURE *fx, const char *path, int format, int channels, int frames, int priming)
{	BYTES raw ;
	CHUNK chunk ;
	PAKT pakt ;
	const unsigned char *payload ;
	uint64_t payload_size, offset ;
	unsigned k ;

	memset (fx, 0, sizeof (*fx)) ;
	snprintf (fx->path, sizeof (fx->path), "%s", path) ;
	fx->format = format ;
	fx->channels = channels ;
	fx->frames = frames ;
	fx->priming = priming ;

	fx->pcm = malloc ((size_t) frames * channels * sizeof (short)) ;
	if (fx->pcm == NULL)
		return 0 ;
	fill_pcm (fx->pcm, channels, frames) ;

	if (! make_alac (path, format, channels, fx->pcm, frames))
		return 0 ;
	if (! read_file (path, &raw))
		return 0 ;

	if (! find_chunk (&raw, "desc", &chunk) || chunk.size < 32)
	{	free_bytes (&raw) ;
		return 0 ;
		} ;
	fx->frames_per_packet = rd32 (chunk.body + 20) ;

	if (! parse_pakt (&raw, &pakt) || pakt.packets == 0 || pakt.packets > MAX_PACKETS)
	{	free_bytes (&raw) ;
		return 0 ;
		} ;

	fx->packets_count = (unsigned) pakt.packets ;

	/* The writer stores its partial final packet padding in the remainder
	** field, so the decoded total of the last packet is the written length
	** minus the full packets before it. */
	fx->decoded_total = frames ;

	if (! patch_pakt_counts (path, pakt.packets, (uint64_t) (frames - priming),
			(uint32_t) priming, (uint32_t) (fx->decoded_total - priming - (frames - priming))))
	{	free_bytes (&raw) ;
		return 0 ;
		} ;

	fx->audible_frames = frames - priming ;

	free_bytes (&raw) ;
	if (! read_file (path, &raw))
		return 0 ;

	if (! parse_pakt (&raw, &pakt) || pakt.packets != fx->packets_count)
	{	free_bytes (&raw) ;
		return 0 ;
		} ;

	payload = data_payload (&raw, &payload_size) ;
	if (payload == NULL)
	{	free_bytes (&raw) ;
		return 0 ;
		} ;

	offset = 0 ;
	for (k = 0 ; k < fx->packets_count ; k++)
	{	fx->packet_sizes [k] = pakt.sizes [k] ;
		if (offset + pakt.sizes [k] > payload_size)
		{	free_bytes (&raw) ;
			return 0 ;
			} ;
		fx->packets [k] = malloc (pakt.sizes [k]) ;
		if (fx->packets [k] == NULL)
		{	free_bytes (&raw) ;
			return 0 ;
			} ;
		memcpy (fx->packets [k], payload + offset, pakt.sizes [k]) ;
		offset += pakt.sizes [k] ;
		} ;

	TASK_CHECK (offset == payload_size) ;

	free_bytes (&raw) ;
	return 1 ;
} /* fixture_build */

static void
fixture_free (FIXTURE *fx)
{	unsigned k ;

	for (k = 0 ; k < fx->packets_count ; k++)
		free (fx->packets [k]) ;
	free (fx->pcm) ;
	memset (fx, 0, sizeof (*fx)) ;
} /* fixture_free */

/* Load the packet bytes of a cut file. */
static int
load_cut_packets (const char *path, unsigned char **packets, unsigned *sizes, unsigned *count)
{	BYTES raw ;
	PAKT pakt ;
	const unsigned char *payload ;
	uint64_t payload_size, offset ;
	unsigned k ;

	if (! read_file (path, &raw))
		return 0 ;

	if (! parse_pakt (&raw, &pakt))
	{	free_bytes (&raw) ;
		return 0 ;
		} ;

	payload = data_payload (&raw, &payload_size) ;
	if (payload == NULL || pakt.packets > MAX_PACKETS)
	{	free_bytes (&raw) ;
		return 0 ;
		} ;

	offset = 0 ;
	for (k = 0 ; k < pakt.packets ; k++)
	{	if (offset + pakt.sizes [k] > payload_size)
		{	free_bytes (&raw) ;
			return 0 ;
			} ;
		sizes [k] = pakt.sizes [k] ;
		packets [k] = malloc (pakt.sizes [k]) ;
		if (packets [k] == NULL)
		{	free_bytes (&raw) ;
			return 0 ;
			} ;
		memcpy (packets [k], payload + offset, pakt.sizes [k]) ;
		offset += pakt.sizes [k] ;
		} ;

	*count = (unsigned) pakt.packets ;
	TASK_CHECK (offset == payload_size) ;

	free_bytes (&raw) ;
	return 1 ;
} /* load_cut_packets */

/* Decoded frame of the last packet in the source. */
static unsigned
last_packet_frames (const FIXTURE *fx)
{	unsigned full = (fx->packets_count - 1) * fx->frames_per_packet ;

	TASK_CHECK (fx->decoded_total >= full) ;
	return fx->decoded_total - full ;
} /* last_packet_frames */

/*------------------------------------------------------------------------------
** packets group.
*/

/* Packets that intersect the window are kept byte for byte, and only those. */
static int
case_packet_bytes (void)
{	FIXTURE fx ;
	PAKT pakt ;
	BYTES raw ;
	const unsigned char *payload ;
	uint64_t payload_size, offset ;
	sf_count_t out_frames ;
	int out_channels ;
	short *out ;
	unsigned k ;

	TASK_CHECK (fixture_build (&fx, "caf_cut_pk_in.caf", SF_FORMAT_ALAC_16, 2, 9000, 600)) ;

	/* Decoded window [600 + 3000, 600 + 3000 + 5000) spans all three packets. */
	TASK_CHECK (sf_caf_alac_cut (fx.path, "caf_cut_pk_out.caf", 3000, 5000) == 0) ;
	TASK_CHECK (read_file ("caf_cut_pk_out.caf", &raw)) ;
	TASK_CHECK (parse_pakt (&raw, &pakt)) ;

	TASK_CHECK (pakt.packets == 3) ;
	TASK_CHECK (pakt.valid == 5000) ;
	TASK_CHECK (pakt.priming == 600 + 3000) ;
	TASK_CHECK (pakt.remainder == 9000 - (600 + 3000 + 5000)) ;

	TASK_CHECK (pakt.sizes [0] == fx.packet_sizes [0]) ;
	TASK_CHECK (pakt.sizes [1] == fx.packet_sizes [1]) ;
	TASK_CHECK (pakt.sizes [2] == fx.packet_sizes [2]) ;

	payload = data_payload (&raw, &payload_size) ;
	TASK_CHECK (payload != NULL) ;
	offset = 0 ;
	for (k = 0 ; k < 3 ; k++)
	{	TASK_CHECK (memcmp (payload + offset, fx.packets [k], pakt.sizes [k]) == 0) ;
		offset += pakt.sizes [k] ;
		} ;
	TASK_CHECK (offset == payload_size) ;

	/* Counts close over the real decoded total of the retained packets. */
	TASK_CHECK ((uint64_t) pakt.priming + pakt.valid + pakt.remainder == 9000) ;
	TASK_CHECK (pakt.priming < fx.frames_per_packet) ;
	TASK_CHECK (pakt.remainder < last_packet_frames (&fx)) ;

	free_bytes (&raw) ;

	out = read_short ("caf_cut_pk_out.caf", &out_frames, &out_channels) ;
	TASK_CHECK (out != NULL) ;
	TASK_CHECK (out_channels == 2 && out_frames == 5000) ;
	TASK_CHECK (compare_window (fx.pcm, 2, 600 + 3000, out, 5000)) ;
	free (out) ;

	fixture_free (&fx) ;
	remove ("caf_cut_pk_in.caf") ;
	remove ("caf_cut_pk_out.caf") ;
	return 0 ;
} /* case_packet_bytes */

/* The input's priming may itself span one or more whole packets. */
static int
case_source_priming (void)
{	FIXTURE fx ;
	unsigned char *packets [MAX_PACKETS] ;
	unsigned sizes [MAX_PACKETS], count, k ;
	PAKT pakt ;
	BYTES raw ;
	sf_count_t out_frames ;
	int out_channels ;
	short *out ;

	/* Priming of 5000 frames crosses the first packet and part of the second. */
	TASK_CHECK (fixture_build (&fx, "caf_cut_prim_in.caf", SF_FORMAT_ALAC_16, 2, 11000, 5000)) ;
	TASK_CHECK (fx.priming > (int) fx.frames_per_packet) ;

	/* start 0 sits at decoded frame 5000, i.e. inside the second packet. */
	TASK_CHECK (sf_caf_alac_cut (fx.path, "caf_cut_prim_out.caf", 0, 1500) == 0) ;
	TASK_CHECK (read_file ("caf_cut_prim_out.caf", &raw)) ;
	TASK_CHECK (parse_pakt (&raw, &pakt)) ;
	free_bytes (&raw) ;

	TASK_CHECK (pakt.packets == 1) ;
	TASK_CHECK (pakt.valid == 1500) ;
	TASK_CHECK (pakt.priming == 5000 - fx.frames_per_packet) ;
	TASK_CHECK (pakt.priming < fx.frames_per_packet) ;

	TASK_CHECK (load_cut_packets ("caf_cut_prim_out.caf", packets, sizes, &count)) ;
	TASK_CHECK (count == 1) ;
	for (k = 0 ; k < count ; k++)
	{	TASK_CHECK (sizes [k] == fx.packet_sizes [1 + k]) ;
		TASK_CHECK (memcmp (packets [k], fx.packets [1 + k], sizes [k]) == 0) ;
		free (packets [k]) ;
		} ;

	out = read_short ("caf_cut_prim_out.caf", &out_frames, &out_channels) ;
	TASK_CHECK (out != NULL) ;
	TASK_CHECK (out_frames == 1500 && out_channels == 2) ;
	TASK_CHECK (compare_window (fx.pcm, 2, 5000, out, 1500)) ;
	free (out) ;

	fixture_free (&fx) ;
	remove ("caf_cut_prim_in.caf") ;
	remove ("caf_cut_prim_out.caf") ;
	return 0 ;
} /* case_source_priming */

/* A window ending before the encoder's real last packet is preserved as is,
** and the new remainder is measured against the real short tail. */
static int
case_short_tail (void)
{	FIXTURE fx ;
	PAKT pakt ;
	BYTES raw ;
	unsigned tail ;
	sf_count_t out_frames ;
	int out_channels ;
	short *out ;

	/* 9000 frames at 4096 frames per packet gives a 808 frame last packet. */
	TASK_CHECK (fixture_build (&fx, "caf_cut_tail_in.caf", SF_FORMAT_ALAC_16, 2, 9000, 0)) ;
	tail = last_packet_frames (&fx) ;
	TASK_CHECK (tail == 808) ;

	/* End the window inside the short last packet. */
	TASK_CHECK (sf_caf_alac_cut (fx.path, "caf_cut_tail_out.caf", 8500, 400) == 0) ;
	TASK_CHECK (read_file ("caf_cut_tail_out.caf", &raw)) ;
	TASK_CHECK (parse_pakt (&raw, &pakt)) ;
	free_bytes (&raw) ;

	TASK_CHECK (pakt.packets == 1) ;
	TASK_CHECK (pakt.valid == 400) ;
	TASK_CHECK (pakt.priming == 8500 - 2 * fx.frames_per_packet) ;
	TASK_CHECK (pakt.priming < tail) ;
	TASK_CHECK (pakt.remainder == tail - pakt.priming - 400) ;
	TASK_CHECK (pakt.remainder < tail) ;
	TASK_CHECK (pakt.sizes [0] == fx.packet_sizes [2]) ;

	out = read_short ("caf_cut_tail_out.caf", &out_frames, &out_channels) ;
	TASK_CHECK (out != NULL) ;
	TASK_CHECK (out_frames == 400) ;
	TASK_CHECK (compare_window (fx.pcm, 2, 8500, out, 400)) ;
	free (out) ;

	fixture_free (&fx) ;
	remove ("caf_cut_tail_in.caf") ;
	remove ("caf_cut_tail_out.caf") ;
	return 0 ;
} /* case_short_tail */

/* Consecutive one frame reads must neither skip nor repeat samples. */
static int
case_small_reads (void)
{	FIXTURE fx ;
	SF_INFO info ;
	SNDFILE *file ;
	short frame [2] ;
	sf_count_t k, total = 0 ;
	int ok = 1 ;

	TASK_CHECK (fixture_build (&fx, "caf_cut_small_in.caf", SF_FORMAT_ALAC_16, 2, 9000, 300)) ;
	TASK_CHECK (sf_caf_alac_cut (fx.path, "caf_cut_small_out.caf", 1234, 3000) == 0) ;

	memset (&info, 0, sizeof (info)) ;
	TASK_CHECK ((file = sf_open ("caf_cut_small_out.caf", SFM_READ, &info)) != NULL) ;
	TASK_CHECK (info.frames == 3000) ;

	for (k = 0 ; k < 3000 ; k++)
	{	if (sf_readf_short (file, frame, 1) != 1)
		{	ok = 0 ;
			break ;
			} ;
		if (frame [0] != fx.pcm [(300 + 1234 + total) * 2]
				|| frame [1] != fx.pcm [(300 + 1234 + total) * 2 + 1])
		{	ok = 0 ;
			break ;
			} ;
		total ++ ;
		} ;

	TASK_CHECK (ok) ;
	TASK_CHECK (total == 3000) ;
	TASK_CHECK (sf_readf_short (file, frame, 1) == 0) ;
	sf_close (file) ;

	fixture_free (&fx) ;
	remove ("caf_cut_small_in.caf") ;
	remove ("caf_cut_small_out.caf") ;
	return 0 ;
} /* case_small_reads */

/* short, int, float and double reads must all agree. */
static int
case_four_outputs (void)
{	FIXTURE fx ;
	SF_INFO info ;
	SNDFILE *file ;
	short *s ;
	int *i ;
	float *f ;
	double *d ;
	sf_count_t k, n = 700 ;
	int ok = 1 ;

	TASK_CHECK (fixture_build (&fx, "caf_cut_four_in.caf", SF_FORMAT_ALAC_16, 2, 9000, 111)) ;
	TASK_CHECK (sf_caf_alac_cut (fx.path, "caf_cut_four_out.caf", 2000, n) == 0) ;

	s = malloc ((size_t) n * 2 * sizeof (short)) ;
	i = malloc ((size_t) n * 2 * sizeof (int)) ;
	f = malloc ((size_t) n * 2 * sizeof (float)) ;
	d = malloc ((size_t) n * 2 * sizeof (double)) ;
	TASK_CHECK (s != NULL && i != NULL && f != NULL && d != NULL) ;

	memset (&info, 0, sizeof (info)) ;
	TASK_CHECK ((file = sf_open ("caf_cut_four_out.caf", SFM_READ, &info)) != NULL) ;
	TASK_CHECK (sf_readf_short (file, s, n) == n) ;
	sf_close (file) ;

	TASK_CHECK ((file = sf_open ("caf_cut_four_out.caf", SFM_READ, &info)) != NULL) ;
	TASK_CHECK (sf_readf_int (file, i, n) == n) ;
	sf_close (file) ;

	TASK_CHECK ((file = sf_open ("caf_cut_four_out.caf", SFM_READ, &info)) != NULL) ;
	TASK_CHECK (sf_readf_float (file, f, n) == n) ;
	sf_close (file) ;

	TASK_CHECK ((file = sf_open ("caf_cut_four_out.caf", SFM_READ, &info)) != NULL) ;
	TASK_CHECK (sf_readf_double (file, d, n) == n) ;
	sf_close (file) ;

	for (k = 0 ; k < n * 2 ; k++)
	{	short src = fx.pcm [(111 + 2000) * 2 + k] ;

		if (s [k] != src)
			ok = 0 ;
		if (i [k] != (int) src * 65536)
			ok = 0 ;
		if (f [k] != (float) src / 32768.0f)
			ok = 0 ;
		if (d [k] != (double) src / 32768.0)
			ok = 0 ;
		} ;
	TASK_CHECK (ok) ;

	free (s) ;
	free (i) ;
	free (f) ;
	free (d) ;
	fixture_free (&fx) ;
	remove ("caf_cut_four_in.caf") ;
	remove ("caf_cut_four_out.caf") ;
	return 0 ;
} /* case_four_outputs */

/* The cut file must read correctly through sf_open_virtual(). */
static int
case_virtual_io (void)
{	FIXTURE fx ;
	BYTES raw ;
	MEM_FILE mem ;
	SF_VIRTUAL_IO vio ;
	SF_INFO info ;
	SNDFILE *file ;
	short *out ;
	sf_count_t out_frames ;

	TASK_CHECK (fixture_build (&fx, "caf_cut_vio_in.caf", SF_FORMAT_ALAC_16, 2, 9000, 250)) ;
	TASK_CHECK (sf_caf_alac_cut (fx.path, "caf_cut_vio_out.caf", 3333, 1200) == 0) ;
	TASK_CHECK (read_file ("caf_cut_vio_out.caf", &raw)) ;

	mem.data = raw.data ;
	mem.size = (sf_count_t) raw.size ;
	mem.pos = 0 ;
	mem.reads = 0 ;

	vio.get_filelen = mem_getlen ;
	vio.seek = mem_seek ;
	vio.read = mem_read ;
	vio.write = NULL ;
	vio.tell = mem_tell ;

	memset (&info, 0, sizeof (info)) ;
	TASK_CHECK ((file = sf_open_virtual (&vio, SFM_READ, &info, &mem)) != NULL) ;
	TASK_CHECK (info.frames == 1200 && info.channels == 2 && info.seekable) ;

	out = malloc ((size_t) 1200 * 2 * sizeof (short)) ;
	TASK_CHECK (out != NULL) ;
	out_frames = sf_readf_short (file, out, 1200) ;
	TASK_CHECK (out_frames == 1200) ;
	TASK_CHECK (mem.reads > 0) ;
	sf_close (file) ;

	TASK_CHECK (compare_window (fx.pcm, 2, 250 + 3333, out, 1200)) ;

	free (out) ;
	free_bytes (&raw) ;
	fixture_free (&fx) ;
	remove ("caf_cut_vio_in.caf") ;
	remove ("caf_cut_vio_out.caf") ;
	return 0 ;
} /* case_virtual_io */

/* Repeated seeks around packet edges must stay on the audible timeline. */
static int
case_repeat_seek (void)
{	FIXTURE fx ;
	SF_INFO info ;
	SNDFILE *file ;
	short frame [2] ;
	sf_count_t fpp, k ;
	int ok = 1 ;

	TASK_CHECK (fixture_build (&fx, "caf_cut_seek_in.caf", SF_FORMAT_ALAC_16, 2, 12000, 700)) ;
	fpp = fx.frames_per_packet ;
	TASK_CHECK (sf_caf_alac_cut (fx.path, "caf_cut_seek_out.caf", 3000, 6000) == 0) ;

	memset (&info, 0, sizeof (info)) ;
	TASK_CHECK ((file = sf_open ("caf_cut_seek_out.caf", SFM_READ, &info)) != NULL) ;
	TASK_CHECK (info.frames == 6000) ;

	/* Inside a packet, at a packet boundary, back to zero and from the end. */
	{
		sf_count_t starts [] = { 17, fpp, fpp + 5, 0, 5999, 4096, 1 } ;

		for (k = 0 ; k < (sf_count_t) (sizeof (starts) / sizeof (starts [0])) ; k++)
		{	sf_count_t pos = starts [k] ;
			sf_count_t base = (700 + 3000) + pos ;

			if (pos >= info.frames)
				continue ;

			if (pos == 5999)
			{	TASK_CHECK (sf_seek (file, -1, SEEK_END) == 5999) ;
				}
			else
			{	TASK_CHECK (sf_seek (file, pos, SEEK_SET) == pos) ;
				} ;

			if (sf_readf_short (file, frame, 1) != 1)
			{	ok = 0 ;
				break ;
				} ;
			if (frame [0] != fx.pcm [base * 2] || frame [1] != fx.pcm [base * 2 + 1])
			{	ok = 0 ;
				break ;
				} ;
			} ;
	}

	/* Seek to the end and read nothing. */
	TASK_CHECK (sf_seek (file, 0, SEEK_END) == info.frames) ;
	TASK_CHECK (sf_readf_short (file, frame, 1) == 0) ;
	TASK_CHECK (sf_seek (file, 0, SEEK_SET) == 0) ;
	TASK_CHECK (sf_readf_short (file, frame, 1) == 1) ;
	TASK_CHECK (frame [0] == fx.pcm [(700 + 3000) * 2]) ;

	sf_close (file) ;
	TASK_CHECK (ok) ;

	fixture_free (&fx) ;
	remove ("caf_cut_seek_in.caf") ;
	remove ("caf_cut_seek_out.caf") ;
	return 0 ;
} /* case_repeat_seek */

/* Cutting a cut must agree with cutting the original. */
static int
case_second_cut (void)
{	FIXTURE fx ;
	short *first, *second ;
	sf_count_t first_frames, second_frames ;
	int first_channels, second_channels ;

	TASK_CHECK (fixture_build (&fx, "caf_cut_twice_in.caf", SF_FORMAT_ALAC_16, 2, 10000, 900)) ;

	TASK_CHECK (sf_caf_alac_cut (fx.path, "caf_cut_twice_mid.caf", 1000, 5000) == 0) ;
	first = read_short ("caf_cut_twice_mid.caf", &first_frames, &first_channels) ;
	TASK_CHECK (first != NULL) ;
	TASK_CHECK (first_frames == 5000) ;
	TASK_CHECK (compare_window (fx.pcm, 2, 900 + 1000, first, 5000)) ;

	TASK_CHECK (sf_caf_alac_cut ("caf_cut_twice_mid.caf", "caf_cut_twice_out.caf", 1200, 2500) == 0) ;
	second = read_short ("caf_cut_twice_out.caf", &second_frames, &second_channels) ;
	TASK_CHECK (second != NULL) ;
	TASK_CHECK (second_frames == 2500 && second_channels == 2) ;
	TASK_CHECK (compare_window (fx.pcm, 2, 900 + 1000 + 1200, second, 2500)) ;
	TASK_CHECK (memcmp (second, first + 1200 * 2, 2500 * 2 * sizeof (short)) == 0) ;

	free (first) ;
	free (second) ;
	fixture_free (&fx) ;
	remove ("caf_cut_twice_in.caf") ;
	remove ("caf_cut_twice_mid.caf") ;
	remove ("caf_cut_twice_out.caf") ;
	return 0 ;
} /* case_second_cut */

/*------------------------------------------------------------------------------
** window group.
*/

/* Several windows, mono and stereo, 16 and 24 bit, must match the source. */
static int
case_window_audio (void)
{	static const struct { int format, channels, frames, priming, start, count ; } wins [] =
	{	{ SF_FORMAT_ALAC_16, 2, 9000, 0, 0, 1 },
		{ SF_FORMAT_ALAC_16, 2, 9000, 500, 100, 4096 },
		{ SF_FORMAT_ALAC_24, 1, 9000, 300, 4095, 2 },
		{ SF_FORMAT_ALAC_24, 2, 12000, 7000, 0, 4000 },
		{ SF_FORMAT_ALAC_16, 1, 5000, 0, 4200, 800 },
	} ;
	size_t w ;

	for (w = 0 ; w < sizeof (wins) / sizeof (wins [0]) ; w++)
	{	FIXTURE fx ;
		short *out ;
		sf_count_t out_frames ;
		int out_channels ;

		TASK_CHECK (fixture_build (&fx, "caf_cut_win_in.caf", wins [w].format, wins [w].channels,
				wins [w].frames, wins [w].priming)) ;
		TASK_CHECK (sf_caf_alac_cut (fx.path, "caf_cut_win_out.caf", wins [w].start, wins [w].count) == 0) ;

		out = read_short ("caf_cut_win_out.caf", &out_frames, &out_channels) ;
		TASK_CHECK (out != NULL) ;
		TASK_CHECK (out_frames == wins [w].count) ;
		TASK_CHECK (out_channels == wins [w].channels) ;
		TASK_CHECK (compare_window (fx.pcm, wins [w].channels, wins [w].priming + wins [w].start, out, wins [w].count)) ;
		free (out) ;

		fixture_free (&fx) ;
		remove ("caf_cut_win_in.caf") ;
		remove ("caf_cut_win_out.caf") ;
		} ;

	return 0 ;
} /* case_window_audio */

/* The new counts close over the retained packets and stay in bounds. */
static int
case_window_counts (void)
{	FIXTURE fx ;
	PAKT pakt ;
	BYTES raw ;
	unsigned k ;
	uint64_t total, first_decoded, last_decoded, packet_start, packet_frames ;

	TASK_CHECK (fixture_build (&fx, "caf_cut_cnt_in.caf", SF_FORMAT_ALAC_16, 2, 9000, 600)) ;
	TASK_CHECK (sf_caf_alac_cut (fx.path, "caf_cut_cnt_out.caf", 100, 5000) == 0) ;
	TASK_CHECK (read_file ("caf_cut_cnt_out.caf", &raw)) ;
	TASK_CHECK (parse_pakt (&raw, &pakt)) ;
	free_bytes (&raw) ;

	/* Window maps to decoded frames [700, 5700), i.e. packets 0 and 1. */
	first_decoded = 600 + 100 ;
	last_decoded = first_decoded + 5000 ;

	/* Both retained packets are full because neither is the source's last. */
	packet_start = (first_decoded / fx.frames_per_packet) * fx.frames_per_packet ;
	packet_frames = (unsigned) ((last_decoded - 1) / fx.frames_per_packet + 1
			- first_decoded / fx.frames_per_packet) * fx.frames_per_packet ;
	total = packet_frames ;

	TASK_CHECK (pakt.packets == 2) ;
	TASK_CHECK (pakt.valid == 5000) ;
	TASK_CHECK (pakt.priming == first_decoded - packet_start) ;
	TASK_CHECK ((uint64_t) pakt.priming + pakt.valid + pakt.remainder == total) ;
	TASK_CHECK (pakt.priming < fx.frames_per_packet) ;
	TASK_CHECK (pakt.remainder < fx.frames_per_packet) ;

	/* Only the packets intersecting the window are present. */
	TASK_CHECK (pakt.packets * fx.frames_per_packet >= pakt.priming + pakt.valid + pakt.remainder) ;

	/* The retained bytes are exactly those two source packets. */
	{
		unsigned char *packets [MAX_PACKETS] ;
		unsigned sizes [MAX_PACKETS], count ;

		TASK_CHECK (load_cut_packets ("caf_cut_cnt_out.caf", packets, sizes, &count)) ;
		TASK_CHECK (count == 2) ;
		for (k = 0 ; k < count ; k++)
		{	TASK_CHECK (sizes [k] == fx.packet_sizes [k]) ;
			TASK_CHECK (memcmp (packets [k], fx.packets [k], sizes [k]) == 0) ;
			free (packets [k]) ;
			} ;
	}

	fixture_free (&fx) ;
	remove ("caf_cut_cnt_in.caf") ;
	remove ("caf_cut_cnt_out.caf") ;
	return 0 ;
} /* case_window_counts */

/* A window ending exactly on the audio end must not drag in extra packets. */
static int
case_window_end (void)
{	FIXTURE fx ;
	PAKT pakt ;
	BYTES raw ;
	unsigned char *packets [MAX_PACKETS] ;
	unsigned sizes [MAX_PACKETS], count ;
	unsigned k ;

	TASK_CHECK (fixture_build (&fx, "caf_cut_end_in.caf", SF_FORMAT_ALAC_16, 2, 9000, 400)) ;
	TASK_CHECK (sf_caf_alac_cut (fx.path, "caf_cut_end_out.caf", 0, 8600) == 0) ;
	TASK_CHECK (read_file ("caf_cut_end_out.caf", &raw)) ;
	TASK_CHECK (parse_pakt (&raw, &pakt)) ;
	free_bytes (&raw) ;

	/* 400 priming + 8600 frames is the whole 9000 frame file. */
	TASK_CHECK (pakt.packets == fx.packets_count) ;
	TASK_CHECK (pakt.valid == 8600) ;
	TASK_CHECK (pakt.priming == 400) ;
	TASK_CHECK (pakt.remainder == 9000 - 400 - 8600) ;

	TASK_CHECK (load_cut_packets ("caf_cut_end_out.caf", packets, sizes, &count)) ;
	TASK_CHECK (count == fx.packets_count) ;
	for (k = 0 ; k < count ; k++)
	{	TASK_CHECK (memcmp (packets [k], fx.packets [k], sizes [k]) == 0) ;
		free (packets [k]) ;
		} ;

	fixture_free (&fx) ;
	remove ("caf_cut_end_in.caf") ;
	remove ("caf_cut_end_out.caf") ;
	return 0 ;
} /* case_window_end */

/* Optional chunks survive while 'free' chunks are dropped. */
static int
case_chunk_preservation (void)
{	FIXTURE fx ;
	BYTES in_raw, out_raw ;
	CHUNK chunk ;
	static const unsigned char info_body [] =
	{	0, 0, 0, 1,		/* one string */
		't', 'i', 't', 'l', 'e', 0, 'e', 'x', 'c', 'e', 'r', 'p', 't', 0 } ;
	static const unsigned char free_body [8] = { 0 } ;
	static const unsigned char chan_body [12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 } ;

	TASK_CHECK (fixture_build (&fx, "caf_cut_chunk_in.caf", SF_FORMAT_ALAC_16, 2, 9000, 0)) ;

	/* The writer already emits a 'free' chunk for a non-ALAC file; for ALAC it
	** does not, so add one explicitly along with 'chan' and 'info'. */
	TASK_CHECK (insert_chunk (fx.path, "free", free_body, sizeof (free_body))) ;
	TASK_CHECK (insert_chunk (fx.path, "chan", chan_body, sizeof (chan_body))) ;
	TASK_CHECK (insert_chunk (fx.path, "info", info_body, sizeof (info_body))) ;

	TASK_CHECK (read_file (fx.path, &in_raw)) ;
	TASK_CHECK (count_chunk (&in_raw, "free") == 1) ;
	free_bytes (&in_raw) ;

	TASK_CHECK (sf_caf_alac_cut (fx.path, "caf_cut_chunk_out.caf", 100, 2000) == 0) ;
	TASK_CHECK (read_file ("caf_cut_chunk_out.caf", &out_raw)) ;

	TASK_CHECK (count_chunk (&out_raw, "free") == 0) ;
	TASK_CHECK (count_chunk (&out_raw, "chan") == 1) ;
	TASK_CHECK (count_chunk (&out_raw, "info") == 1) ;
	TASK_CHECK (count_chunk (&out_raw, "desc") == 1) ;
	TASK_CHECK (count_chunk (&out_raw, "kuki") == 1) ;
	TASK_CHECK (count_chunk (&out_raw, "pakt") == 1) ;
	TASK_CHECK (count_chunk (&out_raw, "data") == 1) ;

	TASK_CHECK (find_chunk (&out_raw, "info", &chunk)) ;
	TASK_CHECK (chunk.size == sizeof (info_body)) ;
	TASK_CHECK (memcmp (chunk.body, info_body, sizeof (info_body)) == 0) ;

	TASK_CHECK (find_chunk (&out_raw, "chan", &chunk)) ;
	TASK_CHECK (chunk.size == sizeof (chan_body)) ;
	TASK_CHECK (memcmp (chunk.body, chan_body, sizeof (chan_body)) == 0) ;

	free_bytes (&out_raw) ;
	fixture_free (&fx) ;
	remove ("caf_cut_chunk_in.caf") ;
	remove ("caf_cut_chunk_out.caf") ;
	return 0 ;
} /* case_chunk_preservation */

/*------------------------------------------------------------------------------
** invalid group.
*/

/* Parameter and target path validation. */
static int
case_reject_args (void)
{	FIXTURE fx ;
	static const char *out = "caf_cut_args_out.caf" ;

	TASK_CHECK (fixture_build (&fx, "caf_cut_args_in.caf", SF_FORMAT_ALAC_16, 2, 5000, 0)) ;

	TASK_CHECK (sf_caf_alac_cut (fx.path, out, 0, 0) != 0) ;
	TASK_CHECK (! file_exists (out)) ;
	TASK_CHECK (sf_caf_alac_cut (fx.path, out, 0, -1) != 0) ;
	TASK_CHECK (! file_exists (out)) ;
	TASK_CHECK (sf_caf_alac_cut (fx.path, out, -1, 10) != 0) ;
	TASK_CHECK (! file_exists (out)) ;
	TASK_CHECK (sf_caf_alac_cut (fx.path, (char *) fx.path, 0, 10) != 0) ;

	/* Range past the audible end. */
	TASK_CHECK (sf_caf_alac_cut (fx.path, out, 4900, 200) != 0) ;
	TASK_CHECK (! file_exists (out)) ;

	/* Target already present. */
	TASK_CHECK (sf_caf_alac_cut (fx.path, out, 0, 10) == 0) ;
	TASK_CHECK (file_exists (out)) ;
	TASK_CHECK (sf_caf_alac_cut (fx.path, out, 0, 10) != 0) ;

	fixture_free (&fx) ;
	remove ("caf_cut_args_in.caf") ;
	remove (out) ;
	return 0 ;
} /* case_reject_args */

/* Broken 'pakt' totals must fail before an output file appears. */
static int
case_bad_counts (void)
{	FIXTURE fx ;
	BYTES raw ;
	PAKT pakt ;
	const char *out = "caf_cut_badcnt_out.caf" ;

	TASK_CHECK (fixture_build (&fx, "caf_cut_badcnt_in.caf", SF_FORMAT_ALAC_16, 2, 9000, 300)) ;
	TASK_CHECK (read_file (fx.path, &raw)) ;
	TASK_CHECK (parse_pakt (&raw, &pakt)) ;
	free_bytes (&raw) ;

	/* Valid frames too large for the packets. */
	TASK_CHECK (patch_pakt_counts (fx.path, pakt.packets, 9000, 300, 0)) ;
	TASK_CHECK (sf_caf_alac_cut (fx.path, out, 0, 10) != 0) ;
	TASK_CHECK (! file_exists (out)) ;

	/* Priming that walks off the decoded range. */
	TASK_CHECK (patch_pakt_counts (fx.path, pakt.packets, 4000, 9000, 0)) ;
	TASK_CHECK (sf_caf_alac_cut (fx.path, out, 0, 10) != 0) ;
	TASK_CHECK (! file_exists (out)) ;

	/* Counts that are not closed by either accepted shape. */
	TASK_CHECK (patch_pakt_counts (fx.path, pakt.packets, 4000, 100, 100)) ;
	TASK_CHECK (sf_caf_alac_cut (fx.path, out, 0, 10) != 0) ;
	TASK_CHECK (! file_exists (out)) ;

	fixture_free (&fx) ;
	remove ("caf_cut_badcnt_in.caf") ;
	remove (out) ;
	return 0 ;
} /* case_bad_counts */

/* A packet table whose sizes do not cover the data chunk must fail. */
static int
case_bad_packet_table (void)
{	FIXTURE fx ;
	const char *out = "caf_cut_badpkt_out.caf" ;
	BYTES raw ;
	PAKT pakt ;

	TASK_CHECK (fixture_build (&fx, "caf_cut_badpkt_in.caf", SF_FORMAT_ALAC_16, 2, 9000, 0)) ;
	TASK_CHECK (read_file (fx.path, &raw)) ;
	TASK_CHECK (parse_pakt (&raw, &pakt)) ;
	free_bytes (&raw) ;

	/* Zero length packet. */
	TASK_CHECK (patch_pakt_size (fx.path, 0, 0)) ;
	TASK_CHECK (sf_caf_alac_cut (fx.path, out, 0, 10) != 0) ;
	TASK_CHECK (! file_exists (out)) ;

	/* A packet size that no longer adds up to the data payload. */
	TASK_CHECK (patch_pakt_size (fx.path, 0, 40)) ;
	TASK_CHECK (sf_caf_alac_cut (fx.path, out, 0, 10) != 0) ;
	TASK_CHECK (! file_exists (out)) ;

	fixture_free (&fx) ;
	remove ("caf_cut_badpkt_in.caf") ;
	remove (out) ;
	return 0 ;
} /* case_bad_packet_table */

/* Formats outside the supported set must fail. */
static int
case_unsupported_format (void)
{	short pcm [3 * 500] ;
	const char *out = "caf_cut_fmt_out.caf" ;

	fill_pcm (pcm, 3, 500) ;

	/* Three channels is unsupported even though ALAC can carry it. */
	TASK_CHECK (make_alac ("caf_cut_fmt_in.caf", SF_FORMAT_ALAC_16, 3, pcm, 500)) ;
	TASK_CHECK (sf_caf_alac_cut ("caf_cut_fmt_in.caf", out, 0, 10) != 0) ;
	TASK_CHECK (! file_exists (out)) ;
	remove ("caf_cut_fmt_in.caf") ;

	/* 20 bit ALAC is outside the supported set. */
	TASK_CHECK (make_alac ("caf_cut_fmt_in.caf", SF_FORMAT_ALAC_20, 2, pcm, 500)) ;
	TASK_CHECK (sf_caf_alac_cut ("caf_cut_fmt_in.caf", out, 0, 10) != 0) ;
	TASK_CHECK (! file_exists (out)) ;
	remove ("caf_cut_fmt_in.caf") ;

	/* A non-CAF file is rejected too. */
	{
		SF_INFO info ;
		SNDFILE *file ;

		memset (&info, 0, sizeof (info)) ;
		info.samplerate = SRATE ;
		info.channels = 2 ;
		info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16 ;
		TASK_CHECK ((file = sf_open ("caf_cut_fmt_in.wav", SFM_WRITE, &info)) != NULL) ;
		TASK_CHECK (sf_writef_short (file, pcm, 500) == 500) ;
		TASK_CHECK (sf_close (file) == 0) ;

		TASK_CHECK (sf_caf_alac_cut ("caf_cut_fmt_in.wav", out, 0, 10) != 0) ;
		TASK_CHECK (! file_exists (out)) ;
		remove ("caf_cut_fmt_in.wav") ;
		} ;

	/* Missing input. */
	TASK_CHECK (sf_caf_alac_cut ("caf_cut_no_such_file.caf", out, 0, 10) != 0) ;
	TASK_CHECK (! file_exists (out)) ;

	return 0 ;
} /* case_unsupported_format */

/*------------------------------------------------------------------------------
** Registration.
*/

const struct task_case task_cases [] =
{	{ "packets", "packet_bytes", case_packet_bytes },
	{ "packets", "source_priming", case_source_priming },
	{ "packets", "short_tail", case_short_tail },
	{ "packets", "small_reads", case_small_reads },
	{ "packets", "four_outputs", case_four_outputs },
	{ "packets", "virtual_io", case_virtual_io },
	{ "packets", "repeat_seek", case_repeat_seek },
	{ "packets", "second_cut", case_second_cut },

	{ "window", "window_audio", case_window_audio },
	{ "window", "window_counts", case_window_counts },
	{ "window", "window_end", case_window_end },
	{ "window", "chunk_preservation", case_chunk_preservation },

	{ "invalid", "reject_args", case_reject_args },
	{ "invalid", "bad_counts", case_bad_counts },
	{ "invalid", "bad_packet_table", case_bad_packet_table },
	{ "invalid", "unsupported_format", case_unsupported_format },
} ;

const size_t task_case_count = sizeof (task_cases) / sizeof (task_cases [0]) ;

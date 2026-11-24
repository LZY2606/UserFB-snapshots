/*-
 * Copyright (c) 2026
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Read-side support for ELF sections carrying the SHF_COMPRESSED flag
 * (see task_support/compression-contract.md, ELF gABI section 3.7).
 *
 * A compressed section's physical bytes start with an ELF32 or ELF64
 * compression header whose integer fields use the containing object's
 * byte order, followed by one complete zlib stream.  Materialization
 * decompresses the stream into a freshly allocated buffer holding the
 * logical file-format bytes; the regular section-type translation then
 * runs on that buffer through the normal elf_getdata() path.
 */

#include <sys/cdefs.h>

#include <assert.h>
#include <gelf.h>
#include <libelf.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "_libelf.h"
#include "zlib.h"

ELFTC_VCSID("$Id$");

/*@ELFTC-DOWNSTREAM-VCSID@*/

#define	LIBELF_ELFCOMPRESS_ZLIB	1U

#define	LIBELF_COMPRESS_HDR32_SIZE	12U
#define	LIBELF_COMPRESS_HDR64_SIZE	24U

int
_libelf_section_is_compressed(Elf_Scn *s)
{
	Elf *e;
	uint64_t sh_flags;
	uint32_t sh_type;
	int elfclass;

	if (s == NULL || (e = s->s_elf) == NULL)
		return (0);

	elfclass = e->e_class;
	assert(elfclass == ELFCLASS32 || elfclass == ELFCLASS64);

	if (elfclass == ELFCLASS32) {
		sh_flags = (uint64_t) s->s_shdr.s_shdr32.sh_flags;
		sh_type  = s->s_shdr.s_shdr32.sh_type;
	} else {
		sh_flags = s->s_shdr.s_shdr64.sh_flags;
		sh_type  = s->s_shdr.s_shdr64.sh_type;
	}

	if ((sh_flags & SHF_COMPRESSED) == 0)
		return (0);

	/*
	 * SHF_COMPRESSED may not be combined with SHF_ALLOC, and the
	 * SHT_NOBITS section type has no physical bytes to compress.
	 */
	if ((sh_flags & SHF_ALLOC) != 0 || sh_type == SHT_NOBITS) {
		LIBELF_SET_ERROR(DATA, 0);
		return (-1);
	}

	return (1);
}

static uint32_t
_libelf_read_u32(const unsigned char *p, int swap)
{
	uint32_t v;

	if (!swap)
		memcpy(&v, p, sizeof(v));
	else {
		v = ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) |
		    ((uint32_t) p[2] << 8) | (uint32_t) p[3];
	}
	return (v);
}

static uint64_t
_libelf_read_u64(const unsigned char *p, int swap)
{
	uint64_t v;

	if (!swap)
		memcpy(&v, p, sizeof(v));
	else {
		v = 0;
		for (int i = 0; i < 8; i++)
			v = (v << 8) | (uint64_t) p[i];
	}
	return (v);
}

/*
 * Validate the compression header and return the logical (decompressed)
 * size in '*logical_sizep' and required alignment in '*alignp'.
 */
static int
_libelf_parse_compression_header(Elf *e, const unsigned char *phys,
    uint64_t phys_size, uint64_t *logical_sizep, uint64_t *alignp)
{
	int elfclass, swap;
	uint32_t ch_type;
	uint64_t ch_size, ch_align;
	size_t hdr_size;

	elfclass = e->e_class;
	swap = e->e_byteorder != LIBELF_PRIVATE(byteorder);

	if (elfclass == ELFCLASS32) {
		hdr_size = LIBELF_COMPRESS_HDR32_SIZE;
		if (phys_size < hdr_size) {
			LIBELF_SET_ERROR(DATA, 0);
			return (0);
		}
		ch_type  = _libelf_read_u32(phys + 0, swap);
		ch_size  = (uint64_t) _libelf_read_u32(phys + 4, swap);
		ch_align = (uint64_t) _libelf_read_u32(phys + 8, swap);
	} else {
		hdr_size = LIBELF_COMPRESS_HDR64_SIZE;
		if (phys_size < hdr_size) {
			LIBELF_SET_ERROR(DATA, 0);
			return (0);
		}
		ch_type = _libelf_read_u32(phys + 0, swap);
		/* The reserved word must be zero. */
		if (_libelf_read_u32(phys + 4, swap) != 0) {
			LIBELF_SET_ERROR(DATA, 0);
			return (0);
		}
		ch_size  = _libelf_read_u64(phys + 8, swap);
		ch_align = _libelf_read_u64(phys + 16, swap);
	}

	if (ch_type != LIBELF_ELFCOMPRESS_ZLIB) {
		LIBELF_SET_ERROR(UNIMPL, 0);
		return (0);
	}

	/* Alignment: zero, one, or a power of two. */
	if (ch_align != 0 && ch_align != 1 &&
	    (ch_align & (ch_align - 1)) != 0) {
		LIBELF_SET_ERROR(DATA, 0);
		return (0);
	}

	/* The declared logical size must fit in a host size_t. */
	if (ch_size > SIZE_MAX) {
		LIBELF_SET_ERROR(RANGE, 0);
		return (0);
	}

	*logical_sizep = ch_size;
	*alignp = ch_align;
	return (1);
}

/*
 * Inflate exactly 'logical_size' bytes from the single zlib stream
 * occupying 'stream'..'stream + stream_size', requiring that the whole
 * physical region be consumed by the one stream.
 */
static unsigned char *
_libelf_inflate(const unsigned char *stream, uint64_t stream_size,
    size_t logical_size)
{
	z_stream zs;
	unsigned char *buf;
	int zerr;
	uint64_t in_left;
	size_t out_left;

	if (stream_size == 0 || stream_size > SIZE_MAX) {
		LIBELF_SET_ERROR(DATA, 0);
		return (NULL);
	}

	if ((buf = malloc(logical_size ? logical_size : 1)) == NULL) {
		LIBELF_SET_ERROR(RESOURCE, 0);
		return (NULL);
	}

	memset(&zs, 0, sizeof(zs));
	zerr = inflateInit(&zs);
	if (zerr != Z_OK) {
		free(buf);
		LIBELF_SET_ERROR(RESOURCE, 0);
		return (NULL);
	}

	zs.next_in = (Bytef *)(uintptr_t) stream;
	zs.avail_in = 0;
	zs.next_out = buf;
	zs.avail_out = 0;
	in_left = stream_size;
	out_left = logical_size;

	/*
	 * Feed the bounded input and output in chunks so that neither
	 * dimension depends on the 32 bit width of zlib's uInt counters.
	 * A well-formed stream reaches Z_STREAM_END exactly when both the
	 * input region and the output budget have been consumed.
	 */
	do {
		if (zs.avail_in == 0 && in_left > 0) {
			uInt n = (in_left > UINT_MAX) ? UINT_MAX :
			    (uInt) in_left;
			zs.avail_in = n;
			in_left -= n;
		}
		if (zs.avail_out == 0 && out_left > 0) {
			uInt n = (out_left > UINT_MAX) ? UINT_MAX :
			    (uInt) out_left;
			zs.avail_out = n;
			out_left -= n;
		}
		zerr = inflate(&zs, Z_FINISH);
	} while (zerr == Z_OK);

	if (zerr != Z_STREAM_END || zs.total_out != logical_size ||
	    zs.total_in != stream_size || in_left != 0 || out_left != 0) {
		(void) inflateEnd(&zs);
		free(buf);
		/*
		 * A truncated stream, corrupt data, trailing bytes after
		 * the stream, or an output count different from the
		 * header's ch_size are all data errors.
		 */
		LIBELF_SET_ERROR(DATA, 0);
		return (NULL);
	}

	if (inflateEnd(&zs) != Z_OK) {
		free(buf);
		LIBELF_SET_ERROR(DATA, 0);
		return (NULL);
	}

	return (buf);
}

/*
 * Materialize the logical, uncompressed file-format bytes of a
 * SHF_COMPRESSED section.  On success returns 1 and stores a malloc'ed
 * buffer of '*logical_sizep' bytes in '*bufp' (NULL when the logical
 * size is zero) and the header's alignment value in '*alignp'.  The
 * caller owns the buffer.  No partial result is published: any failure
 * returns 0 with the appropriate library error set and leaves '*bufp'
 * untouched.
 */
int
_libelf_decompress_section(Elf_Scn *s, unsigned char **bufp,
    size_t *logical_sizep, uint64_t *alignp)
{
	Elf *e;
	int elfclass, compressed;
	uint64_t sh_offset, sh_size, raw_size;
	uint64_t logical_size, ch_align;
	const unsigned char *phys;
	unsigned char *buf;
	size_t hdr_size;

	if (s == NULL || (e = s->s_elf) == NULL) {
		LIBELF_SET_ERROR(ARGUMENT, 0);
		return (0);
	}

	/* Only read-only descriptors may materialize compressed data. */
	if (e->e_cmd != ELF_C_READ) {
		LIBELF_SET_ERROR(UNIMPL, 0);
		return (0);
	}

	elfclass = e->e_class;
	assert(elfclass == ELFCLASS32 || elfclass == ELFCLASS64);

	if (elfclass == ELFCLASS32) {
		sh_offset = (uint64_t) s->s_shdr.s_shdr32.sh_offset;
		sh_size   = (uint64_t) s->s_shdr.s_shdr32.sh_size;
	} else {
		sh_offset = s->s_shdr.s_shdr64.sh_offset;
		sh_size   = s->s_shdr.s_shdr64.sh_size;
	}

	compressed = _libelf_section_is_compressed(s);
	if (compressed < 0)
		return (0);
	if (compressed == 0) {
		LIBELF_SET_ERROR(ARGUMENT, 0);
		return (0);
	}

	raw_size = (uint64_t) e->e_rawsize;
	if (e->e_rawfile == NULL || sh_offset > raw_size ||
	    sh_size > raw_size - sh_offset) {
		LIBELF_SET_ERROR(DATA, 0);
		return (0);
	}

	phys = e->e_rawfile + sh_offset;

	if (!_libelf_parse_compression_header(e, phys, sh_size,
	    &logical_size, &ch_align))
		return (0);

	hdr_size = (elfclass == ELFCLASS32) ?
	    LIBELF_COMPRESS_HDR32_SIZE : LIBELF_COMPRESS_HDR64_SIZE;

	if (sh_size - hdr_size > SIZE_MAX) {
		LIBELF_SET_ERROR(RANGE, 0);
		return (0);
	}

	if (logical_size == 0) {
		/*
		 * A valid empty stream still has to parse: confirm the
		 * zlib stream itself inflates to zero bytes.
		 */
		buf = _libelf_inflate(phys + hdr_size, sh_size - hdr_size, 0);
		if (buf == NULL)
			return (0);
		free(buf);
		*bufp = NULL;
		*logical_sizep = 0;
		*alignp = ch_align;
		return (1);
	}

	buf = _libelf_inflate(phys + hdr_size, sh_size - hdr_size,
	    (size_t) logical_size);
	if (buf == NULL)
		return (0);

	*bufp = buf;
	*logical_sizep = (size_t) logical_size;
	*alignp = ch_align;
	return (1);
}

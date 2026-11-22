/* example_c_cuesplit_file - split a cuesheeted FLAC from stdin into files
 * Copyright (C) 2025  Xiph.Org Foundation
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/*
 * Reads one native FLAC carrying a CUESHEET from a forward-only input
 * (stdin by default) and writes one independent FLAC per track.  Only
 * libFLAC's cue splitter touches the input; the program merely adapts
 * FILE* streams to the four callbacks.
 *
 * Usage:
 *   cuesplit_file [input.flac] [output-prefix]
 *
 * Output files are named "<prefix><track>.flac"; under the PREVIOUS
 * pregap policy the hidden audio before the first INDEX 01, if any, is
 * written to "<prefix>0.flac".
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FLAC/stream_cuesplit.h"

typedef struct {
	FILE *in;
	const char *prefix;
	FILE *out;
	uint32_t out_track;
} Client;

/* Forward-only source: fread() naturally performs short reads, and a
 * zero-length read at end of stream maps to the decoder EOF status. */
static FLAC__StreamDecoderReadStatus read_cb(void *client_data, FLAC__byte buffer[], size_t *bytes)
{
	Client *c = (Client*)client_data;
	size_t got = fread(buffer, 1, *bytes, c->in);
	if(got > 0) {
		*bytes = got;
		return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
	}
	*bytes = 0;
	if(ferror(c->in))
		return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
	return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
}

static FLAC__bool begin_cb(void *client_data, uint32_t track, FLAC__uint64 samples)
{
	char name[1024];
	Client *c = (Client*)client_data;
	(void)samples;
	snprintf(name, sizeof(name), "%s%u.flac", c->prefix, (unsigned)track);
	c->out = fopen(name, "wb+");
	if(c->out == NULL) {
		fprintf(stderr, "cannot open %s\n", name);
		return false;
	}
	c->out_track = track;
	fprintf(stderr, "writing %s (track %u)\n", name, (unsigned)track);
	return true;
}

/* byte_offset is absolute within this output, so the STREAMINFO header
 * can be rewritten when the encoder finishes; fseek() positions the
 * sink without touching the forward-only input at all. */
static FLAC__bool write_cb(void *client_data, uint32_t track, FLAC__uint64 byte_offset, const FLAC__byte buffer[], size_t bytes)
{
	Client *c = (Client*)client_data;
	(void)track;
	if(fseeko(c->out, (off_t)byte_offset, SEEK_SET) != 0)
		return false;
	return fwrite(buffer, 1, bytes, c->out) == bytes;
}

static void end_cb(void *client_data, uint32_t track, FLAC__bool complete)
{
	Client *c = (Client*)client_data;
	(void)track;
	if(c->out != NULL) {
		fclose(c->out);
		c->out = NULL;
	}
	if(!complete)
		fprintf(stderr, "track %u was aborted before it finished\n", (unsigned)track);
}

int main(int argc, char *argv[])
{
	Client client;
	FLAC__CueSplitIO io;
	FLAC__CueSplitStatus status;
	const char *input = (argc > 1 && argv[1][0] != '\0') ? argv[1] : NULL;
	const char *prefix = (argc > 2) ? argv[2] : "track";

	memset(&client, 0, sizeof(client));
	client.prefix = prefix;
	if(input != NULL) {
		client.in = fopen(input, "rb");
		if(client.in == NULL) {
			fprintf(stderr, "cannot open %s\n", input);
			return 1;
		}
	}
	else {
		client.in = stdin; /* a pipe is fine: the splitter never seeks input */
	}

	io.client_data = &client;
	io.read = read_cb;
	io.begin = begin_cb;
	io.write = write_cb;
	io.end = end_cb;

	status = FLAC__stream_cuesplit(&io, FLAC__CUESPLIT_PREGAP_NEXT);
	fprintf(stderr, "split status: %d\n", (int)status);

	if(input != NULL)
		fclose(client.in);
	return status == FLAC__CUESPLIT_STATUS_OK ? 0 : 1;
}

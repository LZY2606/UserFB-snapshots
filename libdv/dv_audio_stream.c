/*
 *  dv_audio_stream.c
 *
 *     Copyright (C) The libdv project contributors
 *
 *  This file is part of libdv, a free DV (IEC 61834/SMPTE 314M)
 *  codec.
 *
 *  libdv is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU Lesser Public License as published by
 *  the Free Software Foundation; either version 2.1, or (at your
 *  option) any later version.
 *
 *  libdv is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser Public License
 *  along with libdv; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 *  The libdv homepage is http://libdv.sourceforge.net/.
 */

/** @file
 *  @ingroup encoder
 *  @brief Bounded streaming muxer for 32 kHz four channel DV audio.
 *
 *  Accepts arbitrary sized chunks of interleaved four channel PCM and
 *  commits one DV frame at a time through dv_encode_full_audio().  The
 *  state holds at most one audio frame worth of PCM plus a fixed
 *  workspace, independent of the recording length.  See dv.h for the
 *  full interface and tail conventions.
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdlib.h>
#include <string.h>

#include "dv.h"

/* Maximum number of samples per channel a DV audio frame can hold at
 * 32 kHz 12 bit (PAL: 6 * 9 * 24 = 1296, NTSC: 5 * 9 * 24 = 1080). */
#define DV_STREAM_MAX_SAMPLES 1296

struct dv_audio_stream_s {
	int      is_pal;
	long long frame_index; /* k of the next frame to commit */
	long     buffered;     /* per-channel frames held in pcm */
	long     budget;       /* per-channel frames needed for frame k */
	int16_t  pcm[DV_STREAM_MAX_SAMPLES * 4]; /* interleaved backlog */
	int16_t  planar[4][DV_STREAM_MAX_SAMPLES]; /* commit workspace */
};

/* Samples per channel carried by frame k.  PAL is a fixed 1280; NTSC
 * follows the exact 30000/1001 rational cadence. */
static long dv_audio_stream_budget(const dv_audio_stream_t *stream)
{
	if (stream->is_pal)
		return 1280;
	return (long) (((stream->frame_index + 1) * 32000LL * 1001LL) / 30000LL
		       - (stream->frame_index * 32000LL * 1001LL) / 30000LL);
}

dv_audio_stream_t *dv_audio_stream_new(int is_pal)
{
	dv_audio_stream_t *stream;

	if (is_pal != 0 && is_pal != 1)
		return NULL;
	stream = (dv_audio_stream_t *) calloc(1, sizeof(dv_audio_stream_t));
	if (!stream)
		return NULL;
	stream->is_pal = is_pal;
	stream->budget = dv_audio_stream_budget(stream);
	return stream;
}

void dv_audio_stream_free(dv_audio_stream_t *stream)
{
	free(stream);
}

void dv_audio_stream_reset(dv_audio_stream_t *stream)
{
	if (!stream)
		return;
	stream->frame_index = 0;
	stream->buffered = 0;
	stream->budget = dv_audio_stream_budget(stream);
}

int dv_audio_stream_feed(dv_audio_stream_t *stream,
			 const int16_t *interleaved,
			 long input_frames, long *used,
			 uint8_t *frame, size_t frame_bytes)
{
	long take, i;
	int c;
	dv_encoder_t encoder;
	int16_t *planes[4];

	if (used)
		*used = 0;
	if (!stream || !used || !frame)
		return -1;
	if (input_frames < 0)
		return -1;
	if (input_frames > 0 && !interleaved)
		return -1;
	if (frame_bytes != (stream->is_pal ? 144000u : 120000u))
		return -1;
	if (((frame[3] & 0x80) ? 1 : 0) != stream->is_pal)
		return -1;

	/* Top up the backlog, never more than the current budget. */
	take = stream->budget - stream->buffered;
	if (take > input_frames)
		take = input_frames;
	if (take > 0) {
		memcpy(stream->pcm + stream->buffered * 4, interleaved,
		       (size_t) take * 4 * sizeof(int16_t));
		stream->buffered += take;
		*used = take;
	}
	if (stream->buffered < stream->budget)
		return 0;

	/* A complete budget is available: commit exactly one frame. */
	for (i = 0; i < stream->budget; i++)
		for (c = 0; c < 4; c++)
			stream->planar[c][i] = stream->pcm[i*4 + c];
	for (c = 0; c < 4; c++)
		planes[c] = stream->planar[c];

	memset(&encoder, 0, sizeof(encoder));
	encoder.samples_this_frame = (int) stream->budget;
	if (dv_encode_full_audio(&encoder, planes, 4, 32000, frame) != 0)
		return -1;

	stream->buffered = 0;
	stream->frame_index++;
	stream->budget = dv_audio_stream_budget(stream);
	return 1;
}

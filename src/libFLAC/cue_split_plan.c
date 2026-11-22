/* libFLAC - Free Lossless Audio Codec library
 * Copyright (C) 2025  Xiph.Org Foundation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *
 * - Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * - Neither the name of the Xiph.org Foundation nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "private/stream_cuesplit.h"
#include "FLAC/metadata.h"

#include <string.h>

/*
 * Absolute source sample position of a track's INDEX 01, or
 * (FLAC__uint64)(-1) if the track has no INDEX 01.
 */
static FLAC__uint64 index01_absolute_(const FLAC__StreamMetadata_CueSheet_Track *track)
{
	uint32_t i;
	for(i = 0; i < track->num_indices; i++) {
		if(track->indices[i].number == 1)
			return track->offset + track->indices[i].offset;
	}
	return (FLAC__uint64)(-1);
}

FLAC__bool FLAC__cuesplit_plan_build(
	const FLAC__StreamMetadata_StreamInfo *stream_info,
	const FLAC__StreamMetadata_CueSheet *cue,
	FLAC__CueSplitPregapPolicy policy,
	FLAC__CueSplitPlan *plan,
	FLAC__CueSplitSegment *scratch
)
{
	uint32_t n, i, j;
	const FLAC__StreamMetadata_CueSheet_Track *leadout;

	if(stream_info == 0 || cue == 0 || plan == 0 || scratch == 0)
		return false;

	if(stream_info->total_samples == 0)
		return false;
	if(stream_info->sample_rate != 44100 && stream_info->sample_rate != 48000)
		return false;
	if(stream_info->channels != 1 && stream_info->channels != 2)
		return false;
	if(stream_info->bits_per_sample != 16 && stream_info->bits_per_sample != 24)
		return false;

	if(cue->is_cd)
		return false;
	/* one to ninety-nine music tracks plus the lead-out */
	if(cue->num_tracks < 2 || cue->num_tracks > 100 || cue->tracks == 0)
		return false;

	n = cue->num_tracks - 1;
	leadout = &cue->tracks[n];

	if(leadout->number != 170 || leadout->num_indices != 0)
		return false;
	if(leadout->offset != stream_info->total_samples)
		return false;

	for(i = 0; i < n; i++) {
		const FLAC__StreamMetadata_CueSheet_Track *track = &cue->tracks[i];
		const FLAC__uint64 next_offset = (i + 1 < n) ? cue->tracks[i + 1].offset : stream_info->total_samples;

		if(track->number != (FLAC__byte)(i + 1))
			return false;
		if(track->type != 0)
			return false;
		if(track->num_indices == 0 || track->indices == 0)
			return false;
		if(i == 0) {
			if(track->offset != 0)
				return false;
		}
		else {
			if(track->offset <= cue->tracks[i - 1].offset)
				return false;
		}
		if(track->offset >= next_offset)
			return false;

		/* index numbering and offsets */
		for(j = 0; j < track->num_indices; j++) {
			const FLAC__StreamMetadata_CueSheet_Index *index = &track->indices[j];

			if(j == 0) {
				if(index->number != 0 && index->number != 1)
					return false;
				if(index->number == 0 && index->offset != 0)
					return false;
				if(index->number == 1 && index->offset != 0)
					return false;
			}
			else {
				if(index->number != track->indices[j - 1].number + 1)
					return false;
				if(index->offset <= track->indices[j - 1].offset)
					return false;
			}

			/* every absolute index point lies in [track offset, next track offset) */
			if(index->offset > stream_info->total_samples)
				return false;
			if(track->offset > stream_info->total_samples - index->offset)
				return false;
			{
				const FLAC__uint64 absolute = track->offset + index->offset;
				if(absolute < track->offset || absolute >= next_offset)
					return false;
			}
		}

		/* INDEX 00, when present, is the first index and INDEX 01 must follow it */
		if(track->indices[0].number == 0) {
			if(track->num_indices < 2 || track->indices[1].number != 1 || track->indices[1].offset == 0)
				return false;
		}
		if(index01_absolute_(track) == (FLAC__uint64)(-1))
			return false;
	}

	plan->cue = cue;
	plan->num_music_tracks = n;
	plan->segments = scratch;
	plan->num_segments = 0;

	if(policy == FLAC__CUESPLIT_PREGAP_NEXT) {
		for(i = 0; i < n; i++) {
			scratch[i].start = cue->tracks[i].offset;
			scratch[i].end = cue->tracks[i + 1].offset;
			scratch[i].output_track = i + 1;
			scratch[i].source_track = i;
		}
		plan->num_segments = n;
	}
	else if(policy == FLAC__CUESPLIT_PREGAP_PREVIOUS) {
		uint32_t out = 0;

		if(index01_absolute_(&cue->tracks[0]) > 0) {
			scratch[0].start = 0;
			scratch[0].end = index01_absolute_(&cue->tracks[0]);
			scratch[0].output_track = 0;
			scratch[0].source_track = (uint32_t)(-1);
			out++;
		}
		for(i = 0; i < n; i++) {
			const FLAC__uint64 start = index01_absolute_(&cue->tracks[i]);
			const FLAC__uint64 end = (i + 1 < n) ?
				index01_absolute_(&cue->tracks[i + 1]) :
				stream_info->total_samples;
			scratch[out].start = start;
			scratch[out].end = end;
			scratch[out].output_track = i + 1;
			scratch[out].source_track = i;
			out++;
		}
		plan->num_segments = out;
	}
	else {
		return false;
	}

	return plan->num_segments > 0;
}

const FLAC__CueSplitSegment *FLAC__cuesplit_plan_locate(
	const FLAC__CueSplitPlan *plan,
	FLAC__uint64 pos,
	uint32_t *hint
)
{
	uint32_t i = *hint;
	while(i + 1 < plan->num_segments && pos >= plan->segments[i + 1].start)
		i++;
	*hint = i;
	return &plan->segments[i];
}

FLAC__StreamMetadata *FLAC__cuesplit_plan_make_cuesheet(
	const FLAC__CueSplitPlan *plan,
	const FLAC__CueSplitSegment *segment
)
{
	FLAC__StreamMetadata *out;
	FLAC__StreamMetadata_CueSheet *dst;
	const FLAC__StreamMetadata_CueSheet_Track *src_track;
	FLAC__StreamMetadata_CueSheet_Track *dst_track, *dst_leadout;
	uint32_t kept = 0, j;

	out = FLAC__metadata_object_new(FLAC__METADATA_TYPE_CUESHEET);
	if(out == 0)
		return 0;
	if(!FLAC__metadata_object_cuesheet_resize_tracks(out, 2)) {
		FLAC__metadata_object_delete(out);
		return 0;
	}
	dst = &out->data.cue_sheet;
	memcpy(dst->media_catalog_number, plan->cue->media_catalog_number, sizeof(dst->media_catalog_number));
	dst->lead_in = 0;
	dst->is_cd = false;

	dst_track = &dst->tracks[0];
	dst_leadout = &dst->tracks[1];

	if(segment->source_track == (uint32_t)(-1)) {
		/* synthetic hidden-audio output: one track numbered 1 with INDEX 01 at 0 */
		dst_track->number = 1;
		dst_track->offset = 0;
		dst_track->type = 0;
		dst_track->pre_emphasis = 0;
		if(!FLAC__metadata_object_cuesheet_track_resize_indices(out, 0, 1))
			goto fail;
		dst_track->indices[0].number = 1;
		dst_track->indices[0].offset = 0;
	}
	else {
		src_track = &plan->cue->tracks[segment->source_track];
		dst_track->number = src_track->number;
		dst_track->offset = 0;
		dst_track->type = 0;
		dst_track->pre_emphasis = src_track->pre_emphasis;
		memcpy(dst_track->isrc, src_track->isrc, sizeof(dst_track->isrc));

		for(j = 0; j < src_track->num_indices; j++) {
			const FLAC__uint64 absolute = src_track->offset + src_track->indices[j].offset;
			if(absolute >= segment->start && absolute < segment->end)
				kept++;
		}
		if(kept == 0 || !FLAC__metadata_object_cuesheet_track_resize_indices(out, 0, kept))
			goto fail;
		kept = 0;
		for(j = 0; j < src_track->num_indices; j++) {
			const FLAC__uint64 absolute = src_track->offset + src_track->indices[j].offset;
			if(absolute >= segment->start && absolute < segment->end) {
				dst_track->indices[kept].number = src_track->indices[j].number;
				dst_track->indices[kept].offset = absolute - segment->start;
				kept++;
			}
		}
	}

	dst_leadout->number = 170;
	dst_leadout->offset = segment->end - segment->start;
	dst_leadout->type = 0;
	dst_leadout->pre_emphasis = 0;
	dst_leadout->num_indices = 0;

	if(!FLAC__format_cuesheet_is_legal(dst, /*check_cd_da_subset=*/false, 0))
		goto fail;

	return out;

fail:
	FLAC__metadata_object_delete(out);
	return 0;
}

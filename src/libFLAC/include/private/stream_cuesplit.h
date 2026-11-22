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

#ifndef FLAC__PRIVATE__STREAM_CUESPLIT_H
#define FLAC__PRIVATE__STREAM_CUESPLIT_H

#include "FLAC/format.h"
#include "FLAC/stream_cuesplit.h"

/*
 * A single output segment of the split.
 *
 * start/end are absolute source sample coordinates of the half-open
 * interval [start, end).  The segments partition [0, total_samples)
 * without gaps or overlaps.  source_track is the music track index in
 * the source CUESHEET (0-based, lead-out excluded); it carries the
 * track number, ISRC and pre-emphasis flags copied into the output
 * CUESHEET.  The synthetic hidden-audio output (track number 0 under
 * the PREVIOUS policy) uses source_track == (uint32_t)(-1).
 */
typedef struct {

	FLAC__uint64 start;
	FLAC__uint64 end;

	uint32_t output_track;
	uint32_t source_track;

} FLAC__CueSplitSegment;

/*
 * The validated routing plan derived from the source CUESHEET.
 *
 * cue points at the original, validated source CUESHEET; the plan
 * code never owns or frees it.  segments is sized for the maximum
 * possible output count, num_segments is the part in use.
 */
typedef struct {

	const FLAC__StreamMetadata_CueSheet *cue;

	uint32_t num_music_tracks; /* lead-out excluded */
	uint32_t num_segments;
	FLAC__CueSplitSegment *segments;

} FLAC__CueSplitPlan;

/*
 * Validates the source metadata and builds the routing plan for the
 * requested pregap policy.
 *
 * stream_info must be the decoded source STREAMINFO and cue the single
 * decoded source CUESHEET (already known to be non-CD by the caller).
 * scratch is caller-owned storage for at most num_music_tracks + 1
 * segments.  Returns false for every metadata conflict described in
 * the public header documentation; on failure *plan is left unused.
 */
FLAC__bool FLAC__cuesplit_plan_build(
	const FLAC__StreamMetadata_StreamInfo *stream_info,
	const FLAC__StreamMetadata_CueSheet *cue,
	FLAC__CueSplitPregapPolicy policy,
	FLAC__CueSplitPlan *plan,
	FLAC__CueSplitSegment *scratch
);

/*
 * Finds the segment that owns source sample position pos.  All plan
 * coordinates are monotonic, so this uses a linear scan with a
 * caller-maintained hint; both arguments are updated.
 */
const FLAC__CueSplitSegment *FLAC__cuesplit_plan_locate(
	const FLAC__CueSplitPlan *plan,
	FLAC__uint64 pos,
	uint32_t *hint
);

/*
 * Builds the non-CD CUESHEET metadata block for one output segment.
 *
 * The result is suitable for FLAC__stream_encoder_set_metadata().
 * Catalog number, track number/ISRC/pre-emphasis, index points and
 * lead-out are translated exactly as documented for the public API.
 * Returns NULL on allocation failure.
 */
FLAC__StreamMetadata *FLAC__cuesplit_plan_make_cuesheet(
	const FLAC__CueSplitPlan *plan,
	const FLAC__CueSplitSegment *segment
);

#endif /* FLAC__PRIVATE__STREAM_CUESPLIT_H */

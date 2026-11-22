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
#include "FLAC/stream_decoder.h"
#include "FLAC/stream_encoder.h"
#include "FLAC/metadata.h"

#include <stdlib.h>
#include <string.h>

typedef struct {

	const FLAC__CueSplitIO *io;
	FLAC__CueSplitPregapPolicy policy;

	/* source metadata, captured during the metadata-only decode pass */
	FLAC__bool have_streaminfo;
	FLAC__StreamMetadata_StreamInfo streaminfo;
	uint32_t cuesheet_count;
	FLAC__StreamMetadata *cuesheet;

	/* validated routing */
	FLAC__CueSplitPlan plan;
	FLAC__CueSplitSegment *scratch;
	uint32_t locate_hint;

	/* single sequential source decoder */
	FLAC__StreamDecoder *decoder;
	FLAC__bool io_failed;
	FLAC__bool decode_failed;
	FLAC__uint64 samples_decoded;

	/* at most one active output encoder */
	FLAC__StreamEncoder *encoder;
	uint32_t active_segment;
	FLAC__bool have_active;
	uint32_t out_track;
	FLAC__uint64 out_pos;

} CueSplitContext;

/* ------------------------------------------------------------------ */
/* Source decoder callbacks                                            */
/* ------------------------------------------------------------------ */

static FLAC__StreamDecoderReadStatus source_read_(const FLAC__StreamDecoder *decoder, FLAC__byte buffer[], size_t *bytes, void *client_data)
{
	CueSplitContext *ctx = (CueSplitContext*)client_data;
	FLAC__StreamDecoderReadStatus status;
	(void)decoder;

	status = ctx->io->read(ctx->io->client_data, buffer, bytes);
	if(status == FLAC__STREAM_DECODER_READ_STATUS_ABORT)
		ctx->io_failed = true;
	return status;
}

static void source_metadata_(const FLAC__StreamDecoder *decoder, const FLAC__StreamMetadata *metadata, void *client_data)
{
	CueSplitContext *ctx = (CueSplitContext*)client_data;
	(void)decoder;

	if(metadata->type == FLAC__METADATA_TYPE_STREAMINFO) {
		ctx->have_streaminfo = true;
		ctx->streaminfo = metadata->data.stream_info;
	}
	else if(metadata->type == FLAC__METADATA_TYPE_CUESHEET) {
		ctx->cuesheet_count++;
		if(ctx->cuesheet_count == 1)
			ctx->cuesheet = FLAC__metadata_object_clone(metadata);
	}
	/* all other metadata blocks are deliberately not inherited */
}

static void source_error_(const FLAC__StreamDecoder *decoder, FLAC__StreamDecoderErrorStatus status, void *client_data)
{
	CueSplitContext *ctx = (CueSplitContext*)client_data;
	(void)decoder, (void)status;
	ctx->decode_failed = true;
}

/* ------------------------------------------------------------------ */
/* Output encoder callbacks                                            */
/* ------------------------------------------------------------------ */

static FLAC__StreamEncoderWriteStatus encoder_write_(const FLAC__StreamEncoder *encoder, const FLAC__byte buffer[], size_t bytes, uint32_t samples, uint32_t current_frame, void *client_data)
{
	CueSplitContext *ctx = (CueSplitContext*)client_data;
	(void)encoder, (void)samples, (void)current_frame;

	if(!ctx->io->write(ctx->io->client_data, ctx->out_track, ctx->out_pos, buffer, bytes)) {
		ctx->io_failed = true;
		return FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;
	}
	ctx->out_pos += bytes;
	return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
}

static FLAC__StreamEncoderSeekStatus encoder_seek_(const FLAC__StreamEncoder *encoder, FLAC__uint64 absolute_byte_offset, void *client_data)
{
	CueSplitContext *ctx = (CueSplitContext*)client_data;
	(void)encoder;
	/* the sink speaks absolute byte offsets, so "seeking" only moves the
	 * write position and lets the STREAMINFO header be rewritten */
	ctx->out_pos = absolute_byte_offset;
	return FLAC__STREAM_ENCODER_SEEK_STATUS_OK;
}

static FLAC__StreamEncoderTellStatus encoder_tell_(const FLAC__StreamEncoder *encoder, FLAC__uint64 *absolute_byte_offset, void *client_data)
{
	CueSplitContext *ctx = (CueSplitContext*)client_data;
	(void)encoder;
	*absolute_byte_offset = ctx->out_pos;
	return FLAC__STREAM_ENCODER_TELL_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* Output lifecycle                                                    */
/* ------------------------------------------------------------------ */

static uint32_t output_blocksize_(const FLAC__StreamMetadata_StreamInfo *si)
{
	uint32_t blocksize = si->max_blocksize;
	if(blocksize < FLAC__MIN_BLOCK_SIZE)
		blocksize = 4096;
	if(blocksize > FLAC__SUBSET_MAX_BLOCK_SIZE_48000HZ)
		blocksize = FLAC__SUBSET_MAX_BLOCK_SIZE_48000HZ;
	if(!FLAC__format_blocksize_is_subset(blocksize, si->sample_rate))
		blocksize = 4096;
	return blocksize;
}

/* finishes and closes the active output; returns false on I/O failure */
static FLAC__bool close_output_(CueSplitContext *ctx, FLAC__bool complete)
{
	FLAC__bool ok = complete;
	uint32_t track;

	if(!ctx->have_active)
		return true;

	track = ctx->out_track;
	if(complete) {
		if(!FLAC__stream_encoder_finish(ctx->encoder)) {
			ctx->io_failed = true;
			ok = false;
		}
	}
	FLAC__stream_encoder_delete(ctx->encoder);
	ctx->encoder = 0;
	ctx->have_active = false;
	ctx->io->end(ctx->io->client_data, track, ok);
	return ok;
}

/* opens the output for segment index seg and prepares its encoder */
static FLAC__bool open_output_(CueSplitContext *ctx, uint32_t seg)
{
	const FLAC__CueSplitSegment *segment = &ctx->plan.segments[seg];
	FLAC__StreamMetadata *out_cue;
	FLAC__StreamMetadata *metadata[1];
	FLAC__StreamEncoderInitStatus init_status;
	const FLAC__uint64 length = segment->end - segment->start;

	ctx->out_track = segment->output_track;
	ctx->out_pos = 0;

	if(!ctx->io->begin(ctx->io->client_data, ctx->out_track, length)) {
		ctx->io_failed = true;
		return false;
	}

	out_cue = FLAC__cuesplit_plan_make_cuesheet(&ctx->plan, segment);
	if(out_cue == 0) {
		ctx->io_failed = true;
		ctx->io->end(ctx->io->client_data, ctx->out_track, false);
		return false;
	}

	ctx->encoder = FLAC__stream_encoder_new();
	if(ctx->encoder == 0) {
		FLAC__metadata_object_delete(out_cue);
		ctx->io_failed = true;
		ctx->io->end(ctx->io->client_data, ctx->out_track, false);
		return false;
	}

	FLAC__stream_encoder_set_channels(ctx->encoder, ctx->streaminfo.channels);
	FLAC__stream_encoder_set_bits_per_sample(ctx->encoder, ctx->streaminfo.bits_per_sample);
	FLAC__stream_encoder_set_sample_rate(ctx->encoder, ctx->streaminfo.sample_rate);
	FLAC__stream_encoder_set_blocksize(ctx->encoder, output_blocksize_(&ctx->streaminfo));
	FLAC__stream_encoder_set_streamable_subset(ctx->encoder, true);
	FLAC__stream_encoder_set_total_samples_estimate(ctx->encoder, length);
	metadata[0] = out_cue;
	FLAC__stream_encoder_set_metadata(ctx->encoder, metadata, 1);

	init_status = FLAC__stream_encoder_init_stream(
		ctx->encoder,
		encoder_write_,
		encoder_seek_,
		encoder_tell_,
		/*metadata_callback=*/0,
		ctx
	);
	/* the cuesheet object is copied into the encoder and may now go */
	FLAC__metadata_object_delete(out_cue);

	if(init_status != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
		FLAC__stream_encoder_delete(ctx->encoder);
		ctx->encoder = 0;
		ctx->io_failed = true;
		ctx->io->end(ctx->io->client_data, ctx->out_track, false);
		return false;
	}

	ctx->have_active = true;
	ctx->active_segment = seg;
	return true;
}

/* ------------------------------------------------------------------ */
/* Frame routing                                                       */
/* ------------------------------------------------------------------ */

static FLAC__StreamDecoderWriteStatus source_write_(const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame, const FLAC__int32 * const buffer[], void *client_data)
{
	CueSplitContext *ctx = (CueSplitContext*)client_data;
	FLAC__uint64 pos = frame->header.number.sample_number;
	uint32_t left = frame->header.blocksize;
	const FLAC__int32 *channel_buffers[FLAC__MAX_CHANNELS];
	uint32_t c;
	(void)decoder;

	ctx->samples_decoded += frame->header.blocksize;

	while(left > 0) {
		const FLAC__CueSplitSegment *segment;
		uint32_t seg_index, take;

		segment = FLAC__cuesplit_plan_locate(&ctx->plan, pos, &ctx->locate_hint);
		seg_index = ctx->locate_hint;

		if(pos >= segment->end) {
			/* frame ran past the validated plan end */
			ctx->decode_failed = true;
			return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
		}
		take = (uint32_t)((segment->end - pos < left) ? segment->end - pos : left);

		if(ctx->have_active && ctx->active_segment != seg_index) {
			if(!close_output_(ctx, true))
				return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
		}
		if(!ctx->have_active) {
			if(!open_output_(ctx, seg_index))
				return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
		}

		for(c = 0; c < frame->header.channels; c++) {
			channel_buffers[c] = buffer[c] + (frame->header.blocksize - left);
		}
		if(!FLAC__stream_encoder_process(ctx->encoder, channel_buffers, take)) {
			ctx->io_failed = true;
			return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
		}

		pos += take;
		left -= take;
	}

	return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* Public entry point                                                  */
/* ------------------------------------------------------------------ */

FLAC_API FLAC__CueSplitStatus FLAC__stream_cuesplit(const FLAC__CueSplitIO *io, FLAC__CueSplitPregapPolicy policy)
{
	CueSplitContext ctx;
	FLAC__CueSplitStatus status = FLAC__CUESPLIT_STATUS_IO_ERROR;
	FLAC__StreamDecoderInitStatus init_status;
	FLAC__bool md5_ok = true;
	FLAC__bool metadata_phase_ok;
	FLAC__bool decoder_finished = false;

	if(io == 0 || io->read == 0 || io->begin == 0 || io->write == 0 || io->end == 0)
		return FLAC__CUESPLIT_STATUS_IO_ERROR;
	if(policy != FLAC__CUESPLIT_PREGAP_NEXT && policy != FLAC__CUESPLIT_PREGAP_PREVIOUS)
		return FLAC__CUESPLIT_STATUS_IO_ERROR;

	memset(&ctx, 0, sizeof(ctx));
	ctx.io = io;
	ctx.policy = policy;

	ctx.decoder = FLAC__stream_decoder_new();
	if(ctx.decoder == 0)
		goto cleanup;

	FLAC__stream_decoder_set_md5_checking(ctx.decoder, true);
	FLAC__stream_decoder_set_metadata_respond(ctx.decoder, FLAC__METADATA_TYPE_CUESHEET);

	init_status = FLAC__stream_decoder_init_stream(
		ctx.decoder,
		source_read_,
		/*seek=*/0,
		/*tell=*/0,
		/*length=*/0,
		/*eof=*/0,
		source_write_,
		source_metadata_,
		source_error_,
		&ctx
	);
	if(init_status != FLAC__STREAM_DECODER_INIT_STATUS_OK)
		goto cleanup;

	/* read and validate all metadata before any output is created */
	metadata_phase_ok = FLAC__stream_decoder_process_until_end_of_metadata(ctx.decoder);
	if(!metadata_phase_ok) {
		status = ctx.io_failed ? FLAC__CUESPLIT_STATUS_IO_ERROR : FLAC__CUESPLIT_STATUS_DECODE_ERROR;
		goto cleanup;
	}

	if(!ctx.have_streaminfo || ctx.cuesheet_count != 1 || ctx.cuesheet == 0) {
		status = FLAC__CUESPLIT_STATUS_INVALID_METADATA;
		goto cleanup;
	}
	if(ctx.cuesheet->data.cue_sheet.is_cd) {
		status = FLAC__CUESPLIT_STATUS_INVALID_METADATA;
		goto cleanup;
	}

	ctx.scratch = (FLAC__CueSplitSegment*)malloc(sizeof(FLAC__CueSplitSegment) * (ctx.cuesheet->data.cue_sheet.num_tracks + 1));
	if(ctx.scratch == 0)
		goto cleanup;

	if(!FLAC__cuesplit_plan_build(
		&ctx.streaminfo,
		&ctx.cuesheet->data.cue_sheet,
		ctx.policy,
		&ctx.plan,
		ctx.scratch
	)) {
		status = FLAC__CUESPLIT_STATUS_INVALID_METADATA;
		goto cleanup;
	}

	/* one sequential pass over the whole audio section */
	while(FLAC__stream_decoder_get_state(ctx.decoder) != FLAC__STREAM_DECODER_END_OF_STREAM) {
		if(!FLAC__stream_decoder_process_single(ctx.decoder))
			break;
		if(FLAC__stream_decoder_get_state(ctx.decoder) == FLAC__STREAM_DECODER_ABORTED)
			break;
	}

	if(ctx.io_failed) {
		status = FLAC__CUESPLIT_STATUS_IO_ERROR;
	}
	else if(ctx.decode_failed ||
		FLAC__stream_decoder_get_state(ctx.decoder) != FLAC__STREAM_DECODER_END_OF_STREAM ||
		ctx.samples_decoded != ctx.streaminfo.total_samples) {
		status = FLAC__CUESPLIT_STATUS_DECODE_ERROR;
	}
	else if(!ctx.have_active || ctx.active_segment != ctx.plan.num_segments - 1) {
		/* every segment must have produced an output */
		status = FLAC__CUESPLIT_STATUS_DECODE_ERROR;
	}
	else if(!close_output_(&ctx, true)) {
		status = FLAC__CUESPLIT_STATUS_IO_ERROR;
	}
	else {
		/* source MD5 is only verifiable once the whole stream is consumed */
		md5_ok = FLAC__stream_decoder_finish(ctx.decoder);
		decoder_finished = true;
		if(!md5_ok)
			status = FLAC__CUESPLIT_STATUS_SOURCE_MD5_MISMATCH;
		else
			status = FLAC__CUESPLIT_STATUS_OK;
	}

cleanup:
	if(ctx.have_active)
		(void)close_output_(&ctx, false);
	if(ctx.decoder != 0) {
		/* finish() is harmless here; the MD5 verdict only matters on the
		 * clean path where it was already consulted above */
		if(!decoder_finished)
			(void)FLAC__stream_decoder_finish(ctx.decoder);
		FLAC__stream_decoder_delete(ctx.decoder);
	}
	if(ctx.cuesheet != 0)
		FLAC__metadata_object_delete(ctx.cuesheet);
	free(ctx.scratch);

	return status;
}

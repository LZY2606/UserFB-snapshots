/* tests/cue_split.c - task test suite for FLAC__stream_cuesplit()
 *
 * Registered through task_support/check.h (no main of its own).
 * Sources are built with the legacy stream encoder + metadata API and
 * every output is independently decoded with the legacy stream
 * decoder and checked sample by sample against hand-computed cuts.
 */

#include "task_support/check.h"

#include <FLAC/stream_encoder.h>
#include <FLAC/stream_decoder.h>
#include <FLAC/metadata.h>
#include <FLAC/stream_cuesplit.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)
#include <malloc/malloc.h>
#include <mach/mach.h>
#define CUESPLIT_HAVE_HEAP_STATS 1
#endif

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

struct membuf {
	FLAC__byte *data;
	size_t used, cap, pos;
};

static void buf_init(struct membuf *b)
{
	memset(b, 0, sizeof(*b));
}

static void buf_free(struct membuf *b)
{
	free(b->data);
	b->data = 0;
	b->used = b->cap = b->pos = 0;
}

static int buf_ensure(struct membuf *b, size_t end)
{
	if(end <= b->cap)
		return 1;
	{
		size_t cap = b->cap ? b->cap : 4096;
		void *p;
		while(cap < end) {
			if(cap > (size_t)-1 / 2)
				return 0;
			cap *= 2;
		}
		p = realloc(b->data, cap);
		if(p == 0)
			return 0;
		b->data = (FLAC__byte*)p;
		b->cap = cap;
	}
	return 1;
}

static void buf_put_at(struct membuf *b, FLAC__uint64 offset, const FLAC__byte *data, size_t n)
{
	if(!buf_ensure(b, (size_t)offset + n)) {
		fprintf(stderr, "out of memory in test sink\n");
		exit(2);
	}
	memcpy(b->data + offset, data, n);
	if((size_t)offset + n > b->used)
		b->used = (size_t)offset + n;
}

/* deterministic, channel distinguishable PCM */
static FLAC__int32 pcm_sample(FLAC__uint64 index, unsigned channel, unsigned bps)
{
	uint32_t span = 1u << (bps - 1);
	uint32_t v = ((uint32_t)index * 1103515245u + 12345u + channel * 2654435761u) % span;
	return (FLAC__int32)v - (FLAC__int32)(span / 2);
}

/* ------------------------------------------------------------------ */
/* Source FLAC construction                                            */
/* ------------------------------------------------------------------ */

struct idx_spec {
	FLAC__byte number;
	FLAC__uint64 offset;
};

struct trk_spec {
	FLAC__byte number;
	const char *isrc;
	int pre_emphasis;
	FLAC__uint64 offset;
	struct idx_spec indices[8];
	uint32_t n_indices;
};

struct src_spec {
	uint32_t rate, channels, bps, blocksize, total_samples;
	const char *catalog;
	const struct trk_spec *tracks;
	uint32_t n_tracks; /* includes the lead-out */
	int extra_metadata;
	int zero_md5;
	int corrupt_md5;
};

static FLAC__StreamMetadata *build_cuesheet(const struct src_spec *spec)
{
	FLAC__StreamMetadata *cue = FLAC__metadata_object_new(FLAC__METADATA_TYPE_CUESHEET);
	uint32_t i, j;
	if(cue == 0)
		return 0;
	if(!FLAC__metadata_object_cuesheet_resize_tracks(cue, spec->n_tracks))
		goto fail;
	cue->data.cue_sheet.lead_in = 0;
	cue->data.cue_sheet.is_cd = false;
	if(spec->catalog)
		strncpy(cue->data.cue_sheet.media_catalog_number, spec->catalog, 128);
	for(i = 0; i < spec->n_tracks; i++) {
		FLAC__StreamMetadata_CueSheet_Track *t = &cue->data.cue_sheet.tracks[i];
		t->number = spec->tracks[i].number;
		t->offset = spec->tracks[i].offset;
		t->type = 0;
		t->pre_emphasis = spec->tracks[i].pre_emphasis ? 1 : 0;
		if(spec->tracks[i].isrc)
			strncpy(t->isrc, spec->tracks[i].isrc, 12);
		if(spec->tracks[i].n_indices > 0) {
			if(!FLAC__metadata_object_cuesheet_track_resize_indices(cue, i, spec->tracks[i].n_indices))
				goto fail;
			for(j = 0; j < spec->tracks[i].n_indices; j++) {
				t->indices[j].number = spec->tracks[i].indices[j].number;
				t->indices[j].offset = spec->tracks[i].indices[j].offset;
			}
		}
	}
	return cue;
fail:
	FLAC__metadata_object_delete(cue);
	return 0;
}

static FLAC__StreamEncoderWriteStatus source_enc_write_(const FLAC__StreamEncoder *e, const FLAC__byte *buf, size_t n, uint32_t s, uint32_t f, void *u)
{
	struct membuf *b = (struct membuf*)u;
	(void)e, (void)s, (void)f;
	if(!buf_ensure(b, b->used + n))
		return FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;
	memcpy(b->data + b->used, buf, n);
	b->used += n;
	return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
}

/* builds the complete source FLAC in out; the cuesheet pointer is only
 * needed for the deliberately malformed metadata cases */
static int build_source(const struct src_spec *spec, struct membuf *out, FLAC__StreamMetadata **built_cue)
{
	FLAC__StreamEncoder *enc;
	FLAC__StreamMetadata *cue = 0;
	FLAC__StreamMetadata *blocks[5];
	uint32_t nblocks = 0;
	FLAC__int32 *pcm;
	uint32_t i, c;
	int ok = 0;

	enc = FLAC__stream_encoder_new();
	if(enc == 0)
		return 0;
	pcm = (FLAC__int32*)malloc(sizeof(FLAC__int32) * spec->total_samples * spec->channels);
	if(pcm == 0)
		goto done;
	for(i = 0; i < spec->total_samples; i++)
		for(c = 0; c < spec->channels; c++)
			pcm[i * spec->channels + c] = pcm_sample(i, c, spec->bps);

	if(spec->tracks) {
		cue = build_cuesheet(spec);
		if(cue == 0)
			goto done;
		blocks[nblocks++] = cue;
	}
	if(spec->extra_metadata) {
		FLAC__StreamMetadata *pad = FLAC__metadata_object_new(FLAC__METADATA_TYPE_PADDING);
		FLAC__StreamMetadata *seek = FLAC__metadata_object_new(FLAC__METADATA_TYPE_SEEKTABLE);
		FLAC__StreamMetadata *vc = FLAC__metadata_object_new(FLAC__METADATA_TYPE_VORBIS_COMMENT);
		FLAC__StreamMetadata *app = FLAC__metadata_object_new(FLAC__METADATA_TYPE_APPLICATION);
		static const FLAC__byte appid[4] = {'t', 'e', 's', 't'};
		FLAC__byte appdata[8] = {1, 2, 3, 4, 5, 6, 7, 8};
		if(pad == 0 || seek == 0 || vc == 0 || app == 0)
			goto done;
		pad->length = 64;
		if(!FLAC__metadata_object_seektable_template_append_spaced_points(seek, 4, spec->total_samples))
			goto done;
		memcpy(app->data.application.id, appid, 4);
		if(!FLAC__metadata_object_application_set_data(app, appdata, sizeof(appdata), /*copy=*/true))
			goto done;
		blocks[nblocks++] = pad;
		blocks[nblocks++] = seek;
		blocks[nblocks++] = vc;
		blocks[nblocks++] = app;
	}

	FLAC__stream_encoder_set_channels(enc, spec->channels);
	FLAC__stream_encoder_set_bits_per_sample(enc, spec->bps);
	FLAC__stream_encoder_set_sample_rate(enc, spec->rate);
	FLAC__stream_encoder_set_blocksize(enc, spec->blocksize);
	FLAC__stream_encoder_set_total_samples_estimate(enc, spec->total_samples);
	if(nblocks > 0 && !FLAC__stream_encoder_set_metadata(enc, blocks, nblocks))
		goto done;
	if(FLAC__stream_encoder_init_stream(enc, source_enc_write_, 0, 0, 0, out) != FLAC__STREAM_ENCODER_INIT_STATUS_OK)
		goto done;
	if(!FLAC__stream_encoder_process_interleaved(enc, pcm, spec->total_samples))
		goto done;
	if(!FLAC__stream_encoder_finish(enc))
		goto done;

	/* STREAMINFO md5 lives at file bytes 26..41: "fLaC"(4) + metadata
	 * header(4) + 18 byte STREAMINFO prefix */
	if(spec->zero_md5)
		memset(out->data + 26, 0, 16);
	if(spec->corrupt_md5)
		out->data[26] ^= 0x5a;

	ok = 1;
	if(built_cue)
		*built_cue = cue;
	else
		cue = 0;
done:
	if(cue)
		FLAC__metadata_object_delete(cue);
	FLAC__stream_encoder_delete(enc);
	free(pcm);
	return ok;
}

/* ------------------------------------------------------------------ */
/* Cuesplit IO harness with event logging                              */
/* ------------------------------------------------------------------ */

#define EVT_BEGIN 1
#define EVT_WRITE 2
#define EVT_END 3

struct evt {
	int kind;
	uint32_t track;
	FLAC__uint64 a, b;
};

struct harness {
	struct membuf src;
	int one_byte;
	int null_sink;

	size_t read_calls;
	FLAC__uint64 read_pos;
	FLAC__uint64 read_bytes;
	int read_monotonic;
	size_t fail_read_after_calls;

	int fail_begin_track;
	int fail_write_track;
	FLAC__uint64 fail_write_after_bytes;

	struct membuf outs[100];
	int began[100], ended[100], end_complete[100];
	FLAC__uint64 begin_samples[100];
	uint64_t begin_seq[100], end_seq[100];
	uint64_t event_seq;
	uint32_t concurrent, max_concurrent;

	struct evt *events;
	size_t n_events, cap_events;
	size_t events_dropped;

	size_t heap_peak;
	size_t heap_base;
	size_t heap_delta_peak;
};

static void harness_init(struct harness *h)
{
	uint32_t i;
	memset(h, 0, sizeof(*h));
	buf_init(&h->src);
	for(i = 0; i < 100; i++)
		buf_init(&h->outs[i]);
	h->read_monotonic = 1;
	h->fail_begin_track = -1;
	h->fail_write_track = -1;
}

static void harness_free(struct harness *h)
{
	uint32_t i;
	buf_free(&h->src);
	for(i = 0; i < 100; i++)
		buf_free(&h->outs[i]);
	free(h->events);
}

#define HARNESS_EVENT_CAP 2048
static void harness_log(struct harness *h, int kind, uint32_t track, FLAC__uint64 a, FLAC__uint64 b)
{
	struct evt *slot;
	if(h->cap_events == 0) {
		struct evt *p = (struct evt*)malloc(HARNESS_EVENT_CAP * sizeof(struct evt));
		if(p == 0) {
			fprintf(stderr, "out of memory in event log\n");
			exit(2);
		}
		h->events = p;
		h->cap_events = HARNESS_EVENT_CAP;
	}
	/* fixed-capacity ring keeps the harness working set independent of
	 * stream length; overflow overwrites the oldest entry */
	if(h->n_events >= h->cap_events) {
		h->events_dropped++;
		slot = &h->events[(h->n_events - h->cap_events) % h->cap_events];
	}
	else {
		slot = &h->events[h->n_events];
	}
	slot->kind = kind;
	slot->track = track;
	slot->a = a;
	slot->b = b;
	h->n_events++;
}

#if defined(CUESPLIT_HAVE_HEAP_STATS)
static size_t heap_live(void)
{
	vm_address_t *zones = 0;
	unsigned count = 0, i;
	size_t sum = 0;
	if(malloc_get_all_zones(mach_task_self(), 0, &zones, &count) != 0)
		return 0;
	for(i = 0; i < count; i++) {
		malloc_statistics_t st;
		malloc_zone_statistics((malloc_zone_t*)zones[i], &st);
		sum += st.size_in_use;
	}
	return sum;
}

static void harness_sample_heap(struct harness *h)
{
	size_t now = heap_live();
	if(now > h->heap_peak)
		h->heap_peak = now;
	if(h->heap_base && now >= h->heap_base && now - h->heap_base > h->heap_delta_peak)
		h->heap_delta_peak = now - h->heap_base;
}

static void harness_mark_heap_base(struct harness *h)
{
	h->heap_base = heap_live();
}
#else
static void harness_sample_heap(struct harness *h)
{
	(void)h;
}
#endif

static FLAC__StreamDecoderReadStatus h_read(void *cd, FLAC__byte *buf, size_t *n)
{
	struct harness *h = (struct harness*)cd;
	size_t want = *n;
	if(h->one_byte && want > 1)
		want = 1;
	h->read_calls++;
	if(h->fail_read_after_calls && h->read_calls > h->fail_read_after_calls) {
		*n = 0;
		return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
	}
	if(h->src.pos == h->src.used) {
		*n = 0;
		return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
	}
	if(want > h->src.used - h->src.pos)
		want = h->src.used - h->src.pos;
	memcpy(buf, h->src.data + h->src.pos, want);
	if((FLAC__uint64)h->src.pos < h->read_pos)
		h->read_monotonic = 0;
	h->read_pos = h->src.pos;
	h->src.pos += want;
	h->read_bytes += want;
	*n = want;
	return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

static FLAC__bool h_begin(void *cd, uint32_t track, FLAC__uint64 samples)
{
	struct harness *h = (struct harness*)cd;
	if(track >= 100)
		return false;
	harness_sample_heap(h);
	harness_log(h, EVT_BEGIN, track, samples, 0);
	h->began[track]++;
	h->begin_samples[track] = samples;
	h->begin_seq[track] = ++h->event_seq;
	h->concurrent++;
	if(h->concurrent > h->max_concurrent)
		h->max_concurrent = h->concurrent;
	if((int)track == h->fail_begin_track)
		return false;
	return true;
}

static FLAC__bool h_write(void *cd, uint32_t track, FLAC__uint64 offset, const FLAC__byte *buf, size_t bytes)
{
	struct harness *h = (struct harness*)cd;
	if(track >= 100)
		return false;
	harness_sample_heap(h);
	harness_log(h, EVT_WRITE, track, offset, bytes);
	if((int)track == h->fail_write_track) {
		if(h->fail_write_after_bytes == 0 || offset >= h->fail_write_after_bytes)
			return false;
	}
	if(!h->null_sink)
		buf_put_at(&h->outs[track], offset, buf, bytes);
	return true;
}

static void h_end(void *cd, uint32_t track, FLAC__bool complete)
{
	struct harness *h = (struct harness*)cd;
	harness_sample_heap(h);
	harness_log(h, EVT_END, track, complete ? 1 : 0, 0);
	if(track < 100) {
		h->ended[track]++;
		h->end_complete[track] = complete ? 1 : 0;
		h->end_seq[track] = ++h->event_seq;
		if(h->concurrent > 0)
			h->concurrent--;
	}
}

static const FLAC__CueSplitIO harness_io = {
	0,
	h_read,
	h_begin,
	h_write,
	h_end
};

/* ------------------------------------------------------------------ */
/* Independent decoding of one output                                  */
/* ------------------------------------------------------------------ */

struct out_decode {
	struct membuf *src;
	size_t cursor; /* independent read position; membuf.pos is not touched */
	FLAC__int32 *pcm[2];
	size_t pcm_cap;
	FLAC__uint64 frames;
	uint32_t channels, bps, rate;
	FLAC__uint64 total_samples;
	int md5_nonzero;
	int got_cue;
	FLAC__StreamMetadata *cue_meta;
	int saw_other_metadata;
	int bad;
};

static FLAC__StreamDecoderReadStatus od_read(const FLAC__StreamDecoder *d, FLAC__byte *buf, size_t *n, void *u)
{
	struct out_decode *o = (struct out_decode*)u;
	size_t want = *n;
	(void)d;
	if(o->cursor == o->src->used) {
		*n = 0;
		return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
	}
	if(want > o->src->used - o->cursor)
		want = o->src->used - o->cursor;
	memcpy(buf, o->src->data + o->cursor, want);
	o->cursor += want;
	*n = want;
	return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

static FLAC__StreamDecoderWriteStatus od_write(const FLAC__StreamDecoder *d, const FLAC__Frame *frame, const FLAC__int32 *const buf[], void *u)
{
	struct out_decode *o = (struct out_decode*)u;
	uint32_t i, c;
	(void)d;
	if((size_t)o->frames + frame->header.blocksize > o->pcm_cap) {
		size_t cap = o->pcm_cap ? o->pcm_cap : 256;
		while(cap < (size_t)o->frames + frame->header.blocksize)
			cap *= 2;
		for(c = 0; c < 2; c++) {
			FLAC__int32 *p = (FLAC__int32*)realloc(o->pcm[c], cap * sizeof(FLAC__int32));
			if(p == 0) {
				o->bad = 1;
				return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
			}
			o->pcm[c] = p;
		}
		o->pcm_cap = cap;
	}
	for(c = 0; c < frame->header.channels; c++)
		for(i = 0; i < frame->header.blocksize; i++)
			o->pcm[c][o->frames + i] = buf[c][i];
	o->frames += frame->header.blocksize;
	return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static void od_meta(const FLAC__StreamDecoder *d, const FLAC__StreamMetadata *m, void *u)
{
	struct out_decode *o = (struct out_decode*)u;
	(void)d;
	if(m->type == FLAC__METADATA_TYPE_STREAMINFO) {
		o->channels = m->data.stream_info.channels;
		o->bps = m->data.stream_info.bits_per_sample;
		o->rate = m->data.stream_info.sample_rate;
		o->total_samples = m->data.stream_info.total_samples;
		{
			int i;
			for(i = 0; i < 16; i++)
				if(m->data.stream_info.md5sum[i])
					o->md5_nonzero = 1;
		}
	}
	else if(m->type == FLAC__METADATA_TYPE_CUESHEET) {
		o->got_cue = 1;
		/* the decoder's block does not outlive the callback, keep a clone */
		o->cue_meta = FLAC__metadata_object_clone(m);
		if(o->cue_meta == 0)
			o->bad = 1;
	}
	else {
		o->saw_other_metadata = 1;
	}
}

static void od_error(const FLAC__StreamDecoder *d, FLAC__StreamDecoderErrorStatus s, void *u)
{
	(void)d, (void)s;
	((struct out_decode*)u)->bad = 1;
}

static int decode_output(struct membuf *b, struct out_decode *o)
{
	FLAC__StreamDecoder *d;
	int ok = 1;
	memset(o, 0, sizeof(*o));
	o->src = b;
	d = FLAC__stream_decoder_new();
	if(d == 0)
		return 0;
	FLAC__stream_decoder_set_md5_checking(d, 1);
	FLAC__stream_decoder_set_metadata_respond(d, FLAC__METADATA_TYPE_CUESHEET);
	if(FLAC__stream_decoder_init_stream(d, od_read, 0, 0, 0, 0, od_write, od_meta, od_error, o) != FLAC__STREAM_DECODER_INIT_STATUS_OK)
		ok = 0;
	else if(!FLAC__stream_decoder_process_until_end_of_stream(d))
		ok = 0;
	else if(FLAC__stream_decoder_get_state(d) != FLAC__STREAM_DECODER_END_OF_STREAM)
		ok = 0;
	else if(!FLAC__stream_decoder_finish(d)) /* MD5 mismatch shows up here */
		o->bad = 1;
	FLAC__stream_decoder_delete(d);
	return ok && !o->bad;
}

static void out_decode_free(struct out_decode *o)
{
	free(o->pcm[0]);
	free(o->pcm[1]);
	if(o->cue_meta)
		FLAC__metadata_object_delete(o->cue_meta);
}

/* ------------------------------------------------------------------ */
/* Expected routing, computed independently from the cue geometry      */
/* ------------------------------------------------------------------ */

struct expect_seg {
	uint32_t output_track;
	FLAC__uint64 start, end;
	int has_source_track; /* 0 for the synthetic hidden-audio output */
	uint32_t source_track;
};

static FLAC__uint64 spec_index01(const struct trk_spec *t)
{
	uint32_t i;
	for(i = 0; i < t->n_indices; i++)
		if(t->indices[i].number == 1)
			return t->offset + t->indices[i].offset;
	return (FLAC__uint64)(-1);
}

static uint32_t compute_expected(const struct src_spec *spec, FLAC__CueSplitPregapPolicy policy, struct expect_seg *out)
{
	uint32_t music = spec->n_tracks - 1;
	uint32_t n = 0, i;
	if(policy == FLAC__CUESPLIT_PREGAP_NEXT) {
		for(i = 0; i < music; i++) {
			out[n].output_track = i + 1;
			out[n].start = spec->tracks[i].offset;
			out[n].end = spec->tracks[i + 1].offset;
			out[n].has_source_track = 1;
			out[n].source_track = i;
			n++;
		}
	}
	else {
		if(spec_index01(&spec->tracks[0]) > 0) {
			out[n].output_track = 0;
			out[n].start = 0;
			out[n].end = spec_index01(&spec->tracks[0]);
			out[n].has_source_track = 0;
			n++;
		}
		for(i = 0; i < music; i++) {
			out[n].output_track = i + 1;
			out[n].start = spec_index01(&spec->tracks[i]);
			out[n].end = (i + 1 < music) ? spec_index01(&spec->tracks[i + 1]) : spec->total_samples;
			out[n].has_source_track = 1;
			out[n].source_track = i;
			n++;
		}
	}
	return n;
}

/* ------------------------------------------------------------------ */
/* Full validation of one split run                                    */
/* ------------------------------------------------------------------ */

static int validate_run(struct harness *h, const struct src_spec *spec, FLAC__CueSplitPregapPolicy policy)
{
	struct expect_seg expect[101];
	uint32_t n_expect, k;

	n_expect = compute_expected(spec, policy, expect);

	TASK_CHECK(h->max_concurrent == 1);
	TASK_CHECK(h->read_monotonic);
	TASK_CHECK(h->src.pos == h->src.used); /* every source byte consumed exactly once */

	for(k = 0; k < n_expect; k++) {
		const struct expect_seg *e = &expect[k];
		struct out_decode o;
		const FLAC__uint64 length = e->end - e->start;
		uint32_t i, c;

		TASK_CHECK(e->output_track < 100);
		TASK_CHECK(h->began[e->output_track] == 1);
		TASK_CHECK(h->ended[e->output_track] == 1);
		TASK_CHECK(h->end_complete[e->output_track] == 1);
		TASK_CHECK(h->begin_samples[e->output_track] == length);
		TASK_CHECK(h->outs[e->output_track].used > 0);

		TASK_CHECK(decode_output(&h->outs[e->output_track], &o));
		TASK_CHECK(o.frames == length);
		TASK_CHECK(o.total_samples == length);
		TASK_CHECK(o.md5_nonzero);
		TASK_CHECK(o.channels == spec->channels);
		TASK_CHECK(o.bps == spec->bps);
		TASK_CHECK(o.rate == spec->rate);
		TASK_CHECK(o.got_cue);
		TASK_CHECK(!o.saw_other_metadata);

		/* sample-exact PCM against hand-computed absolute positions */
		for(i = 0; i < length; i++)
			for(c = 0; c < spec->channels; c++)
				TASK_CHECK(o.pcm[c][i] == pcm_sample(e->start + i, c, spec->bps));

		/* output cuesheet geometry */
		TASK_CHECK(o.cue_meta->data.cue_sheet.is_cd == false);
		TASK_CHECK(o.cue_meta->data.cue_sheet.lead_in == 0);
		TASK_CHECK(o.cue_meta->data.cue_sheet.num_tracks == 2);
		if(spec->catalog)
			TASK_CHECK(strcmp(o.cue_meta->data.cue_sheet.media_catalog_number, spec->catalog) == 0);
		TASK_CHECK(o.cue_meta->data.cue_sheet.tracks[1].number == 170);
		TASK_CHECK(o.cue_meta->data.cue_sheet.tracks[1].num_indices == 0);
		TASK_CHECK(o.cue_meta->data.cue_sheet.tracks[1].offset == length);
		TASK_CHECK(o.cue_meta->data.cue_sheet.tracks[0].offset == 0);

		if(!e->has_source_track) {
			const FLAC__StreamMetadata_CueSheet_Track *t = &o.cue_meta->data.cue_sheet.tracks[0];
			TASK_CHECK(t->number == 1);
			TASK_CHECK(t->num_indices == 1);
			TASK_CHECK(t->indices[0].number == 1);
			TASK_CHECK(t->indices[0].offset == 0);
			TASK_CHECK(t->pre_emphasis == 0);
		}
		else {
			const struct trk_spec *st = &spec->tracks[e->source_track];
			const FLAC__StreamMetadata_CueSheet_Track *t = &o.cue_meta->data.cue_sheet.tracks[0];
			uint32_t want = 0, j;
			TASK_CHECK(t->number == st->number);
			TASK_CHECK(t->pre_emphasis == (st->pre_emphasis ? 1 : 0));
			if(st->isrc)
				TASK_CHECK(strcmp(t->isrc, st->isrc) == 0);
			for(j = 0; j < st->n_indices; j++) {
				FLAC__uint64 absolute = st->offset + st->indices[j].offset;
				if(absolute >= e->start && absolute < e->end)
					want++;
			}
			TASK_CHECK(t->num_indices == want);
			want = 0;
			for(j = 0; j < st->n_indices; j++) {
				FLAC__uint64 absolute = st->offset + st->indices[j].offset;
				if(absolute >= e->start && absolute < e->end) {
					TASK_CHECK(t->indices[want].number == st->indices[j].number);
					TASK_CHECK(t->indices[want].offset == absolute - e->start);
					want++;
				}
			}
		}
		out_decode_free(&o);
	}

	/* no other output tracks were opened */
	{
		uint32_t t;
		for(t = 0; t < 100; t++) {
			int listed = 0;
			for(k = 0; k < n_expect; k++)
				if(expect[k].output_track == t)
					listed = 1;
			if(!listed) {
				TASK_CHECK(h->began[t] == 0);
				TASK_CHECK(h->ended[t] == 0);
			}
		}
	}

	/* event ordering: begin -> end per track, tracks opened in increasing order */
	{
		uint32_t t;
		uint64_t prev_begin = 0;
		for(t = 0; t < 100; t++) {
			if(h->began[t]) {
				TASK_CHECK(h->begin_seq[t] < h->end_seq[t]);
				TASK_CHECK(h->begin_seq[t] > prev_begin);
				prev_begin = h->begin_seq[t];
			}
		}
	}
	return 0;
}

static int run_split(struct harness *h, FLAC__CueSplitPregapPolicy policy, FLAC__CueSplitStatus want)
{
	FLAC__CueSplitIO io = harness_io;
	FLAC__CueSplitStatus status;
	io.client_data = h;
	status = FLAC__stream_cuesplit(&io, policy);
	TASK_CHECK(status == want);
	return status == want ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* Shared geometries                                                   */
/* ------------------------------------------------------------------ */

/* 3 stereo tracks, all carrying INDEX00 pregaps, 44.1k/16bit */
static const struct trk_spec stereo_tracks[4] = {
	{1, "AAA111222333", 1, 0, {{0, 0}, {1, 29}}, 2},
	{2, "BBB444555666", 0, 713, {{0, 0}, {1, 47}, {2, 120}}, 3},
	{3, 0, 1, 1491, {{0, 0}, {1, 101}}, 2},
	{170, 0, 0, 2304, {{0, 0}}, 0}
};

static const struct src_spec stereo_spec = {
	44100, 2, 16, 576, 2304, "CATALOG0001", stereo_tracks, 4, 0, 0, 0
};

/* hidden audio before first INDEX01 plus more pregaps */
static const struct trk_spec hidden_tracks[4] = {
	{1, "ISRC00000001", 0, 0, {{0, 0}, {1, 35}}, 2},
	{2, "ISRC00000002", 0, 200, {{0, 0}, {1, 20}, {2, 50}}, 3},
	{3, 0, 0, 400, {{1, 0}}, 1},
	{170, 0, 0, 500, {{0, 0}}, 0}
};

static const struct src_spec hidden_spec = {
	44100, 2, 16, 64, 500, 0, hidden_tracks, 4, 0, 0, 0
};

/* no pregaps anywhere: INDEX01 at relative offset 0 */
static const struct trk_spec plain_tracks[4] = {
	{1, 0, 0, 0, {{1, 0}}, 1},
	{2, 0, 0, 137, {{1, 0}}, 1},
	{3, 0, 0, 260, {{1, 0}, {2, 40}}, 2},
	{170, 0, 0, 400, {{0, 0}}, 0}
};

static const struct src_spec plain_spec = {
	48000, 2, 16, 128, 400, 0, plain_tracks, 4, 0, 0, 0
};

/* one 64-sample frame crosses two cut points; last frame is short */
static const struct trk_spec tight_tracks[4] = {
	{1, 0, 0, 0, {{1, 0}}, 1},
	{2, 0, 0, 10, {{1, 0}}, 1},
	{3, 0, 0, 27, {{1, 0}}, 1},
	{170, 0, 0, 300, {{0, 0}}, 0}
};

static const struct src_spec tight_spec = {
	44100, 2, 16, 64, 300, 0, tight_tracks, 4, 0, 0, 0
};

/* mono 24 bit/48k, one pregap */
static const struct trk_spec mono_tracks[3] = {
	{1, 0, 0, 0, {{0, 0}, {1, 13}}, 2},
	{2, 0, 0, 250, {{1, 0}}, 1},
	{170, 0, 0, 513, {{0, 0}}, 0}
};

static const struct src_spec mono_spec = {
	48000, 1, 24, 96, 513, 0, mono_tracks, 3, 0, 0, 0
};

/* ------------------------------------------------------------------ */
/* routing group                                                       */
/* ------------------------------------------------------------------ */

static int case_routing_next(void)
{
	struct harness h;
	harness_init(&h);
	TASK_CHECK(build_source(&stereo_spec, &h.src, 0));
	TASK_CHECK(run_split(&h, FLAC__CUESPLIT_PREGAP_NEXT, FLAC__CUESPLIT_STATUS_OK) == 0);
	TASK_CHECK(validate_run(&h, &stereo_spec, FLAC__CUESPLIT_PREGAP_NEXT) == 0);
	harness_free(&h);
	return 0;
}

static int case_routing_previous(void)
{
	struct harness h;
	harness_init(&h);
	TASK_CHECK(build_source(&stereo_spec, &h.src, 0));
	TASK_CHECK(run_split(&h, FLAC__CUESPLIT_PREGAP_PREVIOUS, FLAC__CUESPLIT_STATUS_OK) == 0);
	TASK_CHECK(validate_run(&h, &stereo_spec, FLAC__CUESPLIT_PREGAP_PREVIOUS) == 0);
	harness_free(&h);
	return 0;
}

static int case_routing_hidden_audio(void)
{
	struct harness hn, hp;

	harness_init(&hn);
	TASK_CHECK(build_source(&hidden_spec, &hn.src, 0));
	TASK_CHECK(run_split(&hn, FLAC__CUESPLIT_PREGAP_NEXT, FLAC__CUESPLIT_STATUS_OK) == 0);
	/* NEXT keeps the pregap inside track 1, so no track 0 output exists */
	TASK_CHECK(hn.began[0] == 0);
	TASK_CHECK(validate_run(&hn, &hidden_spec, FLAC__CUESPLIT_PREGAP_NEXT) == 0);
	harness_free(&hn);

	harness_init(&hp);
	TASK_CHECK(build_source(&hidden_spec, &hp.src, 0));
	TASK_CHECK(run_split(&hp, FLAC__CUESPLIT_PREGAP_PREVIOUS, FLAC__CUESPLIT_STATUS_OK) == 0);
	TASK_CHECK(hp.began[0] == 1);
	TASK_CHECK(hp.begin_samples[0] == 35);
	TASK_CHECK(hp.ended[0] == 1 && hp.end_complete[0] == 1);
	TASK_CHECK(validate_run(&hp, &hidden_spec, FLAC__CUESPLIT_PREGAP_PREVIOUS) == 0);
	harness_free(&hp);
	return 0;
}

static int case_routing_no_pregap(void)
{
	struct harness hn, hp;
	harness_init(&hn);
	TASK_CHECK(build_source(&plain_spec, &hn.src, 0));
	TASK_CHECK(run_split(&hn, FLAC__CUESPLIT_PREGAP_NEXT, FLAC__CUESPLIT_STATUS_OK) == 0);
	TASK_CHECK(validate_run(&hn, &plain_spec, FLAC__CUESPLIT_PREGAP_NEXT) == 0);
	harness_free(&hn);

	harness_init(&hp);
	TASK_CHECK(build_source(&plain_spec, &hp.src, 0));
	TASK_CHECK(run_split(&hp, FLAC__CUESPLIT_PREGAP_PREVIOUS, FLAC__CUESPLIT_STATUS_OK) == 0);
	TASK_CHECK(validate_run(&hp, &plain_spec, FLAC__CUESPLIT_PREGAP_PREVIOUS) == 0);
	/* without pregaps both policies cut at the same samples */
	TASK_CHECK(hp.began[0] == 0);
	TASK_CHECK(hp.begin_samples[1] == hn.begin_samples[1]);
	TASK_CHECK(hp.begin_samples[2] == hn.begin_samples[2]);
	TASK_CHECK(hp.begin_samples[3] == hn.begin_samples[3]);
	harness_free(&hp);
	return 0;
}

static int case_routing_multi_cut_and_tail(void)
{
	struct harness h;
	struct expect_seg expect[16];
	uint32_t n;
	harness_init(&h);
	TASK_CHECK(build_source(&tight_spec, &h.src, 0));
	TASK_CHECK(run_split(&h, FLAC__CUESPLIT_PREGAP_NEXT, FLAC__CUESPLIT_STATUS_OK) == 0);
	n = compute_expected(&tight_spec, FLAC__CUESPLIT_PREGAP_NEXT, expect);
	TASK_CHECK(n == 3);
	TASK_CHECK(expect[0].end - expect[0].start == 10);
	TASK_CHECK(expect[1].end - expect[1].start == 17);
	TASK_CHECK(expect[2].end - expect[2].start == 273);
	/* the 300-sample stream ends with a 44-sample partial frame */
	TASK_CHECK(300 % 64 != 0);
	TASK_CHECK(validate_run(&h, &tight_spec, FLAC__CUESPLIT_PREGAP_NEXT) == 0);
	harness_free(&h);
	return 0;
}

static int case_routing_mono24(void)
{
	struct harness hn, hp;
	harness_init(&hn);
	TASK_CHECK(build_source(&mono_spec, &hn.src, 0));
	TASK_CHECK(run_split(&hn, FLAC__CUESPLIT_PREGAP_NEXT, FLAC__CUESPLIT_STATUS_OK) == 0);
	TASK_CHECK(validate_run(&hn, &mono_spec, FLAC__CUESPLIT_PREGAP_NEXT) == 0);
	harness_free(&hn);

	harness_init(&hp);
	TASK_CHECK(build_source(&mono_spec, &hp.src, 0));
	TASK_CHECK(run_split(&hp, FLAC__CUESPLIT_PREGAP_PREVIOUS, FLAC__CUESPLIT_STATUS_OK) == 0);
	TASK_CHECK(hp.began[0] == 1);
	TASK_CHECK(hp.begin_samples[0] == 13);
	TASK_CHECK(validate_run(&hp, &mono_spec, FLAC__CUESPLIT_PREGAP_PREVIOUS) == 0);
	harness_free(&hp);
	return 0;
}

static int case_routing_stereo16_extra_indices(void)
{
	struct harness h;
	struct out_decode o;
	harness_init(&h);
	TASK_CHECK(build_source(&stereo_spec, &h.src, 0));
	TASK_CHECK(run_split(&h, FLAC__CUESPLIT_PREGAP_NEXT, FLAC__CUESPLIT_STATUS_OK) == 0);
	/* track 2 keeps INDEX00/01/02 translated onto its own start at 713 */
	TASK_CHECK(decode_output(&h.outs[2], &o));
	TASK_CHECK(o.got_cue);
	TASK_CHECK(o.cue_meta->data.cue_sheet.tracks[0].num_indices == 3);
	TASK_CHECK(o.cue_meta->data.cue_sheet.tracks[0].indices[0].number == 0);
	TASK_CHECK(o.cue_meta->data.cue_sheet.tracks[0].indices[0].offset == 0);
	TASK_CHECK(o.cue_meta->data.cue_sheet.tracks[0].indices[1].number == 1);
	TASK_CHECK(o.cue_meta->data.cue_sheet.tracks[0].indices[1].offset == 47);
	TASK_CHECK(o.cue_meta->data.cue_sheet.tracks[0].indices[2].number == 2);
	TASK_CHECK(o.cue_meta->data.cue_sheet.tracks[0].indices[2].offset == 120);
	TASK_CHECK(o.cue_meta->data.cue_sheet.tracks[0].pre_emphasis == 0);
	out_decode_free(&o);
	TASK_CHECK(validate_run(&h, &stereo_spec, FLAC__CUESPLIT_PREGAP_NEXT) == 0);
	harness_free(&h);
	return 0;
}

/* ------------------------------------------------------------------ */
/* metadata group                                                      */
/* ------------------------------------------------------------------ */

static int case_meta_missing_cuesheet(void)
{
	struct harness h;
	struct src_spec spec = stereo_spec;
	spec.tracks = 0;
	spec.n_tracks = 0;
	harness_init(&h);
	TASK_CHECK(build_source(&spec, &h.src, 0));
	TASK_CHECK(run_split(&h, FLAC__CUESPLIT_PREGAP_NEXT, FLAC__CUESPLIT_STATUS_INVALID_METADATA) == 0);
	TASK_CHECK(h.n_events == 0);
	harness_free(&h);
	return 0;
}

static int case_meta_two_cuesheets(void)
{
	/* hand-built stream carrying two CUESHEET blocks */
	struct harness h;
	FLAC__StreamEncoder *enc = FLAC__stream_encoder_new();
	FLAC__StreamMetadata *c1 = build_cuesheet(&stereo_spec);
	FLAC__StreamMetadata *c2 = build_cuesheet(&stereo_spec);
	FLAC__StreamMetadata *blocks[2];
	FLAC__int32 *pcm;
	uint32_t i;
	harness_init(&h);
	TASK_CHECK(enc && c1 && c2);
	blocks[0] = c1;
	blocks[1] = c2;
	pcm = (FLAC__int32*)malloc(sizeof(FLAC__int32) * stereo_spec.total_samples * 2);
	TASK_CHECK(pcm != 0);
	for(i = 0; i < stereo_spec.total_samples * 2; i++)
		pcm[i] = pcm_sample(i / 2, i % 2, 16);
	FLAC__stream_encoder_set_channels(enc, 2);
	FLAC__stream_encoder_set_bits_per_sample(enc, 16);
	FLAC__stream_encoder_set_sample_rate(enc, 44100);
	FLAC__stream_encoder_set_blocksize(enc, 576);
	FLAC__stream_encoder_set_total_samples_estimate(enc, stereo_spec.total_samples);
	TASK_CHECK(FLAC__stream_encoder_set_metadata(enc, blocks, 2));
	TASK_CHECK(FLAC__stream_encoder_init_stream(enc, source_enc_write_, 0, 0, 0, &h.src) == FLAC__STREAM_ENCODER_INIT_STATUS_OK);
	TASK_CHECK(FLAC__stream_encoder_process_interleaved(enc, pcm, stereo_spec.total_samples));
	TASK_CHECK(FLAC__stream_encoder_finish(enc));
	TASK_CHECK(run_split(&h, FLAC__CUESPLIT_PREGAP_NEXT, FLAC__CUESPLIT_STATUS_INVALID_METADATA) == 0);
	TASK_CHECK(h.n_events == 0);
	free(pcm);
	FLAC__stream_encoder_delete(enc);
	FLAC__metadata_object_delete(c1);
	FLAC__metadata_object_delete(c2);
	harness_free(&h);
	return 0;
}

static int case_meta_bad_indexes(void)
{
	struct {
		const char *name;
		struct trk_spec tracks[3];
	} variants[4];
	struct trk_spec lead;
	int v;
	memset(&lead, 0, sizeof(lead));
	lead.number = 170;
	lead.offset = 200;

	/* variant 0: INDEX00 not at relative offset 0 */
	variants[0].tracks[0] = (struct trk_spec){1, 0, 0, 0, {{0, 5}, {1, 9}}, 2};
	variants[0].tracks[1] = (struct trk_spec){2, 0, 0, 100, {{1, 0}}, 1};
	variants[0].tracks[2] = lead;
	/* variant 1: index offsets not increasing */
	variants[1].tracks[0] = (struct trk_spec){1, 0, 0, 0, {{1, 0}, {2, 30}}, 2};
	variants[1].tracks[1] = (struct trk_spec){2, 0, 0, 100, {{1, 0}, {2, 20}, {3, 10}}, 3};
	variants[1].tracks[2] = lead;
	/* variant 2: index point outside the track's half-open interval */
	variants[2].tracks[0] = (struct trk_spec){1, 0, 0, 0, {{1, 0}}, 1};
	variants[2].tracks[1] = (struct trk_spec){2, 0, 0, 100, {{1, 0}, {2, 101}}, 2};
	variants[2].tracks[2] = lead;
	/* variant 3: no INDEX01 at all */
	variants[3].tracks[0] = (struct trk_spec){1, 0, 0, 0, {{0, 0}}, 1};
	variants[3].tracks[1] = (struct trk_spec){2, 0, 0, 100, {{1, 0}}, 1};
	variants[3].tracks[2] = lead;

	for(v = 0; v < 4; v++) {
		struct harness h;
		struct src_spec spec;
		harness_init(&h);
		memset(&spec, 0, sizeof(spec));
		spec.rate = 44100;
		spec.channels = 2;
		spec.bps = 16;
		spec.blocksize = 64;
		spec.total_samples = 200;
		spec.tracks = variants[v].tracks;
		spec.n_tracks = 3;
		TASK_CHECK(build_source(&spec, &h.src, 0));
		TASK_CHECK(run_split(&h, FLAC__CUESPLIT_PREGAP_NEXT, FLAC__CUESPLIT_STATUS_INVALID_METADATA) == 0);
		TASK_CHECK(h.n_events == 0);
		harness_free(&h);
	}
	return 0;
}

static int case_meta_bad_leadout_and_order(void)
{
	struct trk_spec tracks[3];
	struct src_spec spec;
	struct harness h;

	memset(tracks, 0, sizeof(tracks));
	tracks[0] = (struct trk_spec){1, 0, 0, 0, {{1, 0}}, 1};
	tracks[1] = (struct trk_spec){2, 0, 0, 100, {{1, 0}}, 1};
	tracks[2] = (struct trk_spec){170, 0, 0, 199, {{0, 0}}, 0}; /* one sample short */
	memset(&spec, 0, sizeof(spec));
	spec.rate = 44100;
	spec.channels = 2;
	spec.bps = 16;
	spec.blocksize = 64;
	spec.total_samples = 200;
	spec.tracks = tracks;
	spec.n_tracks = 3;

	harness_init(&h);
	TASK_CHECK(build_source(&spec, &h.src, 0));
	TASK_CHECK(run_split(&h, FLAC__CUESPLIT_PREGAP_NEXT, FLAC__CUESPLIT_STATUS_INVALID_METADATA) == 0);
	TASK_CHECK(h.n_events == 0);
	harness_free(&h);

	/* strictly increasing track offsets */
	tracks[1].offset = 50;
	harness_init(&h);
	TASK_CHECK(build_source(&spec, &h.src, 0));
	TASK_CHECK(run_split(&h, FLAC__CUESPLIT_PREGAP_NEXT, FLAC__CUESPLIT_STATUS_INVALID_METADATA) == 0);
	TASK_CHECK(h.n_events == 0);
	harness_free(&h);
	return 0;
}

static int case_meta_cd_cuesheet(void)
{
	struct harness h;
	/* a CD-legal geometry: all offsets and index points multiples of 588 */
	struct trk_spec cd_tracks[4];
	struct src_spec cd_spec;
	FLAC__StreamMetadata *cue;
	memset(cd_tracks, 0, sizeof(cd_tracks));
	cd_tracks[0] = (struct trk_spec){1, 0, 0, 0, {{0, 0}, {1, 588}}, 2};
	cd_tracks[1] = (struct trk_spec){2, 0, 0, 1176, {{1, 0}}, 1};
	cd_tracks[2] = (struct trk_spec){3, 0, 0, 1764, {{1, 0}}, 1};
	cd_tracks[3] = (struct trk_spec){170, 0, 0, 2352, {{0, 0}}, 0};
	memset(&cd_spec, 0, sizeof(cd_spec));
	cd_spec.rate = 44100;
	cd_spec.channels = 2;
	cd_spec.bps = 16;
	cd_spec.blocksize = 588;
	cd_spec.total_samples = 2352;
	cd_spec.tracks = cd_tracks;
	cd_spec.n_tracks = 4;

	harness_init(&h);
	TASK_CHECK(build_source(&cd_spec, &h.src, 0));
	cue = build_cuesheet(&cd_spec);
	TASK_CHECK(cue != 0);
	cue->data.cue_sheet.is_cd = true;
	cue->data.cue_sheet.lead_in = 2 * 44100;
	TASK_CHECK(FLAC__format_cuesheet_is_legal(&cue->data.cue_sheet, /*check_cd_da_subset=*/true, 0));
	/* flip the is_cd bit directly in the encoded CUESHEET block byte:
	 * 128 catalog bytes + 8 lead-in bytes, then the IS_CD flag is the
	 * high bit of body byte 136. Locate the block by scanning for its
	 * metadata header (type 5). */
	{
		size_t p = 4; /* skip "fLaC" */
		int found = 0;
		while(p + 4 <= h.src.used) {
			FLAC__byte last = h.src.data[p];
			FLAC__byte type = last & 0x7f;
			uint32_t len = ((uint32_t)h.src.data[p + 1] << 16) | ((uint32_t)h.src.data[p + 2] << 8) | h.src.data[p + 3];
			if(type == 5 && len >= 137) {
				h.src.data[p + 4 + 136] |= 0x80;
				found = 1;
				break;
			}
			p += 4 + len;
			if(last & 0x80)
				break;
		}
		TASK_CHECK(found);
	}
	TASK_CHECK(run_split(&h, FLAC__CUESPLIT_PREGAP_NEXT, FLAC__CUESPLIT_STATUS_INVALID_METADATA) == 0);
	TASK_CHECK(h.n_events == 0);
	FLAC__metadata_object_delete(cue);
	harness_free(&h);
	return 0;
}

static int case_meta_unknown_total(void)
{
	struct harness h;
	harness_init(&h);
	TASK_CHECK(build_source(&stereo_spec, &h.src, 0));
	/* STREAMINFO total_samples: low 36 bits at file bytes 21..25, the
	 * high nibble of byte 21 is shared with bits-per-sample */
	h.src.data[21] &= (FLAC__byte)0xf0;
	memset(h.src.data + 22, 0, 4);
	TASK_CHECK(run_split(&h, FLAC__CUESPLIT_PREGAP_NEXT, FLAC__CUESPLIT_STATUS_INVALID_METADATA) == 0);
	TASK_CHECK(h.n_events == 0);
	harness_free(&h);
	return 0;
}

static int case_meta_unsupported_format(void)
{
	struct harness h;
	struct src_spec spec = stereo_spec;
	struct trk_spec tracks[4];
	memcpy(tracks, stereo_tracks, sizeof(tracks));
	tracks[3].offset = 600;
	spec.channels = 3;
	spec.total_samples = 600;
	spec.tracks = tracks;
	harness_init(&h);
	TASK_CHECK(build_source(&spec, &h.src, 0));
	TASK_CHECK(run_split(&h, FLAC__CUESPLIT_PREGAP_NEXT, FLAC__CUESPLIT_STATUS_INVALID_METADATA) == 0);
	TASK_CHECK(h.n_events == 0);
	harness_free(&h);
	return 0;
}

static int case_meta_blocks_not_inherited(void)
{
	struct harness h;
	struct src_spec spec = stereo_spec;
	spec.extra_metadata = 1;
	harness_init(&h);
	TASK_CHECK(build_source(&spec, &h.src, 0));
	TASK_CHECK(run_split(&h, FLAC__CUESPLIT_PREGAP_NEXT, FLAC__CUESPLIT_STATUS_OK) == 0);
	TASK_CHECK(validate_run(&h, &spec, FLAC__CUESPLIT_PREGAP_NEXT) == 0);
	harness_free(&h);
	return 0;
}

static int case_meta_io_read_failure(void)
{
	struct harness h;
	harness_init(&h);
	TASK_CHECK(build_source(&stereo_spec, &h.src, 0));
	/* byte-at-a-time reads make the failure land reliably inside audio */
	h.one_byte = 1;
	h.fail_read_after_calls = 3000;
	{
		FLAC__CueSplitIO io = harness_io;
		FLAC__CueSplitStatus got;
		io.client_data = &h;
		got = FLAC__stream_cuesplit(&io, FLAC__CUESPLIT_PREGAP_NEXT);
		TASK_CHECK(got == FLAC__CUESPLIT_STATUS_IO_ERROR);
	}
	/* the output that was open gets exactly one end(false), nothing more opens */
	TASK_CHECK(h.max_concurrent <= 1);
	{
		uint32_t t, open_count = 0, failed_ends = 0;
		for(t = 0; t < 100; t++) {
			TASK_CHECK(h.ended[t] <= 1);
			if(h.began[t])
				open_count++;
			if(h.ended[t] && !h.end_complete[t])
				failed_ends++;
		}
		TASK_CHECK(open_count >= 1);
		TASK_CHECK(failed_ends == 1);
	}
	harness_free(&h);
	return 0;
}

static int case_meta_io_begin_failure(void)
{
	struct harness h;
	harness_init(&h);
	TASK_CHECK(build_source(&stereo_spec, &h.src, 0));
	h.fail_begin_track = 2;
	TASK_CHECK(run_split(&h, FLAC__CUESPLIT_PREGAP_NEXT, FLAC__CUESPLIT_STATUS_IO_ERROR) == 0);
	TASK_CHECK(h.began[1] == 1);
	TASK_CHECK(h.ended[1] == 1 && h.end_complete[1] == 1);
	TASK_CHECK(h.began[2] == 1);
	TASK_CHECK(h.ended[2] == 0); /* begin failed: no end callback for it */
	TASK_CHECK(h.began[3] == 0);
	harness_free(&h);
	return 0;
}

static int case_meta_io_write_failure(void)
{
	struct harness h;
	harness_init(&h);
	TASK_CHECK(build_source(&stereo_spec, &h.src, 0));
	h.fail_write_track = 3;
	TASK_CHECK(run_split(&h, FLAC__CUESPLIT_PREGAP_NEXT, FLAC__CUESPLIT_STATUS_IO_ERROR) == 0);
	TASK_CHECK(h.began[1] == 1 && h.end_complete[1] == 1);
	TASK_CHECK(h.began[2] == 1 && h.end_complete[2] == 1);
	TASK_CHECK(h.began[3] == 1);
	TASK_CHECK(h.ended[3] == 1 && h.end_complete[3] == 0);
	harness_free(&h);
	return 0;
}

static int case_meta_corrupt_md5(void)
{
	struct harness h;
	struct out_decode o;
	struct src_spec spec = stereo_spec;
	spec.corrupt_md5 = 1;
	harness_init(&h);
	TASK_CHECK(build_source(&spec, &h.src, 0));
	TASK_CHECK(run_split(&h, FLAC__CUESPLIT_PREGAP_NEXT, FLAC__CUESPLIT_STATUS_SOURCE_MD5_MISMATCH) == 0);
	/* late failure: every output still completed cleanly and is independently valid */
	TASK_CHECK(h.ended[1] == 1 && h.end_complete[1] == 1);
	TASK_CHECK(h.ended[2] == 1 && h.end_complete[2] == 1);
	TASK_CHECK(h.ended[3] == 1 && h.end_complete[3] == 1);
	TASK_CHECK(decode_output(&h.outs[2], &o));
	TASK_CHECK(o.md5_nonzero);
	TASK_CHECK(o.total_samples == h.begin_samples[2]);
	out_decode_free(&o);
	harness_free(&h);
	return 0;
}

static int case_meta_zero_md5(void)
{
	struct harness h;
	struct src_spec spec = stereo_spec;
	spec.zero_md5 = 1;
	harness_init(&h);
	TASK_CHECK(build_source(&spec, &h.src, 0));
	TASK_CHECK(run_split(&h, FLAC__CUESPLIT_PREGAP_NEXT, FLAC__CUESPLIT_STATUS_OK) == 0);
	TASK_CHECK(validate_run(&h, &spec, FLAC__CUESPLIT_PREGAP_NEXT) == 0);
	harness_free(&h);
	return 0;
}

/* ------------------------------------------------------------------ */
/* streaming group                                                     */
/* ------------------------------------------------------------------ */

static int case_stream_one_byte_and_events(void)
{
	struct harness h;
	harness_init(&h);
	h.one_byte = 1;
	TASK_CHECK(build_source(&hidden_spec, &h.src, 0));
	TASK_CHECK(run_split(&h, FLAC__CUESPLIT_PREGAP_PREVIOUS, FLAC__CUESPLIT_STATUS_OK) == 0);
	/* every source byte crossed the forward-only channel exactly once, one at a time */
	TASK_CHECK(h.src.used > 0);
	TASK_CHECK(h.read_bytes == h.src.used);
	TASK_CHECK(h.read_calls >= h.src.used);
	TASK_CHECK(h.read_monotonic);
	TASK_CHECK(h.max_concurrent == 1);
	/* begin/end pairing through the event log */
	{
		uint32_t t;
		for(t = 0; t < 100; t++) {
			if(h.began[t]) {
				TASK_CHECK(h.ended[t] == 1);
				TASK_CHECK(h.end_complete[t] == 1);
			}
		}
	}
	TASK_CHECK(validate_run(&h, &hidden_spec, FLAC__CUESPLIT_PREGAP_PREVIOUS) == 0);
	harness_free(&h);
	return 0;
}

static int case_stream_truncated_source(void)
{
	struct harness h;
	harness_init(&h);
	TASK_CHECK(build_source(&stereo_spec, &h.src, 0));
	h.src.used -= 60; /* cut inside the final frame: premature EOF */
	TASK_CHECK(run_split(&h, FLAC__CUESPLIT_PREGAP_NEXT, FLAC__CUESPLIT_STATUS_DECODE_ERROR) == 0);
	{
		uint32_t t, failed_ends = 0;
		for(t = 0; t < 100; t++) {
			TASK_CHECK(h.ended[t] <= 1);
			if(h.ended[t] && !h.end_complete[t])
				failed_ends++;
		}
		TASK_CHECK(failed_ends == 1);
	}
	harness_free(&h);
	return 0;
}

#if defined(CUESPLIT_HAVE_HEAP_STATS)
static int build_long_source(uint32_t total, struct membuf *out)
{
	struct trk_spec tracks[3];
	struct src_spec spec;
	memset(tracks, 0, sizeof(tracks));
	tracks[0] = (struct trk_spec){1, 0, 0, 0, {{0, 0}, {1, 100}}, 2};
	tracks[1] = (struct trk_spec){2, 0, 0, total / 2, {{1, 0}}, 1};
	tracks[2] = (struct trk_spec){170, 0, 0, total, {{0, 0}}, 0};
	memset(&spec, 0, sizeof(spec));
	spec.rate = 44100;
	spec.channels = 2;
	spec.bps = 16;
	spec.blocksize = 512;
	spec.total_samples = total;
	spec.tracks = tracks;
	spec.n_tracks = 3;
	return build_source(&spec, out, 0);
}

static int case_stream_allocation_peak(void)
{
	struct harness short_h, long_h;
	FLAC__CueSplitIO io;

	harness_init(&short_h);
	short_h.null_sink = 1;
	TASK_CHECK(build_long_source(4096, &short_h.src));
	harness_mark_heap_base(&short_h);
	io = harness_io;
	io.client_data = &short_h;
	TASK_CHECK(FLAC__stream_cuesplit(&io, FLAC__CUESPLIT_PREGAP_NEXT) == FLAC__CUESPLIT_STATUS_OK);
	TASK_CHECK(short_h.heap_delta_peak > 0);

	harness_init(&long_h);
	long_h.null_sink = 1;
	TASK_CHECK(build_long_source(400000, &long_h.src));
	harness_mark_heap_base(&long_h);
	io = harness_io;
	io.client_data = &long_h;
	TASK_CHECK(FLAC__stream_cuesplit(&io, FLAC__CUESPLIT_PREGAP_NEXT) == FLAC__CUESPLIT_STATUS_OK);
	TASK_CHECK(long_h.heap_delta_peak > 0);

	/* ~100x longer stream must not grow the library working set beyond noise */
	TASK_CHECK(long_h.heap_delta_peak <= short_h.heap_delta_peak + 256 * 1024);
	TASK_CHECK(long_h.max_concurrent == 1 && short_h.max_concurrent == 1);
	TASK_CHECK(long_h.ended[1] == 1 && long_h.end_complete[1] == 1);
	TASK_CHECK(long_h.ended[2] == 1 && long_h.end_complete[2] == 1);

	harness_free(&short_h);
	harness_free(&long_h);
	return 0;
}
#else
static int case_stream_allocation_peak(void)
{
	/* without heap statistics just re-confirm blocksize-bounded framing */
	struct harness h;
	harness_init(&h);
	TASK_CHECK(build_source(&tight_spec, &h.src, 0));
	TASK_CHECK(run_split(&h, FLAC__CUESPLIT_PREGAP_NEXT, FLAC__CUESPLIT_STATUS_OK) == 0);
	TASK_CHECK(h.max_concurrent == 1);
	harness_free(&h);
	return 0;
}
#endif

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
	/* routing */
	{"routing", "next_policy_pregaps", case_routing_next},
	{"routing", "previous_policy_pregaps", case_routing_previous},
	{"routing", "hidden_audio_track_zero", case_routing_hidden_audio},
	{"routing", "no_pregap_policies_match", case_routing_no_pregap},
	{"routing", "frame_crosses_multiple_cuts_and_short_tail", case_routing_multi_cut_and_tail},
	{"routing", "mono_24bit", case_routing_mono24},
	{"routing", "extra_indices_translated", case_routing_stereo16_extra_indices},
	/* metadata */
	{"metadata", "missing_cuesheet", case_meta_missing_cuesheet},
	{"metadata", "duplicate_cuesheet", case_meta_two_cuesheets},
	{"metadata", "bad_index_variants", case_meta_bad_indexes},
	{"metadata", "bad_leadout_and_track_order", case_meta_bad_leadout_and_order},
	{"metadata", "cd_cuesheet_rejected", case_meta_cd_cuesheet},
	{"metadata", "unknown_total_samples", case_meta_unknown_total},
	{"metadata", "unsupported_channel_count", case_meta_unsupported_format},
	{"metadata", "other_blocks_not_inherited", case_meta_blocks_not_inherited},
	{"metadata", "read_callback_failure", case_meta_io_read_failure},
	{"metadata", "begin_callback_failure", case_meta_io_begin_failure},
	{"metadata", "write_callback_failure", case_meta_io_write_failure},
	{"metadata", "corrupt_source_md5_late_mismatch", case_meta_corrupt_md5},
	{"metadata", "zero_source_md5_allowed", case_meta_zero_md5},
	/* streaming */
	{"streaming", "one_byte_forward_reads_and_event_log", case_stream_one_byte_and_events},
	{"streaming", "truncated_source_decode_error", case_stream_truncated_source},
	{"streaming", "allocation_peak_independent_of_length", case_stream_allocation_peak}
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);

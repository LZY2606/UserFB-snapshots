/* Tests for the plm_audio_mix_t multi-stream audio mixer.
 *
 * The reference signal is built independently: each selected track is fed
 * through a plain, separate plm_audio_t MP2 decoder and the PCM is stored;
 * a second mixer is never used as an oracle. The test code then performs
 * the same integer-timeline linear interpolation and gain summation itself.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* --- allocation instrumentation ----------------------------------------- */

static size_t test_live_bytes;
static size_t test_peak_bytes;
static unsigned long test_alloc_calls;

static void *test_malloc(size_t n) {
	size_t *header = (size_t *)malloc(n + sizeof(size_t));
	if (!header) {
		return NULL;
	}
	header[0] = n;
	test_live_bytes += n;
	if (test_live_bytes > test_peak_bytes) {
		test_peak_bytes = test_live_bytes;
	}
	test_alloc_calls++;
	return header + 1;
}

static void test_free(void *p) {
	if (!p) {
		return;
	}
	size_t *header = (size_t *)p - 1;
	test_live_bytes -= header[0];
	free(header);
}

static void *test_realloc(void *p, size_t n) {
	if (!p) {
		return test_malloc(n);
	}
	size_t *header = (size_t *)p - 1;
	size_t old = header[0];
	size_t *grown = (size_t *)realloc(header, n + sizeof(size_t));
	if (!grown) {
		return NULL;
	}
	grown[0] = n;
	test_live_bytes += n - old;
	if (test_live_bytes > test_peak_bytes) {
		test_peak_bytes = test_live_bytes;
	}
	test_alloc_calls++;
	return grown + 1;
}

#define PLM_MALLOC test_malloc
#define PLM_FREE test_free
#define PLM_REALLOC test_realloc

#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"

#include "task_support/check.h"

/* --- fixtures: the shipped MP2 clips ------------------------------------ */

struct mp2_clip {
	uint8_t *data;
	size_t size;
	size_t frame_size;
	int frames;
	int rate;
	int mono;
};

static uint8_t *load_file(const char *path, size_t *out_size) {
	FILE *f = fopen(path, "rb");
	if (!f) {
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *buf = (uint8_t *)malloc((size_t)n);
	if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
		free(buf);
		fclose(f);
		return NULL;
	}
	fclose(f);
	*out_size = (size_t)n;
	return buf;
}

static struct mp2_clip *load_clip(const char *path, int rate, int mono) {
	size_t size;
	uint8_t *data = load_file(path, &size);
	if (!data) {
		return NULL;
	}
	struct mp2_clip *clip = (struct mp2_clip *)malloc(sizeof(struct mp2_clip));
	int frames = 0;
	size_t first = 0;
	for (size_t i = 0; i + 1 < size; i++) {
		if (data[i] == 0xff && (data[i + 1] & 0xfe) == 0xfc) {
			if (frames == 0) {
				first = i;
			}
			frames++;
		}
	}
	clip->data = data + first;
	clip->size = size - first;
	clip->frames = frames;
	clip->frame_size = clip->size / (size_t)frames;
	clip->rate = rate;
	clip->mono = mono;
	return clip;
}

/* --- MPEG-PS writer ------------------------------------------------------ */

struct ps_writer {
	uint8_t *data;
	size_t length;
	size_t capacity;
};

static void ps_put(struct ps_writer *w, const uint8_t *bytes, size_t n) {
	if (w->length + n > w->capacity) {
		while (w->length + n > w->capacity) {
			w->capacity *= 2;
		}
		w->data = (uint8_t *)realloc(w->data, w->capacity);
	}
	memcpy(w->data + w->length, bytes, n);
	w->length += n;
}

static void ps_u8(struct ps_writer *w, uint8_t v) {
	ps_put(w, &v, 1);
}

static void ps_put_pts(struct ps_writer *w, uint64_t v) {
	uint8_t b[5] = {
		(uint8_t)(0x21 | (((v >> 30) & 7) << 1)),
		(uint8_t)(v >> 22),
		(uint8_t)((((v >> 15) & 127) << 1) | 1),
		(uint8_t)(v >> 7),
		(uint8_t)(((v & 127) << 1) | 1)
	};
	ps_put(w, b, 5);
}

struct ps_pes {
	int track;
	int frame_index;
	int64_t pts;
};

/* Build an audio-only MPEG-1 Program Stream. `clips` maps track index to
 * the clip supplying its (possibly repeated) frame bytes. */
static uint8_t *build_ps(const struct ps_pes *pes, size_t pes_count,
	struct mp2_clip **clips, int num_tracks, size_t *out_size) {

	struct ps_writer w;
	w.capacity = 4096;
	w.length = 0;
	w.data = (uint8_t *)malloc(w.capacity);

	/* pack header, same layout as the shipped fixture */
	uint8_t pack[] = {0x00, 0x00, 0x01, 0xba, 0x21, 0x00, 0x01, 0x00,
		0x01, 0xc3, 0x33, 0x67};
	ps_put(&w, pack, sizeof(pack));

	/* system header declaring num_tracks audio streams */
	ps_u8(&w, 0x00); ps_u8(&w, 0x00); ps_u8(&w, 0x01); ps_u8(&w, 0xbb);
	uint16_t header_length = (uint16_t)(6 + 3 * num_tracks);
	ps_u8(&w, (uint8_t)(header_length >> 8));
	ps_u8(&w, (uint8_t)(header_length & 0xff));
	ps_u8(&w, 0xc3); ps_u8(&w, 0x33); ps_u8(&w, 0x67);
	ps_u8(&w, (uint8_t)(num_tracks << 2));
	ps_u8(&w, 0x20); ps_u8(&w, 0xff);
	for (int t = 0; t < num_tracks; t++) {
		ps_u8(&w, (uint8_t)(0xc0 + t));
		ps_u8(&w, 0xc0);
		ps_u8(&w, 0x20);
	}

	/* PES packets, already ordered by non-decreasing PTS */
	for (size_t i = 0; i < pes_count; i++) {
		struct mp2_clip *clip = clips[pes[i].track];
		size_t frame_size = clip->frame_size;
		const uint8_t *frame = clip->data +
			(size_t)pes[i].frame_index * frame_size;
		uint16_t pes_length = (uint16_t)(frame_size + 5);
		ps_u8(&w, 0x00); ps_u8(&w, 0x00); ps_u8(&w, 0x01);
		ps_u8(&w, (uint8_t)(0xc0 + pes[i].track));
		ps_u8(&w, (uint8_t)(pes_length >> 8));
		ps_u8(&w, (uint8_t)(pes_length & 0xff));
		ps_put_pts(&w, (uint64_t)pes[i].pts);
		ps_put(&w, frame, frame_size);
	}

	uint8_t end[] = {0x00, 0x00, 0x01, 0xb9};
	ps_put(&w, end, sizeof(end));

	*out_size = w.length;
	return w.data;
}

/* --- independent reference: per-track plain MP2 decoders ---------------- */

struct ref_frame {
	int64_t pts; /* 90 kHz ticks */
	float pcm[PLM_AUDIO_SAMPLES_PER_FRAME * 2];
};

struct ref_track {
	int active;
	int rate;
	int mono;
	int frame_count;
	int frame_capacity;
	struct ref_frame *frames;
};

static void ref_track_init(struct ref_track *t, int active) {
	memset(t, 0, sizeof(*t));
	t->active = active;
	t->frame_capacity = 64;
	t->frames = (struct ref_frame *)malloc(
		sizeof(struct ref_frame) * (size_t)t->frame_capacity);
}

static void ref_track_free(struct ref_track *t) {
	free(t->frames);
	memset(t, 0, sizeof(*t));
}

static void ref_track_push(struct ref_track *t, int64_t pts,
	const float *interleaved, int rate, int mono) {
	if (t->frame_count == t->frame_capacity) {
		t->frame_capacity *= 2;
		t->frames = (struct ref_frame *)realloc(t->frames,
			sizeof(struct ref_frame) * (size_t)t->frame_capacity);
	}
	struct ref_frame *f = &t->frames[t->frame_count++];
	f->pts = pts;
	memcpy(f->pcm, interleaved,
		sizeof(float) * PLM_AUDIO_SAMPLES_PER_FRAME * 2);
	t->rate = rate;
	t->mono = mono;
}

/* Decode every PES of one track with an independent plm_audio_t. */
static void build_reference(const struct ps_pes *pes, size_t pes_count,
	int track_index, struct mp2_clip *clip, struct ref_track *out) {

	plm_buffer_t *buffer = plm_buffer_create_with_capacity(2048);
	plm_audio_t *decoder = plm_audio_create_with_buffer(buffer, TRUE);

	for (size_t i = 0; i < pes_count; i++) {
		if (pes[i].track != track_index) {
			continue;
		}
		size_t frame_size = clip->frame_size;
		uint8_t *frame = clip->data +
			(size_t)pes[i].frame_index * frame_size;
		plm_buffer_write(buffer, frame, frame_size);
		plm_samples_t *samples = plm_audio_decode(decoder);
		if (!samples) {
			continue;
		}
		int rate = plm_audio_get_samplerate(decoder);
		int mono = (decoder->mode == PLM_AUDIO_MODE_MONO);
		ref_track_push(out, pes[i].pts, samples->interleaved, rate, mono);
	}
	plm_audio_destroy(decoder);
}

static int frame_duration_ticks(int rate) {
	return (rate == 32000) ? 3240 : 2160;
}

/* Independent render of one track at t8 (time in 1/8 of a 90k tick). */
static void ref_render_track(const struct ref_track *t, int64_t t8,
	float out[2]) {
	out[0] = out[1] = 0.0f;

	const struct ref_frame *frame = NULL;
	int index = -1;
	for (int i = 0; i < t->frame_count; i++) {
		int64_t start = t->frames[i].pts;
		int64_t end = start + frame_duration_ticks(t->rate);
		if (start * 8 <= t8 && t8 < end * 8) {
			frame = &t->frames[i];
			index = i;
			break;
		}
	}
	if (!frame) {
		return;
	}

	int64_t b = t8 - frame->pts * 8;
	int64_t numerator;
	int64_t denominator;
	if (t->rate == 48000) {
		numerator = b;
		denominator = 15;
	}
	else {
		numerator = b * 2;
		denominator = 45;
	}
	int64_t source_index = numerator / denominator;
	int64_t remainder = numerator - source_index * denominator;
	float fraction = (float)remainder / (float)denominator;

	int at_last = (source_index + 1 >= PLM_AUDIO_SAMPLES_PER_FRAME);
	int64_t frame_end = frame->pts + frame_duration_ticks(t->rate);
	const float *next_pcm = NULL;
	if (at_last && index + 1 < t->frame_count &&
		t->frames[index + 1].pts == frame_end) {
		next_pcm = t->frames[index + 1].pcm;
	}

	if (t->mono) {
		float left = frame->pcm[source_index * 2];
		float right_sample = 0.0f;
		if (!at_last) {
			right_sample = frame->pcm[(source_index + 1) * 2];
		}
		else if (next_pcm) {
			right_sample = next_pcm[0];
		}
		float value = left + fraction * (right_sample - left);
		out[0] = out[1] = value;
	}
	else {
		for (int channel = 0; channel < 2; channel++) {
			float left = frame->pcm[source_index * 2 + channel];
			float right_sample = 0.0f;
			if (!at_last) {
				right_sample = frame->pcm[(source_index + 1) * 2 + channel];
			}
			else if (next_pcm) {
				right_sample = next_pcm[channel];
			}
			out[channel] = left + fraction * (right_sample - left);
		}
	}
}

static void ref_render_mix(const struct ref_track tracks[4],
	unsigned int mask, int64_t origin, const float gains[4],
	uint64_t position, float out[2]) {
	int64_t t8 = origin * 8 + 15 * (int64_t)position;
	out[0] = out[1] = 0.0f;
	for (int i = 0; i < 4; i++) {
		if (!(mask & (1u << i))) {
			continue;
		}
		float contribution[2];
		ref_render_track(&tracks[i], t8, contribution);
		out[0] += gains[i] * contribution[0];
		out[1] += gains[i] * contribution[1];
	}
}

/* --- shared helpers ------------------------------------------------------ */

struct float_vec {
	float *data;
	size_t count;
	size_t capacity;
};

static void vec_push(struct float_vec *v, const float *frames, size_t n) {
	if (v->count + n > v->capacity) {
		while (v->count + n > v->capacity) {
			v->capacity = v->capacity ? v->capacity * 2 : 256;
		}
		v->data = (float *)realloc(v->data, v->capacity * sizeof(float));
	}
	memcpy(v->data + v->count, frames, n * sizeof(float));
	v->count += n;
}

static void vec_free(struct float_vec *v) {
	free(v->data);
	v->data = NULL;
	v->count = v->capacity = 0;
}

/* Drain a fully-fed, ended mixer in varying chunk sizes. */
static void drain_mixer(plm_audio_mix_t *mix, struct float_vec *out,
	size_t chunk) {
	float block[256];
	if (chunk > 128) {
		chunk = 128;
	}
	for (;;) {
		size_t got = plm_audio_mix_read(mix, block, chunk);
		if (got == 0) {
			break;
		}
		vec_push(out, block, got * 2);
	}
}

static int assert_matches_reference(const struct float_vec *out,
	const struct ref_track tracks[4], unsigned int mask, int64_t origin,
	const float gains[4], uint64_t expected_frames) {

	TASK_CHECK(out->count == expected_frames * 2);
	for (uint64_t n = 0; n < expected_frames; n++) {
		float expected[2];
		ref_render_mix(tracks, mask, origin, gains, n, expected);
		TASK_CHECK(out->data[n * 2] == expected[0]);
		TASK_CHECK(out->data[n * 2 + 1] == expected[1]);
	}
	return 0;
}
#define ASSERT_REF(...) TASK_CHECK(assert_matches_reference(__VA_ARGS__) == 0)

/* Build a PES schedule: two tracks with explicit per-frame PTS, reusing
 * clip frame bytes cyclically. */
static struct ps_pes *make_schedule(const int64_t *pts0, int n0,
	const int64_t *pts1, int n1, size_t *out_count) {

	size_t total = (size_t)n0 + (size_t)n1;
	struct ps_pes *pes =
		(struct ps_pes *)malloc(sizeof(struct ps_pes) * total);
	size_t k = 0;
	int i0 = 0, i1 = 0;
	while (i0 < n0 || i1 < n1) {
		int pick;
		if (i0 == n0) {
			pick = 1;
		}
		else if (i1 == n1) {
			pick = 0;
		}
		else if (pts0[i0] <= pts1[i1]) {
			pick = 0;
		}
		else {
			pick = 1;
		}
		if (pick == 0) {
			pes[k].track = 0;
			pes[k].frame_index = i0 % 8;
			pes[k].pts = pts0[i0];
			i0++;
		}
		else {
			pes[k].track = 1;
			pes[k].frame_index = i1 % 8;
			pes[k].pts = pts1[i1];
			i1++;
		}
		k++;
	}
	*out_count = k;
	return pes;
}

/* Merge up to four per-track PTS lists into one non-decreasing-PES schedule.
 * tracks[t] is skipped when counts[t] == 0. Ties keep a stable track order. */
static struct ps_pes *make_schedule_multi(const int64_t *const *pts,
	const int *counts, int num_tracks, size_t *out_count) {

	size_t total = 0;
	for (int t = 0; t < num_tracks; t++) {
		total += (size_t)counts[t];
	}
	struct ps_pes *pes =
		(struct ps_pes *)malloc(sizeof(struct ps_pes) * total);
	int cursor[4] = {0, 0, 0, 0};
	size_t k = 0;
	for (;;) {
		int pick = -1;
		int64_t best = 0;
		for (int t = 0; t < num_tracks; t++) {
			if (cursor[t] >= counts[t]) {
				continue;
			}
			if (pick < 0 || pts[t][cursor[t]] < best) {
				pick = t;
				best = pts[t][cursor[t]];
			}
		}
		if (pick < 0) {
			break;
		}
		pes[k].track = pick;
		pes[k].frame_index = cursor[pick] % 8;
		pes[k].pts = pts[pick][cursor[pick]];
		cursor[pick]++;
		k++;
	}
	*out_count = k;
	return pes;
}

static struct mp2_clip *clip_32k_mono;
static struct mp2_clip *clip_48k_stereo;

static int load_fixtures(void) {
	clip_32k_mono =
		load_clip("task_support/media/mono32.mp2", 32000, 1);
	clip_48k_stereo =
		load_clip("task_support/media/stereo.mp2", 48000, 0);
	return clip_32k_mono && clip_48k_stereo &&
		clip_32k_mono->frames == 8 && clip_48k_stereo->frames == 8;
}

static void build_two_track_refs(const struct ps_pes *pes, size_t count,
	struct ref_track out[4]) {
	for (int t = 0; t < 4; t++) {
		ref_track_init(&out[t], t < 2);
	}
	build_reference(pes, count, 0, clip_32k_mono, &out[0]);
	build_reference(pes, count, 1, clip_48k_stereo, &out[1]);
}

static void free_refs(struct ref_track tracks[4]) {
	for (int t = 0; t < 4; t++) {
		ref_track_free(&tracks[t]);
	}
}

/* ========================================================================= */
/* timeline group                                                           */
/* ========================================================================= */

static int case_dual_full_timeline(void) {
	TASK_CHECK(load_fixtures());

	int64_t pts0[8], pts1[8];
	for (int i = 0; i < 8; i++) {
		pts0[i] = 90000 + 3240 * i;
		pts1[i] = 91080 + 2160 * i;
	}
	size_t pes_count;
	struct ps_pes *pes = make_schedule(pts0, 8, pts1, 8, &pes_count);
	struct mp2_clip *clips[2] = {clip_32k_mono, clip_48k_stereo};

	size_t ps_size;
	uint8_t *ps = build_ps(pes, pes_count, clips, 2, &ps_size);
	plm_buffer_t *buffer = plm_buffer_create_for_appending(4096);
	plm_buffer_write(buffer, ps, ps_size);
	plm_buffer_signal_end(buffer);

	int64_t origin = 90000;
	plm_audio_mix_t *mix =
		plm_audio_mix_create(buffer, 1, 0x3, origin);
	TASK_CHECK(mix != NULL);
	TASK_CHECK(plm_audio_mix_get_position(mix) == 0);

	struct ref_track refs[4];
	build_two_track_refs(pes, pes_count, refs);
	TASK_CHECK(refs[0].frame_count == 8 && refs[1].frame_count == 8);
	TASK_CHECK(refs[0].rate == 32000 && refs[0].mono == 1);
	TASK_CHECK(refs[1].rate == 48000 && refs[1].mono == 0);

	struct float_vec out;
	memset(&out, 0, sizeof(out));
	drain_mixer(mix, &out, 37);

	/* latest exclusive end: track 0 ends at 90000 + 8*3240 = 115920 */
	int64_t latest_end = 115920;
	uint64_t expected =
		(uint64_t)((latest_end - origin) * 48000 / 90000);
	TASK_CHECK(expected == 13824);
	float default_gains[4] = {1, 1, 1, 1};
	ASSERT_REF(&out, refs, 0x3, origin,
		default_gains, expected);
	TASK_CHECK(plm_audio_mix_get_position(mix) == expected);
	TASK_CHECK(plm_audio_mix_has_ended(mix) == 1);

	/* no tail frames: another read after end delivers nothing */
	float tail[4];
	TASK_CHECK(plm_audio_mix_read(mix, tail, 2) == 0);
	TASK_CHECK(plm_audio_mix_get_position(mix) == expected);

	plm_audio_mix_destroy(mix);
	free_refs(refs);
	vec_free(&out);
	free(pes);
	free(ps);
	return 0;
}

static int case_origin_lead_silence(void) {
	TASK_CHECK(load_fixtures());

	int64_t pts0[4], pts1[4];
	for (int i = 0; i < 4; i++) {
		pts0[i] = 90000 + 3240 * i;
		pts1[i] = 91080 + 2160 * i;
	}
	size_t pes_count;
	struct ps_pes *pes = make_schedule(pts0, 4, pts1, 4, &pes_count);
	struct mp2_clip *clips[2] = {clip_32k_mono, clip_48k_stereo};

	size_t ps_size;
	uint8_t *ps = build_ps(pes, pes_count, clips, 2, &ps_size);
	plm_buffer_t *buffer = plm_buffer_create_for_appending(4096);
	plm_buffer_write(buffer, ps, ps_size);
	plm_buffer_signal_end(buffer);

	int64_t origin = 45000; /* 2400 output frames before the first PTS */
	plm_audio_mix_t *mix =
		plm_audio_mix_create(buffer, 1, 0x3, origin);
	TASK_CHECK(mix != NULL);

	struct ref_track refs[4];
	build_two_track_refs(pes, pes_count, refs);

	struct float_vec out;
	memset(&out, 0, sizeof(out));
	drain_mixer(mix, &out, 128);

	int64_t latest_end = 90000 + 4 * 3240; /* 102960 */
	uint64_t expected =
		(uint64_t)((latest_end - origin) * 48000 / 90000);
	TASK_CHECK(expected == (uint64_t)(57960LL * 48000 / 90000));
	float default_gains[4] = {1, 1, 1, 1};
	ASSERT_REF(&out, refs, 0x3, origin,
		default_gains, expected);

	/* every frame strictly before the first PTS is silence on both tracks */
	uint64_t lead = (uint64_t)((90000 - origin) * 48000 / 90000);
	TASK_CHECK(lead == 24000);
	for (uint64_t n = 0; n < lead; n++) {
		TASK_CHECK(out.data[n * 2] == 0.0f);
		TASK_CHECK(out.data[n * 2 + 1] == 0.0f);
	}
	/* the MP2 synthesis window emits leading zero samples; the first real
	 * content lands within the first few hundred output frames of each
	 * track (measured first-nonzero source indices: 97 and 129) */
	int found_non_silent = 0;
	for (uint64_t n = lead; n < lead + 400 && n < expected; n++) {
		if (out.data[n * 2] != 0.0f || out.data[n * 2 + 1] != 0.0f) {
			found_non_silent = 1;
		}
	}
	TASK_CHECK(found_non_silent);

	plm_audio_mix_destroy(mix);
	free_refs(refs);
	vec_free(&out);
	free(pes);
	free(ps);
	return 0;
}

static int case_gaps_and_contiguity(void) {
	TASK_CHECK(load_fixtures());

	/* track 0: frames at 90000, 93240 (contiguous), then a one-frame gap,
	 * then 99720; track 1: contiguous 48k frames starting at 91080. */
	int64_t pts0[3] = {90000, 93240, 99720};
	int64_t pts1[5];
	for (int i = 0; i < 5; i++) {
		pts1[i] = 91080 + 2160 * i;
	}
	size_t pes_count;
	struct ps_pes *pes = make_schedule(pts0, 3, pts1, 5, &pes_count);
	struct mp2_clip *clips[2] = {clip_32k_mono, clip_48k_stereo};

	size_t ps_size;
	uint8_t *ps = build_ps(pes, pes_count, clips, 2, &ps_size);
	plm_buffer_t *buffer = plm_buffer_create_for_appending(4096);
	plm_buffer_write(buffer, ps, ps_size);
	plm_buffer_signal_end(buffer);

	int64_t origin = 90000;
	plm_audio_mix_t *mix =
		plm_audio_mix_create(buffer, 1, 0x3, origin);
	TASK_CHECK(mix != NULL);

	struct ref_track refs[4];
	build_two_track_refs(pes, pes_count, refs);
	TASK_CHECK(refs[0].frame_count == 3);

	struct float_vec out;
	memset(&out, 0, sizeof(out));
	drain_mixer(mix, &out, 11);

	int64_t latest_end = 99720 + 3240; /* 102960; the 32k track ends last */
	uint64_t expected =
		(uint64_t)((latest_end - origin) * 48000 / 90000);
	float default_gains[4] = {1, 1, 1, 1};
	ASSERT_REF(&out, refs, 0x3, origin,
		default_gains, expected);

	/* inside the track-0 gap, the mix equals the track-1 contribution alone */
	int64_t gap_start = 96480; /* exclusive end of the second 32k frame */
	int64_t gap_end = 99720;
	uint64_t n_mid = (uint64_t)(((gap_start + gap_end) / 2 - origin)
		* 48000 / 90000);
	TASK_CHECK(n_mid < expected);
	float track1_only[2];
	int64_t t8_mid = origin * 8 + 15 * (int64_t)n_mid;
	ref_render_track(&refs[1], t8_mid, track1_only);
	TASK_CHECK(out.data[n_mid * 2] == track1_only[0]);
	TASK_CHECK(out.data[n_mid * 2 + 1] == track1_only[1]);

	/* the contiguous boundary on track 1 (two adjacent frames) is rendered
	 * with the next frame's first sample as the right neighbour */
	uint64_t n_boundary =
		(uint64_t)((pts1[1] - origin) * 48000 / 90000);
	TASK_CHECK(n_boundary > 0 && n_boundary < expected);
	float expected_boundary[2];
	ref_render_mix(refs, 0x3, origin, default_gains,
		n_boundary - 1, expected_boundary);
	TASK_CHECK(out.data[(n_boundary - 1) * 2] == expected_boundary[0]);
	TASK_CHECK(out.data[(n_boundary - 1) * 2 + 1] ==
		expected_boundary[1]);

	plm_audio_mix_destroy(mix);
	free_refs(refs);
	vec_free(&out);
	free(pes);
	free(ps);
	return 0;
}

static int case_mask_selects_subset(void) {
	TASK_CHECK(load_fixtures());

	int64_t pts0[6], pts1[6];
	for (int i = 0; i < 6; i++) {
		pts0[i] = 90000 + 3240 * i;
		pts1[i] = 91080 + 2160 * i;
	}
	size_t pes_count;
	struct ps_pes *pes = make_schedule(pts0, 6, pts1, 6, &pes_count);
	struct mp2_clip *clips[2] = {clip_32k_mono, clip_48k_stereo};

	size_t ps_size;
	uint8_t *ps = build_ps(pes, pes_count, clips, 2, &ps_size);
	plm_buffer_t *buffer = plm_buffer_create_for_appending(4096);
	plm_buffer_write(buffer, ps, ps_size);
	plm_buffer_signal_end(buffer);

	int64_t origin = 90000;
	plm_audio_mix_t *mix =
		plm_audio_mix_create(buffer, 1, 0x2, origin); /* track 1 only */
	TASK_CHECK(mix != NULL);

	struct ref_track refs[4];
	build_two_track_refs(pes, pes_count, refs);

	struct float_vec out;
	memset(&out, 0, sizeof(out));
	drain_mixer(mix, &out, 64);

	/* the unselected 32k track must not extend the timeline: track 1 ends
	 * at 91080 + 6*2160 = 104040, earlier than track 0's 109440 */
	int64_t selected_end = 91080 + 6 * 2160;
	uint64_t expected =
		(uint64_t)((selected_end - origin) * 48000 / 90000);
	float gains[4] = {1, 1, 1, 1};
	ASSERT_REF(&out, refs, 0x2, origin, gains, expected);
	TASK_CHECK(plm_audio_mix_has_ended(mix) == 1);

	plm_audio_mix_destroy(mix);
	free_refs(refs);
	vec_free(&out);
	free(pes);
	free(ps);
	return 0;
}

static int case_four_tracks_noncontiguous_mask(void) {
	TASK_CHECK(load_fixtures());

	/* four tracks with staggered starts and lengths; even tracks reuse the
	 * 32k mono clip, odd tracks the 48k stereo clip */
	int64_t pts0[3] = {90000, 93240, 96480};
	int64_t pts1[4] = {92160, 94320, 96480, 98640};
	int64_t pts2[2] = {94320, 97560};
	int64_t pts3[5] = {91080, 93240, 95400, 97560, 99720};
	const int64_t *pts[4] = {pts0, pts1, pts2, pts3};
	int counts[4] = {3, 4, 2, 5};

	size_t pes_count;
	struct ps_pes *pes =
		make_schedule_multi(pts, counts, 4, &pes_count);

	/* global PES order must be non-decreasing PTS */
	for (size_t i = 1; i < pes_count; i++) {
		TASK_CHECK(pes[i].pts >= pes[i - 1].pts);
	}

	struct mp2_clip *clips[4] = {
		clip_32k_mono, clip_48k_stereo,
		clip_32k_mono, clip_48k_stereo
	};
	size_t ps_size;
	uint8_t *ps = build_ps(pes, pes_count, clips, 4, &ps_size);
	plm_buffer_t *buffer = plm_buffer_create_for_appending(4096);
	plm_buffer_write(buffer, ps, ps_size);
	plm_buffer_signal_end(buffer);

	/* non-contiguous mask: select 0xC0 and 0xC2 (bits 0 and 2) only */
	unsigned int mask = 0x5;
	int64_t origin = 90000;
	plm_audio_mix_t *mix =
		plm_audio_mix_create(buffer, 1, mask, origin);
	TASK_CHECK(mix != NULL);
	float gains[4] = {0.5f, 99.0f, 1.25f, 7.0f};
	TASK_CHECK(plm_audio_mix_set_gains(mix, gains) == 1);

	struct ref_track refs[4];
	for (int t = 0; t < 4; t++) {
		ref_track_init(&refs[t], (mask & (1u << t)) ? 1 : 0);
	}
	build_reference(pes, pes_count, 0, clip_32k_mono, &refs[0]);
	build_reference(pes, pes_count, 2, clip_32k_mono, &refs[2]);
	TASK_CHECK(refs[0].frame_count == 3 && refs[2].frame_count == 2);

	struct float_vec out;
	memset(&out, 0, sizeof(out));
	drain_mixer(mix, &out, 23);

	/* unselected tracks 1 and 3 neither extend the timeline nor sound.
	 * Track 0 ends at 96480+3240=99720; track 2 ends at 97560+3240=100800,
	 * so the latest selected exclusive end is 100800. */
	int64_t selected_end = 97560 + 3240;
	uint64_t expected =
		(uint64_t)((selected_end - origin) * 48000 / 90000);
	TASK_CHECK(expected == 5760);
	ASSERT_REF(&out, refs, mask, origin, gains, expected);
	TASK_CHECK(plm_audio_mix_has_ended(mix) == 1);

	/* changing the gains of unselected streams has no audible effect.
	 * rewind resets gains to the defaults, so re-apply the selected-stream
	 * gains (0.5 and 1.25) and only alter the unselected entries */
	plm_audio_mix_rewind(mix);
	float gains_alt[4] = {0.5f, -4.0f, 1.25f, 0.0f};
	TASK_CHECK(plm_audio_mix_set_gains(mix, gains_alt) == 1);
	struct float_vec out2;
	memset(&out2, 0, sizeof(out2));
	drain_mixer(mix, &out2, 7);
	TASK_CHECK(out2.count == out.count);
	TASK_CHECK(memcmp(out2.data, out.data,
		out.count * sizeof(float)) == 0);

	plm_audio_mix_destroy(mix);
	free_refs(refs);
	vec_free(&out);
	vec_free(&out2);
	free(pes);
	free(ps);
	return 0;
}

/* ========================================================================= */
/* mixing group                                                             */
/* ========================================================================= */

static int case_default_gains_mono_duplicate(void) {
	TASK_CHECK(load_fixtures());

	int64_t pts0[2] = {90000, 93240};
	int64_t pts1[2] = {90000, 92160};
	size_t pes_count;
	struct ps_pes *pes = make_schedule(pts0, 2, pts1, 2, &pes_count);
	struct mp2_clip *clips[2] = {clip_32k_mono, clip_48k_stereo};

	size_t ps_size;
	uint8_t *ps = build_ps(pes, pes_count, clips, 2, &ps_size);
	plm_buffer_t *buffer = plm_buffer_create_for_appending(4096);
	plm_buffer_write(buffer, ps, ps_size);
	plm_buffer_signal_end(buffer);

	plm_audio_mix_t *mix =
		plm_audio_mix_create(buffer, 1, 0x3, 90000);
	TASK_CHECK(mix != NULL);

	struct ref_track refs[4];
	build_two_track_refs(pes, pes_count, refs);

	struct float_vec out;
	memset(&out, 0, sizeof(out));
	drain_mixer(mix, &out, 5);

	/* default gains are 1.0; the mono track contributes identical L/R */
	float gains[4] = {1, 1, 1, 1};
	uint64_t expected = (uint64_t)((90000 + 2 * 3240 - 90000)
		* 48000 / 90000);
	ASSERT_REF(&out, refs, 0x3, 90000, gains, expected);

	int stereo_differs = 0;
	for (uint64_t n = 0; n < expected; n++) {
		/* both tracks share the first PTS; the stereo clip's synthesis
		 * window is zero for its first 129 source samples, so up to that
		 * point only the mono track is audible and L must equal R */
		if (n < 100) {
			TASK_CHECK(out.data[n * 2] == out.data[n * 2 + 1]);
		}
		if (out.data[n * 2] != out.data[n * 2 + 1]) {
			stereo_differs = 1;
		}
	}
	TASK_CHECK(stereo_differs); /* the stereo clip really is stereo */

	plm_audio_mix_destroy(mix);
	free_refs(refs);
	vec_free(&out);
	free(pes);
	free(ps);
	return 0;
}

static int case_asymmetric_gains_and_update(void) {
	TASK_CHECK(load_fixtures());

	int64_t pts0[8], pts1[8];
	for (int i = 0; i < 8; i++) {
		pts0[i] = 90000 + 3240 * i;
		pts1[i] = 91080 + 2160 * i;
	}
	size_t pes_count;
	struct ps_pes *pes = make_schedule(pts0, 8, pts1, 8, &pes_count);
	struct mp2_clip *clips[2] = {clip_32k_mono, clip_48k_stereo};

	size_t ps_size;
	uint8_t *ps = build_ps(pes, pes_count, clips, 2, &ps_size);
	plm_buffer_t *buffer = plm_buffer_create_for_appending(4096);
	plm_buffer_write(buffer, ps, ps_size);
	plm_buffer_signal_end(buffer);

	plm_audio_mix_t *mix =
		plm_audio_mix_create(buffer, 1, 0x3, 90000);
	TASK_CHECK(mix != NULL);

	struct ref_track refs[4];
	build_two_track_refs(pes, pes_count, refs);

	float gains_a[4] = {0.5f, 1.5f, 1.0f, 1.0f};
	TASK_CHECK(plm_audio_mix_set_gains(mix, gains_a) == 1);

	struct float_vec out;
	memset(&out, 0, sizeof(out));
	float block[2 * 64];
	uint64_t split = 3072; /* aligned to the 64-frame read chunks */
	while (out.count / 2 < split) {
		size_t want = 64;
		size_t got = plm_audio_mix_read(mix, block, want);
		TASK_CHECK(got > 0);
		vec_push(&out, block, got * 2);
	}

	/* gains change at an actual output position; pre-decoded cached PCM is
	 * rendered with the new gains from here on */
	float gains_b[4] = {2.0f, -0.25f, 1.0f, 1.0f};
	TASK_CHECK(plm_audio_mix_set_gains(mix, gains_b) == 1);
	drain_mixer(mix, &out, 77);

	uint64_t expected = 13824;
	TASK_CHECK(out.count == expected * 2);
	for (uint64_t n = 0; n < expected; n++) {
		const float *gains = (n < split) ? gains_a : gains_b;
		float expected_sample[2];
		ref_render_mix(refs, 0x3, 90000, gains, n, expected_sample);
		TASK_CHECK(out.data[n * 2] == expected_sample[0]);
		TASK_CHECK(out.data[n * 2 + 1] == expected_sample[1]);
	}
	TASK_CHECK(plm_audio_mix_has_ended(mix) == 1);

	plm_audio_mix_destroy(mix);
	free_refs(refs);
	vec_free(&out);
	free(pes);
	free(ps);
	return 0;
}

static int case_invalid_gains_rejected(void) {
	TASK_CHECK(load_fixtures());

	int64_t pts0[1] = {90000};
	int64_t pts1[1] = {90000};
	size_t pes_count;
	struct ps_pes *pes = make_schedule(pts0, 1, pts1, 1, &pes_count);
	struct mp2_clip *clips[2] = {clip_32k_mono, clip_48k_stereo};

	size_t ps_size;
	uint8_t *ps = build_ps(pes, pes_count, clips, 2, &ps_size);
	plm_buffer_t *buffer = plm_buffer_create_for_appending(4096);
	plm_buffer_write(buffer, ps, ps_size);
	plm_buffer_signal_end(buffer);

	plm_audio_mix_t *mix =
		plm_audio_mix_create(buffer, 1, 0x3, 90000);
	TASK_CHECK(mix != NULL);

	float good[4] = {0.25f, 0.75f, 1.0f, 1.0f};
	TASK_CHECK(plm_audio_mix_set_gains(mix, good) == 1);

	float bad_nan[4] = {1.0f, NAN, 1.0f, 1.0f};
	float bad_inf[4] = {1.0f, 1.0f, INFINITY, 1.0f};
	float bad_ninf[4] = {1.0f, 1.0f, 1.0f, -INFINITY};
	TASK_CHECK(plm_audio_mix_set_gains(mix, bad_nan) == 0);
	TASK_CHECK(plm_audio_mix_set_gains(mix, bad_inf) == 0);
	TASK_CHECK(plm_audio_mix_set_gains(mix, bad_ninf) == 0);
	TASK_CHECK(plm_audio_mix_set_gains(mix, NULL) == 0);

	struct ref_track refs[4];
	build_two_track_refs(pes, pes_count, refs);

	struct float_vec out;
	memset(&out, 0, sizeof(out));
	drain_mixer(mix, &out, 9);

	/* rejected updates left the good gains in place */
	uint64_t expected = (uint64_t)(3240 * 48000 / 90000);
	ASSERT_REF(&out, refs, 0x3, 90000, good, expected);

	plm_audio_mix_destroy(mix);
	free_refs(refs);
	vec_free(&out);
	free(pes);
	free(ps);
	return 0;
}

static int case_simultaneous_and_different_endings(void) {
	TASK_CHECK(load_fixtures());

	/* scenario A: both tracks end at the same tick */
	{
		int64_t pts0[3] = {90000, 93240, 96480};
		int64_t pts1[4] = {90000, 92160, 94320, 96480};
		size_t pes_count;
		struct ps_pes *pes =
			make_schedule(pts0, 3, pts1, 4, &pes_count);
		struct mp2_clip *clips[2] = {clip_32k_mono, clip_48k_stereo};
		size_t ps_size;
		uint8_t *ps = build_ps(pes, pes_count, clips, 2, &ps_size);
		plm_buffer_t *buffer = plm_buffer_create_for_appending(4096);
		plm_buffer_write(buffer, ps, ps_size);
		plm_buffer_signal_end(buffer);
		plm_audio_mix_t *mix =
			plm_audio_mix_create(buffer, 1, 0x3, 90000);
		TASK_CHECK(mix != NULL);
		struct float_vec out;
		memset(&out, 0, sizeof(out));
		drain_mixer(mix, &out, 33);
		uint64_t expected =
			(uint64_t)((96480 + 3240 - 90000) * 48000 / 90000);
		TASK_CHECK(out.count == expected * 2);
		TASK_CHECK(plm_audio_mix_get_position(mix) == expected);
		TASK_CHECK(plm_audio_mix_has_ended(mix) == 1);
		plm_audio_mix_destroy(mix);
		vec_free(&out);
		free(pes);
		free(ps);
	}

	/* scenario B: the 32k track ends later; no tail frames from either */
	{
		int64_t pts0[4] = {90000, 93240, 96480, 99720};
		int64_t pts1[2] = {90000, 92160};
		size_t pes_count;
		struct ps_pes *pes =
			make_schedule(pts0, 4, pts1, 2, &pes_count);
		struct mp2_clip *clips[2] = {clip_32k_mono, clip_48k_stereo};
		size_t ps_size;
		uint8_t *ps = build_ps(pes, pes_count, clips, 2, &ps_size);
		plm_buffer_t *buffer = plm_buffer_create_for_appending(4096);
		plm_buffer_write(buffer, ps, ps_size);
		plm_buffer_signal_end(buffer);
		plm_audio_mix_t *mix =
			plm_audio_mix_create(buffer, 1, 0x3, 90000);
		TASK_CHECK(mix != NULL);

		struct ref_track refs[4];
		build_two_track_refs(pes, pes_count, refs);
		struct float_vec out;
		memset(&out, 0, sizeof(out));
		drain_mixer(mix, &out, 17);
		int64_t latest_end = 99720 + 3240; /* 102960 */
		uint64_t expected =
			(uint64_t)((latest_end - 90000) * 48000 / 90000);
		float gains[4] = {1, 1, 1, 1};
		ASSERT_REF(&out, refs, 0x3, 90000,
			gains, expected);
		TASK_CHECK(plm_audio_mix_has_ended(mix) == 1);
		plm_audio_mix_destroy(mix);
		free_refs(refs);
		vec_free(&out);
		free(pes);
		free(ps);
	}
	return 0;
}

/* ========================================================================= */
/* streaming group                                                          */
/* ========================================================================= */

static int case_varied_chunk_sizes(void) {
	TASK_CHECK(load_fixtures());

	int64_t pts0[8], pts1[8];
	for (int i = 0; i < 8; i++) {
		pts0[i] = 90000 + 3240 * i;
		pts1[i] = 91080 + 2160 * i;
	}
	size_t pes_count;
	struct ps_pes *pes = make_schedule(pts0, 8, pts1, 8, &pes_count);
	struct mp2_clip *clips[2] = {clip_32k_mono, clip_48k_stereo};
	size_t ps_size;
	uint8_t *ps = build_ps(pes, pes_count, clips, 2, &ps_size);

	struct ref_track refs[4];
	build_two_track_refs(pes, pes_count, refs);

	size_t chunks[] = {1, 2, 3, 7, 64, 128};
	struct float_vec reference_out;
	memset(&reference_out, 0, sizeof(reference_out));

	for (size_t c = 0; c < sizeof(chunks) / sizeof(chunks[0]); c++) {
		plm_buffer_t *buffer = plm_buffer_create_for_appending(4096);
		plm_buffer_write(buffer, ps, ps_size);
		plm_buffer_signal_end(buffer);
		plm_audio_mix_t *mix =
			plm_audio_mix_create(buffer, 1, 0x3, 90000);
		TASK_CHECK(mix != NULL);

		struct float_vec out;
		memset(&out, 0, sizeof(out));
		drain_mixer(mix, &out, chunks[c]);
		TASK_CHECK(plm_audio_mix_has_ended(mix) == 1);

		if (c == 0) {
			vec_push(&reference_out, out.data, out.count);
		}
		else {
			TASK_CHECK(out.count == reference_out.count);
			TASK_CHECK(memcmp(out.data, reference_out.data,
				out.count * sizeof(float)) == 0);
		}
		plm_audio_mix_destroy(mix);
		vec_free(&out);
	}

	float gains[4] = {1, 1, 1, 1};
	ASSERT_REF(&reference_out, refs, 0x3, 90000,
		gains, 13824);

	vec_free(&reference_out);
	free_refs(refs);
	free(pes);
	free(ps);
	return 0;
}

static int case_byte_by_byte_append(void) {
	TASK_CHECK(load_fixtures());

	int64_t pts0[6], pts1[6];
	for (int i = 0; i < 6; i++) {
		pts0[i] = 90000 + 3240 * i;
		pts1[i] = 91080 + 2160 * i;
	}
	size_t pes_count;
	struct ps_pes *pes = make_schedule(pts0, 6, pts1, 6, &pes_count);
	struct mp2_clip *clips[2] = {clip_32k_mono, clip_48k_stereo};
	size_t ps_size;
	uint8_t *ps = build_ps(pes, pes_count, clips, 2, &ps_size);

	struct ref_track refs[4];
	build_two_track_refs(pes, pes_count, refs);

	plm_buffer_t *buffer = plm_buffer_create_with_capacity(4096);
	plm_audio_mix_t *mix =
		plm_audio_mix_create(buffer, 0, 0x3, 90000);
	TASK_CHECK(mix != NULL);

	struct float_vec out;
	memset(&out, 0, sizeof(out));
	int saw_zero_with_unchanged_position = 0;
	uint64_t last_position = 0;

	for (size_t i = 0; i < ps_size; i++) {
		plm_buffer_write(buffer, ps + i, 1);
		float block[2 * 16];
		size_t got = plm_audio_mix_read(mix, block, 16);
		uint64_t pos = plm_audio_mix_get_position(mix);
		TASK_CHECK(pos >= last_position);
		if (got == 0) {
			TASK_CHECK(pos == last_position);
			saw_zero_with_unchanged_position = 1;
		}
		last_position = pos;
		if (got) {
			vec_push(&out, block, got * 2);
		}
		TASK_CHECK(plm_audio_mix_has_ended(mix) == 0);
	}
	TASK_CHECK(saw_zero_with_unchanged_position);

	plm_buffer_signal_end(buffer);
	drain_mixer(mix, &out, 29);
	TASK_CHECK(plm_audio_mix_has_ended(mix) == 1);

	int64_t latest_end = 90000 + 6 * 3240;
	uint64_t expected =
		(uint64_t)((latest_end - 90000) * 48000 / 90000);
	float gains[4] = {1, 1, 1, 1};
	ASSERT_REF(&out, refs, 0x3, 90000, gains, expected);

	plm_audio_mix_destroy(mix);
	plm_buffer_destroy(buffer); /* mixer was created without ownership */
	free_refs(refs);
	vec_free(&out);
	free(pes);
	free(ps);
	return 0;
}

static int case_starvation_vs_real_eof(void) {
	TASK_CHECK(load_fixtures());

	int64_t pts0[4], pts1[4];
	for (int i = 0; i < 4; i++) {
		pts0[i] = 90000 + 3240 * i;
		pts1[i] = 91080 + 2160 * i;
	}
	size_t pes_count;
	struct ps_pes *pes = make_schedule(pts0, 4, pts1, 4, &pes_count);
	struct mp2_clip *clips[2] = {clip_32k_mono, clip_48k_stereo};
	size_t ps_size;
	uint8_t *ps = build_ps(pes, pes_count, clips, 2, &ps_size);

	plm_buffer_t *buffer = plm_buffer_create_with_capacity(4096);
	plm_audio_mix_t *mix =
		plm_audio_mix_create(buffer, 0, 0x3, 90000);
	TASK_CHECK(mix != NULL);

	struct float_vec out;
	memset(&out, 0, sizeof(out));

	/* feed only a prefix, cut mid-PES: starvation must not look like EOF */
	size_t cut = ps_size / 2;
	plm_buffer_write(buffer, ps, cut);
	float block[2 * 32];
	size_t got = plm_audio_mix_read(mix, block, 32);
	TASK_CHECK(got <= 32);
	TASK_CHECK(plm_audio_mix_has_ended(mix) == 0);
	uint64_t position_after_prefix = plm_audio_mix_get_position(mix);
	vec_push(&out, block, got * 2);

	/* deliver the rest; the already consumed prefix is not re-decoded and
	 * no sample is dropped */
	plm_buffer_write(buffer, ps + cut, ps_size - cut);
	got = plm_audio_mix_read(mix, block, 32);
	TASK_CHECK(plm_audio_mix_get_position(mix) >= position_after_prefix);
	TASK_CHECK(plm_audio_mix_has_ended(mix) == 0);
	vec_push(&out, block, got * 2);

	plm_buffer_signal_end(buffer);
	drain_mixer(mix, &out, 41);
	TASK_CHECK(plm_audio_mix_has_ended(mix) == 1);

	struct ref_track refs[4];
	build_two_track_refs(pes, pes_count, refs);
	uint64_t expected =
		(uint64_t)((90000 + 4 * 3240 - 90000) * 48000 / 90000);
	float gains[4] = {1, 1, 1, 1};
	ASSERT_REF(&out, refs, 0x3, 90000, gains, expected);

	plm_audio_mix_destroy(mix);
	plm_buffer_destroy(buffer); /* mixer was created without ownership */
	free_refs(refs);
	vec_free(&out);
	free(pes);
	free(ps);
	return 0;
}

static int case_rewind_equals_fresh_instance(void) {
	TASK_CHECK(load_fixtures());

	int64_t pts0[8], pts1[8];
	for (int i = 0; i < 8; i++) {
		pts0[i] = 90000 + 3240 * i;
		pts1[i] = 91080 + 2160 * i;
	}
	size_t pes_count;
	struct ps_pes *pes = make_schedule(pts0, 8, pts1, 8, &pes_count);
	struct mp2_clip *clips[2] = {clip_32k_mono, clip_48k_stereo};
	size_t ps_size;
	uint8_t *ps = build_ps(pes, pes_count, clips, 2, &ps_size);

	plm_buffer_t *buffer = plm_buffer_create_for_appending(4096);
	plm_buffer_write(buffer, ps, ps_size);
	plm_buffer_signal_end(buffer);

	plm_audio_mix_t *mix =
		plm_audio_mix_create(buffer, 0, 0x3, 90000);
	TASK_CHECK(mix != NULL);
	float custom_gains[4] = {3.0f, 0.0f, 1.0f, 1.0f};
	TASK_CHECK(plm_audio_mix_set_gains(mix, custom_gains) == 1);

	struct float_vec first;
	memset(&first, 0, sizeof(first));
	drain_mixer(mix, &first, 53);
	TASK_CHECK(plm_audio_mix_has_ended(mix) == 1);
	TASK_CHECK(plm_audio_mix_get_position(mix) == 13824);

	/* rewind clears every decode/interpolation state and resets gains */
	plm_audio_mix_rewind(mix);
	TASK_CHECK(plm_audio_mix_get_position(mix) == 0);
	TASK_CHECK(plm_audio_mix_has_ended(mix) == 0);

	struct float_vec replayed;
	memset(&replayed, 0, sizeof(replayed));
	drain_mixer(mix, &replayed, 91);
	TASK_CHECK(plm_audio_mix_has_ended(mix) == 1);
	TASK_CHECK(replayed.count == first.count);
	TASK_CHECK(memcmp(replayed.data, first.data,
		first.count * sizeof(float)) != 0); /* custom gains differed */

	/* a fresh instance with default gains must match the replayed output */
	plm_buffer_t *buffer2 = plm_buffer_create_for_appending(4096);
	plm_buffer_write(buffer2, ps, ps_size);
	plm_buffer_signal_end(buffer2);
	plm_audio_mix_t *fresh =
		plm_audio_mix_create(buffer2, 1, 0x3, 90000);
	TASK_CHECK(fresh != NULL);
	struct float_vec fresh_out;
	memset(&fresh_out, 0, sizeof(fresh_out));
	drain_mixer(fresh, &fresh_out, 128);
	TASK_CHECK(fresh_out.count == replayed.count);
	TASK_CHECK(memcmp(fresh_out.data, replayed.data,
		replayed.count * sizeof(float)) == 0);

	plm_audio_mix_destroy(fresh);
	plm_audio_mix_destroy(mix);
	plm_buffer_destroy(buffer); /* mix was created without ownership */
	vec_free(&first);
	vec_free(&replayed);
	vec_free(&fresh_out);
	free(pes);
	free(ps);
	return 0;
}

static int case_bounded_allocation(void) {
	TASK_CHECK(load_fixtures());

	size_t peaks[2] = {0, 0};
	unsigned long read_allocs[2] = {0, 0};

	for (int scenario = 0; scenario < 2; scenario++) {
		int frames_per_track = (scenario == 0) ? 8 : 16;
		int64_t *pts0 = (int64_t *)malloc(sizeof(int64_t) *
			(size_t)frames_per_track);
		int64_t *pts1 = (int64_t *)malloc(sizeof(int64_t) *
			(size_t)frames_per_track);
		for (int i = 0; i < frames_per_track; i++) {
			pts0[i] = 90000 + 3240 * i;
			pts1[i] = 91080 + 2160 * i;
		}
		size_t pes_count;
		struct ps_pes *pes = make_schedule(pts0,
			frames_per_track, pts1, frames_per_track, &pes_count);
		struct mp2_clip *clips[2] = {clip_32k_mono, clip_48k_stereo};
		size_t ps_size;
		uint8_t *ps = build_ps(pes, pes_count, clips, 2, &ps_size);

		/* fixed-capacity source so the caller's compressed byte store is
		 * identical in both scenarios and can be excluded from the
		 * mixer-only measurement */
		plm_buffer_t *buffer = plm_buffer_create_for_appending(32768);
		plm_buffer_write(buffer, ps, ps_size);
		plm_buffer_signal_end(buffer);
		size_t baseline_live = test_live_bytes;

		unsigned long allocs_at_create = test_alloc_calls;
		plm_audio_mix_t *mix =
			plm_audio_mix_create(buffer, 0, 0x3, 90000);
		TASK_CHECK(mix != NULL);
		TASK_CHECK(test_live_bytes > baseline_live);

		size_t mixer_peak = 0;
		float block[2 * 64];
		for (;;) {
			unsigned long before = test_alloc_calls;
			size_t got = plm_audio_mix_read(mix, block, 64);
			TASK_CHECK(test_alloc_calls == before); /* zero allocations */
			size_t mixer_live = test_live_bytes - baseline_live;
			if (mixer_live > mixer_peak) {
				mixer_peak = mixer_live;
			}
			if (got == 0) {
				break;
			}
		}
		TASK_CHECK(plm_audio_mix_has_ended(mix) == 1);
		peaks[scenario] = mixer_peak;
		read_allocs[scenario] = test_alloc_calls - allocs_at_create;

		/* all mixer allocations released; the caller buffer remains */
		plm_audio_mix_destroy(mix);
		TASK_CHECK(test_live_bytes == baseline_live);
		plm_buffer_destroy(buffer);
		TASK_CHECK(test_live_bytes == 0);

		free(pes);
		free(ps);
		free(pts0);
		free(pts1);
	}

	/* doubling the program length must not grow the mixer's working set,
	 * and neither scenario may allocate while reading */
	TASK_CHECK(peaks[0] == peaks[1]);
	TASK_CHECK(read_allocs[0] > 0 && read_allocs[1] > 0);
	TASK_CHECK(read_allocs[0] == read_allocs[1]);
	return 0;
}

static int case_tiny_buffer_reallocs_and_no_clipping(void) {
	TASK_CHECK(load_fixtures());

	int64_t pts0[8], pts1[8];
	for (int i = 0; i < 8; i++) {
		pts0[i] = 90000 + 3240 * i;
		pts1[i] = 91080 + 2160 * i;
	}
	size_t pes_count;
	struct ps_pes *pes = make_schedule(pts0, 8, pts1, 8, &pes_count);
	struct mp2_clip *clips[2] = {clip_32k_mono, clip_48k_stereo};
	size_t ps_size;
	uint8_t *ps = build_ps(pes, pes_count, clips, 2, &ps_size);

	struct ref_track refs[4];
	build_two_track_refs(pes, pes_count, refs);

	/* a deliberately tiny appending buffer, fed two bytes at a time, keeps
	 * reallocating its storage while demuxed packets may be held under
	 * back-pressure; the mixer must never reference stale bytes */
	plm_buffer_t *buffer = plm_buffer_create_for_appending(64);
	plm_audio_mix_t *mix =
		plm_audio_mix_create(buffer, 0, 0x3, 90000);
	TASK_CHECK(mix != NULL);
	float big_gains[4] = {4.0f, 4.0f, 1.0f, 1.0f};
	TASK_CHECK(plm_audio_mix_set_gains(mix, big_gains) == 1);

	struct float_vec out;
	memset(&out, 0, sizeof(out));
	for (size_t i = 0; i < ps_size; i += 2) {
		size_t chunk = (ps_size - i < 2) ? 1 : 2;
		plm_buffer_write(buffer, ps + i, chunk);
		float block[2 * 8];
		size_t got = plm_audio_mix_read(mix, block, 8);
		if (got) {
			vec_push(&out, block, got * 2);
		}
		TASK_CHECK(plm_audio_mix_has_ended(mix) == 0);
	}
	plm_buffer_signal_end(buffer);
	drain_mixer(mix, &out, 3);
	TASK_CHECK(plm_audio_mix_has_ended(mix) == 1);

	uint64_t expected = 13824;
	ASSERT_REF(&out, refs, 0x3, 90000, big_gains, expected);

	/* every delivered sample is finite and nothing is hard-clipped: with a
	 * gain of 4 the mix must contain magnitudes strictly above 1.0 */
	int saw_above_unity = 0;
	for (size_t i = 0; i < out.count; i++) {
		TASK_CHECK(isfinite(out.data[i]));
		if (fabsf(out.data[i]) > 1.0f) {
			saw_above_unity = 1;
		}
	}
	TASK_CHECK(saw_above_unity);

	plm_audio_mix_destroy(mix);
	plm_buffer_destroy(buffer);
	free_refs(refs);
	vec_free(&out);
	free(pes);
	free(ps);
	return 0;
}

static int case_invalid_creation_and_zero_capacity(void) {
	TASK_CHECK(load_fixtures());

	plm_buffer_t *buffer = plm_buffer_create_for_appending(256);
	uint8_t byte = 0x00;
	plm_buffer_write(buffer, &byte, 1);
	size_t remaining_before = plm_buffer_get_remaining(buffer);

	TASK_CHECK(plm_audio_mix_create(NULL, 1, 0x1, 0) == NULL);
	TASK_CHECK(plm_audio_mix_create(buffer, 1, 0x0, 0) == NULL);
	TASK_CHECK(plm_audio_mix_create(buffer, 1, 0x10, 0) == NULL);
	TASK_CHECK(plm_audio_mix_create(buffer, 1, 0xFFFFFFFFu, 0) == NULL);
	TASK_CHECK(plm_audio_mix_create(buffer, 1, 0x7, -1) == NULL);

	/* rejected creation must not take over (or destroy) the buffer */
	TASK_CHECK(plm_buffer_get_remaining(buffer) == remaining_before);
	plm_buffer_destroy(buffer);

	/* a valid mixer accepts input that has no complete header yet */
	plm_buffer_t *empty = plm_buffer_create_with_capacity(256);
	plm_audio_mix_t *mix =
		plm_audio_mix_create(empty, 1, 0x1, 0);
	TASK_CHECK(mix != NULL);
	TASK_CHECK(plm_audio_mix_has_ended(mix) == 0);
	float block[4];
	TASK_CHECK(plm_audio_mix_read(mix, block, 2) == 0);
	TASK_CHECK(plm_audio_mix_get_position(mix) == 0);

	/* zero capacity is a no-op that leaves the position untouched */
	TASK_CHECK(plm_audio_mix_read(mix, NULL, 0) == 0);
	TASK_CHECK(plm_audio_mix_get_position(mix) == 0);

	plm_audio_mix_destroy(mix);
	return 0;
}

/* --- registration -------------------------------------------------------- */

const struct task_case task_cases[] = {
	{"timeline", "dual_full_timeline", case_dual_full_timeline},
	{"timeline", "origin_lead_silence", case_origin_lead_silence},
	{"timeline", "gaps_and_contiguity", case_gaps_and_contiguity},
	{"timeline", "mask_selects_subset", case_mask_selects_subset},
	{"timeline", "four_tracks_noncontiguous_mask",
		case_four_tracks_noncontiguous_mask},

	{"mixing", "default_gains_mono_duplicate",
		case_default_gains_mono_duplicate},
	{"mixing", "asymmetric_gains_and_update",
		case_asymmetric_gains_and_update},
	{"mixing", "invalid_gains_rejected", case_invalid_gains_rejected},
	{"mixing", "simultaneous_and_different_endings",
		case_simultaneous_and_different_endings},

	{"streaming", "varied_chunk_sizes", case_varied_chunk_sizes},
	{"streaming", "byte_by_byte_append", case_byte_by_byte_append},
	{"streaming", "starvation_vs_real_eof", case_starvation_vs_real_eof},
	{"streaming", "rewind_equals_fresh_instance",
		case_rewind_equals_fresh_instance},
	{"streaming", "bounded_allocation", case_bounded_allocation},
	{"streaming", "tiny_buffer_reallocs_and_no_clipping",
		case_tiny_buffer_reallocs_and_no_clipping},
	{"streaming", "invalid_creation_and_zero_capacity",
		case_invalid_creation_and_zero_capacity},
};

const size_t task_case_count =
	sizeof(task_cases) / sizeof(task_cases[0]);

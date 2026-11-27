/*
 * Regression tests for decoding several MPEG-1 video sequences concatenated in
 * a single element stream (or MPEG-PS container). Every sequence carries its
 * own sequence header and sequence_end code; resolution, pixel aspect ratio,
 * frame rate and quantizer matrices may change between sequences.
 *
 * The material is generated entirely in C through task_support/fixture.h.
 */
#include "check.h"
#include "fixture.h"
#include "pl_mpeg.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MAX_RECORDS 16
#define TIME_EPS 1e-9

typedef struct {
	int width;
	int height;
	int luma_width;
	int luma_height;
	int y;
	int cb;
	int cr;
	double time;
} record_t;

static void ended_source(plm_buffer_t *buffer, void *user) {
	(void)user;
	plm_buffer_signal_end(buffer);
}

static int time_near(double a, double b) {
	return fabs(a - b) < TIME_EPS;
}

/* Pixel aspect ratios are stored as 32-bit floats in the lookup table. */
static int par_near(double a, double b) {
	return fabs(a - b) < 1e-6;
}

/* Decode a fully supplied element stream through plm_video_decode(). */
static int collect_raw(const fixture_t *es, record_t *records, int max_records) {
	plm_buffer_t *buffer = plm_buffer_create_with_capacity(64);
	plm_buffer_set_load_callback(buffer, ended_source, NULL);
	plm_buffer_write(buffer, (uint8_t *)es->bytes, fixture_size(es));
	plm_buffer_signal_end(buffer);

	plm_video_t *video = plm_video_create_with_buffer(buffer, 1);
	int count = 0;
	plm_frame_t *frame;
	while ((frame = plm_video_decode(video))) {
		if (count >= max_records) {
			break;
		}
		records[count].width = (int)frame->width;
		records[count].height = (int)frame->height;
		records[count].luma_width = (int)frame->y.width;
		records[count].luma_height = (int)frame->y.height;
		records[count].y = frame->y.data[0];
		records[count].cb = frame->cb.data[0];
		records[count].cr = frame->cr.data[0];
		records[count].time = frame->time;
		count++;
	}
	plm_video_destroy(video);
	return count;
}

/* Wrap an element stream in an MPEG-PS and decode it via plm_decode_video(). */
static int collect_ps(const fixture_t *es, record_t *records, int max_records) {
	fixture_t ps = {0};
	fixture_ps(&ps, es);
	size_t length = fixture_size(&ps);
	uint8_t *copy = (uint8_t *)malloc(length);
	memcpy(copy, ps.bytes, length);

	plm_t *player = plm_create_with_memory(copy, length, 1);
	plm_set_audio_enabled(player, 0);

	int count = 0;
	plm_frame_t *frame;
	while ((frame = plm_decode_video(player))) {
		if (count >= max_records) {
			break;
		}
		records[count].width = (int)frame->width;
		records[count].height = (int)frame->height;
		records[count].luma_width = (int)frame->y.width;
		records[count].luma_height = (int)frame->y.height;
		records[count].y = frame->y.data[0];
		records[count].cb = frame->cb.data[0];
		records[count].cr = frame->cr.data[0];
		records[count].time = frame->time;
		count++;
	}
	plm_destroy(player);
	return count;
}

typedef struct {
	record_t records[MAX_RECORDS];
	int count;
} callback_capture_t;

static void on_video(plm_t *player, plm_frame_t *frame, void *user) {
	(void)player;
	callback_capture_t *capture = (callback_capture_t *)user;
	if (capture->count >= MAX_RECORDS) {
		return;
	}
	record_t *record = &capture->records[capture->count++];
	record->width = (int)frame->width;
	record->height = (int)frame->height;
	record->luma_width = (int)frame->y.width;
	record->luma_height = (int)frame->y.height;
	record->y = frame->y.data[0];
	record->cb = frame->cb.data[0];
	record->cr = frame->cr.data[0];
	record->time = frame->time;
}

/* Decode a PS through the video callback driven by plm_decode(). */
static int collect_callback(const fixture_t *es, callback_capture_t *capture) {
	fixture_t ps = {0};
	fixture_ps(&ps, es);
	size_t length = fixture_size(&ps);
	uint8_t *copy = (uint8_t *)malloc(length);
	memcpy(copy, ps.bytes, length);

	plm_t *player = plm_create_with_memory(copy, length, 1);
	plm_set_audio_enabled(player, 0);
	plm_set_video_decode_callback(player, on_video, capture);

	capture->count = 0;
	for (int i = 0; i < 64 && !plm_has_ended(player); i++) {
		plm_decode(player, 1.0);
	}
	int count = capture->count;
	plm_destroy(player);
	return count;
}

static int records_match(const record_t *a, const record_t *b, int count) {
	for (int i = 0; i < count; i++) {
		if (
			a[i].width != b[i].width ||
			a[i].height != b[i].height ||
			a[i].luma_width != b[i].luma_width ||
			a[i].luma_height != b[i].luma_height ||
			a[i].y != b[i].y ||
			a[i].cb != b[i].cb ||
			a[i].cr != b[i].cr ||
			!time_near(a[i].time, b[i].time)
		) {
			return 0;
		}
	}
	return 1;
}

/* ------------------------------------------------------------------ planes */

static int test_planes_size_switch(void) {
	fixture_t es = {0};
	fixture_sequence(&es, 16, 16, 3, 1);
	fixture_intra(&es, 16, 16, 0, 128);
	fixture_intra(&es, 16, 16, 1, 128);
	fixture_end(&es);
	fixture_sequence(&es, 32, 16, 5, 2);
	fixture_intra(&es, 32, 16, 0, 200);
	fixture_intra(&es, 32, 16, 1, 200);
	fixture_end(&es);

	record_t records[MAX_RECORDS];
	int count = collect_raw(&es, records, MAX_RECORDS);
	TASK_CHECK(count == 4);

	/* Old sequence tail frames keep the old dimensions and planes. */
	TASK_CHECK(records[0].width == 16 && records[0].height == 16);
	TASK_CHECK(records[0].luma_width == 16 && records[0].luma_height == 16);
	TASK_CHECK(records[0].y == 128 && records[0].cb == 128 && records[0].cr == 128);
	TASK_CHECK(records[1].width == 16 && records[1].y == 128);

	/* The new sequence's frames use the new, larger planes. */
	TASK_CHECK(records[2].width == 32 && records[2].height == 16);
	TASK_CHECK(records[2].luma_width == 32 && records[2].luma_height == 16);
	TASK_CHECK(records[2].y == 200);
	TASK_CHECK(records[3].width == 32 && records[3].luma_width == 32 && records[3].y == 200);
	return 0;
}

static int test_planes_b_reorder(void) {
	fixture_t es = {0};
	/* Bitstream order is I, P, B; display order must come out I, B, P. */
	fixture_sequence(&es, 16, 16, 3, 1);
	fixture_intra(&es, 16, 16, 0, 128);
	fixture_predictive(&es, 16, 16, 2, 31);   /* P: 128 + 63 = 191 */
	fixture_bidirect(&es, 16, 16, 1);         /* B: (128 + 191 + 1) / 2 = 160 */
	fixture_end(&es);
	/* Second sequence starts with an I picture. */
	fixture_sequence(&es, 32, 16, 5, 2);
	fixture_intra(&es, 32, 16, 0, 200);
	fixture_intra(&es, 32, 16, 1, 200);
	fixture_end(&es);

	record_t records[MAX_RECORDS];
	int count = collect_raw(&es, records, MAX_RECORDS);
	TASK_CHECK(count == 5);

	TASK_CHECK(records[0].width == 16 && records[0].y == 128);
	TASK_CHECK(records[1].width == 16 && records[1].y == 160); /* B delivered second */
	TASK_CHECK(records[2].width == 16 && records[2].y == 191); /* P delivered third */
	TASK_CHECK(records[3].width == 32 && records[3].y == 200);
	TASK_CHECK(records[4].width == 32 && records[4].y == 200);
	return 0;
}

static int test_planes_reference_reset(void) {
	fixture_t es = {0};
	fixture_sequence(&es, 16, 16, 3, 1);
	fixture_intra(&es, 16, 16, 0, 128);
	fixture_end(&es);
	fixture_sequence(&es, 32, 16, 5, 2);
	fixture_intra(&es, 32, 16, 0, 70);
	/* A zero-residual P picture copies its forward reference. It must use the
	 * new sequence's I picture (70), never the previous sequence's frame. */
	fixture_predictive(&es, 32, 16, 1, 0);
	fixture_end(&es);

	record_t records[MAX_RECORDS];
	int count = collect_raw(&es, records, MAX_RECORDS);
	TASK_CHECK(count == 3);
	TASK_CHECK(records[0].width == 16 && records[0].y == 128);
	TASK_CHECK(records[1].width == 32 && records[1].y == 70);
	TASK_CHECK(records[2].width == 32 && records[2].y == 70);
	return 0;
}

static int test_planes_loaned_frame_internally_consistent(void) {
	fixture_t es = {0};
	fixture_sequence(&es, 16, 16, 3, 1);
	fixture_intra(&es, 16, 16, 0, 110);
	fixture_intra(&es, 16, 16, 1, 110);
	fixture_end(&es);
	fixture_sequence(&es, 32, 32, 5, 1);
	fixture_intra(&es, 32, 32, 0, 220);
	fixture_end(&es);

	plm_buffer_t *buffer = plm_buffer_create_with_capacity(64);
	plm_buffer_set_load_callback(buffer, ended_source, NULL);
	plm_buffer_write(buffer, es.bytes, fixture_size(&es));
	plm_buffer_signal_end(buffer);
	plm_video_t *video = plm_video_create_with_buffer(buffer, 1);

	/* Snapshot every delivered frame immediately, as a consumer that copies
	 * the borrowed planes before the next decode call would. */
	record_t seen[MAX_RECORDS];
	int count = 0;
	plm_frame_t *frame;
	while ((frame = plm_video_decode(video))) {
		TASK_CHECK(count < MAX_RECORDS);
		seen[count].width = (int)frame->width;
		seen[count].height = (int)frame->height;
		seen[count].luma_width = (int)frame->y.width;
		seen[count].y = frame->y.data[0];
		seen[count].time = frame->time;
		count++;
	}
	TASK_CHECK(count == 3);
	/* Every boundary delivery is self-consistent: old frames carry old planes,
	 * the new frame carries the reallocated, larger planes. */
	TASK_CHECK(seen[0].width == 16 && seen[0].luma_width == 16 && seen[0].y == 110);
	TASK_CHECK(seen[1].width == 16 && seen[1].luma_width == 16 && seen[1].y == 110);
	TASK_CHECK(seen[2].width == 32 && seen[2].luma_width == 32 && seen[2].y == 220);
	plm_video_destroy(video);
	return 0;
}

/* ------------------------------------------------------------------- clock */

static int test_clock_continuous_across_rate_change(void) {
	fixture_t es = {0};
	fixture_sequence(&es, 16, 16, 3, 1); /* 25 fps */
	fixture_intra(&es, 16, 16, 0, 128);
	fixture_intra(&es, 16, 16, 1, 128);
	fixture_end(&es);
	fixture_sequence(&es, 32, 16, 5, 2); /* 30 fps */
	fixture_intra(&es, 32, 16, 0, 200);
	fixture_intra(&es, 32, 16, 1, 200);
	fixture_end(&es);

	record_t records[MAX_RECORDS];
	int count = collect_raw(&es, records, MAX_RECORDS);
	TASK_CHECK(count == 4);

	double p25 = 1.0 / 25.0;
	double p30 = 1.0 / 30.0;
	TASK_CHECK(time_near(records[0].time, 0.0));
	TASK_CHECK(time_near(records[1].time, p25));
	/* Time keeps accumulating with the new frame period; it never resets. */
	TASK_CHECK(time_near(records[2].time, 2.0 * p25));
	TASK_CHECK(time_near(records[3].time, 2.0 * p25 + p30));
	return 0;
}

static int test_clock_get_time_is_next_display_time(void) {
	fixture_t es = {0};
	fixture_sequence(&es, 16, 16, 3, 1);
	fixture_intra(&es, 16, 16, 0, 128);
	fixture_intra(&es, 16, 16, 1, 128);
	fixture_end(&es);

	plm_buffer_t *buffer = plm_buffer_create_with_capacity(64);
	plm_buffer_set_load_callback(buffer, ended_source, NULL);
	plm_buffer_write(buffer, es.bytes, fixture_size(&es));
	plm_buffer_signal_end(buffer);
	plm_video_t *video = plm_video_create_with_buffer(buffer, 1);

	TASK_CHECK(time_near(plm_video_get_time(video), 0.0));
	plm_frame_t *first = plm_video_decode(video);
	TASK_CHECK(first != NULL && time_near(first->time, 0.0));
	/* After delivering the first frame, get_time() points at the next one. */
	TASK_CHECK(time_near(plm_video_get_time(video), 1.0 / 25.0));
	plm_frame_t *second = plm_video_decode(video);
	TASK_CHECK(second != NULL && time_near(second->time, 1.0 / 25.0));
	TASK_CHECK(time_near(plm_video_get_time(video), 2.0 / 25.0));
	plm_video_destroy(video);
	return 0;
}

static int test_clock_getters_follow_active_sequence(void) {
	fixture_t es = {0};
	fixture_sequence(&es, 16, 16, 3, 1); /* aspect code 1 -> 1.0 */
	fixture_intra(&es, 16, 16, 0, 128);
	fixture_end(&es);
	fixture_sequence(&es, 32, 16, 5, 2); /* aspect code 2 -> 0.6735 */
	fixture_intra(&es, 32, 16, 0, 200);
	fixture_end(&es);

	plm_buffer_t *buffer = plm_buffer_create_with_capacity(64);
	plm_buffer_set_load_callback(buffer, ended_source, NULL);
	plm_buffer_write(buffer, es.bytes, fixture_size(&es));
	plm_buffer_signal_end(buffer);
	plm_video_t *video = plm_video_create_with_buffer(buffer, 1);

	plm_frame_t *old = plm_video_decode(video);
	TASK_CHECK(old != NULL && old->width == 16);
	/* While delivering the old sequence's tail, queries report the old config. */
	TASK_CHECK(plm_video_get_width(video) == 16);
	TASK_CHECK(plm_video_get_height(video) == 16);
	TASK_CHECK(time_near(plm_video_get_framerate(video), 25.0));
	TASK_CHECK(par_near(plm_video_get_pixel_aspect_ratio(video), 1.0));

	plm_frame_t *fresh = plm_video_decode(video);
	TASK_CHECK(fresh != NULL && fresh->width == 32);
	/* Once the new sequence's first frame is delivered, queries follow it. */
	TASK_CHECK(plm_video_get_width(video) == 32);
	TASK_CHECK(plm_video_get_height(video) == 16);
	TASK_CHECK(time_near(plm_video_get_framerate(video), 30.0));
	TASK_CHECK(par_near(plm_video_get_pixel_aspect_ratio(video), 0.6735));
	plm_video_destroy(video);
	return 0;
}

static int test_clock_rewind_restarts_at_zero(void) {
	fixture_t es = {0};
	fixture_sequence(&es, 16, 16, 3, 1);
	fixture_intra(&es, 16, 16, 0, 128);
	fixture_intra(&es, 16, 16, 1, 128);
	fixture_end(&es);
	fixture_sequence(&es, 32, 16, 5, 2);
	fixture_intra(&es, 32, 16, 0, 200);
	fixture_intra(&es, 32, 16, 1, 200);
	fixture_end(&es);

	fixture_t ps = {0};
	fixture_ps(&ps, &es);
	size_t length = fixture_size(&ps);
	uint8_t *copy = (uint8_t *)malloc(length);
	memcpy(copy, ps.bytes, length);
	plm_t *player = plm_create_with_memory(copy, length, 1);
	plm_set_audio_enabled(player, 0);

	int first_pass = 0;
	while (plm_decode_video(player)) {
		first_pass++;
	}
	TASK_CHECK(first_pass == 4);

	plm_rewind(player);
	TASK_CHECK(time_near(plm_get_time(player), 0.0));
	plm_frame_t *first = plm_decode_video(player);
	TASK_CHECK(first != NULL);
	int first_width = (int)first->width;
	double first_time = first->time;
	int first_y = first->y.data[0];
	TASK_CHECK(first_width == 16 && first_y == 128 && time_near(first_time, 0.0));

	int second_pass = 1;
	while (plm_decode_video(player)) {
		second_pass++;
	}
	TASK_CHECK(second_pass == 4);
	plm_destroy(player);
	return 0;
}

/* ------------------------------------------------------------------ stream */

static int test_stream_partial_first_header(void) {
	fixture_t es = {0};
	fixture_sequence(&es, 16, 16, 3, 1);
	fixture_intra(&es, 16, 16, 0, 128);
	fixture_end(&es);
	fixture_sequence(&es, 32, 16, 5, 2);
	fixture_intra(&es, 32, 16, 0, 200);
	fixture_end(&es);
	size_t length = fixture_size(&es);

	plm_buffer_t *buffer = plm_buffer_create_with_capacity(64);
	plm_buffer_set_load_callback(buffer, ended_source, NULL);
	plm_video_t *video = plm_video_create_with_buffer(buffer, 1);

	/* A cut inside the first sequence header: no frame, no active config. */
	size_t cut = 8;
	plm_buffer_write(buffer, es.bytes, cut);
	TASK_CHECK(plm_video_decode(video) == NULL);
	TASK_CHECK(plm_video_get_width(video) == 0);

	/* Completing the feed must decode the whole stream with no mixed config. */
	plm_buffer_write(buffer, es.bytes + cut, length - cut);
	plm_buffer_signal_end(buffer);
	record_t records[MAX_RECORDS];
	int count = 0;
	plm_frame_t *frame;
	while ((frame = plm_video_decode(video))) {
		TASK_CHECK(count < MAX_RECORDS);
		records[count].width = (int)frame->width;
		records[count].y = frame->y.data[0];
		records[count].time = frame->time;
		count++;
	}
	TASK_CHECK(count == 2);
	TASK_CHECK(records[0].width == 16 && records[0].y == 128);
	TASK_CHECK(records[1].width == 32 && records[1].y == 200);
	plm_video_destroy(video);
	return 0;
}

static int test_stream_half_new_header_keeps_old_config(void) {
	fixture_t es = {0};
	fixture_sequence(&es, 16, 16, 3, 1);
	fixture_intra(&es, 16, 16, 0, 128);
	fixture_end(&es);
	/* The second header carries a custom non-intra matrix, making it longer. */
	fixture_sequence_quant(&es, 32, 16, 5, 2, 0, 1);
	fixture_intra(&es, 32, 16, 0, 200);
	fixture_end(&es);
	size_t length = fixture_size(&es);

	/* Find the second sequence start code. */
	size_t second_header = 0;
	int seen = 0;
	for (size_t i = 0; i + 3 < length; i++) {
		if (
			es.bytes[i] == 0x00 && es.bytes[i + 1] == 0x00 &&
			es.bytes[i + 2] == 0x01 && es.bytes[i + 3] == 0xb3
		) {
			seen++;
			if (seen == 2) {
				second_header = i;
				break;
			}
		}
	}
	TASK_CHECK(seen == 2);
	size_t cut = second_header + 20; /* inside the quantizer matrix bytes */

	plm_buffer_t *buffer = plm_buffer_create_with_capacity(64);
	plm_buffer_set_load_callback(buffer, ended_source, NULL);
	plm_video_t *video = plm_video_create_with_buffer(buffer, 1);
	plm_buffer_write(buffer, es.bytes, cut);

	int old_count = 0;
	while (plm_video_decode(video)) {
		old_count++;
	}
	TASK_CHECK(old_count == 1);
	/* A half-consumed new header must not change the active configuration. */
	TASK_CHECK(plm_video_get_width(video) == 16);
	TASK_CHECK(time_near(plm_video_get_framerate(video), 25.0));

	plm_buffer_write(buffer, es.bytes + cut, length - cut);
	plm_buffer_signal_end(buffer);
	plm_frame_t *fresh = plm_video_decode(video);
	TASK_CHECK(fresh != NULL && fresh->width == 32 && fresh->y.data[0] == 200);
	TASK_CHECK(plm_video_get_width(video) == 32);
	TASK_CHECK(plm_video_decode(video) == NULL); /* no tail redelivery */
	plm_video_destroy(video);
	return 0;
}

static int test_stream_repeated_header_is_not_a_frame(void) {
	fixture_t es = {0};
	fixture_sequence(&es, 16, 16, 3, 1);
	fixture_intra(&es, 16, 16, 0, 128);
	fixture_predictive(&es, 16, 16, 1, 31); /* 191 */
	/* Same parameters, no sequence_end: still the same sequence. */
	fixture_sequence(&es, 16, 16, 3, 1);
	fixture_predictive(&es, 16, 16, 2, 0);  /* copy of the previous reference */
	fixture_end(&es);

	record_t records[MAX_RECORDS];
	int count = collect_raw(&es, records, MAX_RECORDS);
	TASK_CHECK(count == 3); /* repeated header adds no picture */
	TASK_CHECK(records[0].y == 128);
	TASK_CHECK(records[1].y == 191);
	TASK_CHECK(records[2].y == 191); /* references survived the repeated header */
	return 0;
}

static int test_stream_quant_default_does_not_leak(void) {
	fixture_t custom = {0};
	fixture_sequence_quant(&custom, 16, 16, 3, 1, 0, 1);
	fixture_intra(&custom, 16, 16, 0, 128);
	fixture_predictive(&custom, 16, 16, 1, 31);
	fixture_end(&custom);

	fixture_t deflt = {0};
	fixture_sequence(&deflt, 16, 16, 3, 1);
	fixture_intra(&deflt, 16, 16, 0, 128);
	fixture_predictive(&deflt, 16, 16, 1, 31);
	fixture_end(&deflt);

	record_t a[MAX_RECORDS], b[MAX_RECORDS];
	int ca = collect_raw(&custom, a, MAX_RECORDS);
	int cb = collect_raw(&deflt, b, MAX_RECORDS);
	TASK_CHECK(ca == 2 && cb == 2);
	/* Same residual level, different matrices -> different decoded luma. */
	TASK_CHECK(a[1].y != b[1].y);
	/* The default matrix reproduces the documented 128 + 63 = 191 result. */
	TASK_CHECK(b[1].y == 191);
	return 0;
}

static int test_stream_raw_ps_callback_parity(void) {
	fixture_t es = {0};
	fixture_sequence(&es, 16, 16, 3, 1);
	fixture_intra(&es, 16, 16, 0, 128);
	fixture_predictive(&es, 16, 16, 2, 31);
	fixture_bidirect(&es, 16, 16, 1);
	fixture_end(&es);
	fixture_sequence(&es, 32, 16, 5, 2);
	fixture_intra(&es, 32, 16, 0, 200);
	fixture_intra(&es, 32, 16, 1, 200);
	fixture_end(&es);

	record_t raw[MAX_RECORDS], ps[MAX_RECORDS];
	int raw_count = collect_raw(&es, raw, MAX_RECORDS);
	int ps_count = collect_ps(&es, ps, MAX_RECORDS);
	callback_capture_t capture = {0};
	int callback_count = collect_callback(&es, &capture);

	TASK_CHECK(raw_count == 5);
	TASK_CHECK(ps_count == raw_count);
	TASK_CHECK(callback_count == raw_count);
	TASK_CHECK(records_match(raw, ps, raw_count));
	TASK_CHECK(records_match(raw, capture.records, raw_count));
	return 0;
}

static int test_stream_tail_not_redelivered(void) {
	fixture_t es = {0};
	fixture_sequence(&es, 16, 16, 3, 1);
	fixture_intra(&es, 16, 16, 0, 128);
	fixture_end(&es);
	fixture_sequence(&es, 32, 16, 5, 2);
	fixture_intra(&es, 32, 16, 0, 200);
	fixture_end(&es);

	plm_buffer_t *buffer = plm_buffer_create_with_capacity(64);
	plm_buffer_set_load_callback(buffer, ended_source, NULL);
	plm_buffer_write(buffer, es.bytes, fixture_size(&es));
	plm_buffer_signal_end(buffer);
	plm_video_t *video = plm_video_create_with_buffer(buffer, 1);

	int count = 0;
	while (plm_video_decode(video)) {
		count++;
	}
	TASK_CHECK(count == 2);
	/* Reading past the end keeps returning NULL; the last frame is not reused. */
	TASK_CHECK(plm_video_decode(video) == NULL);
	TASK_CHECK(plm_video_decode(video) == NULL);
	TASK_CHECK(plm_video_has_ended(video) == 1);
	plm_video_destroy(video);
	return 0;
}

static int test_stream_ps_rewind_full_sequence(void) {
	fixture_t es = {0};
	fixture_sequence(&es, 16, 16, 3, 1);
	fixture_intra(&es, 16, 16, 0, 128);
	fixture_intra(&es, 16, 16, 1, 128);
	fixture_end(&es);
	fixture_sequence(&es, 32, 16, 5, 2);
	fixture_intra(&es, 32, 16, 0, 200);
	fixture_end(&es);

	fixture_t ps = {0};
	fixture_ps(&ps, &es);
	size_t length = fixture_size(&ps);
	uint8_t *copy = (uint8_t *)malloc(length);
	memcpy(copy, ps.bytes, length);
	plm_t *player = plm_create_with_memory(copy, length, 1);
	plm_set_audio_enabled(player, 0);

	record_t first[MAX_RECORDS];
	int first_count = collect_ps(&es, first, MAX_RECORDS); /* independent reference */

	int pass = 0;
	plm_frame_t *frame;
	while ((frame = plm_decode_video(player))) {
		pass++;
	}
	TASK_CHECK(pass == first_count);

	plm_rewind(player);
	record_t again[MAX_RECORDS];
	int again_count = 0;
	while ((frame = plm_decode_video(player))) {
		TASK_CHECK(again_count < MAX_RECORDS);
		again[again_count].width = (int)frame->width;
		again[again_count].y = frame->y.data[0];
		again[again_count].time = frame->time;
		again_count++;
	}
	TASK_CHECK(again_count == first_count);
	TASK_CHECK(again[0].width == 16 && again[0].y == 128 && time_near(again[0].time, 0.0));
	TASK_CHECK(again[first_count - 1].width == 32);
	plm_destroy(player);
	return 0;
}

const struct task_case task_cases[] = {
	{ "planes", "size_switch", test_planes_size_switch },
	{ "planes", "b_reorder", test_planes_b_reorder },
	{ "planes", "reference_reset", test_planes_reference_reset },
	{ "planes", "loaned_frame_consistency", test_planes_loaned_frame_internally_consistent },

	{ "clock", "continuous_rate_change", test_clock_continuous_across_rate_change },
	{ "clock", "get_time_next_display", test_clock_get_time_is_next_display_time },
	{ "clock", "getters_follow_sequence", test_clock_getters_follow_active_sequence },
	{ "clock", "rewind_restarts_at_zero", test_clock_rewind_restarts_at_zero },

	{ "stream", "partial_first_header", test_stream_partial_first_header },
	{ "stream", "half_new_header", test_stream_half_new_header_keeps_old_config },
	{ "stream", "repeated_header_not_frame", test_stream_repeated_header_is_not_a_frame },
	{ "stream", "quant_default_isolation", test_stream_quant_default_does_not_leak },
	{ "stream", "raw_ps_callback_parity", test_stream_raw_ps_callback_parity },
	{ "stream", "tail_not_redelivered", test_stream_tail_not_redelivered },
	{ "stream", "ps_rewind", test_stream_ps_rewind_full_sequence },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);

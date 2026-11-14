/* Regression tests for decoding consecutive MPEG-1 video sequences.
 *
 * A playable stream may concatenate several sequences, each with its own
 * sequence header (dimensions, pixel aspect ratio, framerate, quantizer
 * matrices) and sequence end. These tests pin down the behavior at the
 * boundaries: plane sizes and content, the display clock and the various
 * input paths (raw ES, PS container, callbacks, fragmented writes).
 */
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "pl_mpeg.h"
#include "fixture.h"
#include "check.h"

#define MAX_FRAMES 64

typedef struct {
	unsigned width, height;
	unsigned yw, yh, cw, ch;
	double time;
	unsigned y0, cb0, cr0;
	unsigned gw, gh; /* plm(_video)_get_width/height at delivery time */
	uint32_t yhash, cbhash, crhash;
} frame_rec_t;

typedef struct {
	frame_rec_t frames[MAX_FRAMES];
	unsigned count;
} rec_t;

static uint32_t hash_plane(const plm_plane_t *p) {
	uint32_t h = 2166136261u;
	size_t n = (size_t)p->width * p->height, i;
	for (i = 0; i < n; i++) {
		h = (h ^ p->data[i]) * 16777619u;
	}
	return h;
}

static frame_rec_t *rec_push(rec_t *r, const plm_frame_t *f) {
	frame_rec_t *o;
	if (r->count >= MAX_FRAMES) {
		return NULL;
	}
	o = &r->frames[r->count++];
	o->width = f->width; o->height = f->height;
	o->yw = f->y.width; o->yh = f->y.height;
	o->cw = f->cb.width; o->ch = f->cb.height;
	o->time = f->time;
	o->y0 = f->y.data[0]; o->cb0 = f->cb.data[0]; o->cr0 = f->cr.data[0];
	o->gw = o->gh = 0;
	o->yhash = hash_plane(&f->y);
	o->cbhash = hash_plane(&f->cb);
	o->crhash = hash_plane(&f->cr);
	return o;
}

static void ended_source(plm_buffer_t *b, void *user) {
	(void)user;
	plm_buffer_signal_end(b);
}

static void on_video_frame(plm_t *plm, plm_frame_t *frame, void *user) {
	(void)plm;
	rec_push((rec_t *)user, frame);
}

static plm_video_t *video_from(fixture_t *es) {
	plm_buffer_t *b = plm_buffer_create_with_capacity(64);
	plm_buffer_write(b, es->bytes, fixture_size(es));
	plm_buffer_set_load_callback(b, ended_source, NULL);
	plm_buffer_signal_end(b);
	return plm_video_create_with_buffer(b, 1);
}

static void decode_all(plm_video_t *v, rec_t *r) {
	plm_frame_t *f;
	while ((f = plm_video_decode(v))) {
		rec_push(r, f);
	}
}

static int check_time(double got, double want) {
	return fabs(got - want) < 1e-9;
}

/* ---------------------------------------------------------------- planes */

/* Three segments with different sizes (grow, then shrink); the frame slots
 * and macroblock planes must follow the current segment. */
static int planes_resize_chain(void) {
	fixture_t es = {0};
	fixture_sequence(&es, 16, 16, 3, 1);
	fixture_intra(&es, 16, 16, 0, 80);
	fixture_intra(&es, 16, 16, 1, 90);
	fixture_end(&es);
	fixture_sequence(&es, 32, 16, 6, 2);
	fixture_intra(&es, 32, 16, 0, 140);
	fixture_intra(&es, 32, 16, 1, 150);
	fixture_end(&es);
	fixture_sequence(&es, 16, 32, 3, 1);
	fixture_intra(&es, 16, 32, 0, 200);
	fixture_intra(&es, 16, 32, 1, 210);
	fixture_end(&es);

	static const unsigned w[] = {16, 16, 32, 32, 16, 16};
	static const unsigned h[] = {16, 16, 16, 16, 32, 32};
	static const unsigned gray[] = {80, 90, 140, 150, 200, 210};
	static const double rate[] = {25, 25, 50, 50, 25, 25};
	static const double par[] = {1.0, 1.0, 0.6735, 0.6735, 1.0, 1.0};

	plm_video_t *v = video_from(&es);
	rec_t r = {{0}, 0};
	plm_frame_t *f;
	unsigned i = 0;
	while ((f = plm_video_decode(v))) {
		frame_rec_t *o = rec_push(&r, f);
		/* The public queries always describe the delivered frame. */
		TASK_CHECK(plm_video_get_width(v) == (int)f->width);
		TASK_CHECK(plm_video_get_height(v) == (int)f->height);
		if (o) { o->gw = (unsigned)plm_video_get_width(v); o->gh = (unsigned)plm_video_get_height(v); }
		TASK_CHECK(i < 6 && fabs(plm_video_get_framerate(v) - rate[i]) < 1e-9);
		TASK_CHECK(i < 6 && fabs(plm_video_get_pixel_aspect_ratio(v) - par[i]) < 1e-4);
		i++;
	}
	TASK_CHECK(r.count == 6);
	for (i = 0; i < r.count && i < 6; i++) {
		TASK_CHECK(r.frames[i].width == w[i]);
		TASK_CHECK(r.frames[i].height == h[i]);
		TASK_CHECK(r.frames[i].yw == w[i] && r.frames[i].yh == h[i]);
		TASK_CHECK(r.frames[i].cw == w[i] / 2 && r.frames[i].ch == h[i] / 2);
		TASK_CHECK(r.frames[i].y0 == gray[i]);
		TASK_CHECK(r.frames[i].cb0 == 128 && r.frames[i].cr0 == 128);
		TASK_CHECK(r.frames[i].gw == w[i] && r.frames[i].gh == h[i]);
	}
	plm_video_destroy(v);
	return 0;
}

/* I/P/B reordering inside a segment and across the sequence boundary. The
 * B pictures copy their references, so the display order is recognizable
 * by content. */
static int planes_ipb_reorder(void) {
	fixture_t es = {0};
	fixture_sequence(&es, 32, 16, 3, 1);
	fixture_intra(&es, 32, 16, 0, 80);   /* I  -> 80 */
	fixture_p(&es, 32, 16, 1, 160);      /* P  -> 160 */
	fixture_b_copy(&es, 32, 16, 2, 1);   /* B, forward copy  -> 80 */
	fixture_b_copy(&es, 32, 16, 3, 0);   /* B, backward copy -> 160 */
	fixture_end(&es);
	fixture_sequence(&es, 16, 16, 6, 1);
	fixture_intra(&es, 16, 16, 0, 40);   /* I  -> 40 */
	fixture_p(&es, 16, 16, 1, 60);       /* P  -> 60 */
	fixture_end(&es);

	plm_video_t *v = video_from(&es);
	rec_t r = {{0}, 0};
	decode_all(v, &r);
	plm_video_destroy(v);

	/* Display order: I(80) B(80) B(160) P(160) | I(40) P(60) */
	static const unsigned gray[] = {80, 80, 160, 160, 40, 60};
	static const unsigned w[] = {32, 32, 32, 32, 16, 16};
	static const double t[] = {0.0, 0.04, 0.08, 0.12, 0.16, 0.18};
	unsigned i;
	TASK_CHECK(r.count == 6);
	for (i = 0; i < r.count && i < 6; i++) {
		TASK_CHECK(r.frames[i].y0 == gray[i]);
		TASK_CHECK(r.frames[i].width == w[i]);
		TASK_CHECK(check_time(r.frames[i].time, t[i]));
	}
	/* The B pictures really are copies of their references. */
	if (r.count == 6) {
		TASK_CHECK(r.frames[1].yhash == r.frames[0].yhash);
		TASK_CHECK(r.frames[2].yhash == r.frames[3].yhash);
	}
	return 0;
}

/* A new sequence must not borrow prediction references from the previous
 * one, even when the dimensions stay the same. */
static int planes_reference_reset(void) {
	fixture_t es = {0};
	fixture_sequence(&es, 16, 16, 3, 1);
	fixture_intra(&es, 16, 16, 0, 200);
	fixture_end(&es);
	fixture_sequence(&es, 16, 16, 3, 1);
	fixture_intra(&es, 16, 16, 0, 30);
	fixture_p_copy(&es, 16, 16, 1); /* predicts from this segment's I */
	fixture_end(&es);

	plm_video_t *v = video_from(&es);
	rec_t r = {{0}, 0};
	decode_all(v, &r);
	plm_video_destroy(v);

	TASK_CHECK(r.count == 3);
	if (r.count >= 3) {
		TASK_CHECK(r.frames[0].y0 == 200);
		TASK_CHECK(r.frames[1].y0 == 30);
		TASK_CHECK(r.frames[2].y0 == 30);
		/* The P picture copied the new segment's reference, not the old. */
		TASK_CHECK(r.frames[2].yhash == r.frames[1].yhash);
		TASK_CHECK(r.frames[2].yhash != r.frames[0].yhash);
	}
	return 0;
}

/* A new sequence header that arrives without a sequence end (while the
 * previous sequence still has an unpublished reference frame) must deliver
 * that frame with the old configuration first; the new configuration is
 * committed on the next decode call. */
static int planes_header_without_end(void) {
	fixture_t es = {0};
	fixture_sequence(&es, 16, 16, 3, 1);
	fixture_intra(&es, 16, 16, 0, 50);
	fixture_p(&es, 16, 16, 1, 60);
	/* no fixture_end() here */
	fixture_sequence(&es, 32, 16, 6, 1);
	fixture_intra(&es, 32, 16, 0, 100);
	fixture_intra(&es, 32, 16, 1, 110);
	fixture_end(&es);

	plm_video_t *v = video_from(&es);
	rec_t r = {{0}, 0};
	plm_frame_t *f;
	unsigned i = 0;
	while ((f = plm_video_decode(v))) {
		frame_rec_t *o = rec_push(&r, f);
		/* The public queries always match the delivered frame. */
		TASK_CHECK(plm_video_get_width(v) == (int)f->width);
		if (o) { o->gw = (unsigned)plm_video_get_width(v); }
		i++;
	}
	plm_video_destroy(v);

	static const unsigned w[] = {16, 16, 32, 32};
	static const unsigned gray[] = {50, 60, 100, 110};
	static const double t[] = {0.0, 0.04, 0.08, 0.10};
	TASK_CHECK(r.count == 4);
	for (i = 0; i < r.count && i < 4; i++) {
		TASK_CHECK(r.frames[i].width == w[i]);
		TASK_CHECK(r.frames[i].y0 == gray[i]);
		TASK_CHECK(r.frames[i].gw == w[i]);
		TASK_CHECK(check_time(r.frames[i].time, t[i]));
	}
	return 0;
}

/* Quantizer matrices: custom matrices must affect subsequent pictures and
 * must not leak into a segment (or repeated header) that asks for the
 * default matrices. */
static int planes_quant_matrix(void) {
	uint8_t custom[64];
	unsigned i;
	for (i = 0; i < 64; i++) { custom[i] = 1; }

	/* A: default matrices only */
	fixture_t a = {0};
	fixture_sequence(&a, 16, 16, 3, 1);
	fixture_intra_ac(&a, 16, 16, 0, 100, 1);
	fixture_intra_ac(&a, 16, 16, 1, 100, 1);
	fixture_end(&a);

	/* B: custom-matrix segment, then a default-matrix segment */
	fixture_t b = {0};
	fixture_sequence_matrix(&b, 16, 16, 3, 1, custom, NULL);
	fixture_intra_ac(&b, 16, 16, 0, 100, 1);
	fixture_end(&b);
	fixture_sequence(&b, 16, 16, 3, 1);
	fixture_intra_ac(&b, 16, 16, 0, 100, 1);
	fixture_end(&b);

	/* C: repeated header (same parameters, no matrices) mid-sequence */
	fixture_t c = {0};
	fixture_sequence_matrix(&c, 16, 16, 3, 1, custom, NULL);
	fixture_intra_ac(&c, 16, 16, 0, 100, 1);
	fixture_sequence(&c, 16, 16, 3, 1);
	fixture_intra_ac(&c, 16, 16, 1, 100, 1);
	fixture_end(&c);

	rec_t ra = {{0}, 0}, rb = {{0}, 0}, rc = {{0}, 0};
	plm_video_t *v = video_from(&a);
	decode_all(v, &ra);
	plm_video_destroy(v);
	v = video_from(&b);
	decode_all(v, &rb);
	plm_video_destroy(v);
	v = video_from(&c);
	decode_all(v, &rc);
	plm_video_destroy(v);

	TASK_CHECK(ra.count == 2 && rb.count == 2 && rc.count == 2);
	if (ra.count == 2 && rb.count == 2 && rc.count == 2) {
		/* The custom matrix changed the decoded pixels. */
		TASK_CHECK(rb.frames[0].yhash != ra.frames[0].yhash);
		/* The second segment fell back to the default matrices. */
		TASK_CHECK(rb.frames[1].yhash == ra.frames[0].yhash);
		/* A repeated header without matrices restores the defaults, too. */
		TASK_CHECK(rc.frames[0].yhash == rb.frames[0].yhash);
		TASK_CHECK(rc.frames[1].yhash == ra.frames[0].yhash);
		/* Both default-matrix frames of A are identical. */
		TASK_CHECK(ra.frames[0].yhash == ra.frames[1].yhash);
	}
	return 0;
}

/* ----------------------------------------------------------------- clock */

/* The display clock accumulates each segment's actual frame period; it is
 * neither reset nor recomputed with the new framerate. */
static int clock_continuous_timeline(void) {
	fixture_t es = {0};
	fixture_sequence(&es, 16, 16, 3, 1); /* 25 fps */
	fixture_intra(&es, 16, 16, 0, 70);
	fixture_p(&es, 16, 16, 1, 90);
	fixture_end(&es);
	fixture_sequence(&es, 16, 16, 6, 1); /* 50 fps, same dimensions */
	fixture_intra(&es, 16, 16, 0, 120);
	fixture_p_copy(&es, 16, 16, 1);
	fixture_end(&es);

	static const double t[] = {0.0, 0.04, 0.08, 0.10};
	static const unsigned gray[] = {70, 90, 120, 120};

	plm_video_t *v = video_from(&es);
	TASK_CHECK(check_time(plm_video_get_time(v), 0.0));
	rec_t r = {{0}, 0};
	plm_frame_t *f;
	unsigned i = 0;
	while ((f = plm_video_decode(v))) {
		rec_push(&r, f);
		TASK_CHECK(i < 4 && check_time(f->time, t[i]));
		/* get_time is the time of the next frame to be displayed. */
		TASK_CHECK(i < 4 && check_time(plm_video_get_time(v), t[i] + (i < 2 ? 0.04 : 0.02)));
		i++;
	}
	TASK_CHECK(r.count == 4);
	for (i = 0; i < r.count && i < 4; i++) {
		TASK_CHECK(r.frames[i].y0 == gray[i]);
	}
	plm_video_destroy(v);
	return 0;
}

/* Rewind restarts from the first segment's configuration at time zero. */
static int clock_rewind(void) {
	fixture_t es = {0};
	fixture_sequence(&es, 16, 16, 3, 1);
	fixture_intra(&es, 16, 16, 0, 60);
	fixture_intra(&es, 16, 16, 1, 64);
	fixture_end(&es);
	fixture_sequence(&es, 32, 16, 6, 1);
	fixture_intra(&es, 32, 16, 0, 160);
	fixture_intra(&es, 32, 16, 1, 170);
	fixture_end(&es);

	plm_buffer_t *buf = plm_buffer_create_with_capacity(64);
	plm_buffer_write(buf, es.bytes, fixture_size(&es));
	plm_buffer_signal_end(buf);
	plm_video_t *v = plm_video_create_with_buffer(buf, 1);

	rec_t r1 = {{0}, 0};
	decode_all(v, &r1);
	TASK_CHECK(r1.count == 4);

	/* A capacity buffer drops read bytes, so the source is offered again
	 * after the rewind, just like a file or network source would be. */
	plm_video_rewind(v);
	plm_buffer_write(buf, es.bytes, fixture_size(&es));
	plm_buffer_signal_end(buf);
	TASK_CHECK(check_time(plm_video_get_time(v), 0.0));

	rec_t r2 = {{0}, 0};
	decode_all(v, &r2);
	plm_video_destroy(v);

	TASK_CHECK(r2.count == r1.count);
	if (r2.count == r1.count) {
		unsigned i;
		for (i = 0; i < r1.count; i++) {
			TASK_CHECK(r2.frames[i].width == r1.frames[i].width);
			TASK_CHECK(r2.frames[i].height == r1.frames[i].height);
			TASK_CHECK(check_time(r2.frames[i].time, r1.frames[i].time));
			TASK_CHECK(r2.frames[i].yhash == r1.frames[i].yhash);
		}
		/* Back at the first segment's configuration and timeline. */
		if (r2.count) {
			TASK_CHECK(r2.frames[0].width == 16);
			TASK_CHECK(check_time(r2.frames[0].time, 0.0));
		}
	}
	return 0;
}

/* Reading past the end must not deliver the tail frame again. */
static int clock_end_of_stream(void) {
	fixture_t es = {0};
	fixture_sequence(&es, 16, 16, 3, 1);
	fixture_intra(&es, 16, 16, 0, 70);
	fixture_p(&es, 16, 16, 1, 90);
	fixture_end(&es);
	fixture_sequence(&es, 16, 16, 3, 1);
	fixture_intra(&es, 16, 16, 0, 120);
	fixture_end(&es);

	plm_video_t *v = video_from(&es);
	rec_t r = {{0}, 0};
	decode_all(v, &r);
	TASK_CHECK(r.count == 3);
	double end_time = plm_video_get_time(v);
	TASK_CHECK(check_time(end_time, 0.12));
	TASK_CHECK(plm_video_decode(v) == NULL);
	TASK_CHECK(plm_video_decode(v) == NULL);
	TASK_CHECK(plm_video_decode(v) == NULL);
	TASK_CHECK(r.count == 3);
	TASK_CHECK(check_time(plm_video_get_time(v), end_time));
	plm_video_destroy(v);
	return 0;
}

/* ---------------------------------------------------------------- stream */

/* Data trickling into a capacity buffer: incomplete headers or matrices
 * must wait for more data instead of producing a mixed configuration or
 * being mistaken for the end of the stream. */
static int stream_fragmented_feed(void) {
	fixture_t seg1 = {0};
	fixture_sequence(&seg1, 16, 16, 3, 1);
	fixture_intra(&seg1, 16, 16, 0, 70);
	fixture_p(&seg1, 16, 16, 1, 90);
	fixture_end(&seg1);

	fixture_t es = {0};
	fixture_sequence(&es, 16, 16, 3, 1);
	fixture_intra(&es, 16, 16, 0, 70);
	fixture_p(&es, 16, 16, 1, 90);
	fixture_end(&es);
	size_t boundary = fixture_size(&es);
	fixture_sequence(&es, 32, 16, 6, 1);
	fixture_intra(&es, 32, 16, 0, 120);
	fixture_intra(&es, 32, 16, 1, 130);
	fixture_end(&es);
	size_t total = fixture_size(&es);
	TASK_CHECK(boundary == fixture_size(&seg1));

	plm_buffer_t *buf = plm_buffer_create_with_capacity(64);
	plm_video_t *v = plm_video_create_with_buffer(buf, 1);
	rec_t r = {{0}, 0};
	plm_frame_t *f;

	/* Feed the first segment and the start code plus two bytes of the
	 * second sequence header. The header is incomplete: no frame may come
	 * out and the reported configuration must stay the old one. */
	size_t pos = boundary + 6;
	TASK_CHECK(pos < total);
	plm_buffer_write(buf, es.bytes, pos);
	while ((f = plm_video_decode(v))) { rec_push(&r, f); }
	TASK_CHECK(r.count == 2);
	TASK_CHECK(plm_video_get_width(v) == 16);
	TASK_CHECK(plm_video_get_height(v) == 16);

	/* Dribble in the rest, 7 bytes at a time. */
	while (pos < total) {
		size_t n = total - pos;
		if (n > 7) { n = 7; }
		plm_buffer_write(buf, es.bytes + pos, n);
		pos += n;
		while ((f = plm_video_decode(v))) { rec_push(&r, f); }
	}
	plm_buffer_signal_end(buf);
	while ((f = plm_video_decode(v))) { rec_push(&r, f); }
	plm_video_destroy(v);

	/* Compare against a one-shot decode of the same stream. */
	rec_t whole = {{0}, 0};
	v = video_from(&es);
	decode_all(v, &whole);
	plm_video_destroy(v);

	TASK_CHECK(r.count == 4 && whole.count == 4);
	if (r.count == whole.count) {
		unsigned i;
		for (i = 0; i < r.count; i++) {
			TASK_CHECK(r.frames[i].width == whole.frames[i].width);
			TASK_CHECK(check_time(r.frames[i].time, whole.frames[i].time));
			TASK_CHECK(r.frames[i].yhash == whole.frames[i].yhash);
			TASK_CHECK(r.frames[i].cbhash == whole.frames[i].cbhash);
		}
	}
	return 0;
}

/* A repeated sequence header with identical parameters (and no sequence
 * end) belongs to the current sequence: no extra frame, no cleared
 * references, no clock reset. */
static int stream_repeated_headers(void) {
	fixture_t es = {0};
	fixture_sequence(&es, 16, 16, 3, 1);
	fixture_intra(&es, 16, 16, 0, 90);
	fixture_intra(&es, 16, 16, 1, 91);
	fixture_sequence(&es, 16, 16, 3, 1); /* identical repeat */
	fixture_intra(&es, 16, 16, 2, 92);
	fixture_p_copy(&es, 16, 16, 3);
	fixture_end(&es);

	plm_video_t *v = video_from(&es);
	rec_t r = {{0}, 0};
	decode_all(v, &r);
	plm_video_destroy(v);

	static const unsigned gray[] = {90, 91, 92, 92};
	unsigned i;
	TASK_CHECK(r.count == 4);
	for (i = 0; i < r.count && i < 4; i++) {
		TASK_CHECK(r.frames[i].y0 == gray[i]);
		TASK_CHECK(check_time(r.frames[i].time, 0.04 * (double)i));
	}
	/* The P picture still predicts from the I before the repeated header. */
	if (r.count == 4) {
		TASK_CHECK(r.frames[3].yhash == r.frames[2].yhash);
	}
	return 0;
}

/* Raw ES, PS container and the callback interface must all produce the
 * same display sequence. */
static int stream_container_and_callback(void) {
	fixture_t es = {0};
	fixture_sequence(&es, 32, 16, 3, 1);
	fixture_intra(&es, 32, 16, 0, 80);
	fixture_p(&es, 32, 16, 1, 160);
	fixture_b_copy(&es, 32, 16, 2, 1);
	fixture_end(&es);
	fixture_sequence(&es, 16, 16, 6, 1);
	fixture_intra(&es, 16, 16, 0, 40);
	fixture_p(&es, 16, 16, 1, 60);
	fixture_end(&es);

	/* Raw elementary stream */
	rec_t raw = {{0}, 0};
	plm_video_t *v = video_from(&es);
	decode_all(v, &raw);
	plm_video_destroy(v);
	TASK_CHECK(raw.count == 5);

	/* Same stream in a PS container */
	fixture_t ps = {0};
	fixture_ps(&ps, &es);
	rec_t via_ps = {{0}, 0};
	plm_t *p = plm_create_with_memory(ps.bytes, fixture_size(&ps), 0);
	TASK_CHECK(p != NULL);
	plm_set_audio_enabled(p, 0);
	plm_frame_t *f;
	while ((f = plm_decode_video(p))) {
		frame_rec_t *o = rec_push(&via_ps, f);
		TASK_CHECK(plm_get_width(p) == (int)f->width);
		TASK_CHECK(plm_get_height(p) == (int)f->height);
		if (o) { o->gw = (unsigned)plm_get_width(p); o->gh = (unsigned)plm_get_height(p); }
	}
	plm_destroy(p);

	/* Same stream through the video decode callback */
	rec_t via_cb = {{0}, 0};
	p = plm_create_with_memory(ps.bytes, fixture_size(&ps), 0);
	TASK_CHECK(p != NULL);
	plm_set_audio_enabled(p, 0);
	plm_set_video_decode_callback(p, on_video_frame, &via_cb);
	plm_decode(p, 1000.0);
	plm_destroy(p);

	TASK_CHECK(via_ps.count == raw.count);
	TASK_CHECK(via_cb.count == raw.count);
	if (via_ps.count == raw.count && via_cb.count == raw.count) {
		unsigned i;
		for (i = 0; i < raw.count; i++) {
			TASK_CHECK(via_ps.frames[i].width == raw.frames[i].width);
			TASK_CHECK(via_ps.frames[i].height == raw.frames[i].height);
			TASK_CHECK(check_time(via_ps.frames[i].time, raw.frames[i].time));
			TASK_CHECK(via_ps.frames[i].yhash == raw.frames[i].yhash);
			TASK_CHECK(via_ps.frames[i].gw == raw.frames[i].width);
			TASK_CHECK(via_cb.frames[i].width == raw.frames[i].width);
			TASK_CHECK(check_time(via_cb.frames[i].time, raw.frames[i].time));
			TASK_CHECK(via_cb.frames[i].yhash == raw.frames[i].yhash);
		}
	}
	return 0;
}

/* Rewind of the high-level interface replays the whole chain. */
static int stream_ps_rewind(void) {
	fixture_t es = {0};
	fixture_sequence(&es, 16, 16, 3, 1);
	fixture_intra(&es, 16, 16, 0, 60);
	fixture_intra(&es, 16, 16, 1, 64);
	fixture_end(&es);
	fixture_sequence(&es, 32, 16, 6, 1);
	fixture_intra(&es, 32, 16, 0, 160);
	fixture_intra(&es, 32, 16, 1, 170);
	fixture_end(&es);

	fixture_t ps = {0};
	fixture_ps(&ps, &es);
	plm_t *p = plm_create_with_memory(ps.bytes, fixture_size(&ps), 0);
	TASK_CHECK(p != NULL);
	plm_set_audio_enabled(p, 0);

	rec_t r1 = {{0}, 0}, r2 = {{0}, 0};
	plm_frame_t *f;
	while ((f = plm_decode_video(p))) { rec_push(&r1, f); }
	TASK_CHECK(r1.count == 4);

	plm_rewind(p);
	TASK_CHECK(check_time(plm_get_time(p), 0.0));
	while ((f = plm_decode_video(p))) { rec_push(&r2, f); }
	plm_destroy(p);

	TASK_CHECK(r2.count == r1.count);
	if (r2.count == r1.count) {
		unsigned i;
		for (i = 0; i < r1.count; i++) {
			TASK_CHECK(r2.frames[i].width == r1.frames[i].width);
			TASK_CHECK(check_time(r2.frames[i].time, r1.frames[i].time));
			TASK_CHECK(r2.frames[i].yhash == r1.frames[i].yhash);
		}
		if (r2.count) {
			TASK_CHECK(r2.frames[0].width == 16);
			TASK_CHECK(check_time(r2.frames[0].time, 0.0));
		}
	}
	return 0;
}

const struct task_case task_cases[] = {
	{"planes", "resize_chain", planes_resize_chain},
	{"planes", "ipb_reorder", planes_ipb_reorder},
	{"planes", "reference_reset", planes_reference_reset},
	{"planes", "header_without_end", planes_header_without_end},
	{"planes", "quant_matrix", planes_quant_matrix},
	{"clock", "continuous_timeline", clock_continuous_timeline},
	{"clock", "rewind", clock_rewind},
	{"clock", "end_of_stream", clock_end_of_stream},
	{"stream", "fragmented_feed", stream_fragmented_feed},
	{"stream", "repeated_headers", stream_repeated_headers},
	{"stream", "container_and_callback", stream_container_and_callback},
	{"stream", "ps_rewind", stream_ps_rewind},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);

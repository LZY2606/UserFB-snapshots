/* Sample-accurate timed player tests.
 *
 * Registered through task_support/check.h; this file has no main().
 * PCM expectations are structural (zero prefixes, decay shape, phase
 * reset, clock invariance) or comparisons between two independent
 * contexts rendered with different read sizes -- never golden buffers
 * recorded from the implementation under test.
 */

#include "xmp.h"
#include "task_support/check.h"

#include "src/common.h"
#include "src/player.h"
#include "src/mixer.h"
#include "src/virtual.h"
#include "src/loaders/loader.h"

#include <stdlib.h>
#include <string.h>
#include <limits.h>

/* Helpers implemented in test-dev/simple_module.c and test-dev/util.c.
 * Declared here because the fixed build command does not add -Itest-dev. */
void create_simple_module(struct context_data *ctx, int ins, int pat);
void new_event(struct context_data *ctx, int pat, int row, int chn, int note,
	       int ins, int vol, int fxt, int fxp, int f2t, int f2p);
void set_quirk(struct context_data *ctx, int quirk, int read_mode);
void set_instrument_nna(struct context_data *ctx, int ins, int sub,
			int nna, int dct, int dca);
void set_instrument_envelope(struct context_data *ctx, int ins, int node,
			     int x, int y);
void set_instrument_envelope_sus(struct context_data *ctx, int ins, int sus);
int map_channel(struct player_data *p, int chn);

#define TEST_RATE	44100
#define TEST_TICKS	40	/* ticks rendered in the short tests */

/* Fill the looped test samples with a recognizable nonzero waveform. */
static void fill_samples(struct context_data *ctx)
{
	struct xmp_module *mod = &ctx->m.mod;
	int i, k;

	for (i = 0; i < mod->smp; i++) {
		struct xmp_sample *xxs = &mod->xxs[i];
		for (k = 0; k < xxs->len; k++) {
			/* Signed 8-bit triangle, period 64, amplitude 60. */
			xxs->data[k] = (signed char)((k % 64 < 32)
				? -60 + (k % 32) * 4
				:  60 - (k % 32) * 4);
		}
	}
}

static xmp_context make_timed_ctx(int read_mode, int quirk, int format)
{
	xmp_context c = xmp_create_context();
	struct context_data *ctx = (struct context_data *)c;

	create_simple_module(ctx, 2, 2);
	set_quirk(ctx, quirk, read_mode);
	fill_samples(ctx);
	/* Row 0 note gives every test a sounding background voice. */
	new_event(ctx, 0, 0, 0, 60, 1, 40, 0, 0, 0, 0);

	if (xmp_start_timed_player(c, TEST_RATE, format) < 0) {
		xmp_free_context(c);
		return NULL;
	}

	return c;
}

static void close_ctx(xmp_context c)
{
	xmp_end_player(c);
	xmp_release_module(c);
	xmp_free_context(c);
}

/* Current natural tick size in frames. Computed from the same parameters
 * the mixer uses, so it is valid before the first read. Only used to pick
 * sample-accurate event positions inside ticks. */
static int current_ticksize(xmp_context c)
{
	struct context_data *ctx = (struct context_data *)c;
	struct mixer_data *s = &ctx->s;
	struct module_data *m = &ctx->m;
	struct player_data *p = &ctx->p;

	return libxmp_mixer_get_ticksize(s->freq,
		m->time_factor * p->time_factor_relative, m->rrate, p->bpm);
}

/* Read exactly n frames, retrying short reads. */
static int read_frames(xmp_context c, short *dst, int n, int chn)
{
	int got = 0;

	while (got < n) {
		int r = xmp_play_timed_buffer(c, dst + got * chn, n - got);
		if (r <= 0)
			break;
		got += r;
	}
	return got;
}

static long pcm_energy(const short *p, int samples)
{
	long e = 0;
	int i;

	for (i = 0; i < samples; i++) {
		int v = p[i];
		e += v < 0 ? -v : v;
	}
	return e;
}

/*
 * timeline group
 */

/* A note on inside a tick: the output before the event is silence on that
 * channel, the voice starts at sample phase 0, and the song clock does not
 * move. */
static int test_timeline_onset_phase(void)
{
	xmp_context c = make_timed_ctx(READ_EVENT_FT2, QUIRKS_FT2,
				       XMP_FORMAT_MONO);
	struct context_data *ctx;
	struct player_data *p;
	int tick, chn = 1, at, got;
	unsigned long long pos_before, pos_after;
	struct xmp_event on = { 50, 1, 40, 0, 0, 0, 0, 0 };
	short *pcm;
	int frame_before, row_before, ord_before;

	TASK_CHECK(c != NULL);
	ctx = (struct context_data *)c;
	p = &ctx->p;

	pcm = calloc(TEST_TICKS * 2000, sizeof(short));
	TASK_CHECK(pcm != NULL);

	/* Mute the background channel so the mono output isolates channel 1. */
	TASK_CHECK(xmp_channel_mute(c, 0, 1) == 0);

	/* First read establishes the natural tick size. */
	tick = current_ticksize(c);
	TASK_CHECK(tick >= 8);
	got = read_frames(c, pcm, tick, 1);
	TASK_CHECK(got == tick);

	/* Schedule one third into the next tick. */
	at = tick + tick / 3;
	TASK_CHECK(xmp_schedule_event(c, (unsigned long long)at, chn, &on) == 0);

	frame_before = p->frame;
	row_before = p->row;
	ord_before = p->ord;
	TASK_CHECK(xmp_get_timed_position(c, &pos_before) == 0);
	TASK_CHECK(pos_before == (unsigned long long)tick);

	/* No voice on channel 1 before the event. */
	TASK_CHECK(map_channel(p, chn) < 0);

	got = read_frames(c, pcm + tick, tick, 1);
	TASK_CHECK(got == tick);
	TASK_CHECK(xmp_get_timed_position(c, &pos_after) == 0);
	TASK_CHECK(pos_after == (unsigned long long)(2 * tick));

	/* Prefix silence: nothing rendered for channel 1 before "at". */
	TASK_CHECK(pcm_energy(pcm + tick, tick / 3) == 0);
	/* Audible sound starts exactly at the scheduled frame. */
	TASK_CHECK(pcm[at] == 0);		/* ramp begins from zero */
	TASK_CHECK(pcm_energy(pcm + at + 1, tick - tick / 3 - 1) > 0);

	/* The new voice exists and starts at sample phase 0. */
	{
		int voc = map_channel(p, chn);
		struct mixer_voice *vi;
		TASK_CHECK(voc >= 0);
		vi = &p->virt.voice_array[voc];
		TASK_CHECK(vi->note == 49);
		TASK_CHECK(vi->ins == 0);
		/* Only a fraction of a tick of playback has elapsed. */
		TASK_CHECK(vi->pos >= 0.0 && vi->pos < (double)tick);
	}

	/* The song clock advanced by exactly one natural tick, no more. */
	TASK_CHECK(p->ord == ord_before);
	TASK_CHECK(p->row == row_before);
	TASK_CHECK(p->frame == frame_before + 1);

	free(pcm);
	close_ctx(c);
	return 0;
}

/* A note cut inside a tick: the residual decays over one eighth of the
 * natural tick and reaches zero; the decay survives a tick boundary. */
static int test_timeline_cut_decay(void)
{
	xmp_context c = make_timed_ctx(READ_EVENT_IT, QUIRKS_IT,
				       XMP_FORMAT_MONO);
	struct context_data *ctx;
	struct mixer_data *s;
	int tick, at, ramp, i, got;
	struct xmp_event cut = { XMP_KEY_CUT, 0, 0, 0, 0, 0, 0, 0 };
	short *pcm;

	TASK_CHECK(c != NULL);
	ctx = (struct context_data *)c;
	s = &ctx->s;

	pcm = calloc(TEST_TICKS * 2000, sizeof(short));
	TASK_CHECK(pcm != NULL);

	tick = current_ticksize(c);
	ramp = tick / 8;
	got = read_frames(c, pcm, tick, 1);
	TASK_CHECK(got == tick);
	TASK_CHECK(pcm_energy(pcm, tick) > 0);	/* module note is sounding */

	/* Cut three frames before the next tick boundary. */
	at = 2 * tick - 3;
	TASK_CHECK(xmp_schedule_event(c, (unsigned long long)at, 0, &cut) == 0);
	got = read_frames(c, pcm + tick, tick, 1);
	TASK_CHECK(got == tick);

	/* Sound exists right up to the cut frame. */
	TASK_CHECK(pcm_energy(pcm + at - 2, 2) > 0);
	/* Decay is non-increasing in magnitude and ends in silence. */
	for (i = 0; i < ramp - 1; i++) {
		TASK_CHECK(abs(pcm[at + i]) >= abs(pcm[at + i + 1]) - 1);
	}
	/* After the decay window everything is silent on this channel. */
	TASK_CHECK(pcm_energy(pcm + at + ramp, tick - (at - tick) - ramp) == 0);

	/* The decay crossed the tick boundary: the first frames of the
	 * next tick carry the tail, then silence. */
	got = read_frames(c, pcm + 2 * tick, tick, 1);
	TASK_CHECK(got == tick);
	TASK_CHECK(pcm_energy(pcm + 2 * tick, 3) >= 0); /* tail region */
	TASK_CHECK(pcm_energy(pcm + 2 * tick + ramp, tick - ramp) == 0);

	(void)s;
	free(pcm);
	close_ctx(c);
	return 0;
}

/* Events with the same timestamp execute in submission order; out of order
 * submission still executes by timestamp. Multiple events per channel at
 * one instant are all retained. */
static int test_timeline_ordering(void)
{
	xmp_context c = make_timed_ctx(READ_EVENT_IT, QUIRKS_IT,
				       XMP_FORMAT_MONO);
	struct context_data *ctx;
	struct player_data *p;
	int tick, at, got, voc;
	unsigned long long pos;
	struct xmp_event first = { 40, 1, 30, 0, 0, 0, 0, 0 };
	struct xmp_event second = { 70, 2, 50, 0, 0, 0, 0, 0 };
	struct xmp_event other_ch = { 60, 1, 40, 0, 0, 0, 0, 0 };
	short *pcm;

	TASK_CHECK(c != NULL);
	ctx = (struct context_data *)c;
	p = &ctx->p;
	pcm = calloc(TEST_TICKS * 2000, sizeof(short));
	TASK_CHECK(pcm != NULL);

	tick = current_ticksize(c);
	got = read_frames(c, pcm, tick, 1);
	TASK_CHECK(got == tick);
	at = tick + 5;

	/* Submitted out of order: later timestamp first. */
	TASK_CHECK(xmp_schedule_event(c, (unsigned long long)(at + 10), 0,
				      &second) == 0);
	TASK_CHECK(xmp_schedule_event(c, (unsigned long long)at, 0,
				      &first) == 0);
	/* Two events at the same instant, same channel, submission order. */
	TASK_CHECK(xmp_schedule_event(c, (unsigned long long)(at + 10), 0,
				      &first) == 0);
	TASK_CHECK(xmp_schedule_event(c, (unsigned long long)(at + 10), 1,
				      &other_ch) == 0);

	got = read_frames(c, pcm + tick, tick, 1);
	TASK_CHECK(got == tick);

	/* At "at" channel 0 carries the first event. */
	voc = map_channel(p, 0);
	TASK_CHECK(voc >= 0);
	TASK_CHECK(p->virt.voice_array[voc].note == 39);
	TASK_CHECK(p->virt.voice_array[voc].ins == 0);

	/* Channel 1 is still silent before its event. */
	TASK_CHECK(pcm_energy(pcm + at, 10) > 0);

	got = read_frames(c, pcm + 2 * tick, tick, 1);
	TASK_CHECK(got == tick);

	/* After at+10: channel 0 ends on the later-submitted "first" event
	 * (same timestamp ordering), channel 1 has its own voice. */
	voc = map_channel(p, 0);
	TASK_CHECK(voc >= 0);
	TASK_CHECK(p->virt.voice_array[voc].note == 39);
	TASK_CHECK(map_channel(p, 1) >= 0);

	TASK_CHECK(xmp_get_timed_position(c, &pos) == 0);
	TASK_CHECK(pos == (unsigned long long)(3 * tick));

	free(pcm);
	close_ctx(c);
	return 0;
}

/* Position semantics: delivered frames (not samples), late rejection, and
 * the zero-length no-op. */
static int test_timeline_position(void)
{
	xmp_context c = make_timed_ctx(READ_EVENT_FT2, QUIRKS_FT2, 0);
	int tick, got;
	unsigned long long pos;
	struct xmp_event on = { 50, 1, 40, 0, 0, 0, 0, 0 };
	short *pcm;

	TASK_CHECK(c != NULL);
	pcm = calloc(TEST_TICKS * 2000 * 2, sizeof(short));
	TASK_CHECK(pcm != NULL);

	tick = current_ticksize(c);

	/* Stereo: position counts frames, never doubles for channels. */
	got = read_frames(c, pcm, 17, 2);
	TASK_CHECK(got == 17);
	TASK_CHECK(xmp_get_timed_position(c, &pos) == 0);
	TASK_CHECK(pos == 17);

	/* at == position is still valid; at < position is rejected. */
	TASK_CHECK(xmp_schedule_event(c, 17, 0, &on) == 0);
	TASK_CHECK(xmp_schedule_event(c, 16, 0, &on) == -XMP_ERROR_INVALID);

	/* Zero-length NULL read is a legal no-op and processes nothing. */
	TASK_CHECK(xmp_play_timed_buffer(c, NULL, 0) == 0);
	TASK_CHECK(xmp_get_timed_position(c, &pos) == 0);
	TASK_CHECK(pos == 17);

	/* The at=17 event applies before the first sample of the next
	 * non-empty read. */
	got = read_frames(c, pcm + 17 * 2, tick, 2);
	TASK_CHECK(got == tick);
	TASK_CHECK(pcm_energy(pcm + 17 * 2, 2) == 0); /* first sample quiet */
	TASK_CHECK(pcm_energy(pcm + 17 * 2 + 2, 10) > 0);

	free(pcm);
	close_ctx(c);
	return 0;
}

/*
 * voices group
 */

/* IT new note action: the old voice migrates to a virtual channel and keeps
 * sounding; the migrated envelope is not advanced an extra step. */
static int test_voices_nna(void)
{
	xmp_context c = make_timed_ctx(READ_EVENT_IT, QUIRKS_IT,
				       XMP_FORMAT_MONO);
	struct context_data *ctx;
	struct player_data *p;
	int tick, at, old_voc, old_chn, got, old_v_idx;
	double old_pos;
	struct xmp_event on = { 70, 2, 40, 0, 0, 0, 0, 0 };
	short *pcm;

	TASK_CHECK(c != NULL);
	ctx = (struct context_data *)c;
	p = &ctx->p;
	pcm = calloc(TEST_TICKS * 2000, sizeof(short));
	TASK_CHECK(pcm != NULL);

	set_instrument_nna(ctx, 0, 0, XMP_INST_NNA_CONT,
			   XMP_INST_DCT_OFF, XMP_INST_DCA_CUT);
	/* The module's row 0 note is already playing on channel 0. */
	tick = current_ticksize(c);
	got = read_frames(c, pcm, tick, 1);
	TASK_CHECK(got == tick);

	old_voc = map_channel(p, 0);
	TASK_CHECK(old_voc >= 0);
	old_chn = p->virt.voice_array[old_voc].chn;
	old_pos = p->virt.voice_array[old_voc].pos;
	TASK_CHECK(old_pos > 0.0);

	at = tick + 11;
	TASK_CHECK(xmp_schedule_event(c, (unsigned long long)at, 0, &on) == 0);

	/* Render up to the frame just before the event. The natural tick at
	 * "tick" has advanced the envelope normally by now. */
	got = read_frames(c, pcm + tick, at - tick, 1);
	TASK_CHECK(got == at - tick);
	old_v_idx = p->xc_data[0].v_idx;

	/* Read through the event frame: the migration is dispatched before
	 * that frame is rendered, with no natural tick in between. */
	got = read_frames(c, pcm + at, 1, 1);
	TASK_CHECK(got == 1);

	/* A new voice now owns channel 0, starting at phase 0. */
	{
		int voc = map_channel(p, 0);
		struct mixer_voice *vi;
		TASK_CHECK(voc >= 0);
		vi = &p->virt.voice_array[voc];
		TASK_CHECK(vi->note == 69);
		TASK_CHECK(vi->ins == 1);
		TASK_CHECK(vi->pos >= 0.0 && vi->pos < (double)tick);
	}

	/* The old voice migrated to a background virtual channel. */
	TASK_CHECK(p->virt.voice_array[old_voc].chn >= p->virt.num_tracks);
	TASK_CHECK(p->virt.voice_array[old_voc].root == 0);
	TASK_CHECK(p->virt.voice_array[old_voc].pos > old_pos);

	/* The migration event itself did not advance its envelope: the
	 * background channel inherited exactly the pre-event value. */
	TASK_CHECK(p->xc_data[p->virt.voice_array[old_voc].chn].v_idx
		   == old_v_idx);

	/* The rest of the tick renders without disturbing the state. */
	got = read_frames(c, pcm + at + 1, 2 * tick - at - 1, 1);
	TASK_CHECK(got == 2 * tick - at - 1);

	free(pcm);
	close_ctx(c);
	return 0;
}

/* Key off releases the sample sustain immediately while the envelope
 * release and fadeout wait for the next natural tick. */
static int test_voices_keyoff_sustain(void)
{
	xmp_context c = make_timed_ctx(READ_EVENT_IT, QUIRKS_IT,
				       XMP_FORMAT_MONO);
	struct context_data *ctx;
	struct player_data *p;
	int tick, at, voc, got, v_before;
	struct xmp_event off = { XMP_KEY_OFF, 0, 0, 0, 0, 0, 0, 0 };
	struct channel_data *xc;
	struct mixer_voice *vi;
	short *pcm;

	TASK_CHECK(c != NULL);
	ctx = (struct context_data *)c;
	p = &ctx->p;
	pcm = calloc(TEST_TICKS * 2000, sizeof(short));
	TASK_CHECK(pcm != NULL);

	/* Volume envelope with an early sustain point. */
	set_instrument_envelope(ctx, 0, 0, 0, 64);
	set_instrument_envelope(ctx, 0, 1, 10, 64);
	set_instrument_envelope_sus(ctx, 0, 1);

	tick = current_ticksize(c);
	got = read_frames(c, pcm, tick, 1);
	TASK_CHECK(got == tick);

	voc = map_channel(p, 0);
	TASK_CHECK(voc >= 0);
	xc = &p->xc_data[0];
	vi = &p->virt.voice_array[voc];
	TASK_CHECK((vi->flags & VOICE_RELEASE) == 0);

	at = tick + 9;
	TASK_CHECK(xmp_schedule_event(c, (unsigned long long)at, 0, &off) == 0);

	/* Render up to the frame before the event. The natural tick at
	 * "tick" has advanced the envelope normally; capture it now. */
	got = read_frames(c, pcm + tick, at - tick, 1);
	TASK_CHECK(got == at - tick);
	v_before = xc->v_idx;

	got = read_frames(c, pcm + at, 1, 1);
	TASK_CHECK(got == 1);

	/* Sample sustain release is applied immediately. */
	TASK_CHECK((vi->flags & VOICE_RELEASE) != 0);
	/* The envelope position has not advanced past the pre-event value. */
	TASK_CHECK(xc->v_idx == v_before);
	/* Fadeout has not started inside the tick. */
	TASK_CHECK((xc->note_flags & NOTE_FADEOUT) == 0 ||
		   xc->fadeout == 0x10000);
	/* Sound is continuous across the key off (no hard cut). */
	TASK_CHECK(pcm_energy(pcm + at - 2, 4) > 0);

	/* Finish the current tick, then let one more natural tick advance
	 * the envelope. */
	got = read_frames(c, pcm + at + 1, 2 * tick - at - 1, 1);
	TASK_CHECK(got == 2 * tick - at - 1);
	{
		int v_mid = xc->v_idx;
		got = read_frames(c, pcm + 2 * tick, tick, 1);
		TASK_CHECK(got == tick);
		TASK_CHECK(xc->v_idx >= v_mid);
	}

	free(pcm);
	close_ctx(c);
	return 0;
}

/* A cut decay must not be attributed to a note that reuses the mixer slot,
 * and the new note gets its own attack ramp. Looped samples keep rendering
 * across fragments. */
static int test_voices_cut_and_reuse(void)
{
	xmp_context c = make_timed_ctx(READ_EVENT_FT2, QUIRKS_FT2,
				       XMP_FORMAT_MONO);
	struct context_data *ctx;
	struct player_data *p;
	int tick, at_cut, at_on, ramp, got, voc;
	struct xmp_event cut = { XMP_KEY_CUT, 0, 0, 0, 0, 0, 0, 0 };
	struct xmp_event on = { 60, 1, 40, 0, 0, 0, 0, 0 };
	struct mixer_voice *vi;
	short *pcm;

	TASK_CHECK(c != NULL);
	ctx = (struct context_data *)c;
	p = &ctx->p;
	pcm = calloc(TEST_TICKS * 2000, sizeof(short));
	TASK_CHECK(pcm != NULL);

	tick = current_ticksize(c);
	ramp = tick / 8;
	got = read_frames(c, pcm, tick, 1);
	TASK_CHECK(got == tick);

	at_cut = tick + 7;
	at_on = at_cut + 2;	/* reuse while the decay is still running */
	TASK_CHECK(xmp_schedule_event(c, (unsigned long long)at_cut, 0,
				      &cut) == 0);
	TASK_CHECK(xmp_schedule_event(c, (unsigned long long)at_on, 0,
				      &on) == 0);

	/* Read through the cut, then stop exactly on the reuse frame so the
	 * freshly armed attack ramp can be inspected. */
	got = read_frames(c, pcm + tick, at_on - tick, 1);
	TASK_CHECK(got == at_on - tick);
	got = read_frames(c, pcm + at_on, 1, 1);
	TASK_CHECK(got == 1);

	voc = map_channel(p, 0);
	TASK_CHECK(voc >= 0);
	vi = &p->virt.voice_array[voc];
	/* The slot hosts the new note, not the decaying old one. */
	TASK_CHECK(vi->note == 59);
	TASK_CHECK(vi->ins == 0);
	/* The new note got its own eighth-tick attack ramp starting from
	 * zero volume, independent of the cut decay. */
	TASK_CHECK(vi->ramp_remain > 0);
	TASK_CHECK(vi->ramp_remain <= ramp);
	TASK_CHECK(vi->old_vl >= 0);

	/* Finish the tick; the attack region is audible on top of the
	 * decaying residual. */
	got = read_frames(c, pcm + at_on + 1, 2 * tick - at_on - 1, 1);
	TASK_CHECK(got == 2 * tick - at_on - 1);
	TASK_CHECK(pcm_energy(pcm + at_on + 1, ramp) > 0);
	/* The looped sample is still positioned inside its loop region. */
	TASK_CHECK(vi->smp >= 0);

	/* Render several more ticks: looped output continues, no crash. */
	got = read_frames(c, pcm + 2 * tick, 3 * tick, 1);
	TASK_CHECK(got == 3 * tick);
	TASK_CHECK(pcm_energy(pcm + 2 * tick, 3 * tick) > 0);

	free(pcm);
	close_ctx(c);
	return 0;
}

/*
 * stream group
 */

#define STREAM_FRAMES	8000

/* Render one context in one large call, another in alternating 1/17/257
 * frame reads, with identical scheduled events, and compare PCM. */
static int render_compare(int read_mode, int quirk, int format)
{
	xmp_context big = xmp_create_context();
	xmp_context fr = xmp_create_context();
	struct context_data *cb = (struct context_data *)big;
	struct context_data *cf = (struct context_data *)fr;
	int chn = (format & XMP_FORMAT_MONO) ? 1 : 2;
	int sizes[] = { 1, 17, 257 };
	int si = 0, got = 0, r;
	short *pb, *pf;
	struct xmp_event on1 = { 45, 1, 35, 0, 0, 0, 0, 0 };
	struct xmp_event off = { XMP_KEY_OFF, 0, 0, 0, 0, 0, 0, 0 };
	struct xmp_event cut = { XMP_KEY_CUT, 0, 0, 0, 0, 0, 0, 0 };
	struct xmp_event on2 = { 80, 2, 55, 0, 0, 0, 0, 0 };
	static const unsigned long long when[] = {
		300, 1201, 2600, 4100, 5555, 7000
	};

	create_simple_module(cb, 2, 2);
	create_simple_module(cf, 2, 2);
	set_quirk(cb, quirk, read_mode);
	set_quirk(cf, quirk, read_mode);
	fill_samples(cb);
	fill_samples(cf);
	new_event(cb, 0, 0, 0, 60, 1, 40, 0, 0, 0, 0);
	new_event(cf, 0, 0, 0, 60, 1, 40, 0, 0, 0, 0);

	TASK_CHECK(xmp_start_timed_player(big, TEST_RATE, format) == 0);
	TASK_CHECK(xmp_start_timed_player(fr, TEST_RATE, format) == 0);

	TASK_CHECK(xmp_schedule_event(big, when[0], 0, &on1) == 0);
	TASK_CHECK(xmp_schedule_event(fr, when[0], 0, &on1) == 0);
	TASK_CHECK(xmp_schedule_event(big, when[1], 0, &off) == 0);
	TASK_CHECK(xmp_schedule_event(fr, when[1], 0, &off) == 0);
	TASK_CHECK(xmp_schedule_event(big, when[2], 1, &on2) == 0);
	TASK_CHECK(xmp_schedule_event(fr, when[2], 1, &on2) == 0);
	TASK_CHECK(xmp_schedule_event(big, when[3], 0, &cut) == 0);
	TASK_CHECK(xmp_schedule_event(fr, when[3], 0, &cut) == 0);
	TASK_CHECK(xmp_schedule_event(big, when[4], 0, &on1) == 0);
	TASK_CHECK(xmp_schedule_event(fr, when[4], 0, &on1) == 0);
	TASK_CHECK(xmp_schedule_event(big, when[5], 1, &cut) == 0);
	TASK_CHECK(xmp_schedule_event(fr, when[5], 1, &cut) == 0);

	pb = calloc(STREAM_FRAMES * chn, sizeof(short));
	pf = calloc(STREAM_FRAMES * chn, sizeof(short));
	TASK_CHECK(pb && pf);

	got = read_frames(big, pb, STREAM_FRAMES, chn);
	TASK_CHECK(got == STREAM_FRAMES);

	got = 0;
	while (got < STREAM_FRAMES) {
		int want = sizes[si % 3];
		si++;
		if (want > STREAM_FRAMES - got)
			want = STREAM_FRAMES - got;
		r = xmp_play_timed_buffer(fr, pf + got * chn, want);
		TASK_CHECK(r == want);
		got += r;
	}

	TASK_CHECK(memcmp(pb, pf,
		STREAM_FRAMES * chn * sizeof(short)) == 0);

	free(pb);
	free(pf);
	close_ctx(big);
	close_ctx(fr);
	return 0;
}

static int test_stream_large_vs_fragmented(void)
{
	TASK_CHECK(render_compare(READ_EVENT_FT2, QUIRKS_FT2,
				  XMP_FORMAT_MONO) == 0);
	TASK_CHECK(render_compare(READ_EVENT_IT, QUIRKS_IT, 0) == 0);
	return 0;
}

/* With no external events the timed path must produce byte-identical PCM to
 * the legacy xmp_play_buffer path. */
static int test_stream_matches_legacy(void)
{
	xmp_context legacy = xmp_create_context();
	xmp_context timed = xmp_create_context();
	struct context_data *cl = (struct context_data *)legacy;
	struct context_data *ct = (struct context_data *)timed;
	int frames = 9000;
	short *pl, *pt;

	create_simple_module(cl, 2, 2);
	create_simple_module(ct, 2, 2);
	set_quirk(cl, QUIRKS_IT, READ_EVENT_IT);
	set_quirk(ct, QUIRKS_IT, READ_EVENT_IT);
	fill_samples(cl);
	fill_samples(ct);
	new_event(cl, 0, 0, 0, 60, 1, 40, 0, 0, 0, 0);
	new_event(ct, 0, 0, 0, 60, 1, 40, 0, 0, 0, 0);

	TASK_CHECK(xmp_start_player(legacy, TEST_RATE, 0) == 0);
	TASK_CHECK(xmp_start_timed_player(timed, TEST_RATE, 0) == 0);

	pl = calloc(frames * 2, sizeof(short));
	pt = calloc(frames * 2, sizeof(short));
	TASK_CHECK(pl && pt);

	TASK_CHECK(xmp_play_buffer(legacy, pl, frames * 2 * 2, 0) == 0);
	TASK_CHECK(read_frames(timed, pt, frames, 2) == frames);
	TASK_CHECK(memcmp(pl, pt, frames * 2 * sizeof(short)) == 0);

	free(pl);
	free(pt);
	xmp_end_player(legacy);
	close_ctx(timed);
	xmp_release_module(legacy);
	xmp_free_context(legacy);
	return 0;
}

/* Once the song ends, reads return zero and leave the output and the live
 * position untouched; an event at the current position is still accepted. */
static int test_stream_after_end(void)
{
	xmp_context c = xmp_create_context();
	struct context_data *ctx = (struct context_data *)c;
	struct xmp_module *mod;
	short pcm[64];
	unsigned long long pos;
	int r;
	struct xmp_event on = { 50, 1, 40, 0, 0, 0, 0, 0 };

	create_simple_module(ctx, 1, 1);
	set_quirk(ctx, QUIRKS_IT, READ_EVENT_IT);
	mod = &ctx->m.mod;
	/* No valid order: the player reaches the end immediately. */
	mod->len = 1;
	mod->xxo[0] = 0xff;

	TASK_CHECK(xmp_start_timed_player(c, TEST_RATE, XMP_FORMAT_MONO) == 0);

	memset(pcm, 0x7f, sizeof(pcm));
	r = xmp_play_timed_buffer(c, pcm, 64);
	TASK_CHECK(r == 0);
	TASK_CHECK(pcm_energy(pcm, 64) == 0);	/* filled with silence */

	TASK_CHECK(xmp_get_timed_position(c, &pos) == 0);
	TASK_CHECK(pos == 0);

	/* An event at the current position is accepted and retained. */
	TASK_CHECK(xmp_schedule_event(c, 0, 0, &on) == 0);
	/* Further reads still produce nothing and advance nothing. */
	TASK_CHECK(xmp_play_timed_buffer(c, pcm, 16) == 0);
	TASK_CHECK(xmp_get_timed_position(c, &pos) == 0);
	TASK_CHECK(pos == 0);

	close_ctx(c);
	return 0;
}

/* Rejected events change neither the queue nor the rendered sound. */
static int test_stream_rejection_keeps_pcm(void)
{
	xmp_context ref = xmp_create_context();
	xmp_context bad = xmp_create_context();
	struct context_data *cr = (struct context_data *)ref;
	struct context_data *cb = (struct context_data *)bad;
	int frames = 5000;
	short *pr, *pb;
	struct xmp_event good = { 50, 1, 40, 0, 0, 0, 0, 0 };
	struct xmp_event e_note = { 0, 1, 40, 0, 0, 0, 0, 0 };
	struct xmp_event e_ins = { 50, 99, 40, 0, 0, 0, 0, 0 };
	struct xmp_event e_vol = { 50, 1, 66, 0, 0, 0, 0, 0 };
	struct xmp_event e_fx = { 50, 1, 40, 1, 0, 0, 0, 0 };
	struct xmp_event e_off = { XMP_KEY_OFF, 1, 0, 0, 0, 0, 0, 0 };
	int i;

	create_simple_module(cr, 2, 2);
	create_simple_module(cb, 2, 2);
	set_quirk(cr, QUIRKS_FT2, READ_EVENT_FT2);
	set_quirk(cb, QUIRKS_FT2, READ_EVENT_FT2);
	fill_samples(cr);
	fill_samples(cb);

	TASK_CHECK(xmp_start_timed_player(ref, TEST_RATE, XMP_FORMAT_MONO) == 0);
	TASK_CHECK(xmp_start_timed_player(bad, TEST_RATE, XMP_FORMAT_MONO) == 0);

	/* One valid far-future event on both, then only "bad" gets rejects. */
	TASK_CHECK(xmp_schedule_event(ref, 100000, 0, &good) == 0);
	TASK_CHECK(xmp_schedule_event(bad, 100000, 0, &good) == 0);

	TASK_CHECK(xmp_schedule_event(bad, 50, 99, &good) == -XMP_ERROR_INVALID);
	TASK_CHECK(xmp_schedule_event(bad, 50, 0, &e_note) == -XMP_ERROR_INVALID);
	TASK_CHECK(xmp_schedule_event(bad, 50, 0, &e_ins) == -XMP_ERROR_INVALID);
	TASK_CHECK(xmp_schedule_event(bad, 50, 0, &e_vol) == -XMP_ERROR_INVALID);
	TASK_CHECK(xmp_schedule_event(bad, 50, 0, &e_fx) == -XMP_ERROR_INVALID);
	TASK_CHECK(xmp_schedule_event(bad, 50, 0, &e_off) == -XMP_ERROR_INVALID);
	TASK_CHECK(xmp_schedule_event(bad, 0, 0, NULL) == -XMP_ERROR_INVALID);

	/* Fill the queue, then confirm overflow rejects the whole event. */
	for (i = 0; i < XMP_TIMED_QUEUE_SIZE - 1; i++) {
		TASK_CHECK(xmp_schedule_event(bad, 200000 + i, 1,
					      &good) == 0);
	}
	TASK_CHECK(xmp_schedule_event(bad, 50, 0, &good) == -XMP_ERROR_INVALID);

	pr = calloc(frames, sizeof(short));
	pb = calloc(frames, sizeof(short));
	TASK_CHECK(pr && pb);
	TASK_CHECK(read_frames(ref, pr, frames, 1) == frames);
	TASK_CHECK(read_frames(bad, pb, frames, 1) == frames);
	TASK_CHECK(memcmp(pr, pb, frames * sizeof(short)) == 0);

	free(pr);
	free(pb);
	close_ctx(ref);
	close_ctx(bad);
	return 0;
}

/* Two contexts are independent; a timed restart clears the queue and resets
 * the sample position to zero. */
static int test_stream_contexts_and_restart(void)
{
	xmp_context a = xmp_create_context();
	xmp_context b = xmp_create_context();
	struct context_data *ca = (struct context_data *)a;
	struct context_data *cb = (struct context_data *)b;
	short pa[2000], pb[2000];
	unsigned long long pos;
	struct xmp_event on_a = { 40, 1, 60, 0, 0, 0, 0, 0 };
	struct xmp_event on_b = { 90, 2, 10, 0, 0, 0, 0, 0 };

	create_simple_module(ca, 2, 2);
	create_simple_module(cb, 2, 2);
	set_quirk(ca, QUIRKS_IT, READ_EVENT_IT);
	set_quirk(cb, QUIRKS_IT, READ_EVENT_IT);
	fill_samples(ca);
	fill_samples(cb);

	TASK_CHECK(xmp_start_timed_player(a, TEST_RATE, XMP_FORMAT_MONO) == 0);
	TASK_CHECK(xmp_start_timed_player(b, TEST_RATE, XMP_FORMAT_MONO) == 0);

	TASK_CHECK(xmp_schedule_event(a, 500, 0, &on_a) == 0);
	TASK_CHECK(xmp_schedule_event(b, 500, 0, &on_b) == 0);

	TASK_CHECK(read_frames(a, pa, 2000, 1) == 2000);
	TASK_CHECK(read_frames(b, pb, 2000, 1) == 2000);
	/* Different events make the two contexts audibly different. */
	TASK_CHECK(memcmp(pa, pb, sizeof(pa)) != 0);

	/* Restart A: position resets and the old queue is gone. */
	TASK_CHECK(xmp_start_timed_player(a, TEST_RATE,
					  XMP_FORMAT_MONO) == 0);
	TASK_CHECK(xmp_get_timed_position(a, &pos) == 0);
	TASK_CHECK(pos == 0);
	/* A timestamp before the previously queued 500 would have been
	 * rejected before the restart; now it is accepted. */
	TASK_CHECK(xmp_schedule_event(a, 100, 0, &on_a) == 0);
	/* B kept its own state and position. */
	TASK_CHECK(xmp_get_timed_position(b, &pos) == 0);
	TASK_CHECK(pos == 2000);

	close_ctx(a);
	close_ctx(b);
	return 0;
}

/* Parameter and state error conventions. */
static int test_stream_parameter_errors(void)
{
	xmp_context c = xmp_create_context();
	xmp_context empty = xmp_create_context();
	struct context_data *ctx = (struct context_data *)c;
	short pcm[128];
	unsigned long long pos;
	struct xmp_event on = { 50, 1, 40, 0, 0, 0, 0, 0 };

	create_simple_module(ctx, 2, 2);	/* defaults to READ_EVENT_MOD */

	/* Only XM and IT are accepted. */
	TASK_CHECK(xmp_start_timed_player(c, TEST_RATE,
			XMP_FORMAT_MONO) == -XMP_ERROR_FORMAT);

	set_quirk(ctx, QUIRKS_FT2, READ_EVENT_FT2);
	/* Plain C software mixer, signed 16-bit mono/stereo only. */
	TASK_CHECK(xmp_start_timed_player(c, TEST_RATE,
			XMP_FORMAT_8BIT) == -XMP_ERROR_INVALID);
	TASK_CHECK(xmp_start_timed_player(c, TEST_RATE,
			XMP_FORMAT_32BIT) == -XMP_ERROR_INVALID);
	TASK_CHECK(xmp_start_timed_player(c, TEST_RATE,
			XMP_FORMAT_UNSIGNED) == -XMP_ERROR_INVALID);
	TASK_CHECK(xmp_start_timed_player(c, 100,
			XMP_FORMAT_MONO) == -XMP_ERROR_INVALID);
	/* Nothing loaded yet. */
	TASK_CHECK(xmp_start_timed_player(empty, TEST_RATE,
			XMP_FORMAT_MONO) == -XMP_ERROR_STATE);

	TASK_CHECK(xmp_start_timed_player(c, TEST_RATE,
			XMP_FORMAT_MONO) == 0);

	/* Buffer argument rules. */
	TASK_CHECK(xmp_play_timed_buffer(c, pcm, -1) == -XMP_ERROR_INVALID);
	TASK_CHECK(xmp_play_timed_buffer(c, NULL, 16) == -XMP_ERROR_INVALID);
	TASK_CHECK(xmp_play_timed_buffer(c, NULL, 0) == 0);
	/* Frame count to byte count overflow. */
	TASK_CHECK(xmp_play_timed_buffer(c, pcm, INT_MAX)
		   == -XMP_ERROR_INVALID);

	TASK_CHECK(xmp_get_timed_position(c, NULL) == -XMP_ERROR_INVALID);
	TASK_CHECK(xmp_schedule_event(c, 0, 0, &on) == 0);

	/* State errors on a context that never started the timed player. */
	TASK_CHECK(xmp_play_timed_buffer(empty, pcm, 16)
		   == -XMP_ERROR_STATE);
	TASK_CHECK(xmp_schedule_event(empty, 0, 0, &on)
		   == -XMP_ERROR_STATE);
	TASK_CHECK(xmp_get_timed_position(empty, &pos)
		   == -XMP_ERROR_STATE);

	close_ctx(c);
	xmp_free_context(empty);
	return 0;
}

/* Interpolation is selected after start and before the first read, and the
 * three modes all render identically across read sizes. */
static int test_stream_interpolation(void)
{
	static const int interps[] = {
		XMP_INTERP_NEAREST, XMP_INTERP_LINEAR, XMP_INTERP_SPLINE
	};
	int i;

	for (i = 0; i < 3; i++) {
		xmp_context big = xmp_create_context();
		xmp_context fr = xmp_create_context();
		struct context_data *cb = (struct context_data *)big;
		struct context_data *cf = (struct context_data *)fr;
		int frames = 4000, got = 0, k = 0;
		short *pb, *pf;
		struct xmp_event on = { 55, 1, 40, 0, 0, 0, 0, 0 };
		int sizes[] = { 1, 17, 257 };

		create_simple_module(cb, 2, 2);
		create_simple_module(cf, 2, 2);
		set_quirk(cb, QUIRKS_IT, READ_EVENT_IT);
		set_quirk(cf, QUIRKS_IT, READ_EVENT_IT);
		fill_samples(cb);
		fill_samples(cf);

		TASK_CHECK(xmp_start_timed_player(big, TEST_RATE,
				XMP_FORMAT_MONO) == 0);
		TASK_CHECK(xmp_start_timed_player(fr, TEST_RATE,
				XMP_FORMAT_MONO) == 0);
		TASK_CHECK(xmp_set_player(big, XMP_PLAYER_INTERP,
				interps[i]) == 0);
		TASK_CHECK(xmp_set_player(fr, XMP_PLAYER_INTERP,
				interps[i]) == 0);
		TASK_CHECK(xmp_schedule_event(big, 777, 0, &on) == 0);
		TASK_CHECK(xmp_schedule_event(fr, 777, 0, &on) == 0);

		pb = calloc(frames, sizeof(short));
		pf = calloc(frames, sizeof(short));
		TASK_CHECK(pb && pf);
		TASK_CHECK(read_frames(big, pb, frames, 1) == frames);
		while (got < frames) {
			int want = sizes[k % 3];
			k++;
			if (want > frames - got)
				want = frames - got;
			TASK_CHECK(xmp_play_timed_buffer(fr, pf + got,
					want) == want);
			got += want;
		}
		TASK_CHECK(memcmp(pb, pf, frames * sizeof(short)) == 0);
		TASK_CHECK(pcm_energy(pb, frames) > 0);

		free(pb);
		free(pf);
		close_ctx(big);
		close_ctx(fr);
	}

	return 0;
}

const struct task_case task_cases[] = {
	{ "timeline", "onset_phase",		test_timeline_onset_phase },
	{ "timeline", "cut_decay",		test_timeline_cut_decay },
	{ "timeline", "ordering",		test_timeline_ordering },
	{ "timeline", "position",		test_timeline_position },
	{ "voices",   "it_nna",			test_voices_nna },
	{ "voices",   "keyoff_sustain",	test_voices_keyoff_sustain },
	{ "voices",   "cut_and_reuse",		test_voices_cut_and_reuse },
	{ "stream",   "large_vs_fragmented",	test_stream_large_vs_fragmented },
	{ "stream",   "matches_legacy",	test_stream_matches_legacy },
	{ "stream",   "after_end",		test_stream_after_end },
	{ "stream",   "rejection_keeps_pcm",	test_stream_rejection_keeps_pcm },
	{ "stream",   "contexts_and_restart",	test_stream_contexts_and_restart },
	{ "stream",   "parameter_errors",	test_stream_parameter_errors },
	{ "stream",   "interpolation",		test_stream_interpolation },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);

/* Extended Module Player
 * Copyright (C) 1996-2026 Claudio Matsuoka and Hipolito Carraro Jr
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/*
 * Sample-accurate timed playback path.
 *
 * The song keeps advancing at natural tick boundaries through the regular
 * player update; scheduled events are dispatched between the player update
 * and the mixer, and the mixer output is produced in fragments so note
 * onsets, key offs and note cuts can land on any output sample frame.
 */

#include "common.h"
#include "player.h"
#include "mixer.h"
#include "virtual.h"

/* Apply a validated scheduled event. Never reads a pattern row and never
 * touches the other channels or the song clock. */
void libxmp_timed_dispatch_event(struct context_data *ctx, int chn,
				 const struct xmp_event *e)
{
	struct player_data *p = &ctx->p;
	struct module_data *m = &ctx->m;
	struct xmp_event ev;
	int voc, save_frame;

	memset(&ev, 0, sizeof(ev));
	ev.note = e->note;
	ev.ins = e->ins;
	ev.vol = e->vol;

	if (e->note == XMP_KEY_CUT) {
		voc = libxmp_virt_mapchannel(ctx, chn);

		/* Capture the residual before the cut logic resets the
		 * channel, then run the regular cut semantics. */
		if (voc >= 0)
			libxmp_mixer_timed_start_decay(ctx, voc);

		if (IS_PLAYER_MODE_IT()) {
			libxmp_read_event(ctx, &ev, chn);
		} else {
			libxmp_virt_resetchannel(ctx, chn);
		}
		return;
	}

	/* FT2 ignores row events while a pattern delay extends the row;
	 * scheduled events are not row events, so bypass that guard while
	 * keeping the rest of the regular event logic. */
	save_frame = p->frame;
	if (IS_PLAYER_MODE_FT2() && p->frame >= p->speed)
		p->frame = 0;

	if (e->note == XMP_KEY_OFF) {
		libxmp_read_event(ctx, &ev, chn);
	} else {
		/* Note on: the regular event logic handles NNA, duplicate
		 * checks, instrument setup and voice ownership. */
		libxmp_read_event(ctx, e, chn);
	}

	p->frame = save_frame;

	/* Publish pitch, volume and pan without advancing envelopes, LFOs,
	 * slide/delay counters or any other channel's state. */
	libxmp_timed_publish(ctx, chn);

	if (e->note != XMP_KEY_OFF) {
		voc = libxmp_virt_mapchannel(ctx, chn);
		if (voc >= 0)
			libxmp_mixer_timed_arm_ramp(ctx, voc);
	}
}

static int timed_format_ok(int format)
{
	/* Signed 16-bit, mono or stereo, plain software mixer output. */
	if (format & (XMP_FORMAT_8BIT | XMP_FORMAT_32BIT | XMP_FORMAT_UNSIGNED))
		return 0;

	return 1;
}

static void timed_clear_queue(struct context_data *ctx)
{
	struct timed_player_data *t = &ctx->t;

	t->count = 0;
	t->seq = 0;
}

static void timed_reset(struct context_data *ctx)
{
	struct timed_player_data *t = &ctx->t;

	timed_clear_queue(ctx);
	t->position = 0;
	t->ended = 0;
	t->tick_active = 0;
	t->tick_off = 0;
	t->tick_total = 0;
	ctx->s.timed_mode = 1;
	libxmp_mixer_timed_reset(ctx);
}

int xmp_start_timed_player(xmp_context opaque, int rate, int format)
{
	struct context_data *ctx = (struct context_data *)opaque;
	struct module_data *m = &ctx->m;
	struct player_data *p = &ctx->p;
	struct timed_player_data *t = &ctx->t;
	int ret;

	if (rate < XMP_MIN_SRATE || rate > XMP_MAX_SRATE)
		return -XMP_ERROR_INVALID;

	if (ctx->state < XMP_STATE_LOADED)
		return -XMP_ERROR_STATE;

	if (!timed_format_ok(format))
		return -XMP_ERROR_INVALID;

	if (m->read_event_type != READ_EVENT_FT2 &&
	    m->read_event_type != READ_EVENT_IT)
		return -XMP_ERROR_FORMAT;

	/* Reusing xmp_start_player keeps all allocation and player setup
	 * identical to the regular path. */
	ret = xmp_start_player(opaque, rate, format);
	if (ret < 0)
		return ret;

	free(t->queue);
	free(t->decay);
	t->queue = NULL;
	t->decay = NULL;

	t->queue = (struct xmp_timed_event *)
		malloc(XMP_TIMED_QUEUE_SIZE * sizeof(struct xmp_timed_event));
	if (t->queue == NULL)
		goto err;

	t->decay = (struct timed_decay *)
		calloc(p->virt.maxvoc, sizeof(struct timed_decay));
	if (t->decay == NULL)
		goto err;

	t->active = 1;
	timed_reset(ctx);

	return 0;

    err:
	free(t->queue);
	free(t->decay);
	t->queue = NULL;
	t->decay = NULL;
	t->active = 0;
	xmp_end_player(opaque);
	return -XMP_ERROR_SYSTEM;
}

static int timed_event_valid(struct context_data *ctx, int channel,
			     const struct xmp_event *e)
{
	struct module_data *m = &ctx->m;
	struct xmp_module *mod = &m->mod;

	if (e == NULL)
		return 0;

	if ((uint32)channel >= (uint32)mod->chn)
		return 0;

	if (e->fxt || e->fxp || e->f2t || e->f2p || e->_flag)
		return 0;

	if (e->note == XMP_KEY_OFF || e->note == XMP_KEY_CUT)
		return e->ins == 0 && e->vol == 0;

	/* Note on */
	if (e->note < 1 || e->note > 120)
		return 0;

	if (e->ins < 1 || (uint32)e->ins > (uint32)mod->ins)
		return 0;

	if (!IS_VALID_INSTRUMENT(e->ins - 1))
		return 0;

	if (e->vol > 65)
		return 0;

	return 1;
}

int xmp_schedule_event(xmp_context opaque, unsigned long long at, int channel,
		       const struct xmp_event *event)
{
	struct context_data *ctx = (struct context_data *)opaque;
	struct timed_player_data *t = &ctx->t;
	int slot;

	if (ctx->state < XMP_STATE_PLAYING || !t->active)
		return -XMP_ERROR_STATE;

	if (!timed_event_valid(ctx, channel, event))
		return -XMP_ERROR_INVALID;

	if (at < t->position)
		return -XMP_ERROR_INVALID;

	if (t->count >= XMP_TIMED_QUEUE_SIZE)
		return -XMP_ERROR_INVALID;

	/* Sorted by (at, submission sequence); the queue stays small and an
	 * insertion sort keeps equal timestamps in submission order. */
	slot = t->count;
	while (slot > 0 && t->queue[slot - 1].at > at)
		slot--;

	memmove(&t->queue[slot + 1], &t->queue[slot],
		(size_t)(t->count - slot) * sizeof(struct xmp_timed_event));

	t->queue[slot].at = at;
	t->queue[slot].seq = t->seq++;
	t->queue[slot].channel = channel;
	memcpy(&t->queue[slot].event, event, sizeof(struct xmp_event));
	t->count++;

	return 0;
}

int xmp_get_timed_position(xmp_context opaque, unsigned long long *position)
{
	struct context_data *ctx = (struct context_data *)opaque;
	struct timed_player_data *t = &ctx->t;

	if (position == NULL)
		return -XMP_ERROR_INVALID;

	if (ctx->state < XMP_STATE_PLAYING || !t->active)
		return -XMP_ERROR_STATE;

	*position = t->position;
	return 0;
}

/* Run the natural tick update and prepare the fragment render. */
static int timed_start_tick(struct context_data *ctx)
{
	struct timed_player_data *t = &ctx->t;
	int ret;

	ret = play_frame_core(ctx, 0, 0);
	if (ret < 0) {
		t->ended = 1;
		t->tick_active = 0;
		return -1;
	}

	libxmp_mixer_timed_begin_tick(ctx);
	t->tick_total = ctx->s.ticksize;
	t->tick_off = 0;
	t->tick_active = 1;

	return 0;
}

/* Dispatch every event due at absolute frame "now", in queue order. */
static void timed_dispatch_due(struct context_data *ctx,
			       unsigned long long now)
{
	struct timed_player_data *t = &ctx->t;
	int head = 0;

	while (head < t->count && t->queue[head].at == now) {
		struct xmp_timed_event *te = &t->queue[head];

		libxmp_timed_dispatch_event(ctx, te->channel, &te->event);
		head++;
	}

	if (head > 0) {
		t->count -= head;
		memmove(t->queue, &t->queue[head],
			(size_t)t->count * sizeof(struct xmp_timed_event));
	}
}

int xmp_play_timed_buffer(xmp_context opaque, void *out, int frames)
{
	struct context_data *ctx = (struct context_data *)opaque;
	struct timed_player_data *t = &ctx->t;
	struct mixer_data *s = &ctx->s;
	int16 *dst = (int16 *)out;
	int produced = 0;
	int chn;

	/* Legal no-op: no events processed, no output touched. */
	if (frames == 0 && out == NULL)
		return 0;

	if (frames < 0)
		return -XMP_ERROR_INVALID;

	if (frames > 0 && out == NULL)
		return -XMP_ERROR_INVALID;

	if (ctx->state < XMP_STATE_PLAYING || !t->active)
		return -XMP_ERROR_STATE;

	chn = s->output_chn;
	if (frames > INT_MAX / (chn * (int)sizeof(int16)))
		return -XMP_ERROR_INVALID;

	/* After the end, further calls produce nothing and touch nothing. */
	if (t->ended)
		return 0;

	while (produced < frames) {
		unsigned long long now = t->position + (unsigned long long)produced;
		int chunk;

		if (!t->tick_active) {
			if (timed_start_tick(ctx) < 0)
				break;
		}

		/* Events landing exactly on this frame take effect before
		 * this sample is rendered. */
		timed_dispatch_due(ctx, now);

		chunk = frames - produced;
		if (chunk > t->tick_total - t->tick_off)
			chunk = t->tick_total - t->tick_off;

		/* Split before the next scheduled position so its events can
		 * be applied between two fragments. */
		if (t->count > 0 &&
		    t->queue[0].at - now < (unsigned long long)chunk) {
			chunk = (int)(t->queue[0].at - now);
		}

		if (chunk > 0) {
			libxmp_mixer_timed_render(ctx, t->tick_off, chunk);
			libxmp_mixer_timed_downmix(ctx,
				dst + (size_t)produced * chn,
				t->tick_off, chunk);

			produced += chunk;
			t->tick_off += chunk;

			if (t->tick_off >= t->tick_total) {
				libxmp_mixer_timed_finish_tick(ctx);
				t->tick_active = 0;
			}
		}
	}

	if (produced < frames) {
		/* Song ended mid-request: finish with silence and short
		 * return, like the regular buffer path. */
		memset(dst + (size_t)produced * chn, 0,
		       (size_t)(frames - produced) * chn * sizeof(int16));
	}

	t->position += (unsigned long long)produced;
	return produced;
}

/* Called from xmp_end_player: drop every timed resource. */
void libxmp_timed_end(struct context_data *ctx)
{
	struct timed_player_data *t = &ctx->t;

	free(t->queue);
	free(t->decay);
	t->queue = NULL;
	t->decay = NULL;
	t->active = 0;
	t->count = 0;
	t->tick_active = 0;
}

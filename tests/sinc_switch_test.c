/*
** Copyright (c) 2002-2021, Erik de Castro Lopo <erikd@mega-nerd.com>
** All rights reserved.
**
** This code is released under 2-clause BSD license. Please see the
** file at : https://github.com/libsndfile/libsamplerate/blob/master/COPYING
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <samplerate.h>

#include "../task_support/check.h"

/*
** Verifies the optional seamless sinc quality switching implemented by
** src_prepare_sinc_switch() and src_set_sinc_quality(). Crossfade results are
** checked against ordinary converter instances that run the old and the new
** quality over the whole stream, evaluated at the same source coordinates.
*/

#define SINC_QUALITIES		3
#define	MAX_BIG_FRAMES		140000
#define	MAX_SMALL_FRAMES	32768
#define	MAX_SMALL_IN		24000
#define	MAX_CHAN			3

#define	BLEND_TOL			2e-6

#define LOCAL_ARRAY_LEN(x)	((int) (sizeof (x) / sizeof ((x) [0])))

static float big_in [MAX_BIG_FRAMES] ;
static float big_ref_old [MAX_BIG_FRAMES] ;
static float big_ref_new [MAX_BIG_FRAMES] ;
static float big_sw [MAX_BIG_FRAMES] ;

static float sm_in [MAX_SMALL_IN * MAX_CHAN] ;
static float sm_ref_old [MAX_SMALL_FRAMES * MAX_CHAN] ;
static float sm_ref_new [MAX_SMALL_FRAMES * MAX_CHAN] ;
static float sm_sw [MAX_SMALL_FRAMES * MAX_CHAN] ;
static float sm_sw2 [MAX_SMALL_FRAMES * MAX_CHAN] ;

typedef struct
{	long			at ;			/* global output frame at which to arm the switch */
	int				quality ;		/* target quality */
	long			fade ;			/* fade length in frames */
	int				fired ;
	long			start ;			/* global frame where the fade actually started */
} SWITCH_PLAN ;

static void
gen_signal (float *buf, long frames, int channels)
{
	for (long f = 0 ; f < frames ; f++)
	{	double v = 0.45 * sin (0.0713 * f + 0.3)
				+ 0.25 * cos (0.0197 * f + 1.1)
				+ 0.12 * sin (0.231 * f) ;
		/* Deterministic broadband component. */
		unsigned int r = (unsigned int) (f * 2654435761u + 12345u) ;
		v += 0.08 * (((r >> 16) & 0xffff) / 32768.0 - 1.0) ;
		for (int ch = 0 ; ch < channels ; ch++)
			buf [f * channels + ch] = (float) (v * (1.0 - 0.25 * ch) + 0.05 * ch * sin (0.11 * f + ch)) ;
	} ;
} /* gen_signal */

/*
** Drive one converter through the whole input in fixed size process chunks.
** When prepared != 0 the converter is expected to have been prepared already.
** Returns the number of output frames generated and writes the consumed input
** count through *input_used.
*/
static long
run_process (SRC_STATE *state, int channels, double ratio, const float *in, long in_frames,
			long in_chunk, long out_cap, SWITCH_PLAN *plan, float *out, long *input_used)
{
	long current_in = 0, current_out = 0, used_total = 0 ;

	while (1)
	{	SRC_DATA d ;
		long this_in = in_frames - current_in ;
		long this_out ;

		if (this_in > in_chunk)
			this_in = in_chunk ;
		this_out = out_cap ;

		/* Arm the switch at an exact output frame, independent of chunking. */
		if (plan != NULL && ! plan->fired && current_out < plan->at)
		{	long room = plan->at - current_out ;
			if (this_out > room)
				this_out = room ;
		} ;

		memset (&d, 0, sizeof (d)) ;
		d.data_in = in + current_in * channels ;
		d.input_frames = this_in ;
		d.data_out = out + current_out * channels ;
		d.output_frames = this_out ;
		d.src_ratio = ratio ;
		d.end_of_input = (current_in + this_in >= in_frames) ? 1 : 0 ;

		if (plan != NULL && ! plan->fired && current_out >= plan->at)
		{	int err = src_set_sinc_quality (state, plan->quality, plan->fade) ;
			if (err != 0)
				return -1 ;
			plan->fired = 1 ;
			plan->start = current_out ;
		} ;

		if (src_process (state, &d) != 0)
			return -1 ;

		current_in += d.input_frames_used ;
		current_out += d.output_frames_gen ;
		used_total += d.input_frames_used ;

		if (d.end_of_input && d.output_frames_gen == 0)
			break ;
		if (current_out >= MAX_BIG_FRAMES || current_in > in_frames)
			return -1 ;
	} ;

	if (input_used != NULL)
		*input_used = used_total ;

	return current_out ;
} /* run_process */

static SRC_STATE *
make_ref (int quality, int channels, int prepare)
{
	int err = 0 ;
	SRC_STATE *s = src_new (quality, channels, &err) ;
	if (s == NULL || err != 0)
		return NULL ;
	if (prepare)
	{	if (src_prepare_sinc_switch (s) != 0)
		{	src_delete (s) ;
			return NULL ;
		} ;
	} ;
	return s ;
} /* make_ref */

/* Check a crossfaded stream against always-old and always-new references. */
static int
check_blend_output (const float *sw, const float *old, const float *new, int channels,
			long frames, long start, long fade)
{
	for (long f = 0 ; f < frames ; f++)
	{
		for (int ch = 0 ; ch < channels ; ch++)
		{
			long idx = f * channels + ch ;
			double expect ;

			if (f < start)
				expect = old [idx] ;
			else if (f < start + fade)
			{	long j = f - start + 1 ;
				double w_new = (double) j / (double) fade ;
				expect = old [idx] * (1.0 - w_new) + new [idx] * w_new ;
			}
			else
				expect = new [idx] ;

			if (fabs (sw [idx] - expect) > BLEND_TOL)
			{	fprintf (stderr, "blend mismatch frame=%ld ch=%d got=%.9f expect=%.9f\n",
							f, ch, sw [idx], expect) ;
				return 0 ;
			} ;
		} ;
	} ;

	return 1 ;
} /* check_blend_output */

/* Full crossfade check for one ordered quality pair and channel count. */
static int
blend_case (int old_q, int new_q, int channels, long fade, double ratio)
{
	const long in_frames = 6000 ;
	long in_used_o = 0, in_used_n = 0, in_used_s = 0 ;
	SRC_STATE *ro, *rn, *sw ;
	SWITCH_PLAN plan = { .at = 2200, .quality = new_q, .fade = fade } ;
	long fo, fn, fs ;

	gen_signal (sm_in, in_frames, channels) ;

	ro = make_ref (old_q, channels, 0) ;
	rn = make_ref (new_q, channels, 0) ;
	sw = make_ref (old_q, channels, 1) ;
	if (ro == NULL || rn == NULL || sw == NULL)
		return 0 ;

	fo = run_process (ro, channels, ratio, sm_in, in_frames, 137, 97, NULL, sm_ref_old, &in_used_o) ;
	fn = run_process (rn, channels, ratio, sm_in, in_frames, 137, 97, NULL, sm_ref_new, &in_used_n) ;
	fs = run_process (sw, channels, ratio, sm_in, in_frames, 137, 97, &plan, sm_sw, &in_used_s) ;

	src_delete (ro) ;
	src_delete (rn) ;
	src_delete (sw) ;

	if (fo <= 0 || fn <= 0 || fs <= 0)
		return 0 ;
	if (fo != fn || fn != fs)
	{	fprintf (stderr, "frame count mismatch old=%ld new=%ld sw=%ld\n", fo, fn, fs) ;
		return 0 ;
	} ;
	if (in_used_o != in_used_n || in_used_n != in_used_s)
	{	fprintf (stderr, "input used mismatch\n") ;
		return 0 ;
	} ;
	if (! plan.fired)
		return 0 ;

	return check_blend_output (sm_sw, sm_ref_old, sm_ref_new, channels, fs, plan.start, fade) ;
} /* blend_case */

static int
case_blend_fast_to_best (void)
{
	TASK_CHECK (blend_case (SRC_SINC_FASTEST, SRC_SINC_BEST_QUALITY, 2, 512, 0.75)) ;
	return 0 ;
}

static int
case_blend_best_to_fast (void)
{
	TASK_CHECK (blend_case (SRC_SINC_BEST_QUALITY, SRC_SINC_FASTEST, 2, 384, 1.5)) ;
	return 0 ;
}

static int
case_blend_medium_pairs (void)
{
	TASK_CHECK (blend_case (SRC_SINC_FASTEST, SRC_SINC_MEDIUM_QUALITY, 2, 256, 1.0)) ;
	TASK_CHECK (blend_case (SRC_SINC_MEDIUM_QUALITY, SRC_SINC_BEST_QUALITY, 2, 256, 1.25)) ;
	TASK_CHECK (blend_case (SRC_SINC_BEST_QUALITY, SRC_SINC_MEDIUM_QUALITY, 3, 300, 0.6)) ;
	TASK_CHECK (blend_case (SRC_SINC_MEDIUM_QUALITY, SRC_SINC_FASTEST, 3, 300, 1.4)) ;
	return 0 ;
}

/* fade_frames == 0: new quality applies from the very next output frame. */
static int
case_blend_zero_fade (void)
{
	const long in_frames = 5000 ;
	SRC_STATE *rn, *sw ;
	SWITCH_PLAN plan = { .at = 1800, .quality = SRC_SINC_BEST_QUALITY, .fade = 0 } ;
	long fn, fs ;

	gen_signal (sm_in, in_frames, 2) ;

	rn = make_ref (SRC_SINC_BEST_QUALITY, 2, 0) ;
	sw = make_ref (SRC_SINC_FASTEST, 2, 1) ;
	TASK_CHECK (rn != NULL && sw != NULL) ;

	fn = run_process (rn, 2, 0.9, sm_in, in_frames, 211, 88, NULL, sm_ref_new, NULL) ;
	fs = run_process (sw, 2, 0.9, sm_in, in_frames, 211, 88, &plan, sm_sw, NULL) ;

	src_delete (rn) ;
	src_delete (sw) ;

	TASK_CHECK (fn == fs && fn > 0) ;
	TASK_CHECK (plan.fired) ;

	/* From the switch frame onwards the output must equal the new reference. */
	for (long f = plan.start ; f < fs ; f++)
		for (int ch = 0 ; ch < 2 ; ch++)
			TASK_CHECK (fabs (sm_sw [f * 2 + ch] - sm_ref_new [f * 2 + ch]) <= BLEND_TOL) ;

	return 0 ;
} /* case_blend_zero_fade */

/* Asking for the current quality is a no-op and starts no transition. */
static int
case_blend_same_quality_noop (void)
{
	const long in_frames = 4000 ;
	SRC_STATE *ro, *sw ;
	long fo, fs ;

	gen_signal (sm_in, in_frames, 2) ;

	ro = make_ref (SRC_SINC_MEDIUM_QUALITY, 2, 0) ;
	sw = make_ref (SRC_SINC_MEDIUM_QUALITY, 2, 1) ;
	TASK_CHECK (ro != NULL && sw != NULL) ;

	TASK_CHECK (src_set_sinc_quality (sw, SRC_SINC_MEDIUM_QUALITY, 400) == 0) ;

	fo = run_process (ro, 2, 1.1, sm_in, in_frames, 150, 120, NULL, sm_ref_old, NULL) ;
	fs = run_process (sw, 2, 1.1, sm_in, in_frames, 150, 120, NULL, sm_sw, NULL) ;

	src_delete (ro) ;
	src_delete (sw) ;

	TASK_CHECK (fo == fs && fo > 0) ;
	for (long i = 0 ; i < fo * 2 ; i++)
		TASK_CHECK (sm_sw [i] == sm_ref_old [i]) ;

	return 0 ;
} /* case_blend_same_quality_noop */

/*--------------------------------------------------------------------------------------
** Streaming group.
*/

/* A long run on the narrow quality must not discard samples a later upgrade needs. */
static int
case_stream_deep_history (void)
{
	const long in_frames = 24000 ;
	SRC_STATE *ro, *rn, *sw ;
	SWITCH_PLAN plan = { .at = 9000, .quality = SRC_SINC_BEST_QUALITY, .fade = 700 } ;
	long fo, fn, fs ;

	gen_signal (sm_in, in_frames, 2) ;

	ro = make_ref (SRC_SINC_FASTEST, 2, 0) ;
	rn = make_ref (SRC_SINC_BEST_QUALITY, 2, 0) ;
	sw = make_ref (SRC_SINC_FASTEST, 2, 1) ;
	TASK_CHECK (ro != NULL && rn != NULL && sw != NULL) ;

	fo = run_process (ro, 2, 1.0, sm_in, in_frames, 257, 193, NULL, sm_ref_old, NULL) ;
	fn = run_process (rn, 2, 1.0, sm_in, in_frames, 257, 193, NULL, sm_ref_new, NULL) ;
	fs = run_process (sw, 2, 1.0, sm_in, in_frames, 257, 193, &plan, sm_sw, NULL) ;

	src_delete (ro) ;
	src_delete (rn) ;
	src_delete (sw) ;

	TASK_CHECK (fo == fn && fn == fs && fs > 0) ;
	TASK_CHECK (plan.fired) ;
	/* The long fast prefix must still contain every sample the wide window
	** needs: the blended new voice matches the full-stream best reference.
	*/
	TASK_CHECK (check_blend_output (sm_sw, sm_ref_old, sm_ref_new, 2, fs, plan.start, plan.fade)) ;

	return 0 ;
} /* case_stream_deep_history */

/* Fixed ratio, different chunking must give the same frames within 2e-6. */
static int
case_stream_async_chunks (void)
{
	const long in_frames = 7000 ;
	SRC_STATE *a, *b ;
	SWITCH_PLAN pa = { .at = 2600, .quality = SRC_SINC_MEDIUM_QUALITY, .fade = 480 } ;
	SWITCH_PLAN pb = { .at = 2600, .quality = SRC_SINC_MEDIUM_QUALITY, .fade = 480 } ;
	long fa, fb ;

	gen_signal (sm_in, in_frames, 2) ;

	a = make_ref (SRC_SINC_FASTEST, 2, 1) ;
	b = make_ref (SRC_SINC_FASTEST, 2, 1) ;
	TASK_CHECK (a != NULL && b != NULL) ;

	fa = run_process (a, 2, 0.83, sm_in, in_frames, 64, 53, &pa, sm_sw, NULL) ;
	fb = run_process (b, 2, 0.83, sm_in, in_frames, 333, 257, &pb, sm_sw2, NULL) ;

	src_delete (a) ;
	src_delete (b) ;

	TASK_CHECK (fa == fb && fa > 0) ;
	TASK_CHECK (pa.start == pb.start) ;

	for (long i = 0 ; i < fa * 2 ; i++)
	{	double diff = fabs (sm_sw [i] - sm_sw2 [i]) ;
		TASK_CHECK (diff <= 2e-6) ;
	} ;

	return 0 ;
} /* case_stream_async_chunks */

/* Ring buffer compaction/wrap on the prepared path stays identical to a plain run. */
static int
case_stream_ring_wrap (void)
{
	const long in_frames = 130000 ;
	SRC_STATE *plain, *prepared ;
	long fp, fs ;

	gen_signal (big_in, in_frames, 1) ;

	plain = make_ref (SRC_SINC_FASTEST, 1, 0) ;
	prepared = make_ref (SRC_SINC_FASTEST, 1, 1) ;
	TASK_CHECK (plain != NULL && prepared != NULL) ;

	fp = run_process (plain, 1, 0.9, big_in, in_frames, 1024, 777, NULL, big_ref_old, NULL) ;
	fs = run_process (prepared, 1, 0.9, big_in, in_frames, 1024, 777, NULL, big_sw, NULL) ;

	src_delete (plain) ;
	src_delete (prepared) ;

	TASK_CHECK (fp == fs && fp > 0) ;
	for (long i = 0 ; i < fp ; i++)
		TASK_CHECK (big_sw [i] == big_ref_old [i]) ;

	return 0 ;
} /* case_stream_ring_wrap */

/*
** Force ring buffer compaction while running the narrow quality, then upgrade
** to the widest quality. If compaction had discarded more history than the
** widest window needs, the new voice would read overwritten samples and the
** per coordinate blend check below would fail.
*/
static int
case_stream_compaction_then_upgrade (void)
{
	const long in_frames = 130000 ;
	SRC_STATE *ro, *rn, *sw ;
	/* Switch well past the first ring buffer compaction. */
	SWITCH_PLAN plan = { .at = 90000, .quality = SRC_SINC_BEST_QUALITY, .fade = 800 } ;
	long fo, fn, fs ;

	gen_signal (big_in, in_frames, 1) ;

	ro = make_ref (SRC_SINC_FASTEST, 1, 0) ;
	rn = make_ref (SRC_SINC_BEST_QUALITY, 1, 0) ;
	sw = make_ref (SRC_SINC_FASTEST, 1, 1) ;
	TASK_CHECK (ro != NULL && rn != NULL && sw != NULL) ;

	fo = run_process (ro, 1, 1.0, big_in, in_frames, 1024, 777, NULL, big_ref_old, NULL) ;
	fn = run_process (rn, 1, 1.0, big_in, in_frames, 1024, 777, NULL, big_ref_new, NULL) ;
	fs = run_process (sw, 1, 1.0, big_in, in_frames, 1024, 777, &plan, big_sw, NULL) ;

	src_delete (ro) ;
	src_delete (rn) ;
	src_delete (sw) ;

	TASK_CHECK (fo == fn && fn == fs && fs > plan.at) ;
	TASK_CHECK (plan.fired) ;
	/* Every frame, including after compaction, must match the exact blend of
	** the two full stream references at the same source coordinate.
	*/
	TASK_CHECK (check_blend_output (big_sw, big_ref_old, big_ref_new, 1, fs, plan.start, plan.fade)) ;

	return 0 ;
} /* case_stream_compaction_then_upgrade */

/* End of input must terminate the stream normally, with no frames added. */
static int
case_stream_eof (void)
{
	const long in_frames = 4500 ;
	SRC_STATE *ro, *rn, *sw ;
	SWITCH_PLAN plan = { .at = 3000, .quality = SRC_SINC_MEDIUM_QUALITY, .fade = 900 } ;
	long fo, fn, fs, f ;

	gen_signal (sm_in, in_frames, 2) ;

	ro = make_ref (SRC_SINC_FASTEST, 2, 0) ;
	rn = make_ref (SRC_SINC_MEDIUM_QUALITY, 2, 0) ;
	sw = make_ref (SRC_SINC_FASTEST, 2, 1) ;
	TASK_CHECK (ro != NULL && rn != NULL && sw != NULL) ;

	fo = run_process (ro, 2, 1.15, sm_in, in_frames, 173, 141, NULL, sm_ref_old, NULL) ;
	fn = run_process (rn, 2, 1.15, sm_in, in_frames, 173, 141, NULL, sm_ref_new, NULL) ;
	fs = run_process (sw, 2, 1.15, sm_in, in_frames, 173, 141, &plan, sm_sw, NULL) ;

	src_delete (ro) ;
	src_delete (rn) ;
	src_delete (sw) ;

	TASK_CHECK (fo > 0 && fo == fn && fn == fs) ;
	TASK_CHECK (check_blend_output (sm_sw, sm_ref_old, sm_ref_new, 2, fs, plan.start, plan.fade)) ;
	for (f = 0 ; f < fs * 2 ; f++)
		TASK_CHECK (isfinite (sm_sw [f])) ;

	return 0 ;
} /* case_stream_eof */

/*
** Varying ratio must keep its original semantics on the prepared path. A
** prepared instance that never switches must stay bit identical to a plain
** instance driven by the same per-call ratio sequence, and a switched run
** must keep the same frame count as plain references.
*/
static long
run_varispeed (SRC_STATE *state, int channels, const float *in, long in_frames,
			SWITCH_PLAN *plan, float *out)
{
	long current_in = 0, current_out = 0 ;
	const double ratios [] = { 0.6, 0.85, 1.0, 1.25, 1.6, 1.1, 0.7, 0.95 } ;
	int rstep = 0 ;

	while (current_in < in_frames)
	{	SRC_DATA d ;
		long this_in = in_frames - current_in ;
		if (this_in > 149)
			this_in = 149 ;
		memset (&d, 0, sizeof (d)) ;
		d.data_in = in + current_in * channels ;
		d.input_frames = this_in ;
		d.data_out = out + current_out * channels ;
		d.output_frames = 131 ;
		d.src_ratio = ratios [rstep % LOCAL_ARRAY_LEN (ratios)] ;
		rstep ++ ;
		d.end_of_input = (current_in + this_in >= in_frames) ? 1 : 0 ;

		if (plan != NULL && ! plan->fired && current_out >= plan->at)
		{	if (src_set_sinc_quality (state, plan->quality, plan->fade) != 0)
				return -1 ;
			plan->fired = 1 ;
			plan->start = current_out ;
		} ;

		if (src_process (state, &d) != 0)
			return -1 ;
		current_in += d.input_frames_used ;
		current_out += d.output_frames_gen ;
		if (d.end_of_input && d.output_frames_gen == 0)
			break ;
	} ;

	return current_out ;
} /* run_varispeed */

static int
case_stream_varispeed (void)
{
	const long in_frames = 8000 ;
	SRC_STATE *plain, *prepared, *rn ;
	SWITCH_PLAN plan = { .at = 2400, .quality = SRC_SINC_BEST_QUALITY, .fade = 500 } ;
	long fp, fs, fn ;

	gen_signal (sm_in, in_frames, 2) ;

	plain = make_ref (SRC_SINC_FASTEST, 2, 0) ;
	prepared = make_ref (SRC_SINC_FASTEST, 2, 1) ;
	rn = make_ref (SRC_SINC_BEST_QUALITY, 2, 0) ;
	TASK_CHECK (plain != NULL && prepared != NULL && rn != NULL) ;

	/* Same ratio sequence, no switch: the prepared path stays bit identical. */
	fp = run_varispeed (plain, 2, sm_in, in_frames, NULL, sm_ref_old) ;
	fs = run_varispeed (prepared, 2, sm_in, in_frames, NULL, sm_sw) ;
	TASK_CHECK (fp == fs && fp > 0) ;
	for (long i = 0 ; i < fp * 2 ; i++)
		TASK_CHECK (sm_ref_old [i] == sm_sw [i]) ;

	/* A switch under varying ratio follows the original (per quality) end of
	** input semantics: qualities may legitimately differ in total frame count,
	** but the pre switch prefix is pure fast quality and must match the plain
	** reference exactly, and the whole result stays finite and bounded.
	*/
	{
		SRC_STATE *sw2 = make_ref (SRC_SINC_FASTEST, 2, 1) ;
		long f2, lo, hi ;
		TASK_CHECK (sw2 != NULL) ;
		fn = run_varispeed (rn, 2, sm_in, in_frames, NULL, sm_ref_new) ;
		f2 = run_varispeed (sw2, 2, sm_in, in_frames, &plan, sm_sw2) ;
		TASK_CHECK (f2 > 0 && plan.fired) ;

		for (long f = 0 ; f < plan.start && f < f2 ; f++)
			for (int ch = 0 ; ch < 2 ; ch++)
				TASK_CHECK (sm_sw2 [f * 2 + ch] == sm_ref_old [f * 2 + ch]) ;
		for (long i = 0 ; i < f2 * 2 ; i++)
			TASK_CHECK (isfinite (sm_sw2 [i])) ;

		lo = (fp < fn ? fp : fn) - 512 ;
		hi = (fp > fn ? fp : fn) + 512 ;
		TASK_CHECK (f2 >= lo && f2 <= hi) ;
		src_delete (sw2) ;
	} ;

	src_delete (plain) ;
	src_delete (prepared) ;
	src_delete (rn) ;
	return 0 ;
} /* case_stream_varispeed */

struct cb_feed
{	const float	*in ;
	long		total, pos, chunk ;
	int			channels ;
} ;

static long
cb_func (void *arg, float **ptr)
{
	struct cb_feed *feed = (struct cb_feed *) arg ;
	long n = feed->total - feed->pos ;
	if (n > feed->chunk)
		n = feed->chunk ;
	*ptr = (float *) (feed->in + feed->pos * feed->channels) ;
	feed->pos += n ;
	return n ;
} /* cb_func */

static long
run_callback (int quality, int prepare, int channels, double ratio, long in_frames,
			long read_cap, SWITCH_PLAN *plan, float *out)
{
	struct cb_feed feed ;
	SRC_STATE *s ;
	int err = 0 ;
	long done = 0 ;

	feed.in = sm_in ;
	feed.total = in_frames ;
	feed.pos = 0 ;
	feed.chunk = 109 ;
	feed.channels = channels ;

	s = src_callback_new (cb_func, quality, channels, &err, &feed) ;
	if (s == NULL || err != 0)
		return -1 ;
	if (prepare && src_prepare_sinc_switch (s) != 0)
	{	src_delete (s) ;
		return -1 ;
	} ;

	while (done < MAX_SMALL_FRAMES)
	{	long n ;
		if (plan != NULL && ! plan->fired && done >= plan->at)
		{	if (src_set_sinc_quality (s, plan->quality, plan->fade) != 0)
			{	src_delete (s) ;
				return -1 ;
			} ;
			plan->fired = 1 ;
			plan->start = done ;
		} ;

		n = src_callback_read (s, ratio, read_cap, out + done * channels) ;
		if (n <= 0)
			break ;
		done += n ;
	} ;

	src_delete (s) ;
	return done ;
} /* run_callback */

/* Quality switching through src_callback_read, checked against callback references. */
static int
case_stream_callback (void)
{
	const long in_frames = 6000 ;
	SWITCH_PLAN plan = { .at = 2000, .quality = SRC_SINC_BEST_QUALITY, .fade = 600 } ;
	long fo, fn, fs ;

	gen_signal (sm_in, in_frames, 2) ;

	fo = run_callback (SRC_SINC_FASTEST, 0, 2, 0.7, in_frames, 130, NULL, sm_ref_old) ;
	fn = run_callback (SRC_SINC_BEST_QUALITY, 0, 2, 0.7, in_frames, 130, NULL, sm_ref_new) ;
	fs = run_callback (SRC_SINC_FASTEST, 1, 2, 0.7, in_frames, 130, &plan, sm_sw) ;

	TASK_CHECK (fo > 0 && fo == fn && fn == fs) ;
	TASK_CHECK (plan.fired) ;
	TASK_CHECK (check_blend_output (sm_sw, sm_ref_old, sm_ref_new, 2, fs, plan.start, plan.fade)) ;

	return 0 ;
} /* case_stream_callback */

/*
** A fade armed while no input is available must not advance: a zero input
** call emits nothing and leaves the crossfade pending. Once input arrives the
** first emitted frame carries the j = 1 blend weight.
*/
static int
case_stream_starvation_holds_fade (void)
{
	const long in_frames = 4000 ;
	const long fade = 300 ;
	SRC_STATE *sw, *ro, *rn ;
	SRC_DATA d ;
	long fs, fo, fn, ci, co ;

	gen_signal (sm_in, in_frames, 2) ;

	sw = make_ref (SRC_SINC_FASTEST, 2, 1) ;
	TASK_CHECK (sw != NULL) ;

	/* Arm the crossfade before any input is committed. */
	TASK_CHECK (src_set_sinc_quality (sw, SRC_SINC_BEST_QUALITY, fade) == 0) ;

	memset (&d, 0, sizeof (d)) ;
	d.data_in = NULL ;
	d.input_frames = 0 ;
	d.data_out = sm_sw ;
	d.output_frames = 64 ;
	d.src_ratio = 1.0 ;
	TASK_CHECK (src_process (sw, &d) == 0) ;
	TASK_CHECK (d.output_frames_gen == 0) ;

	/* The fade must still be pending, so a second request is rejected. */
	TASK_CHECK (src_set_sinc_quality (sw, SRC_SINC_MEDIUM_QUALITY, 10) != 0) ;

	/* Now feed the whole stream. */
	ci = 0 ; co = 0 ;
	while (ci < in_frames)
	{	long ti = in_frames - ci ;
		if (ti > 197)
			ti = 197 ;
		memset (&d, 0, sizeof (d)) ;
		d.data_in = sm_in + ci * 2 ;
		d.input_frames = ti ;
		d.data_out = sm_sw + co * 2 ;
		d.output_frames = 173 ;
		d.src_ratio = 1.0 ;
		d.end_of_input = (ci + ti >= in_frames) ;
		TASK_CHECK (src_process (sw, &d) == 0) ;
		ci += d.input_frames_used ;
		co += d.output_frames_gen ;
		if (d.end_of_input && d.output_frames_gen == 0)
			break ;
	} ;
	fs = co ;
	TASK_CHECK (fs >= fade) ;

	ro = make_ref (SRC_SINC_FASTEST, 2, 0) ;
	rn = make_ref (SRC_SINC_BEST_QUALITY, 2, 0) ;
	TASK_CHECK (ro != NULL && rn != NULL) ;

	ci = 0 ; co = 0 ;
	while (ci < in_frames)
	{	long ti = in_frames - ci ; if (ti > 197) ti = 197 ;
		memset (&d, 0, sizeof (d)) ;
		d.data_in = sm_in + ci * 2 ; d.input_frames = ti ;
		d.data_out = sm_ref_old + co * 2 ; d.output_frames = 173 ;
		d.src_ratio = 1.0 ; d.end_of_input = (ci + ti >= in_frames) ;
		TASK_CHECK (src_process (ro, &d) == 0) ;
		ci += d.input_frames_used ; co += d.output_frames_gen ;
		if (d.end_of_input && d.output_frames_gen == 0) break ;
	} ;
	fo = co ;

	ci = 0 ; co = 0 ;
	while (ci < in_frames)
	{	long ti = in_frames - ci ; if (ti > 197) ti = 197 ;
		memset (&d, 0, sizeof (d)) ;
		d.data_in = sm_in + ci * 2 ; d.input_frames = ti ;
		d.data_out = sm_ref_new + co * 2 ; d.output_frames = 173 ;
		d.src_ratio = 1.0 ; d.end_of_input = (ci + ti >= in_frames) ;
		TASK_CHECK (src_process (rn, &d) == 0) ;
		ci += d.input_frames_used ; co += d.output_frames_gen ;
		if (d.end_of_input && d.output_frames_gen == 0) break ;
	} ;
	fn = co ;

	TASK_CHECK (fo >= fade && fn >= fade) ;

	/* Fade frames j = 1..N match the per coordinate old/new blend. */
	for (long f = 0 ; f < fade ; f++)
	{	double w_new = (double) (f + 1) / (double) fade ;
		for (int ch = 0 ; ch < 2 ; ch++)
		{	double expect = sm_ref_old [f * 2 + ch] * (1.0 - w_new)
						+ sm_ref_new [f * 2 + ch] * w_new ;
			TASK_CHECK (fabs (sm_sw [f * 2 + ch] - expect) <= BLEND_TOL) ;
		} ;
	} ;

	/* After the fade the result is pure new quality over the common tail. */
	{	long tail = fs < fn ? fs : fn ;
		for (long f = fade ; f < tail ; f++)
			TASK_CHECK (fabs (sm_sw [f * 2] - sm_ref_new [f * 2]) <= BLEND_TOL) ;
	} ;

	src_delete (sw) ;
	src_delete (ro) ;
	src_delete (rn) ;
	return 0 ;
} /* case_stream_starvation_holds_fade */

/*--------------------------------------------------------------------------------------
** Lifecycle group.
*/

static int
case_lifecycle_prepare_rules (void)
{
	int err = 0 ;
	SRC_STATE *s ;
	SRC_DATA d ;

	/* LINEAR and ZOH cannot be prepared. */
	s = src_new (SRC_LINEAR, 2, &err) ;
	TASK_CHECK (s != NULL) ;
	TASK_CHECK (src_prepare_sinc_switch (s) != 0) ;
	src_delete (s) ;

	s = src_new (SRC_ZERO_ORDER_HOLD, 2, &err) ;
	TASK_CHECK (s != NULL) ;
	TASK_CHECK (src_prepare_sinc_switch (s) != 0) ;
	src_delete (s) ;

	/* Prepare on a fresh sinc succeeds and is idempotent. */
	s = src_new (SRC_SINC_FASTEST, 2, &err) ;
	TASK_CHECK (s != NULL) ;
	TASK_CHECK (src_prepare_sinc_switch (s) == 0) ;
	TASK_CHECK (src_prepare_sinc_switch (s) == 0) ;

	/* An unprepared instance that has already committed input cannot be prepared. */
	{
		SRC_STATE *late = src_new (SRC_SINC_FASTEST, 2, &err) ;
		TASK_CHECK (late != NULL) ;
		memset (&d, 0, sizeof (d)) ;
		d.data_in = sm_in ;
		d.input_frames = 64 ;
		d.data_out = sm_sw ;
		d.output_frames = 64 ;
		d.src_ratio = 1.0 ;
		TASK_CHECK (src_process (late, &d) == 0) ;
		TASK_CHECK (src_prepare_sinc_switch (late) != 0) ;
		/* A reset restores the pristine condition and preparation succeeds. */
		TASK_CHECK (src_reset (late) == 0) ;
		TASK_CHECK (src_prepare_sinc_switch (late) == 0) ;
		src_delete (late) ;
	} ;

	/* The already prepared instance keeps working after input is committed. */
	memset (&d, 0, sizeof (d)) ;
	d.data_in = sm_in ;
	d.input_frames = 64 ;
	d.data_out = sm_sw ;
	d.output_frames = 64 ;
	d.src_ratio = 1.0 ;
	TASK_CHECK (src_process (s, &d) == 0) ;

	/* Reset clears usage, preparation survives and can be reconfirmed. */
	TASK_CHECK (src_reset (s) == 0) ;
	TASK_CHECK (src_prepare_sinc_switch (s) == 0) ;

	src_delete (s) ;
	return 0 ;
} /* case_lifecycle_prepare_rules */

static int
case_lifecycle_set_validation (void)
{
	int err = 0 ;
	SRC_STATE *s = src_new (SRC_SINC_FASTEST, 2, &err) ;
	TASK_CHECK (s != NULL) ;

	/* Not prepared. */
	TASK_CHECK (src_set_sinc_quality (s, SRC_SINC_MEDIUM_QUALITY, 100) != 0) ;

	TASK_CHECK (src_prepare_sinc_switch (s) == 0) ;

	/* Bad converter types, including LINEAR/ZOH and out of range values. */
	TASK_CHECK (src_set_sinc_quality (s, SRC_LINEAR, 100) != 0) ;
	TASK_CHECK (src_set_sinc_quality (s, SRC_ZERO_ORDER_HOLD, 100) != 0) ;
	TASK_CHECK (src_set_sinc_quality (s, 99, 100) != 0) ;
	TASK_CHECK (src_set_sinc_quality (s, -1, 100) != 0) ;

	/* Negative fade length. */
	TASK_CHECK (src_set_sinc_quality (s, SRC_SINC_MEDIUM_QUALITY, -1) != 0) ;

	/* NULL state must be rejected rather than crash. */
	TASK_CHECK (src_prepare_sinc_switch (NULL) != 0) ;
	TASK_CHECK (src_set_sinc_quality (NULL, SRC_SINC_MEDIUM_QUALITY, 100) != 0) ;

	src_delete (s) ;
	return 0 ;
} /* case_lifecycle_set_validation */

/* A second request during a running fade errors out and leaves the fade intact. */
static int
case_lifecycle_reject_second_switch (void)
{
	const long in_frames = 6000 ;
	SRC_STATE *ro, *rn, *sw ;
	SWITCH_PLAN plan = { .at = 1500, .quality = SRC_SINC_BEST_QUALITY, .fade = 1000 } ;
	long fo, fn, fs ;
	int rejected = 0 ;

	gen_signal (sm_in, in_frames, 2) ;

	ro = make_ref (SRC_SINC_FASTEST, 2, 0) ;
	rn = make_ref (SRC_SINC_BEST_QUALITY, 2, 0) ;
	sw = make_ref (SRC_SINC_FASTEST, 2, 1) ;
	TASK_CHECK (ro != NULL && rn != NULL && sw != NULL) ;

	fo = run_process (ro, 2, 1.0, sm_in, in_frames, 137, 97, NULL, sm_ref_old, NULL) ;
	fn = run_process (rn, 2, 1.0, sm_in, in_frames, 137, 97, NULL, sm_ref_new, NULL) ;

	/* Drive the switched instance manually so a second request lands mid-fade. */
	{
		long current_in = 0, current_out = 0 ;
		while (current_out < fo)
		{	SRC_DATA d ;
			long this_in = in_frames - current_in ;
			if (this_in > 137)
				this_in = 137 ;
			memset (&d, 0, sizeof (d)) ;
			d.data_in = sm_in + current_in * 2 ;
			d.input_frames = this_in ;
			d.data_out = sm_sw + current_out * 2 ;
			d.output_frames = 97 ;
			d.src_ratio = 1.0 ;
			d.end_of_input = (current_in + this_in >= in_frames) ? 1 : 0 ;

			if (! plan.fired && current_out >= plan.at)
			{	TASK_CHECK (src_set_sinc_quality (sw, plan.quality, plan.fade) == 0) ;
				plan.fired = 1 ;
				plan.start = current_out ;
			} ;

			TASK_CHECK (src_process (sw, &d) == 0) ;

			/* While the first fade is active, every further request fails.
			** The fade is still running after this call iff the generated
			** frames are strictly inside the [start, start + fade) range.
			*/
			{
				long post_out = current_out + d.output_frames_gen ;
				if (plan.fired && post_out > plan.start && post_out < plan.start + plan.fade)
				{	TASK_CHECK (src_set_sinc_quality (sw, SRC_SINC_MEDIUM_QUALITY, 50) != 0) ;
					TASK_CHECK (src_set_sinc_quality (sw, SRC_SINC_FASTEST, 0) != 0) ;
					rejected = 1 ;
				} ;
			} ;

			current_in += d.input_frames_used ;
			current_out += d.output_frames_gen ;
			if (d.end_of_input && d.output_frames_gen == 0)
				break ;
		} ;
		fs = current_out ;
	} ;

	src_delete (ro) ;
	src_delete (rn) ;
	src_delete (sw) ;

	TASK_CHECK (rejected) ;
	TASK_CHECK (fo == fn && fn == fs) ;
	TASK_CHECK (check_blend_output (sm_sw, sm_ref_old, sm_ref_new, 2, fs, plan.start, plan.fade)) ;

	return 0 ;
} /* case_lifecycle_reject_second_switch */

/* Reset during an unfinished fade restores the old, previously stable quality. */
static int
case_lifecycle_reset_mid_fade (void)
{
	SRC_STATE *sw, *ref ;
	SRC_DATA d ;
	int err = 0 ;

	sw = src_new (SRC_SINC_FASTEST, 1, &err) ;
	TASK_CHECK (sw != NULL) ;
	TASK_CHECK (src_prepare_sinc_switch (sw) == 0) ;

	memset (&d, 0, sizeof (d)) ;
	d.data_in = sm_in ;
	d.input_frames = 512 ;
	d.data_out = sm_sw ;
	d.output_frames = 512 ;
	d.src_ratio = 1.0 ;
	TASK_CHECK (src_process (sw, &d) == 0) ;

	/* Start a long fade and emit part of it. */
	TASK_CHECK (src_set_sinc_quality (sw, SRC_SINC_BEST_QUALITY, 2000) == 0) ;
	memset (&d, 0, sizeof (d)) ;
	d.data_in = sm_in + 512 ;
	d.input_frames = 256 ;
	d.data_out = sm_sw ;
	d.output_frames = 256 ;
	d.src_ratio = 1.0 ;
	TASK_CHECK (src_process (sw, &d) == 0) ;

	TASK_CHECK (src_reset (sw) == 0) ;

	/* Still prepared: a new switch is accepted without preparing again. */
	TASK_CHECK (src_set_sinc_quality (sw, SRC_SINC_MEDIUM_QUALITY, 100) == 0) ;
	TASK_CHECK (src_reset (sw) == 0) ;

	/* Stable quality after resetting the unfinished fade is the old quality. */
	ref = src_new (SRC_SINC_FASTEST, 1, &err) ;
	TASK_CHECK (ref != NULL) ;

	{
		float fresh_in [1024], out_a [1024], out_b [1024] ;
		SRC_DATA da, db ;
		gen_signal (fresh_in, 1024, 1) ;

		memset (&da, 0, sizeof (da)) ;
		da.data_in = fresh_in ; da.input_frames = 1024 ;
		da.data_out = out_a ; da.output_frames = 1024 ;
		da.src_ratio = 1.0 ; da.end_of_input = 1 ;
		memcpy (&db, &da, sizeof (db)) ;
		db.data_out = out_b ;

		TASK_CHECK (src_process (sw, &da) == 0) ;
		TASK_CHECK (src_process (ref, &db) == 0) ;
		TASK_CHECK (da.output_frames_gen == db.output_frames_gen) ;
		for (long i = 0 ; i < da.output_frames_gen ; i++)
			TASK_CHECK (out_a [i] == out_b [i]) ;
	} ;

	src_delete (sw) ;
	src_delete (ref) ;
	return 0 ;
} /* case_lifecycle_reset_mid_fade */

/* A clone continues a half finished fade independently of the original. */
static int
case_lifecycle_clone_fade (void)
{
	const long in_frames = 6000 ;
	SRC_STATE *ro, *rn, *sw, *cl ;
	SWITCH_PLAN plan = { .at = 1800, .quality = SRC_SINC_BEST_QUALITY, .fade = 1200 } ;
	long fo, fn ;
	int err = 0 ;

	gen_signal (sm_in, in_frames, 2) ;

	ro = make_ref (SRC_SINC_FASTEST, 2, 0) ;
	rn = make_ref (SRC_SINC_BEST_QUALITY, 2, 0) ;
	sw = make_ref (SRC_SINC_FASTEST, 2, 1) ;
	TASK_CHECK (ro != NULL && rn != NULL && sw != NULL) ;

	fo = run_process (ro, 2, 1.0, sm_in, in_frames, 137, 97, NULL, sm_ref_old, NULL) ;
	fn = run_process (rn, 2, 1.0, sm_in, in_frames, 137, 97, NULL, sm_ref_new, NULL) ;

	/* Run up to the middle of the fade, then clone. */
	{
		long current_in = 0, current_out = 0, clone_out = 0 ;
		while (! plan.fired || current_out < plan.start + plan.fade / 2)
		{	SRC_DATA d ;
			long this_in = in_frames - current_in ;
			if (this_in > 137)
				this_in = 137 ;
			memset (&d, 0, sizeof (d)) ;
			d.data_in = sm_in + current_in * 2 ;
			d.input_frames = this_in ;
			d.data_out = sm_sw + current_out * 2 ;
			d.output_frames = 97 ;
			d.src_ratio = 1.0 ;
			d.end_of_input = (current_in + this_in >= in_frames) ? 1 : 0 ;

			if (! plan.fired && current_out >= plan.at)
			{	TASK_CHECK (src_set_sinc_quality (sw, plan.quality, plan.fade) == 0) ;
				plan.fired = 1 ;
				plan.start = current_out ;
			} ;
			TASK_CHECK (src_process (sw, &d) == 0) ;
			current_in += d.input_frames_used ;
			current_out += d.output_frames_gen ;
			if (d.end_of_input && d.output_frames_gen == 0)
				break ;
		} ;

		cl = src_clone (sw, &err) ;
		TASK_CHECK (cl != NULL && err == 0) ;
		clone_out = current_out ;

		/* Continue both handles over the remaining input. */
		while (current_out < fo)
		{	SRC_DATA d1, d2 ;
			long this_in = in_frames - current_in ;
			if (this_in > 137)
				this_in = 137 ;

			memset (&d1, 0, sizeof (d1)) ;
			d1.data_in = sm_in + current_in * 2 ;
			d1.input_frames = this_in ;
			d1.data_out = sm_sw + current_out * 2 ;
			d1.output_frames = 97 ;
			d1.src_ratio = 1.0 ;
			d1.end_of_input = (current_in + this_in >= in_frames) ? 1 : 0 ;

			memcpy (&d2, &d1, sizeof (d2)) ;
			d2.data_out = sm_sw2 + current_out * 2 ;

			TASK_CHECK (src_process (sw, &d1) == 0) ;
			TASK_CHECK (src_process (cl, &d2) == 0) ;
			TASK_CHECK (d1.output_frames_gen == d2.output_frames_gen) ;
			TASK_CHECK (d1.input_frames_used == d2.input_frames_used) ;

			current_in += d1.input_frames_used ;
			current_out += d1.output_frames_gen ;
			if (d1.end_of_input && d1.output_frames_gen == 0)
				break ;
		} ;

		/* The clone must produce exactly the same continuation from its clone point. */
		for (long i = clone_out * 2 ; i < current_out * 2 ; i++)
			TASK_CHECK (sm_sw [i] == sm_sw2 [i]) ;

		TASK_CHECK (current_out == fo && fo == fn) ;
		TASK_CHECK (check_blend_output (sm_sw, sm_ref_old, sm_ref_new, 2, current_out, plan.start, plan.fade)) ;
	} ;

	src_delete (cl) ;
	src_delete (ro) ;
	src_delete (rn) ;
	src_delete (sw) ;
	return 0 ;
} /* case_lifecycle_clone_fade */

/* Unprepared sinc instances keep their original output and allocation behaviour. */
static int
case_lifecycle_normal_unchanged (void)
{
	const long in_frames = 5000 ;
	SRC_STATE *a, *b ;
	long fa, fb ;

	gen_signal (sm_in, in_frames, 2) ;

	a = make_ref (SRC_SINC_MEDIUM_QUALITY, 2, 0) ;
	b = make_ref (SRC_SINC_MEDIUM_QUALITY, 2, 0) ;
	TASK_CHECK (a != NULL && b != NULL) ;

	fa = run_process (a, 2, 1.3, sm_in, in_frames, 129, 77, NULL, sm_ref_old, NULL) ;
	fb = run_process (b, 2, 1.3, sm_in, in_frames, 129, 77, NULL, sm_sw, NULL) ;

	src_delete (a) ;
	src_delete (b) ;

	TASK_CHECK (fa == fb && fa > 0) ;
	for (long i = 0 ; i < fa * 2 ; i++)
		TASK_CHECK (sm_ref_old [i] == sm_sw [i]) ;

	return 0 ;
} /* case_lifecycle_normal_unchanged */

const struct task_case task_cases [] =
{
	{ "blend", "fast_to_best_crossfade",		case_blend_fast_to_best },
	{ "blend", "best_to_fast_crossfade",		case_blend_best_to_fast },
	{ "blend", "medium_quality_pairs",			case_blend_medium_pairs },
	{ "blend", "zero_fade_next_frame",			case_blend_zero_fade },
	{ "blend", "same_quality_is_noop",			case_blend_same_quality_noop },

	{ "streaming", "deep_history_upgrade",		case_stream_deep_history },
	{ "streaming", "async_chunk_equivalence",	case_stream_async_chunks },
	{ "streaming", "ring_wrap_identity",		case_stream_ring_wrap },
	{ "streaming", "compaction_then_upgrade",	case_stream_compaction_then_upgrade },
	{ "streaming", "varispeed_semantics",		case_stream_varispeed },
	{ "streaming", "eof_no_extra_frames",		case_stream_eof },
	{ "streaming", "callback_crossfade",		case_stream_callback },
	{ "streaming", "starvation_holds_fade",		case_stream_starvation_holds_fade },

	{ "lifecycle", "prepare_and_reset_rules",	case_lifecycle_prepare_rules },
	{ "lifecycle", "set_quality_validation",	case_lifecycle_set_validation },
	{ "lifecycle", "reject_switch_during_fade",	case_lifecycle_reject_second_switch },
	{ "lifecycle", "reset_restores_old_quality",	case_lifecycle_reset_mid_fade },
	{ "lifecycle", "clone_continues_fade",		case_lifecycle_clone_fade },
	{ "lifecycle", "normal_instances_unchanged",	case_lifecycle_normal_unchanged },
} ;

const size_t task_case_count = sizeof (task_cases) / sizeof (task_cases [0]) ;

/*
** Persistent checkpoint tests for libsamplerate.
**
** Three registered groups:
**   format   - wire format, byte-for-byte save determinism and rejection of
**              unsupported, truncated or self-contradictory checkpoints
**   resume   - process mode checkpoints across channels 1/2/4/6/7 and all
**              three sinc converters, resumed mid stream and while the
**              announced EOF is draining
**   callback - callback mode checkpoints including unconsumed remainder
**              frames, destruction of the original buffer, independent
**              clones, reset reuse and a fresh child process
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/wait.h>

#include <samplerate.h>
#include "common.h"
#include "task_support/check.h"
#include "checkpoint_util.h"

#define	TEST_N_FRAMES		((long) CKPT_SIGNAL_FRAMES)
#define	TEST_CB_N_FRAMES	((long) CKPT_CB_SIGNAL_FRAMES)
#define	TEST_CHUNK			((long) CKPT_CHUNK_FRAMES)
#define	MAX_RECORDS			40000
#define	MAX_OUT_FRAMES		46000
#define	MAX_CHANNELS		8

typedef struct
{	long used, gen ;
} CallRec ;

typedef struct
{	const float	*data ;
	long		N, cursor ;
	int			channels ;
	unsigned	calls ;
} CbSrc ;

static long
test_callback (void *ctx, float **data)
{
	CbSrc *c = (CbSrc *) ctx ;
	long take ;

	c->calls ++ ;

	if (c->cursor >= c->N)
		return 0 ;

	take = c->N - c->cursor ;
	if (take > CKPT_CB_BATCH)
		take = CKPT_CB_BATCH ;

	*data = (float *) (c->data + c->cursor * c->channels) ;
	c->cursor += take ;

	return take ;
} /* test_callback */

static float *
alloc_signal (long frames, int channels, uint32_t seed)
{
	float *data = (float *) malloc ((size_t) frames * channels * sizeof (float)) ;

	if (data != NULL)
		ckpt_gen_signal (data, frames, channels, seed) ;

	return data ;
} /* alloc_signal */

/*----------------------------------------------------------------------------------------
** Process mode: one shared driver, optionally taking checkpoints.
**
** A checkpoint is taken the first time the fractional phase is non zero with
** input already consumed, and again once while the announced EOF is draining.
** The save must not perturb the instance: the resulting PCM is compared
** bit-for-bit against a checkpoint-free reference run on the same schedule.
*/

static int
process_run (int converter, int channels, double ratio, const float *in,
			float *out, long *out_total, CallRec *recs, int *nrec, int do_save)
{
	SRC_STATE *state ;
	long in_off = 0, ot = 0 ;
	int step = 0, err ;
	int saved_early = 0, saved_eof = 0 ;

	state = src_new (converter, channels, &err) ;
	TASK_CHECK (state != NULL && err == 0) ;

	for ( ; ; step++)
	{
		long chunk = TEST_N_FRAMES - in_off ;
		int eof ;
		long cap ;
		SRC_DATA d ;

		if (chunk > TEST_CHUNK)
			chunk = TEST_CHUNK ;
		eof = (in_off + chunk >= TEST_N_FRAMES) ;
		cap = ckpt_cap_at (step) ;

		if (do_save && !saved_early && step >= 2 && in_off > 0 &&
				((struct SRC_STATE_tag *) state)->last_position > 0.0)
		{
			size_t need = src_checkpoint_size (state) ;
			unsigned char *b1, *b2 ;
			SRC_STATE *re ;
			int rerr = 0 ;

			TASK_CHECK (need > 0) ;
			b1 = (unsigned char *) malloc (need) ;
			b2 = (unsigned char *) malloc (need) ;
			TASK_CHECK (b1 != NULL && b2 != NULL) ;

			TASK_CHECK (src_checkpoint_write (state, b1, need) == 0) ;
			/* Repeated saves of an unchanged scene are byte identical. */
			TASK_CHECK (src_checkpoint_write (state, b2, need) == 0) ;
			TASK_CHECK (memcmp (b1, b2, need) == 0) ;
			/* A smaller buffer is rejected rather than truncated. */
			TASK_CHECK (src_checkpoint_write (state, b2, need - 1) != 0) ;

			/* Continue the run on the restored instance. */
			re = src_checkpoint_read (b1, need, NULL, NULL, &rerr) ;
			TASK_CHECK (re != NULL && rerr == 0) ;
			TASK_CHECK (src_checkpoint_size (re) == need) ;
			src_delete (state) ;
			state = re ;
			free (b1) ;
			free (b2) ;
			saved_early = 1 ;
		} ;

		memset (&d, 0, sizeof (d)) ;
		d.data_in = in + in_off * channels ;
		d.input_frames = chunk ;
		d.data_out = out + ot * channels ;
		d.output_frames = cap ;
		d.src_ratio = ratio ;
		d.end_of_input = eof ;

		TASK_CHECK (src_process (state, &d) == 0) ;

		recs [*nrec] = (CallRec) { d.input_frames_used, d.output_frames_gen } ;
		(*nrec) ++ ;
		TASK_CHECK (*nrec < MAX_RECORDS) ;

		in_off += d.input_frames_used ;
		ot += d.output_frames_gen ;
		TASK_CHECK (ot < MAX_OUT_FRAMES) ;

		if (do_save && !saved_eof && eof && d.output_frames_gen > 0)
		{
			size_t need = src_checkpoint_size (state) ;
			unsigned char *blob ;
			SRC_STATE *re ;
			int rerr = 0 ;

			TASK_CHECK (need > 0) ;
			blob = (unsigned char *) malloc (need) ;
			TASK_CHECK (blob != NULL) ;
			TASK_CHECK (src_checkpoint_write (state, blob, need) == 0) ;
			/* Restart on the restored instance while the announced EOF is
			** still draining.
			*/
			re = src_checkpoint_read (blob, need, NULL, NULL, &rerr) ;
			TASK_CHECK (re != NULL && rerr == 0) ;
			src_delete (state) ;
			state = re ;
			free (blob) ;
			saved_eof = 1 ;
		} ;

		if (eof && d.output_frames_gen == 0)
			break ;
	} ;

	TASK_CHECK (!do_save || (saved_early && saved_eof)) ;

	src_delete (state) ;
	*out_total = ot ;
	return 0 ;
} /* process_run */

static int
check_resume_case (int converter, int channels)
{
	const double ratio = 48000.0 / 44100.0 ;
	float *in ;
	float *ref_out, *ck_out ;
	CallRec *ref_rec, *ck_rec ;
	int ref_n = 0, ck_n = 0 ;
	long ref_total = 0, ck_total = 0 ;
	int k ;

	in = alloc_signal (TEST_N_FRAMES, channels, 0x12340000u + (uint32_t) converter * 97u + (uint32_t) channels) ;
	ref_out = (float *) malloc ((size_t) MAX_OUT_FRAMES * channels * sizeof (float)) ;
	ck_out = (float *) malloc ((size_t) MAX_OUT_FRAMES * channels * sizeof (float)) ;
	ref_rec = (CallRec *) malloc (sizeof (CallRec) * MAX_RECORDS) ;
	ck_rec = (CallRec *) malloc (sizeof (CallRec) * MAX_RECORDS) ;
	TASK_CHECK (in && ref_out && ck_out && ref_rec && ck_rec) ;

	TASK_CHECK (process_run (converter, channels, ratio, in, ref_out,
					&ref_total, ref_rec, &ref_n, 0) == 0) ;
	TASK_CHECK (process_run (converter, channels, ratio, in, ck_out,
					&ck_total, ck_rec, &ck_n, 1) == 0) ;

	TASK_CHECK (ref_total > 0 && ref_total == ck_total) ;
	TASK_CHECK (ref_n == ck_n) ;

	for (k = 0 ; k < ck_n ; k++)
	{
		TASK_CHECK (ref_rec [k].used == ck_rec [k].used) ;
		TASK_CHECK (ref_rec [k].gen == ck_rec [k].gen) ;
	} ;

	/* Full bit-for-bit PCM comparison, not just the first frame. */
	TASK_CHECK (memcmp (ref_out, ck_out, (size_t) ref_total * channels * sizeof (float)) == 0) ;

	free (in) ;
	free (ref_out) ;
	free (ck_out) ;
	free (ref_rec) ;
	free (ck_rec) ;
	return 0 ;
} /* check_resume_case */

static int
resume_fastest (void)
{
	const int chans [] = { 1, 2, 4, 6, 7 } ;

	for (int k = 0 ; k < (int) ARRAY_LEN (chans) ; k++)
		TASK_CHECK (check_resume_case (SRC_SINC_FASTEST, chans [k]) == 0) ;
	return 0 ;
} /* resume_fastest */

static int
resume_medium (void)
{
	const int chans [] = { 1, 2, 4, 6, 7 } ;

	for (int k = 0 ; k < (int) ARRAY_LEN (chans) ; k++)
		TASK_CHECK (check_resume_case (SRC_SINC_MEDIUM_QUALITY, chans [k]) == 0) ;
	return 0 ;
} /* resume_medium */

static int
resume_best (void)
{
	const int chans [] = { 1, 2, 4, 6, 7 } ;

	for (int k = 0 ; k < (int) ARRAY_LEN (chans) ; k++)
		TASK_CHECK (check_resume_case (SRC_SINC_BEST_QUALITY, chans [k]) == 0) ;
	return 0 ;
} /* resume_best */

/*----------------------------------------------------------------------------------------
** Callback mode.
*/

static int
drive_prefix (SRC_STATE *state, double ratio, int steps, int channels,
			float *out, long *total)
{
	for (int step = 0 ; step < steps ; step++)
	{
		long cap = ckpt_cap_at (step) ;
		long got = src_callback_read (state, ratio, cap, out + *total * channels) ;

		TASK_CHECK (got == cap) ;
		*total += got ;
	} ;
	return 0 ;
} /* drive_prefix */

static int
drive_continuation (SRC_STATE *state, double ratio, int step_offset, int channels,
			float *out, long want_frames, long *total, CallRec *recs, int *nrec)
{
	int step = 0 ;

	while (*total < want_frames)
	{
		long cap = ckpt_cap_at (step + step_offset) ;
		long got ;

		if (cap > want_frames - *total)
			cap = want_frames - *total ;

		got = src_callback_read (state, ratio, cap, out + *total * channels) ;
		recs [(*nrec) ++] = (CallRec) { 0, got } ;
		TASK_CHECK (*nrec < MAX_RECORDS) ;
		*total += got ;
		step ++ ;

		if (got == 0)
			break ;
	} ;

	return 0 ;
} /* drive_continuation */

static int
callback_resume_case (int converter, int channels)
{
	const double ratio = 1.375 ;
	const long want_frames = 14000 ;
	const int save_steps = 4 ;
	const uint32_t seed = 0xABC00000u + (uint32_t) converter * 131u + (uint32_t) channels ;
	float *in_ref, *in_ck, *in_new ;
	float *ref_out, *ck_out, *clone_out ;
	CallRec *ref_rec, *ck_rec ;
	int ref_n = 0, ck_n = 0 ;
	CbSrc ref_src, ck_src, new_src, clone_src ;
	SRC_STATE *ref_state, *ck_state, *restored, *probe_state, *clone_state, *fresh_state ;
	int err ;
	size_t need ;
	unsigned char *blob ;
	long ref_total = 0, ck_total = 0, clone_total = 0, ck_prefix = 0 ;
	long consumed_before, pending_at_save ;
	int k ;

	in_ref = alloc_signal (TEST_CB_N_FRAMES, channels, seed) ;
	in_ck = alloc_signal (TEST_CB_N_FRAMES, channels, seed) ;
	in_new = alloc_signal (TEST_CB_N_FRAMES, channels, seed) ;
	ref_out = (float *) malloc ((size_t) want_frames * channels * sizeof (float)) ;
	ck_out = (float *) malloc ((size_t) want_frames * channels * sizeof (float)) ;
	clone_out = (float *) malloc ((size_t) want_frames * channels * sizeof (float)) ;
	ref_rec = (CallRec *) malloc (sizeof (CallRec) * MAX_RECORDS) ;
	ck_rec = (CallRec *) malloc (sizeof (CallRec) * MAX_RECORDS) ;
	TASK_CHECK (in_ref && in_ck && in_new && ref_out && ck_out && clone_out && ref_rec && ck_rec) ;
	TASK_CHECK (memcmp (in_ref, in_ck, (size_t) TEST_CB_N_FRAMES * channels * sizeof (float)) == 0) ;
	TASK_CHECK (memcmp (in_ref, in_new, (size_t) TEST_CB_N_FRAMES * channels * sizeof (float)) == 0) ;

	ref_src = (CbSrc) { in_ref, TEST_CB_N_FRAMES, 0, channels, 0 } ;
	ck_src = (CbSrc) { in_ck, TEST_CB_N_FRAMES, 0, channels, 0 } ;

	ref_state = src_callback_new (test_callback, converter, channels, &err, &ref_src) ;
	ck_state = src_callback_new (test_callback, converter, channels, &err, &ck_src) ;
	TASK_CHECK (ref_state != NULL && ck_state != NULL && err == 0) ;

	TASK_CHECK (drive_prefix (ref_state, ratio, save_steps, channels, ref_out, &ref_total) == 0) ;
	TASK_CHECK (drive_prefix (ck_state, ratio, save_steps, channels, ck_out, &ck_total) == 0) ;
	TASK_CHECK (ref_total == ck_total) ;
	TASK_CHECK (memcmp (ref_out, ck_out, (size_t) ref_total * channels * sizeof (float)) == 0) ;

	/* A callback delivered more frames than the filter consumed. */
	pending_at_save = ((struct SRC_STATE_tag *) ck_state)->saved_frames ;
	TASK_CHECK (pending_at_save > 0) ;
	TASK_CHECK (((struct SRC_STATE_tag *) ck_state)->pending_frames == pending_at_save) ;

	need = src_checkpoint_size (ck_state) ;
	TASK_CHECK (need > 0) ;
	blob = (unsigned char *) malloc (need) ;
	TASK_CHECK (blob != NULL) ;
	TASK_CHECK (src_checkpoint_write (ck_state, blob, need) == 0) ;
	{
		unsigned char *blob2 = (unsigned char *) malloc (need) ;

		TASK_CHECK (blob2 != NULL) ;
		/* Repeated saves of the unchanged callback scene are byte identical. */
		TASK_CHECK (src_checkpoint_write (ck_state, blob2, need) == 0) ;
		TASK_CHECK (memcmp (blob, blob2, need) == 0) ;
		TASK_CHECK (src_checkpoint_write (ck_state, blob2, need - 1) != 0) ;
		free (blob2) ;
	} ;

	consumed_before = ck_src.cursor - pending_at_save ;
	TASK_CHECK (consumed_before > 0 && consumed_before < ck_src.cursor) ;

	/* The original instance, its callback buffer and the snapshot can all be
	** destroyed; the restored instance must stand on its own storage.
	*/
	src_delete (ck_state) ;
	free (in_ck) ;

	/* Probe instance: serialised remainder frames are fed out before the new
	** callback is ever asked for more data. The probe is discarded afterwards
	** so it cannot perturb the bit-exact continuation below.
	*/
	{
		CbSrc probe_src = { in_new, TEST_CB_N_FRAMES, consumed_before, channels, 0 } ;

		probe_state = src_checkpoint_read (blob, need, test_callback, &probe_src, &err) ;
		TASK_CHECK (probe_state != NULL && err == 0) ;
		TASK_CHECK (((struct SRC_STATE_tag *) probe_state)->pending_frames == pending_at_save) ;

		while (((struct SRC_STATE_tag *) probe_state)->pending_frames > 0)
		{
			float one [MAX_CHANNELS] ;
			long got = src_callback_read (probe_state, ratio, 1, one) ;

			TASK_CHECK (got == 1) ;
			/* Remainder alone satisfied this read: no new callback batch. */
			TASK_CHECK (probe_src.calls == 0) ;
		} ;

		/* Once the remainder is gone, the next read pulls a fresh batch. */
		{
			float one [MAX_CHANNELS] ;
			TASK_CHECK (src_callback_read (probe_state, ratio, 1, one) == 1) ;
			TASK_CHECK (probe_src.calls >= 1) ;
		} ;
		src_delete (probe_state) ;
	} ;

	/* The instance used for the real continuation gets a pristine callback
	** source positioned at the saved boundary.
	*/
	new_src = (CbSrc) { in_new, TEST_CB_N_FRAMES, consumed_before, channels, 0 } ;

	/* Mode rules: callback snapshots require a real callback. */
	TASK_CHECK (src_checkpoint_read (blob, need, NULL, NULL, &err) == NULL && err != 0) ;

	restored = src_checkpoint_read (blob, need, test_callback, &new_src, &err) ;
	TASK_CHECK (restored != NULL && err == 0) ;
	TASK_CHECK (((struct SRC_STATE_tag *) restored)->pending_frames == pending_at_save) ;
	TASK_CHECK (((struct SRC_STATE_tag *) restored)->saved_data ==
					((struct SRC_STATE_tag *) restored)->pending_data) ;

	/* Clone before either one is advanced: both then run the identical
	** continuation schedule on independent callback sources and storage.
	*/
	ck_prefix = ck_total ;
	clone_src = (CbSrc) { in_new, TEST_CB_N_FRAMES, consumed_before, channels, 0 } ;
	clone_state = src_clone (restored, &err) ;
	TASK_CHECK (clone_state != NULL && err == 0) ;
	if (((struct SRC_STATE_tag *) restored)->pending_data != NULL)
		TASK_CHECK (((struct SRC_STATE_tag *) clone_state)->pending_data !=
						((struct SRC_STATE_tag *) restored)->pending_data) ;
	((struct SRC_STATE_tag *) clone_state)->callback_func = test_callback ;
	((struct SRC_STATE_tag *) clone_state)->user_callback_data = &clone_src ;

	TASK_CHECK (drive_continuation (ref_state, ratio, save_steps, channels,
					ref_out, want_frames, &ref_total, ref_rec, &ref_n) == 0) ;
	TASK_CHECK (drive_continuation (restored, ratio, save_steps, channels,
					ck_out, want_frames, &ck_total, ck_rec, &ck_n) == 0) ;
	{
		int clone_n = 0 ;
		CallRec *clone_rec = (CallRec *) malloc (sizeof (CallRec) * MAX_RECORDS) ;

		TASK_CHECK (clone_rec != NULL) ;
		/* Start at the same prefix position so the capacity schedule is
		** identical to the restored instance's continuation.
		*/
		clone_total = ck_prefix ;
		TASK_CHECK (drive_continuation (clone_state, ratio, save_steps, channels,
						clone_out, want_frames, &clone_total, clone_rec, &clone_n) == 0) ;
		TASK_CHECK (clone_n == ck_n) ;
		for (k = 0 ; k < clone_n ; k++)
			TASK_CHECK (clone_rec [k].gen == ck_rec [k].gen) ;
		free (clone_rec) ;
	} ;

	TASK_CHECK (ref_total == ck_total && ref_total == want_frames) ;
	TASK_CHECK (clone_total == want_frames) ;
	TASK_CHECK (ref_n == ck_n) ;
	for (k = 0 ; k < ck_n ; k++)
		TASK_CHECK (ref_rec [k].gen == ck_rec [k].gen) ;
	TASK_CHECK (memcmp (ref_out, ck_out, (size_t) ref_total * channels * sizeof (float)) == 0) ;
	/* The clone's continuation PCM matches the restored instance's. */
	TASK_CHECK (memcmp (ck_out + ck_prefix * channels, clone_out + ck_prefix * channels,
					(size_t) (want_frames - ck_prefix) * channels * sizeof (float)) == 0) ;

	/* The new callback cursors never moved backwards, so the remainder
	** frames were not delivered twice.
	*/
	TASK_CHECK (new_src.cursor >= consumed_before && new_src.cursor <= TEST_CB_N_FRAMES) ;
	TASK_CHECK (clone_src.cursor >= consumed_before && clone_src.cursor <= TEST_CB_N_FRAMES) ;
	src_delete (clone_state) ;

	/* Reset clears restored audio and remainder; the instance then behaves
	** like a brand new one over a fresh callback source.
	*/
	TASK_CHECK (src_reset (restored) == 0) ;
	TASK_CHECK (((struct SRC_STATE_tag *) restored)->pending_frames == 0) ;
	TASK_CHECK (((struct SRC_STATE_tag *) restored)->pending_data == NULL) ;
	TASK_CHECK (((struct SRC_STATE_tag *) restored)->saved_frames == 0) ;
	{
		CbSrc fresh_src = { in_new, TEST_CB_N_FRAMES, 0, channels, 0 } ;
		CbSrc reset_src = { in_new, TEST_CB_N_FRAMES, 0, channels, 0 } ;
		float fresh_out [64 * MAX_CHANNELS], reset_out [64 * MAX_CHANNELS] ;

		fresh_state = src_callback_new (test_callback, converter, channels, &err, &fresh_src) ;
		TASK_CHECK (fresh_state != NULL) ;
		restored->mode = SRC_MODE_CALLBACK ;
		restored->callback_func = test_callback ;
		restored->user_callback_data = &reset_src ;

		for (k = 0 ; k < 4 ; k++)
		{
			long g1 = src_callback_read (fresh_state, ratio, 13, fresh_out) ;
			long g2 = src_callback_read (restored, ratio, 13, reset_out) ;

			TASK_CHECK (g1 == g2 && g1 == 13) ;
			TASK_CHECK (memcmp (fresh_out, reset_out, g1 * channels * sizeof (float)) == 0) ;
		} ;
		src_delete (fresh_state) ;
	} ;

	src_delete (restored) ;
	src_delete (ref_state) ;
	free (blob) ;
	free (in_ref) ;
	free (in_new) ;
	free (ref_out) ;
	free (ck_out) ;
	free (clone_out) ;
	free (ref_rec) ;
	free (ck_rec) ;
	return 0 ;
} /* callback_resume_case */

static int
callback_fastest (void)
{
	return callback_resume_case (SRC_SINC_FASTEST, 2) ;
} /* callback_fastest */

static int
callback_multichannel (void)
{
	return callback_resume_case (SRC_SINC_MEDIUM_QUALITY, 7) ;
} /* callback_multichannel */

static int
callback_best_mono (void)
{
	return callback_resume_case (SRC_SINC_BEST_QUALITY, 1) ;
} /* callback_best_mono */

/*----------------------------------------------------------------------------------------
** Fresh child process: the snapshot is written to a file and resumed by
** .task-build/checkpoint-child, which writes its continuation PCM to another
** file. Bit exact output proves cross-process equivalence.
*/

static int
callback_subprocess (void)
{
	const int channels = 6 ;
	const int converter = SRC_SINC_FASTEST ;
	const double ratio = 1.1 ;
	const long want_frames = 4000 ;
	const int save_steps = 3 ;
	const uint32_t seed = 0x51510000u ;
	const char *tmpdir = getenv ("TMPDIR") ;
	const char *child_exe = getenv ("CHECKPOINT_CHILD") ;
	char ck_path [512], out_path [512] ;
	float *in ;
	CbSrc src, ref_src ;
	SRC_STATE *state, *ref_state ;
	int err ;
	size_t need ;
	unsigned char *blob ;
	long prefix = 0, ref_prefix = 0, resume_cursor, cont_frames ;
	float *ref_out ;
	CallRec *recs ;
	int nrec = 0 ;
	pid_t pid ;
	int status ;
	FILE *fp ;
	long file_bytes ;
	float *child_out ;

	if (tmpdir == NULL)
		tmpdir = "/tmp" ;
	if (child_exe == NULL)
		child_exe = ".task-build/checkpoint-child" ;

	snprintf (ck_path, sizeof (ck_path), "%s/ckpt-%d.snap", tmpdir, (int) getpid ()) ;
	snprintf (out_path, sizeof (out_path), "%s/ckpt-%d.pcm", tmpdir, (int) getpid ()) ;

	in = alloc_signal (TEST_CB_N_FRAMES, channels, seed) ;
	ref_out = (float *) malloc ((size_t) want_frames * channels * sizeof (float)) ;
	recs = (CallRec *) malloc (sizeof (CallRec) * MAX_RECORDS) ;
	TASK_CHECK (in != NULL && ref_out != NULL && recs != NULL) ;

	src = (CbSrc) { in, TEST_CB_N_FRAMES, 0, channels, 0 } ;
	ref_src = (CbSrc) { in, TEST_CB_N_FRAMES, 0, channels, 0 } ;
	state = src_callback_new (test_callback, converter, channels, &err, &src) ;
	ref_state = src_callback_new (test_callback, converter, channels, &err, &ref_src) ;
	TASK_CHECK (state != NULL && ref_state != NULL) ;

	TASK_CHECK (drive_prefix (state, ratio, save_steps, channels, ref_out, &prefix) == 0) ;
	TASK_CHECK (drive_prefix (ref_state, ratio, save_steps, channels, ref_out, &ref_prefix) == 0) ;
	TASK_CHECK (prefix == ref_prefix) ;

	TASK_CHECK (((struct SRC_STATE_tag *) state)->saved_frames > 0) ;
	need = src_checkpoint_size (state) ;
	TASK_CHECK (need > 0) ;
	blob = (unsigned char *) malloc (need) ;
	TASK_CHECK (blob != NULL) ;
	TASK_CHECK (src_checkpoint_write (state, blob, need) == 0) ;

	fp = fopen (ck_path, "wb") ;
	TASK_CHECK (fp != NULL) ;
	TASK_CHECK ((long) fwrite (blob, 1, need, fp) == (long) need) ;
	fclose (fp) ;

	resume_cursor = src.cursor - ((struct SRC_STATE_tag *) state)->saved_frames ;
	cont_frames = want_frames - prefix ;
	TASK_CHECK (cont_frames > 0) ;
	TASK_CHECK (drive_continuation (ref_state, ratio, save_steps, channels,
					ref_out + prefix * channels, cont_frames,
					&(long) { 0 }, recs, &nrec) == 0) ;

	src_delete (state) ;
	src_delete (ref_state) ;
	free (blob) ;
	free (in) ;

	pid = fork () ;
	TASK_CHECK (pid >= 0) ;
	if (pid == 0)
	{
		char arg_seed [32], arg_n [32], arg_ch [32], arg_cur [32] ;
		char arg_ratio [32], arg_want [32], arg_step [32] ;

		snprintf (arg_seed, sizeof (arg_seed), "%u", seed) ;
		snprintf (arg_n, sizeof (arg_n), "%ld", TEST_CB_N_FRAMES) ;
		snprintf (arg_ch, sizeof (arg_ch), "%d", channels) ;
		snprintf (arg_cur, sizeof (arg_cur), "%ld", resume_cursor) ;
		snprintf (arg_ratio, sizeof (arg_ratio), "%.17g", ratio) ;
		snprintf (arg_want, sizeof (arg_want), "%ld", cont_frames) ;
		snprintf (arg_step, sizeof (arg_step), "%d", save_steps) ;
		execl (child_exe, child_exe, ck_path, out_path, arg_seed, arg_n, arg_ch,
				arg_cur, arg_ratio, arg_want, arg_step, (char *) NULL) ;
		_exit (127) ;
	} ;
	TASK_CHECK (waitpid (pid, &status, 0) == pid) ;
	TASK_CHECK (WIFEXITED (status) && WEXITSTATUS (status) == 0) ;

	fp = fopen (out_path, "rb") ;
	TASK_CHECK (fp != NULL) ;
	TASK_CHECK (fseek (fp, 0, SEEK_END) == 0) ;
	file_bytes = ftell (fp) ;
	TASK_CHECK (file_bytes == cont_frames * channels * (long) sizeof (float)) ;
	rewind (fp) ;
	child_out = (float *) malloc ((size_t) file_bytes) ;
	TASK_CHECK (child_out != NULL) ;
	TASK_CHECK ((long) fread (child_out, 1, file_bytes, fp) == file_bytes) ;
	fclose (fp) ;

	TASK_CHECK (memcmp (child_out, ref_out + prefix * channels, (size_t) file_bytes) == 0) ;

	free (child_out) ;
	free (ref_out) ;
	free (recs) ;
	unlink (ck_path) ;
	unlink (out_path) ;
	return 0 ;
} /* callback_subprocess */

/*----------------------------------------------------------------------------------------
** Format group.
*/

static int
format_unsupported (void)
{
	int err = 0 ;
	SRC_STATE *linear = src_new (SRC_LINEAR, 2, &err) ;
	SRC_STATE *zoh = src_new (SRC_ZERO_ORDER_HOLD, 2, &err) ;
	char scratch [64] ;

	TASK_CHECK (linear != NULL && zoh != NULL) ;
	TASK_CHECK (src_checkpoint_size (NULL) == 0) ;
	TASK_CHECK (src_checkpoint_size (linear) == 0) ;
	TASK_CHECK (src_checkpoint_size (zoh) == 0) ;
	TASK_CHECK (src_checkpoint_write (linear, scratch, sizeof (scratch)) != 0) ;
	TASK_CHECK (src_checkpoint_write (zoh, scratch, sizeof (scratch)) != 0) ;
	TASK_CHECK (src_checkpoint_write (NULL, scratch, sizeof (scratch)) != 0) ;

	src_delete (linear) ;
	src_delete (zoh) ;
	return 0 ;
} /* format_unsupported */

static int
format_null_and_basic (void)
{
	int err = 99 ;
	SRC_STATE *state ;
	size_t need ;
	unsigned char *blob ;

	state = src_new (SRC_SINC_FASTEST, 2, &err) ;
	TASK_CHECK (state != NULL) ;
	need = src_checkpoint_size (state) ;
	TASK_CHECK (need > 0) ;
	blob = (unsigned char *) malloc (need) ;
	TASK_CHECK (blob != NULL) ;
	TASK_CHECK (src_checkpoint_write (state, blob, need) == 0) ;

	TASK_CHECK (src_checkpoint_read (NULL, need, NULL, NULL, &err) == NULL && err != 0) ;
	TASK_CHECK (src_checkpoint_read (blob, 0, NULL, NULL, &err) == NULL && err != 0) ;
	TASK_CHECK (src_checkpoint_write (state, NULL, need) != 0) ;

	/* A round tripped process snapshot is usable. */
	{
		SRC_STATE *re = src_checkpoint_read (blob, need, NULL, NULL, &err) ;
		TASK_CHECK (re != NULL && err == 0) ;
		TASK_CHECK (src_get_channels (re) == 2) ;
		src_delete (re) ;
	} ;

	src_delete (state) ;
	free (blob) ;
	return 0 ;
} /* format_null_and_basic */

static int
format_corrupt (void)
{
	int err ;
	SRC_STATE *state ;
	size_t need ;
	unsigned char *blob, *bad ;
	uint16_t u16 ;
	uint32_t u32 ;

	state = src_new (SRC_SINC_MEDIUM_QUALITY, 4, &err) ;
	TASK_CHECK (state != NULL) ;
	need = src_checkpoint_size (state) ;
	blob = (unsigned char *) malloc (need) ;
	bad = (unsigned char *) malloc (need + 16) ;
	TASK_CHECK (blob != NULL && bad != NULL) ;
	TASK_CHECK (src_checkpoint_write (state, blob, need) == 0) ;

	/* Bad magic. */
	memcpy (bad, blob, need) ;
	bad [0] = 'X' ;
	TASK_CHECK (src_checkpoint_read (bad, need, NULL, NULL, &err) == NULL && err != 0) ;

	/* Unsupported version. */
	memcpy (bad, blob, need) ;
	u16 = 2 ;
	bad [4] = (unsigned char) u16 ;
	bad [5] = (unsigned char) (u16 >> 8) ;
	TASK_CHECK (src_checkpoint_read (bad, need, NULL, NULL, &err) == NULL && err != 0) ;

	/* Converter number that no build provides. */
	memcpy (bad, blob, need) ;
	u16 = 9 ;
	bad [6] = (unsigned char) u16 ;
	bad [7] = (unsigned char) (u16 >> 8) ;
	TASK_CHECK (src_checkpoint_read (bad, need, NULL, NULL, &err) == NULL && err != 0) ;

	/* Zero channels. */
	memcpy (bad, blob, need) ;
	bad [8] = 0 ;
	bad [9] = 0 ;
	TASK_CHECK (src_checkpoint_read (bad, need, NULL, NULL, &err) == NULL && err != 0) ;

	/* Invalid mode. */
	memcpy (bad, blob, need) ;
	u16 = 7 ;
	bad [10] = (unsigned char) u16 ;
	bad [11] = (unsigned char) (u16 >> 8) ;
	TASK_CHECK (src_checkpoint_read (bad, need, NULL, NULL, &err) == NULL && err != 0) ;

	/* NaN in the ratio field (high 32 bits of the little endian double). */
	memcpy (bad, blob, need) ;
	u32 = 0x7ff80000u ;
	for (int k = 0 ; k < 4 ; k++)
		bad [16 + k] = (unsigned char) (u32 >> (8 * k)) ;
	TASK_CHECK (src_checkpoint_read (bad, need, NULL, NULL, &err) == NULL && err != 0) ;

	/* Truncation at several points. */
	TASK_CHECK (src_checkpoint_read (blob, 4, NULL, NULL, &err) == NULL && err != 0) ;
	TASK_CHECK (src_checkpoint_read (blob, 31, NULL, NULL, &err) == NULL && err != 0) ;
	TASK_CHECK (src_checkpoint_read (blob, need / 2, NULL, NULL, &err) == NULL && err != 0) ;
	TASK_CHECK (src_checkpoint_read (blob, need - 1, NULL, NULL, &err) == NULL && err != 0) ;

	/* Trailing garbage: the payload no longer ends at the buffer's end. */
	memcpy (bad, blob, need) ;
	bad [need] = 0 ;
	TASK_CHECK (src_checkpoint_read (bad, need + 1, NULL, NULL, &err) == NULL && err != 0) ;

	src_delete (state) ;
	free (blob) ;
	free (bad) ;
	return 0 ;
} /* format_corrupt */

static int
format_payload_contradiction (void)
{
	int err ;
	SRC_STATE *state ;
	size_t need ;
	unsigned char *blob, *bad ;

	/* Payload layout: 32 byte header, 4 byte payload length prefix, then
	** b_current @36, b_end @40, b_real_end @44, b_len @48.
	*/
	state = src_new (SRC_SINC_BEST_QUALITY, 2, &err) ;
	TASK_CHECK (state != NULL) ;
	need = src_checkpoint_size (state) ;
	blob = (unsigned char *) malloc (need) ;
	bad = (unsigned char *) malloc (need) ;
	TASK_CHECK (blob != NULL && bad != NULL) ;
	TASK_CHECK (src_checkpoint_write (state, blob, need) == 0) ;

	/* Tampered b_len must not match this converter/channel pair. */
	memcpy (bad, blob, need) ;
	bad [48] = (unsigned char) (bad [48] + 1) ;
	TASK_CHECK (src_checkpoint_read (bad, need, NULL, NULL, &err) == NULL && err != 0) ;

	/* b_current must stay channel aligned. */
	memcpy (bad, blob, need) ;
	bad [36] = 1 ;
	TASK_CHECK (src_checkpoint_read (bad, need, NULL, NULL, &err) == NULL && err != 0) ;

	/* b_end outside the ring. */
	memcpy (bad, blob, need) ;
	bad [40] = (unsigned char) 0xff ;
	bad [41] = (unsigned char) 0xff ;
	TASK_CHECK (src_checkpoint_read (bad, need, NULL, NULL, &err) == NULL && err != 0) ;

	/* A clean snapshot still restores. */
	{
		SRC_STATE *ok = src_checkpoint_read (blob, need, NULL, NULL, &err) ;
		TASK_CHECK (ok != NULL && err == 0) ;
		src_delete (ok) ;
	} ;

	src_delete (state) ;
	free (blob) ;
	free (bad) ;
	return 0 ;
} /* format_payload_contradiction */

static int
format_callback_modes (void)
{
	int err ;
	float *in ;
	CbSrc src ;
	SRC_STATE *cb_state, *proc_state, *ok ;
	size_t need_cb, need_proc ;
	unsigned char *cb_blob, *proc_blob ;
	float tmp [32] ;

	in = alloc_signal (TEST_CB_N_FRAMES, 2, 0x77u) ;
	TASK_CHECK (in != NULL) ;
	src = (CbSrc) { in, TEST_CB_N_FRAMES, 0, 2, 0 } ;

	cb_state = src_callback_new (test_callback, SRC_SINC_FASTEST, 2, &err, &src) ;
	TASK_CHECK (cb_state != NULL) ;
	/* The oversized first batch cannot fit into the ring, so frames remain
	** in state owned storage after the read.
	*/
	TASK_CHECK (src_callback_read (cb_state, 1.2, 7, tmp) == 7) ;
	TASK_CHECK (((struct SRC_STATE_tag *) cb_state)->saved_frames > 0) ;

	need_cb = src_checkpoint_size (cb_state) ;
	cb_blob = (unsigned char *) malloc (need_cb) ;
	TASK_CHECK (cb_blob != NULL) ;
	TASK_CHECK (src_checkpoint_write (cb_state, cb_blob, need_cb) == 0) ;

	/* Callback snapshot requires a non-NULL callback. */
	TASK_CHECK (src_checkpoint_read (cb_blob, need_cb, NULL, NULL, &err) == NULL && err != 0) ;
	ok = src_checkpoint_read (cb_blob, need_cb, test_callback, &src, &err) ;
	TASK_CHECK (ok != NULL && err == 0) ;
	src_delete (ok) ;

	proc_state = src_new (SRC_SINC_FASTEST, 2, &err) ;
	TASK_CHECK (proc_state != NULL) ;
	need_proc = src_checkpoint_size (proc_state) ;
	proc_blob = (unsigned char *) malloc (need_proc) ;
	TASK_CHECK (proc_blob != NULL) ;
	TASK_CHECK (src_checkpoint_write (proc_state, proc_blob, need_proc) == 0) ;

	/* Process snapshot must be restored with a NULL callback. */
	TASK_CHECK (src_checkpoint_read (proc_blob, need_proc, test_callback, &src, &err) == NULL && err != 0) ;
	ok = src_checkpoint_read (proc_blob, need_proc, NULL, NULL, &err) ;
	TASK_CHECK (ok != NULL && err == 0) ;
	src_delete (ok) ;

	/* A callback blob rewritten to claim process mode contradicts its own
	** non-zero remainder count and must be rejected.
	*/
	cb_blob [10] = 0 ;
	cb_blob [11] = 0 ;
	TASK_CHECK (src_checkpoint_read (cb_blob, need_cb, NULL, NULL, &err) == NULL && err != 0) ;

	src_delete (cb_state) ;
	src_delete (proc_state) ;
	free (cb_blob) ;
	free (proc_blob) ;
	free (in) ;
	return 0 ;
} /* format_callback_modes */

/*----------------------------------------------------------------------------------------
** Registration.
*/

const struct task_case task_cases [] =
{
	{ "format", "unsupported_converters",	format_unsupported },
	{ "format", "null_arguments",			format_null_and_basic },
	{ "format", "corrupt_headers",			format_corrupt },
	{ "format", "payload_contradictions",	format_payload_contradiction },
	{ "format", "callback_mode_rules",		format_callback_modes },

	{ "resume", "sinc_fastest_all_channels",	resume_fastest },
	{ "resume", "sinc_medium_all_channels",		resume_medium },
	{ "resume", "sinc_best_all_channels",		resume_best },

	{ "callback", "fastest_stereo_remainder",	callback_fastest },
	{ "callback", "medium_seven_channel",		callback_multichannel },
	{ "callback", "best_mono",					callback_best_mono },
	{ "callback", "subprocess_resume",			callback_subprocess },
} ;

const size_t task_case_count = sizeof (task_cases) / sizeof (task_cases [0]) ;

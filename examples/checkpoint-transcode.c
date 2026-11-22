/*
** Persistent checkpoint example: pausing and resuming a long resampling
** transcoding job, optionally across processes.
**
** The program works on raw interleaved float32 files so that it has no
** dependency on any audio file library. It has three modes:
**
**     checkpoint-transcode reference input.raw output.raw
**     checkpoint-transcode pause     input.raw partial.raw snapshot.bin
**     checkpoint-transcode resume    snapshot.bin partial.raw \
**                                    remaining.raw output.raw
**
** "reference" runs the whole conversion in one process. "pause" converts the
** first part of the input, writes the resumable state to snapshot.bin and
** exits. "resume" runs in a brand new process, reads the snapshot and
** finishes the job. The remaining.raw file starts at the input frame right
** after the last frame consumed by the pausing process; the caller is
** responsible for positioning that cursor, since consumed audio is never
** stored in the checkpoint. Concatenating the partial output and the resume
** output gives the exact bytes of the reference output.
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <samplerate.h>

#define	CHANNELS		2
#define	SRC_RATIO		(48000.0 / 44100.0)
#define	CHUNK_FRAMES	512
#define	OUT_CAP_FRAMES	2048
#define	PAUSE_AFTER		7	/* pause after this many input chunks */

static long
read_frames (FILE *fp, float *buffer, long frames)
{
	size_t wanted = (size_t) frames * CHANNELS ;
	size_t got ;

	got = fread (buffer, sizeof (float), wanted, fp) ;
	return (long) (got / CHANNELS) ;
} /* read_frames */

static int
write_frames (FILE *fp, const float *buffer, long frames)
{
	size_t count = (size_t) frames * CHANNELS ;

	return frames == 0 || fwrite (buffer, sizeof (float), count, fp) == count ;
} /* write_frames */

/* Drive one process-mode state to end of input and flush the filter tail. */
static int
convert_stream (SRC_STATE *state, FILE *in, FILE *out)
{
	float in_buf [CHUNK_FRAMES * CHANNELS] ;
	float out_buf [OUT_CAP_FRAMES * CHANNELS] ;
	SRC_DATA data ;
	long chunk ;

	while ((chunk = read_frames (in, in_buf, CHUNK_FRAMES)) > 0)
	{
		memset (&data, 0, sizeof (data)) ;
		data.data_in = in_buf ;
		data.input_frames = chunk ;
		data.data_out = out_buf ;
		data.output_frames = OUT_CAP_FRAMES ;
		data.src_ratio = SRC_RATIO ;
		data.end_of_input = (chunk < CHUNK_FRAMES) ;

		if (src_process (state, &data) != 0)
		{	fprintf (stderr, "src_process failed: %s\n",
					src_strerror (src_error (state))) ;
			return 1 ;
			} ;

		if (!write_frames (out, out_buf, data.output_frames_gen))
		{	fprintf (stderr, "output write failed\n") ;
			return 1 ;
			} ;

		if (data.end_of_input)
			break ;
	} ;

	/* Drain the filter after the announced end of input. */
	memset (&data, 0, sizeof (data)) ;
	data.data_out = out_buf ;
	data.output_frames = OUT_CAP_FRAMES ;
	data.src_ratio = SRC_RATIO ;
	data.end_of_input = 1 ;
	while (src_process (state, &data) == 0 && data.output_frames_gen > 0)
	{
		if (!write_frames (out, out_buf, data.output_frames_gen))
			return 1 ;
		data.data_out = out_buf ;
		data.output_frames = OUT_CAP_FRAMES ;
	} ;

	return 0 ;
} /* convert_stream */

static int
run_reference (const char *in_path, const char *out_path)
{
	FILE *in = fopen (in_path, "rb") ;
	FILE *out = fopen (out_path, "wb") ;
	SRC_STATE *state ;
	int err, rc ;

	if (in == NULL || out == NULL)
	{	fprintf (stderr, "cannot open input or output file\n") ;
		return 1 ;
		} ;

	state = src_new (SRC_SINC_FASTEST, CHANNELS, &err) ;
	if (state == NULL)
	{	fprintf (stderr, "src_new failed: %s\n", src_strerror (err)) ;
		return 1 ;
		} ;

	rc = convert_stream (state, in, out) ;

	src_delete (state) ;
	fclose (in) ;
	fclose (out) ;

	if (rc == 0)
		printf ("reference conversion written to %s\n", out_path) ;
	return rc ;
} /* run_reference */

static int
run_pause (const char *in_path, const char *partial_path, const char *snap_path)
{
	FILE *in = fopen (in_path, "rb") ;
	FILE *out = fopen (partial_path, "wb") ;
	FILE *snap ;
	SRC_STATE *state ;
	SRC_DATA data ;
	float in_buf [CHUNK_FRAMES * CHANNELS] ;
	float out_buf [OUT_CAP_FRAMES * CHANNELS] ;
	long chunk ;
	int err, chunks = 0 ;
	size_t need ;
	unsigned char *blob ;

	if (in == NULL || out == NULL)
	{	fprintf (stderr, "cannot open input or partial file\n") ;
		return 1 ;
		} ;

	state = src_new (SRC_SINC_FASTEST, CHANNELS, &err) ;
	if (state == NULL)
	{	fprintf (stderr, "src_new failed: %s\n", src_strerror (err)) ;
		return 1 ;
		} ;

	while (chunks < PAUSE_AFTER && (chunk = read_frames (in, in_buf, CHUNK_FRAMES)) > 0)
	{
		memset (&data, 0, sizeof (data)) ;
		data.data_in = in_buf ;
		data.input_frames = chunk ;
		data.data_out = out_buf ;
		data.output_frames = OUT_CAP_FRAMES ;
		data.src_ratio = SRC_RATIO ;

		if (src_process (state, &data) != 0 ||
				!write_frames (out, out_buf, data.output_frames_gen))
			return 1 ;
		chunks ++ ;
	} ;

	need = src_checkpoint_size (state) ;
	if (need == 0)
	{	fprintf (stderr, "checkpointing is not supported by this converter\n") ;
		return 1 ;
		} ;

	blob = (unsigned char *) malloc (need) ;
	if (blob == NULL)
		return 1 ;

	if ((err = src_checkpoint_write (state, blob, need)) != 0)
	{	fprintf (stderr, "src_checkpoint_write failed: %s\n", src_strerror (err)) ;
		return 1 ;
		} ;

	snap = fopen (snap_path, "wb") ;
	if (snap == NULL || fwrite (blob, 1, need, snap) != need)
	{	fprintf (stderr, "cannot write snapshot\n") ;
		return 1 ;
		} ;
	fclose (snap) ;

	/* The source instance, the input file and the blob can all go away now. */
	free (blob) ;
	src_delete (state) ;
	fclose (in) ;
	fclose (out) ;
	printf ("paused after %d chunks; snapshot written to %s\n", chunks, snap_path) ;
	return 0 ;
} /* run_pause */

static int
run_resume (const char *snap_path, const char *partial_path,
			const char *remaining_path, const char *out_path)
{
	FILE *snap = fopen (snap_path, "rb") ;
	FILE *partial = fopen (partial_path, "rb") ;
	FILE *remaining = fopen (remaining_path, "rb") ;
	FILE *out = fopen (out_path, "wb") ;
	SRC_STATE *state ;
	int err, rc ;
	unsigned char *blob ;
	long snap_bytes ;
	float copy_buf [256 * CHANNELS] ;
	size_t got ;

	if (snap == NULL || partial == NULL || remaining == NULL || out == NULL)
	{	fprintf (stderr, "cannot open snapshot, partial, remaining or output file\n") ;
		return 1 ;
		} ;

	if (fseek (snap, 0, SEEK_END) != 0 || (snap_bytes = ftell (snap)) <= 0)
	{	fprintf (stderr, "bad snapshot size\n") ;
		return 1 ;
		} ;
	rewind (snap) ;

	blob = (unsigned char *) malloc ((size_t) snap_bytes) ;
	if (blob == NULL ||
			fread (blob, 1, (size_t) snap_bytes, snap) != (size_t) snap_bytes)
	{	fprintf (stderr, "cannot read snapshot\n") ;
		return 1 ;
		} ;
	fclose (snap) ;

	/* Process mode snapshot: restore with a NULL callback. */
	state = src_checkpoint_read (blob, (size_t) snap_bytes, NULL, NULL, &err) ;
	free (blob) ;
	if (state == NULL)
	{	fprintf (stderr, "src_checkpoint_read failed: %s\n", src_strerror (err)) ;
		return 1 ;
		} ;
	if (src_get_channels (state) != CHANNELS)
	{	fprintf (stderr, "channel count mismatch in snapshot\n") ;
		return 1 ;
		} ;

	/* Carry the PCM produced before the pause into the final output file. */
	while ((got = fread (copy_buf, sizeof (float),
					sizeof (copy_buf) / sizeof (float), partial)) > 0)
	{
		if (fwrite (copy_buf, sizeof (float), got, out) != got)
		{	fprintf (stderr, "partial output copy failed\n") ;
			return 1 ;
			} ;
	} ;
	fclose (partial) ;

	/* Continue at the same resampling position using the remaining input. */
	rc = convert_stream (state, remaining, out) ;

	src_delete (state) ;
	fclose (remaining) ;
	fclose (out) ;

	if (rc == 0)
		printf ("resumed conversion finished; output written to %s\n", out_path) ;
	return rc ;
} /* run_resume */

int
main (int argc, char **argv)
{
	if (argc == 4 && strcmp (argv [1], "reference") == 0)
		return run_reference (argv [2], argv [3]) ;

	if (argc == 5 && strcmp (argv [1], "pause") == 0)
		return run_pause (argv [2], argv [3], argv [4]) ;

	if (argc == 6 && strcmp (argv [1], "resume") == 0)
		return run_resume (argv [2], argv [3], argv [4], argv [5]) ;

	fprintf (stderr,
		"usage:\n"
		"  %s reference input.raw output.raw\n"
		"  %s pause input.raw partial.raw snapshot.bin\n"
		"  %s resume snapshot.bin partial.raw remaining.raw output.raw\n",
		argv [0], argv [0], argv [0]) ;
	return 2 ;
} /* main */

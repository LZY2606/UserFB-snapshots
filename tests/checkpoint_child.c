/*
** Helper for the cross-process checkpoint test.
**
** Invoked by tests/checkpoint_test.c as
**
**     checkpoint-child snapshot.pcm output.pcm seed frames channels \
**                     cursor ratio want_frames step_offset
**
** It reads a checkpoint file written by another process, rebuilds the same
** deterministic input signal, positions a fresh callback at the saved
** boundary and continues the conversion with the same output capacity
** schedule. The continuation PCM is written verbatim to the output file.
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <samplerate.h>
#include "checkpoint_util.h"

typedef struct
{	const float	*data ;
	long		N, cursor ;
	int			channels ;
} ChildSrc ;

static long
child_callback (void *ctx, float **data)
{
	ChildSrc *c = (ChildSrc *) ctx ;
	long take ;

	if (c->cursor >= c->N)
		return 0 ;

	take = c->N - c->cursor ;
	if (take > CKPT_CB_BATCH)
		take = CKPT_CB_BATCH ;

	*data = (float *) (c->data + c->cursor * c->channels) ;
	c->cursor += take ;

	return take ;
} /* child_callback */

static unsigned char *
read_whole_file (const char *path, size_t *length)
{
	FILE *fp = fopen (path, "rb") ;
	unsigned char *buffer ;
	long file_len ;

	if (fp == NULL)
		return NULL ;

	if (fseek (fp, 0, SEEK_END) != 0)
	{	fclose (fp) ;
		return NULL ;
		} ;

	file_len = ftell (fp) ;
	if (file_len <= 0)
	{	fclose (fp) ;
		return NULL ;
		} ;

	rewind (fp) ;
	buffer = (unsigned char *) malloc ((size_t) file_len) ;
	if (buffer == NULL)
	{	fclose (fp) ;
		return NULL ;
		} ;

	if (fread (buffer, 1, (size_t) file_len, fp) != (size_t) file_len)
	{	free (buffer) ;
		fclose (fp) ;
		return NULL ;
		} ;

	fclose (fp) ;
	*length = (size_t) file_len ;
	return buffer ;
} /* read_whole_file */

int
main (int argc, char **argv)
{
	const char *snap_path, *out_path ;
	uint32_t seed ;
	long N, cursor, want_frames ;
	int channels, step_offset ;
	double ratio ;
	size_t snap_len ;
	unsigned char *snap ;
	float *input, *output ;
	ChildSrc src ;
	SRC_STATE *state ;
	int err ;
	long total = 0 ;
	int step = 0 ;
	FILE *out ;

	if (argc != 10)
	{	fprintf (stderr, "usage: %s snap out seed N ch cursor ratio want step\n", argv [0]) ;
		return 2 ;
		} ;

	snap_path = argv [1] ;
	out_path = argv [2] ;
	seed = (uint32_t) strtoul (argv [3], NULL, 10) ;
	N = (long) strtol (argv [4], NULL, 10) ;
	channels = (int) strtol (argv [5], NULL, 10) ;
	cursor = (long) strtol (argv [6], NULL, 10) ;
	ratio = strtod (argv [7], NULL) ;
	want_frames = (long) strtol (argv [8], NULL, 10) ;
	step_offset = (int) strtol (argv [9], NULL, 10) ;

	if (N <= 0 || channels < 1 || want_frames <= 0)
		return 2 ;

	snap = read_whole_file (snap_path, &snap_len) ;
	if (snap == NULL)
		return 3 ;

	input = (float *) malloc ((size_t) N * channels * sizeof (float)) ;
	output = (float *) malloc ((size_t) want_frames * channels * sizeof (float)) ;
	if (input == NULL || output == NULL)
		return 4 ;
	ckpt_gen_signal (input, N, channels, seed) ;

	src = (ChildSrc) { input, N, cursor, channels } ;

	state = src_checkpoint_read (snap, snap_len, child_callback, &src, &err) ;
	if (state == NULL)
	{	fprintf (stderr, "src_checkpoint_read failed: %d\n", err) ;
		return 5 ;
		} ;

	while (total < want_frames)
	{
		long cap = ckpt_cap_at (step + step_offset) ;
		long got ;

		if (cap > want_frames - total)
			cap = want_frames - total ;

		got = src_callback_read (state, ratio, cap, output + total * channels) ;
		if (got != cap)
		{	fprintf (stderr, "short read: got %ld want %ld at step %d\n", got, cap, step) ;
			return 6 ;
			} ;

		total += got ;
		step ++ ;
	} ;

	out = fopen (out_path, "wb") ;
	if (out == NULL)
		return 7 ;
	if ((long) fwrite (output, sizeof (float), total * channels, out) != total * channels)
	{	fclose (out) ;
		return 8 ;
		} ;
	fclose (out) ;

	src_delete (state) ;
	free (snap) ;
	free (input) ;
	free (output) ;
	return 0 ;
} /* main */

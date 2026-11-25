/* Sample-accurate timed playback demo.
 *
 * Schedules note events at absolute output-sample positions while rendering
 * an XM or IT module, showing how a host can react to its audio callback
 * pulling arbitrary buffer sizes without losing timing precision.
 *
 * This file is in the public domain.
 */

#include <stdio.h>
#include <stdlib.h>
#include <xmp.h>

#define RATE	44100
#define FRAMES	1024

int main(int argc, char **argv)
{
	xmp_context ctx;
	struct xmp_module_info mi;
	short buffer[FRAMES * 2]; /* stereo 16-bit */
	unsigned long long position = 0;
	int ret;

	if (argc < 2) {
		fprintf(stderr, "usage: %s module.xm|module.it\n", argv[0]);
		return 1;
	}

	ctx = xmp_create_context();
	if (xmp_load_module(ctx, argv[1]) < 0) {
		fprintf(stderr, "cannot load %s\n", argv[1]);
		xmp_free_context(ctx);
		return 1;
	}

	/* Signed 16-bit stereo software mixing; XM and IT only. */
	ret = xmp_start_timed_player(ctx, RATE, 0);
	if (ret < 0) {
		fprintf(stderr, "timed player rejected this module/format: %d\n",
			ret);
		xmp_release_module(ctx);
		xmp_free_context(ctx);
		return 1;
	}

	xmp_get_module_info(ctx, &mi);
	printf("%s (%s)\n", mi.mod->name, mi.mod->type);

	/* A note on one second in and a note cut two seconds in. The event
	 * time is measured in delivered frames, independent of how many
	 * buffers the audio callback requests. */
	{
		struct xmp_event note = { 50, 1, 40, 0, 0, 0, 0, 0 };
		struct xmp_event cut  = { XMP_KEY_CUT, 0, 0, 0, 0, 0, 0, 0 };

		if (xmp_schedule_event(ctx, RATE, 0, &note) < 0)
			fprintf(stderr, "could not schedule note\n");
		if (xmp_schedule_event(ctx, 2ULL * RATE, 0, &cut) < 0)
			fprintf(stderr, "could not schedule cut\n");
	}

	/* Pull whatever chunk size the host wants; timing stays exact. */
	while (position < 5ULL * RATE) {
		int frames = xmp_play_timed_buffer(ctx, buffer, FRAMES);
		if (frames <= 0)
			break;
		position += frames;
		/* A real host would hand "buffer" to the device here. */
	}

	xmp_end_player(ctx);
	xmp_release_module(ctx);
	xmp_free_context(ctx);
	return 0;
}

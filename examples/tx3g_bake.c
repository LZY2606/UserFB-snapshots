/*
 * tx3g_bake.c - Convert krok/highlight tx3g samples to ordinary styled tx3g.
 *
 * Usage: tx3g_bake INPUT.mp4 OUTPUT.mp4 TRACK_NUMBER
 */
#include <gpac/isomedia.h>

#include <stdio.h>
#include <stdlib.h>
#include <limits.h>

int main(int argc, char **argv)
{
	GF_ISOFile *src = NULL, *dst = NULL;
	GF_Err e;
	u32 track, out_track = 0;
	char *end = NULL;
	unsigned long track_value;

	if (argc != 4) {
		fprintf(stderr, "usage: %s INPUT OUTPUT TRACK\n", argv[0]);
		return 2;
	}
	track_value = strtoul(argv[3], &end, 10);
	if (!argv[3][0] || (end && *end) || track_value == 0 || track_value > UINT_MAX) {
		fprintf(stderr, "invalid track number: %s\n", argv[3]);
		return 2;
	}
	track = (u32)track_value;

	e = gf_sys_init(GF_MemTrackerNone, "n");
	if (e != GF_OK) {
		fprintf(stderr, "GPAC initialization failed: %s\n", gf_error_to_string(e));
		return 1;
	}

	src = gf_isom_open(argv[1], GF_ISOM_OPEN_READ, NULL);
	if (!src) {
		fprintf(stderr, "cannot open source %s: %s\n", argv[1],
		        gf_error_to_string(gf_isom_last_error(NULL)));
		goto fail;
	}

	dst = gf_isom_open(argv[2], GF_ISOM_WRITE_EDIT, NULL);
	if (!dst) {
		fprintf(stderr, "cannot open destination %s: %s\n", argv[2],
		        gf_error_to_string(gf_isom_last_error(NULL)));
		goto fail;
	}

	e = gf_isom_text_bake_karaoke(src, track, dst, &out_track);
	if (e != GF_OK) {
		fprintf(stderr, "karaoke baking failed: %s\n", gf_error_to_string(e));
		goto fail;
	}

	e = gf_isom_close(dst);
	dst = NULL;
	if (e != GF_OK) {
		fprintf(stderr, "cannot save destination: %s\n", gf_error_to_string(e));
		goto fail;
	}
	e = gf_isom_close(src);
	src = NULL;
	if (e != GF_OK) {
		fprintf(stderr, "cannot close source: %s\n", gf_error_to_string(e));
		goto fail;
	}

	printf("baked track %u from source track %u into %s\n", out_track, track, argv[2]);
	gf_sys_close();
	return 0;

fail:
	if (dst) gf_isom_close(dst);
	if (src) gf_isom_close(src);
	gf_sys_close();
	return 1;
}

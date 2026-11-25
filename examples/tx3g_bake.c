/*
 *			GPAC - Multimedia Framework C SDK
 *
 *			Authors: Jean Le Feuvre
 *			Copyright (c) Telecom ParisTech 2000-2025
 *					All rights reserved
 *
 *  This file is part of GPAC / ISO Media File Format sub-project
 *
 *  GPAC is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  GPAC is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

/*
 *	tx3g_bake: converts the karaoke (krok) / highlight (hlit, hclr) modifiers of a tx3g
 *	subtitle track into plain tx3g styles, so that players only supporting regular text
 *	styles can render the karaoke coloring.
 *
 *	usage: tx3g_bake <input.mp4> <output.mp4> <track_number>
 */

#include <gpac/isomedia.h>
#include <gpac/tools.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
	GF_ISOFile *src, *dst;
	GF_Err e;
	u32 track, out_track = 0;

	if (argc != 4) {
		fprintf(stderr, "usage: %s <input.mp4> <output.mp4> <track_number>\n", argv[0]);
		return 1;
	}
	track = (u32) atoi(argv[3]);
	if (!track) {
		fprintf(stderr, "invalid track number '%s'\n", argv[3]);
		return 1;
	}

	e = gf_sys_init(GF_MemTrackerNone, "n");
	if (e != GF_OK) {
		fprintf(stderr, "failed to initialize GPAC: %s\n", gf_error_to_string(e));
		return 1;
	}

	src = gf_isom_open(argv[1], GF_ISOM_OPEN_READ, NULL);
	if (!src) {
		fprintf(stderr, "cannot open input file '%s'\n", argv[1]);
		gf_sys_close();
		return 1;
	}
	dst = gf_isom_open(argv[2], GF_ISOM_WRITE_EDIT, NULL);
	if (!dst) {
		fprintf(stderr, "cannot open output file '%s' for writing\n", argv[2]);
		gf_isom_close(src);
		gf_sys_close();
		return 1;
	}

	e = gf_isom_text_bake_karaoke(src, track, dst, &out_track);
	if (e != GF_OK) {
		fprintf(stderr, "karaoke baking failed: %s\n", gf_error_to_string(e));
	} else {
		printf("baked karaoke of track %u into new track %u of '%s'\n", track, out_track, argv[2]);
	}

	gf_isom_close(src);
	if (gf_isom_close(dst) != GF_OK && e == GF_OK)
		e = GF_IO_ERR;
	gf_sys_close();
	return (e == GF_OK) ? 0 : 1;
}

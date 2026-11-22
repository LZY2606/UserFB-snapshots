/*
	Audio File Library

	Copyright 2016, Michael Pruett <michael@68k.org>

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License along
	with this program; if not, write to the Free Software Foundation, Inc.,
	51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

/*
	spliceaiff.c

	C example of in-place AIFF frame editing.  Opens a named,
	uncompressed AIFF file with the edit mode "r+", erases a range of
	frames and inserts native, big-endian, interleaved PCM silence in
	the same operation.  Markers and instrument loops stored in the file
	are preserved and remapped automatically.

	usage: spliceaiff file.aiff start erase insert
*/

#include <stdio.h>
#include <stdlib.h>

#include "audiofile.h"

int main(int argc, char **argv)
{
	if (argc != 5)
	{
		fprintf(stderr,
			"usage: %s file.aiff start erase insert\n", argv[0]);
		return EXIT_FAILURE;
	}

	const char *path = argv[1];
	AFframecount start = (AFframecount) atol(argv[2]);
	AFframecount eraseFrames = (AFframecount) atol(argv[3]);
	AFframecount insertFrames = (AFframecount) atol(argv[4]);

	/*
		"r+" opens the existing AIFF for reading and in-place editing.
		Only named, seekable, uncompressed AIFF files are accepted;
		other files fail here without being truncated.
	*/
	AFfilehandle file = afOpenFile(path, "r+", NULL);
	if (!file)
	{
		fprintf(stderr, "could not open '%s' for editing\n", path);
		return EXIT_FAILURE;
	}

	AFframecount frameCount = afGetFrameCount(file, AF_DEFAULT_TRACK);
	if (start < 0 || eraseFrames < 0 || insertFrames < 0 ||
		start > frameCount || eraseFrames > frameCount - start)
	{
		fprintf(stderr, "splice range is out of bounds\n");
		afCloseFile(file);
		return EXIT_FAILURE;
	}

	/*
		insertPCM is file-native PCM: big-endian, interleaved bytes with
		no virtual-format conversion.  A zeroed buffer is valid silence
		for every supported two's-complement width.  afGetFrameSize with
		expand3to4 == 0 reports the packed, on-disk frame size.
	*/
	void *insertPCM = NULL;
	if (insertFrames > 0)
	{
		int frameBytes = (int) afGetFrameSize(file, AF_DEFAULT_TRACK, 0);
		if (frameBytes <= 0)
		{
			fprintf(stderr, "could not determine frame size\n");
			afCloseFile(file);
			return EXIT_FAILURE;
		}
		insertPCM = calloc((size_t) insertFrames, (size_t) frameBytes);
		if (!insertPCM)
		{
			fprintf(stderr, "out of memory\n");
			afCloseFile(file);
			return EXIT_FAILURE;
		}
	}

	/* afSpliceFrames returns 0 on success and a negative value on
	   failure; these are AF_SUCCEED/AF_FAIL inside the library. */
	if (afSpliceFrames(file, AF_DEFAULT_TRACK, start, eraseFrames,
			insertPCM, insertFrames) != 0)
	{
		fprintf(stderr, "splice failed; the file is unchanged\n");
		free(insertPCM);
		afCloseFile(file);
		return EXIT_FAILURE;
	}

	free(insertPCM);

	printf("frames: %ld -> %ld\n",
		(long) frameCount,
		(long) afGetFrameCount(file, AF_DEFAULT_TRACK));
	printf("read cursor after splice: %ld\n",
		(long) afTellFrame(file, AF_DEFAULT_TRACK));

	afCloseFile(file);
	return EXIT_SUCCESS;
}

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
	spliceaiff.cpp

	C++ example of in-place AIFF frame editing.  Reads every frame of an
	existing uncompressed AIFF through the virtual format, removes a
	middle section, and appends a tail frame using the native big-endian
	byte stream.  The same handle immediately reflects the new frame
	count, markers and read position without reopening the file.

	usage: spliceaiff file.aiff
*/

#include <audiofile.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

int main(int argc, char **argv)
{
	if (argc != 2)
	{
		std::fprintf(stderr, "usage: %s file.aiff\n", argv[0]);
		return EXIT_FAILURE;
	}

	const char *path = argv[1];

	// Read the file once to establish its geometry.
	AFfilehandle reader = afOpenFile(path, "r", NULL);
	if (!reader)
	{
		std::fprintf(stderr, "could not open '%s' for reading\n", path);
		return EXIT_FAILURE;
	}
	const int channels = afGetChannels(reader, AF_DEFAULT_TRACK);
	const AFframecount frames = afGetFrameCount(reader, AF_DEFAULT_TRACK);
	const int frameBytes =
		(int) afGetFrameSize(reader, AF_DEFAULT_TRACK, 0);
	if (channels <= 0 || frames <= 0 || frameBytes <= 0)
	{
		std::fprintf(stderr, "unsupported or empty input file\n");
		afCloseFile(reader);
		return EXIT_FAILURE;
	}

	// Build one native, big-endian frame of silence for the tail append.
	std::vector<unsigned char> tailFrame(frameBytes, 0);
	afCloseFile(reader);

	// Reopen in edit mode; ineligible files fail without truncation.
	AFfilehandle file = afOpenFile(path, "r+", NULL);
	if (!file)
	{
		std::fprintf(stderr, "file is not eligible for in-place editing\n");
		return EXIT_FAILURE;
	}

	// Erase the middle third, leaving the head and tail intact.
	// afSpliceFrames returns 0 on success and a negative value on failure.
	AFframecount eraseStart = frames / 3;
	AFframecount eraseCount = frames / 3;
	if (afSpliceFrames(file, AF_DEFAULT_TRACK, eraseStart, eraseCount,
			NULL, 0) != 0)
	{
		std::fprintf(stderr, "erase splice failed\n");
		afCloseFile(file);
		return EXIT_FAILURE;
	}

	// Append one frame at the new audio tail.
	AFframecount newCount = afGetFrameCount(file, AF_DEFAULT_TRACK);
	if (afSpliceFrames(file, AF_DEFAULT_TRACK, newCount, 0,
			tailFrame.data(), 1) != 0)
	{
		std::fprintf(stderr, "tail splice failed\n");
		afCloseFile(file);
		return EXIT_FAILURE;
	}

	// The live handle already reports the updated geometry and cursor.
	std::printf("edited '%s': %lld -> %lld frames, cursor=%lld\n",
		path, (long long) frames,
		(long long) afGetFrameCount(file, AF_DEFAULT_TRACK),
		(long long) afTellFrame(file, AF_DEFAULT_TRACK));

	afCloseFile(file);
	return EXIT_SUCCESS;
}

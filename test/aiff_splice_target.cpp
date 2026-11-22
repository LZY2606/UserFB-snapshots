/*
	Audio File Library
	Copyright (C) 2016, Michael Pruett <michael@68k.org>

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.
*/

/*
	aiff_splice_target.cpp

	Target tests for in-place AIFF frame editing (afOpenFile "r+" and
	afSpliceFrames).  AIFF files are assembled and traversed directly as
	bytes in this file rather than through the library under test, so the
	on-disk layout is checked independently.  Cases are registered with
	task_support/check.h in the layout, anchors and live_handle groups;
	there is deliberately no main() here.
*/

#include <audiofile.h>
#include "afinternal.h"
#include "../task_support/check.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>

namespace {

// ---------------------------------------------------------------------------
// Byte-level AIFF construction and traversal (independent of the library).
// ---------------------------------------------------------------------------

typedef std::vector<uint8_t> Bytes;

static void putU8(Bytes &b, uint8_t x) { b.push_back(x); }
static void putU16(Bytes &b, uint16_t x) { b.push_back((uint8_t)(x >> 8)); b.push_back((uint8_t) x); }
static void putU32(Bytes &b, uint32_t x)
{
	for (int i=3; i>=0; --i) b.push_back((uint8_t)(x >> (8*i)));
}
static void putTag(Bytes &b, const char *t)
{
	for (int i=0; i<4; ++i) b.push_back((uint8_t) t[i]);
}
static void putBytes(Bytes &b, const uint8_t *p, size_t n)
{
	b.insert(b.end(), p, p + n);
}

// IEEE 80-bit extended encoding of 48000.0 (40 0E BB 80 00 ...).
static const uint8_t kRate48000[10] =
{ 0x40, 0x0E, 0xBB, 0x80, 0, 0, 0, 0, 0, 0 };

struct ChunkInfo
{
	char tag[5];
	uint32_t size;
	size_t headerOffset;	// absolute file offset of the chunk tag
	bool padded;
};

static uint32_t getU32(const Bytes &b, size_t off)
{
	return ((uint32_t) b[off] << 24) | ((uint32_t) b[off+1] << 16) |
		((uint32_t) b[off+2] << 8) | (uint32_t) b[off+3];
}
static uint16_t getU16(const Bytes &b, size_t off)
{
	return (uint16_t)((b[off] << 8) | b[off+1]);
}

// Walk every top-level FORM chunk, enforcing adjacency and padding.
static bool parseChunks(const Bytes &file, std::vector<ChunkInfo> &out,
	uint32_t &formSize)
{
	out.clear();
	if (file.size() < 12 || memcmp(&file[0], "FORM", 4) != 0 ||
		memcmp(&file[8], "AIFF", 4) != 0)
		return false;
	formSize = getU32(file, 4);
	if ((uint64_t) formSize + 8 != file.size())
		return false;
	size_t pos = 12;
	while (pos + 8 <= file.size())
	{
		ChunkInfo ci;
		memcpy(ci.tag, &file[pos], 4);
		ci.tag[4] = '\0';
		ci.size = getU32(file, pos + 4);
		ci.headerOffset = pos;
		ci.padded = (ci.size & 1) != 0;
		size_t occupancy = 8 + ci.size + (ci.padded ? 1 : 0);
		if (pos + occupancy > file.size())
			return false;
		out.push_back(ci);
		pos += occupancy;
	}
	return pos == file.size();
}

static const ChunkInfo *findChunk(const std::vector<ChunkInfo> &chunks,
	const char *tag)
{
	for (size_t i=0; i<chunks.size(); ++i)
		if (memcmp(chunks[i].tag, tag, 4) == 0)
			return &chunks[i];
	return NULL;
}

static void appendChunk(Bytes &body, const char *tag, const Bytes &chunkData)
{
	putTag(body, tag);
	putU32(body, (uint32_t) chunkData.size());
	putBytes(body, chunkData.data(), chunkData.size());
	if (chunkData.size() & 1)
		putU8(body, 0);
}

static Bytes commChunk(int channels, int frames, int width)
{
	Bytes d;
	putU16(d, (uint16_t) channels);
	putU32(d, (uint32_t) frames);
	putU16(d, (uint16_t) width);
	putBytes(d, kRate48000, 10);
	return d;
}

struct MarkerSpec { int id; uint32_t position; std::string name; };

static Bytes markChunk(const std::vector<MarkerSpec> &markers)
{
	Bytes d;
	putU16(d, (uint16_t) markers.size());
	for (size_t i=0; i<markers.size(); ++i)
	{
		putU16(d, (uint16_t) markers[i].id);
		putU32(d, markers[i].position);
		uint8_t nameLen = (uint8_t) markers[i].name.size();
		putU8(d, nameLen);
		for (size_t k=0; k<markers[i].name.size(); ++k)
			putU8(d, (uint8_t) markers[i].name[k]);
		if ((nameLen % 2) == 0)
			putU8(d, 0);
	}
	return d;
}

struct LoopSpec { uint16_t mode, begin, end; };

// 20-byte INST chunk; instrument parameters carry distinctive values.
static Bytes instChunk(const LoopSpec &sustain, const LoopSpec &release,
	uint8_t baseNote = 61, int8_t detune = -3)
{
	Bytes d;
	putU8(d, baseNote);
	putU8(d, (uint8_t) detune);
	putU8(d, 2); putU8(d, 120);	// low/high note
	putU8(d, 1); putU8(d, 127);	// low/high velocity
	putU16(d, 0);				// gain
	putU16(d, sustain.mode); putU16(d, sustain.begin); putU16(d, sustain.end);
	putU16(d, release.mode); putU16(d, release.begin); putU16(d, release.end);
	return d;
}

static int widthBytes(int width) { return width / 8; }

static void encodeSample(Bytes &b, int width, int32_t value)
{
	int n = widthBytes(width);
	for (int i=n-1; i>=0; --i)
		putU8(b, (uint8_t)(value >> (8*i)));
}

// Deterministic, width-independent logical sample values.
static int32_t sampleValue(int frame, int channel)
{
	return frame * 3 - channel * 2 - 4;
}
static int32_t insertValue(int index, int channel)
{
	return 40 + index * 2 + channel;
}

struct SpliceSpec
{
	int start, eraseFrames, insertFrames;
};

struct FileSpec
{
	int width;
	int channels;
	int frames;
	int ssndOffset;
	bool unknownBefore;
	bool unknownAfter;
	std::vector<MarkerSpec> markers;
	bool hasInst;
	LoopSpec sustain;
	LoopSpec release;
};

static Bytes buildFile(const FileSpec &spec,
	const std::vector<SpliceSpec> *alreadyApplied = NULL)
{
	Bytes body;
	putTag(body, "AIFF");

	if (spec.unknownBefore)
	{
		Bytes odd;
		odd.push_back(0x11); odd.push_back(0x22); odd.push_back(0x33);
		appendChunk(body, "uPre", odd);	// odd-sized unknown chunk
	}

	appendChunk(body, "COMM",
		commChunk(spec.channels, spec.frames, spec.width));

	if (!spec.markers.empty())
		appendChunk(body, "MARK", markChunk(spec.markers));
	if (spec.hasInst)
		appendChunk(body, "INST",
			instChunk(spec.sustain, spec.release));

	Bytes ssnd;
	putU32(ssnd, (uint32_t) spec.ssndOffset);
	putU32(ssnd, 0);	// block size
	for (int i=0; i<spec.ssndOffset; ++i)
		putU8(ssnd, 0x5A);
	for (int f=0; f<spec.frames; ++f)
		for (int c=0; c<spec.channels; ++c)
			encodeSample(ssnd, spec.width, sampleValue(f, c));
	appendChunk(body, "SSND", ssnd);

	if (spec.unknownAfter)
	{
		Bytes even;
		even.push_back(0x91); even.push_back(0x92);
		even.push_back(0x93); even.push_back(0x94);
		appendChunk(body, "uAft", even);	// even-sized unknown chunk
	}

	Bytes file;
	putTag(file, "FORM");
	putU32(file, (uint32_t) body.size());
	putBytes(file, body.data(), body.size());
	(void) alreadyApplied;
	return file;
}

static bool writeFile(const std::string &path, const Bytes &data)
{
	FILE *f = fopen(path.c_str(), "wb");
	if (!f) return false;
	bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
	fclose(f);
	return ok;
}

static bool readFile(const std::string &path, Bytes &data)
{
	data.clear();
	FILE *f = fopen(path.c_str(), "rb");
	if (!f) return false;
	int c;
	while ((c = fgetc(f)) != EOF)
		data.push_back((uint8_t) c);
	fclose(f);
	return true;
}

static bool tempPath(std::string &path)
{
	const char *dir = getenv("TMPDIR");
	if (!dir || !*dir) dir = "/tmp";
	char tmpl[512];
	snprintf(tmpl, sizeof tmpl, "%s/aiff_splice_XXXXXX", dir);
	int fd = mkstemp(tmpl);
	if (fd < 0) return false;
	close(fd);
	path = tmpl;
	return true;
}

// Big-endian native PCM bytes for insertFrames of fresh sample data.
static Bytes insertPCMBytes(const FileSpec &spec, int insertFrames)
{
	Bytes b;
	for (int k=0; k<insertFrames; ++k)
		for (int c=0; c<spec.channels; ++c)
			encodeSample(b, spec.width, insertValue(k, c));
	return b;
}

// The expected post-splice sequence of per-frame logical sample rows.
static std::vector<std::vector<int32_t> > expectedFrames(
	const FileSpec &spec, const std::vector<SpliceSpec> &splices)
{
	std::vector<std::vector<int32_t> > frames;
	for (int f=0; f<spec.frames; ++f)
	{
		std::vector<int32_t> row(spec.channels);
		for (int c=0; c<spec.channels; ++c)
			row[c] = sampleValue(f, c);
		frames.push_back(row);
	}
	for (size_t s=0; s<splices.size(); ++s)
	{
		const SpliceSpec &sp = splices[s];
		std::vector<std::vector<int32_t> > out;
		for (int f=0; f<sp.start; ++f)
			out.push_back(frames[f]);
		for (int k=0; k<sp.insertFrames; ++k)
		{
			std::vector<int32_t> row(spec.channels);
			for (int c=0; c<spec.channels; ++c)
				row[c] = insertValue(k, c);
			out.push_back(row);
		}
		for (int f=sp.start + sp.eraseFrames;
			f < (int) frames.size(); ++f)
			out.push_back(frames[f]);
		frames.swap(out);
	}
	return frames;
}

// Compare the file's SSND payload against the independently expected rows.
static int verifyPayload(const Bytes &file, const FileSpec &spec,
	const std::vector<std::vector<int32_t> > &rows)
{
	std::vector<ChunkInfo> chunks;
	uint32_t formSize;
	TASK_CHECK(parseChunks(file, chunks, formSize));
	const ChunkInfo *ssnd = findChunk(chunks, "SSND");
	TASK_CHECK(ssnd != NULL);
	size_t dataStart = ssnd->headerOffset + 16 + spec.ssndOffset;
	size_t bytesPerFrame = (size_t) widthBytes(spec.width) * spec.channels;
	TASK_CHECK(ssnd->size == 8 + spec.ssndOffset +
		rows.size() * bytesPerFrame);
	TASK_CHECK(dataStart + rows.size() * bytesPerFrame <= file.size());
	size_t p = dataStart;
	for (size_t f=0; f<rows.size(); ++f)
	{
		for (int c=0; c<spec.channels; ++c)
		{
			int32_t value = rows[f][c];
			int n = widthBytes(spec.width);
			for (int i=n-1; i>=0; --i, ++p)
				TASK_CHECK(file[p] == (uint8_t)(value >> (8*i)));
		}
	}
	// SSND offset area bytes are preserved.
	for (int i=0; i<spec.ssndOffset; ++i)
		TASK_CHECK(file[ssnd->headerOffset + 16 + i] == 0x5A);
	return 0;
}

// Read the whole track as virtual 32-bit samples regardless of file width.
static int readVirtual32(AFfilehandle handle, int frames, int channels,
	std::vector<int32_t> &out)
{
	TASK_CHECK(afSetVirtualSampleFormat(handle, AF_DEFAULT_TRACK,
		AF_SAMPFMT_TWOSCOMP, 32) == AF_SUCCEED);
	// Read from the beginning: a splice deliberately remaps the read
	// cursor through the same rule as marker positions.
	TASK_CHECK(afSeekFrame(handle, AF_DEFAULT_TRACK, 0) == 0);
	out.assign((size_t) frames * channels, 0);
	if (frames > 0)
	{
		AFframecount got = afReadFrames(handle, AF_DEFAULT_TRACK,
			out.data(), frames);
		TASK_CHECK(got == frames);
	}
	return 0;
}

static int32_t virtual32Expected(int32_t logical, int width)
{
	return logical << (32 - width);
}

static int splice(AFfilehandle handle, const FileSpec &spec,
	const SpliceSpec &sp)
{
	Bytes insert = insertPCMBytes(spec, sp.insertFrames);
	const void *pcmp = sp.insertFrames ? insert.data() : NULL;
	TASK_CHECK(afSpliceFrames(handle, AF_DEFAULT_TRACK, sp.start,
		sp.eraseFrames, pcmp, sp.insertFrames) == AF_SUCCEED);
	return 0;
}

// ---------------------------------------------------------------------------
// layout group: on-disk structure, padding, offsets, unknown chunks.
// ---------------------------------------------------------------------------

static int layout_odd_24bit_payload()
{
	std::string path;
	TASK_CHECK(tempPath(path));
	FileSpec spec = { 24, 1, 5, 0, false, true,
		std::vector<MarkerSpec>(), false, LoopSpec(), LoopSpec() };
	TASK_CHECK(writeFile(path, buildFile(spec)));

	AFfilehandle h = afOpenFile(path.c_str(), "r+", NULL);
	TASK_CHECK(h != NULL);
	// 5 frames * 3 bytes = 15-byte odd payload; inserting one frame makes
	// the payload even and removes the SSND pad byte.
	SpliceSpec sp = { 2, 0, 1 };
	TASK_CHECK(splice(h, spec, sp) == 0);
	TASK_CHECK(afGetFrameCount(h, AF_DEFAULT_TRACK) == 6);
	afCloseFile(h);

	Bytes file;
	TASK_CHECK(readFile(path, file));
	std::vector<ChunkInfo> chunks;
	uint32_t formSize;
	TASK_CHECK(parseChunks(file, chunks, formSize));
	const ChunkInfo *ssnd = findChunk(chunks, "SSND");
	const ChunkInfo *after = findChunk(chunks, "uAft");
	TASK_CHECK(ssnd != NULL && after != NULL);
	TASK_CHECK(ssnd->size == 8 + 6 * 3);	// 26, even: no pad
	TASK_CHECK(ssnd->padded == false);
	// Trailing chunk sits immediately after the now pad-less SSND.
	TASK_CHECK(after->headerOffset ==
		ssnd->headerOffset + 8 + ssnd->size);
	TASK_CHECK(file[after->headerOffset + 8] == 0x91);
	TASK_CHECK(verifyPayload(file, spec,
		expectedFrames(spec, std::vector<SpliceSpec>(1, sp))) == 0);
	unlink(path.c_str());
	return 0;
}

static int layout_ssnd_offset_and_unknown_chunks()
{
	std::string path;
	TASK_CHECK(tempPath(path));
	FileSpec spec = { 16, 2, 8, 4, true, true,
		std::vector<MarkerSpec>(), false, LoopSpec(), LoopSpec() };
	TASK_CHECK(writeFile(path, buildFile(spec)));

	AFfilehandle h = afOpenFile(path.c_str(), "r+", NULL);
	TASK_CHECK(h != NULL);
	SpliceSpec grow = { 3, 0, 2 };
	SpliceSpec shrink = { 1, 2, 0 };
	TASK_CHECK(splice(h, spec, grow) == 0);
	TASK_CHECK(splice(h, spec, shrink) == 0);
	TASK_CHECK(afGetFrameCount(h, AF_DEFAULT_TRACK) == 8);
	afCloseFile(h);

	Bytes file;
	TASK_CHECK(readFile(path, file));
	std::vector<ChunkInfo> chunks;
	uint32_t formSize;
	TASK_CHECK(parseChunks(file, chunks, formSize));
	const ChunkInfo *pre = findChunk(chunks, "uPre");
	const ChunkInfo *comm = findChunk(chunks, "COMM");
	const ChunkInfo *ssnd = findChunk(chunks, "SSND");
	const ChunkInfo *after = findChunk(chunks, "uAft");
	TASK_CHECK(pre && comm && ssnd && after);
	// Relative order is preserved; the pre-SSND unknown chunk never moves.
	TASK_CHECK(pre->headerOffset < comm->headerOffset);
	TASK_CHECK(comm->headerOffset < ssnd->headerOffset);
	TASK_CHECK(ssnd->headerOffset < after->headerOffset);
	TASK_CHECK(pre->size == 3 && pre->padded == true);
	TASK_CHECK(file[pre->headerOffset + 8] == 0x11);
	TASK_CHECK(file[pre->headerOffset + 10] == 0x33);
	TASK_CHECK(file[after->headerOffset + 11] == 0x94);
	// SSND offset and blockSize fields survive unchanged.
	TASK_CHECK(getU32(file, ssnd->headerOffset + 8) == (uint32_t) spec.ssndOffset);
	TASK_CHECK(getU32(file, ssnd->headerOffset + 12) == 0);
	TASK_CHECK(verifyPayload(file, spec, expectedFrames(spec,
		std::vector<SpliceSpec> { grow, shrink })) == 0);
	unlink(path.c_str());
	return 0;
}

static int layout_delete_all_and_rebuild()
{
	std::string path;
	TASK_CHECK(tempPath(path));
	FileSpec spec = { 16, 1, 6, 2, true, true,
		std::vector<MarkerSpec>(), false, LoopSpec(), LoopSpec() };
	TASK_CHECK(writeFile(path, buildFile(spec)));

	AFfilehandle h = afOpenFile(path.c_str(), "r+", NULL);
	TASK_CHECK(h != NULL);
	SpliceSpec eraseAll = { 0, 6, 0 };
	TASK_CHECK(splice(h, spec, eraseAll) == 0);
	TASK_CHECK(afGetFrameCount(h, AF_DEFAULT_TRACK) == 0);
	afCloseFile(h);

	Bytes emptied;
	TASK_CHECK(readFile(path, emptied));
	std::vector<ChunkInfo> chunks;
	uint32_t formSize;
	TASK_CHECK(parseChunks(emptied, chunks, formSize));
	const ChunkInfo *ssnd = findChunk(chunks, "SSND");
	TASK_CHECK(ssnd != NULL);
	TASK_CHECK(ssnd->size == (uint32_t)(8 + spec.ssndOffset));
	// Offset area still present even with no audio frames.
	TASK_CHECK(emptied[ssnd->headerOffset + 16] == 0x5A);

	// Reopen the empty file and append fresh audio at the tail.
	AFfilehandle e = afOpenFile(path.c_str(), "r+", NULL);
	TASK_CHECK(e != NULL);
	SpliceSpec tail = { 0, 0, 3 };
	TASK_CHECK(splice(e, spec, tail) == 0);
	TASK_CHECK(afGetFrameCount(e, AF_DEFAULT_TRACK) == 3);
	afCloseFile(e);

	Bytes rebuilt;
	TASK_CHECK(readFile(path, rebuilt));
	TASK_CHECK(verifyPayload(rebuilt, spec,
		expectedFrames(spec, std::vector<SpliceSpec> { eraseAll, tail })) == 0);
	unlink(path.c_str());
	return 0;
}

static int layout_repeated_edits()
{
	std::string path;
	TASK_CHECK(tempPath(path));
	FileSpec spec = { 8, 1, 9, 0, true, true,
		std::vector<MarkerSpec>(), false, LoopSpec(), LoopSpec() };
	TASK_CHECK(writeFile(path, buildFile(spec)));

	AFfilehandle h = afOpenFile(path.c_str(), "r+", NULL);
	TASK_CHECK(h != NULL);
	SpliceSpec a = { 0, 1, 0 };		// erase first frame (deleted endpoint)
	SpliceSpec b = { 7, 1, 0 };		// erase last frame of the remainder
	SpliceSpec c = { 4, 0, 2 };		// insert two frames mid-file
	TASK_CHECK(splice(h, spec, a) == 0);
	TASK_CHECK(splice(h, spec, b) == 0);
	TASK_CHECK(splice(h, spec, c) == 0);
	TASK_CHECK(afGetFrameCount(h, AF_DEFAULT_TRACK) == 9);
	afCloseFile(h);

	Bytes file;
	TASK_CHECK(readFile(path, file));
	std::vector<SpliceSpec> all;
	all.push_back(a); all.push_back(b); all.push_back(c);
	TASK_CHECK(verifyPayload(file, spec, expectedFrames(spec, all)) == 0);
	// A fresh open yields the same structure.
	AFfilehandle r = afOpenFile(path.c_str(), "r", NULL);
	TASK_CHECK(r != NULL);
	TASK_CHECK(afGetFrameCount(r, AF_DEFAULT_TRACK) == 9);
	afCloseFile(r);
	unlink(path.c_str());
	return 0;
}

// ---------------------------------------------------------------------------
// anchors group: marker positions, identities, instrument loops.
// ---------------------------------------------------------------------------

static int anchors_three_zones()
{
	std::string path;
	TASK_CHECK(tempPath(path));
	std::vector<MarkerSpec> markers;
	markers.push_back(MarkerSpec { 1, 1, "low" });	// < start
	markers.push_back(MarkerSpec { 2, 3, "mid" });	// inside erased range
	markers.push_back(MarkerSpec { 3, 7, "high" });	// >= start+erase
	markers.push_back(MarkerSpec { 4, 8, "tail" });	// at audio tail
	FileSpec spec = { 16, 1, 8, 0, false, false, markers, false,
		LoopSpec(), LoopSpec() };
	TASK_CHECK(writeFile(path, buildFile(spec)));

	AFfilehandle h = afOpenFile(path.c_str(), "r+", NULL);
	TASK_CHECK(h != NULL);
	SpliceSpec sp = { 3, 3, 2 };
	TASK_CHECK(splice(h, spec, sp) == 0);
	// x<start unchanged; erased zone snaps to start+insert; tail shifts.
	TASK_CHECK(afGetMarkPosition(h, AF_DEFAULT_TRACK, 1) == 1);
	TASK_CHECK(afGetMarkPosition(h, AF_DEFAULT_TRACK, 2) == 5);
	TASK_CHECK(afGetMarkPosition(h, AF_DEFAULT_TRACK, 3) == 6);
	TASK_CHECK(afGetMarkPosition(h, AF_DEFAULT_TRACK, 4) == 7);
	TASK_CHECK(afGetFrameCount(h, AF_DEFAULT_TRACK) == 7);
	afCloseFile(h);

	// Reopen and insert purely at frame 1, where marker 1 now sits.
	// A marker exactly on the insertion start moves past the inserted
	// segment rather than staying put.
	AFfilehandle p = afOpenFile(path.c_str(), "r+", NULL);
	TASK_CHECK(p != NULL);
	SpliceSpec pure = { 1, 0, 2 };
	TASK_CHECK(splice(p, spec, pure) == 0);
	TASK_CHECK(afGetFrameCount(p, AF_DEFAULT_TRACK) == 9);
	TASK_CHECK(afGetMarkPosition(p, AF_DEFAULT_TRACK, 1) == 3);
	TASK_CHECK(afGetMarkPosition(p, AF_DEFAULT_TRACK, 2) == 7);
	afCloseFile(p);
	unlink(path.c_str());
	return 0;
}

static int anchors_identities_preserved()
{
	std::string path;
	TASK_CHECK(tempPath(path));
	std::vector<MarkerSpec> markers;
	markers.push_back(MarkerSpec { 30, 0, "zero" });
	markers.push_back(MarkerSpec { 10, 4, "four" });
	markers.push_back(MarkerSpec { 20, 6, "six" });
	FileSpec spec = { 16, 1, 7, 0, true, false, markers, false,
		LoopSpec(), LoopSpec() };
	TASK_CHECK(writeFile(path, buildFile(spec)));

	AFfilehandle h = afOpenFile(path.c_str(), "r+", NULL);
	TASK_CHECK(h != NULL);
	SpliceSpec sp = { 2, 2, 1 };
	TASK_CHECK(splice(h, spec, sp) == 0);

	// IDs, names and on-disk order are untouched; only positions change.
	int ids[8];
	TASK_CHECK(afGetMarkIDs(h, AF_DEFAULT_TRACK, ids) == 3);
	TASK_CHECK(ids[0] == 30 && ids[1] == 10 && ids[2] == 20);
	TASK_CHECK(strcmp(afGetMarkName(h, AF_DEFAULT_TRACK, 30), "zero") == 0);
	TASK_CHECK(strcmp(afGetMarkName(h, AF_DEFAULT_TRACK, 10), "four") == 0);
	TASK_CHECK(strcmp(afGetMarkName(h, AF_DEFAULT_TRACK, 20), "six") == 0);
	afCloseFile(h);

	Bytes file;
	TASK_CHECK(readFile(path, file));
	std::vector<ChunkInfo> chunks;
	uint32_t formSize;
	TASK_CHECK(parseChunks(file, chunks, formSize));
	const ChunkInfo *mark = findChunk(chunks, "MARK");
	TASK_CHECK(mark != NULL);
	// Independently decode the MARK chunk: order, ids, names, positions.
	size_t cursor = mark->headerOffset + 8;
	TASK_CHECK(getU16(file, cursor) == 3);
	cursor += 2;
	const int expectID[3] = { 30, 10, 20 };
	const uint32_t expectPos[3] = { 0, 3, 5 };
	const char *expectName[3] = { "zero", "four", "six" };
	for (int i=0; i<3; ++i)
	{
		TASK_CHECK(getU16(file, cursor) == (uint16_t) expectID[i]);
		TASK_CHECK(getU32(file, cursor + 2) == expectPos[i]);
		uint8_t nameLen = file[cursor + 6];
		TASK_CHECK(nameLen == strlen(expectName[i]));
		TASK_CHECK(memcmp(&file[cursor + 7], expectName[i], nameLen) == 0);
		cursor += 7 + nameLen + ((nameLen % 2) == 0 ? 1 : 0);
	}
	TASK_CHECK(cursor == mark->headerOffset + 8 + mark->size);
	unlink(path.c_str());
	return 0;
}

static int anchors_loop_inversion()
{
	std::string path;
	TASK_CHECK(tempPath(path));
	std::vector<MarkerSpec> markers;
	markers.push_back(MarkerSpec { 1, 1, "a" });
	markers.push_back(MarkerSpec { 2, 4, "b" });
	markers.push_back(MarkerSpec { 3, 8, "c" });
	LoopSpec sustain = { AF_LOOP_MODE_FORW, 1, 2 };	// collapses to equal ends
	LoopSpec release = { AF_LOOP_MODE_FORWBAKW, 1, 3 };	// stays valid
	FileSpec spec = { 16, 1, 10, 0, false, true, markers, true,
		sustain, release };
	TASK_CHECK(writeFile(path, buildFile(spec)));

	AFfilehandle h = afOpenFile(path.c_str(), "r+", NULL);
	TASK_CHECK(h != NULL);
	// Erase frames 1..4: markers 1 and 2 both collapse onto frame 1,
	// while marker 8 shifts to frame 4.
	SpliceSpec sp = { 1, 4, 0 };
	TASK_CHECK(splice(h, spec, sp) == 0);
	TASK_CHECK(afGetMarkPosition(h, AF_DEFAULT_TRACK, 1) == 1);
	TASK_CHECK(afGetMarkPosition(h, AF_DEFAULT_TRACK, 2) == 1);
	TASK_CHECK(afGetMarkPosition(h, AF_DEFAULT_TRACK, 3) == 4);
	TASK_CHECK(afGetLoopMode(h, AF_DEFAULT_INST, 1) == AF_LOOP_MODE_NOLOOP);
	TASK_CHECK(afGetLoopMode(h, AF_DEFAULT_INST, 2) == AF_LOOP_MODE_FORWBAKW);
	// Non-loop instrument parameters are unchanged.
	TASK_CHECK(afGetInstParamLong(h, AF_DEFAULT_INST,
		AF_INST_MIDI_BASENOTE) == 61);
	TASK_CHECK(afGetInstParamLong(h, AF_DEFAULT_INST,
		AF_INST_NUMCENTS_DETUNE) == -3);
	afCloseFile(h);

	// The disabled mode is persisted in the INST bytes.
	Bytes file;
	TASK_CHECK(readFile(path, file));
	std::vector<ChunkInfo> chunks;
	uint32_t formSize;
	TASK_CHECK(parseChunks(file, chunks, formSize));
	const ChunkInfo *inst = findChunk(chunks, "INST");
	TASK_CHECK(inst != NULL);
	TASK_CHECK(getU16(file, inst->headerOffset + 8 + 8) == AF_LOOP_MODE_NOLOOP);
	TASK_CHECK(getU16(file, inst->headerOffset + 8 + 14) ==
		AF_LOOP_MODE_FORWBAKW);
	unlink(path.c_str());
	return 0;
}

static int anchors_persist_reopen()
{
	std::string path;
	TASK_CHECK(tempPath(path));
	std::vector<MarkerSpec> markers;
	markers.push_back(MarkerSpec { 1, 0, "m0" });
	markers.push_back(MarkerSpec { 2, 5, "m5" });
	LoopSpec sustain = { AF_LOOP_MODE_FORW, 1, 2 };
	LoopSpec release = { AF_LOOP_MODE_NOLOOP, 0, 0 };
	FileSpec spec = { 16, 1, 6, 0, false, false, markers, true,
		sustain, release };
	TASK_CHECK(writeFile(path, buildFile(spec)));

	AFfilehandle h = afOpenFile(path.c_str(), "r+", NULL);
	TASK_CHECK(h != NULL);
	SpliceSpec sp = { 1, 2, 1 };
	TASK_CHECK(splice(h, spec, sp) == 0);
	AFframecount live0 = afGetMarkPosition(h, AF_DEFAULT_TRACK, 1);
	AFframecount live5 = afGetMarkPosition(h, AF_DEFAULT_TRACK, 2);
	afCloseFile(h);

	AFfilehandle r = afOpenFile(path.c_str(), "r", NULL);
	TASK_CHECK(r != NULL);
	TASK_CHECK(afGetFrameCount(r, AF_DEFAULT_TRACK) == 5);
	TASK_CHECK(afGetMarkPosition(r, AF_DEFAULT_TRACK, 1) == live0);
	TASK_CHECK(afGetMarkPosition(r, AF_DEFAULT_TRACK, 2) == live5);
	TASK_CHECK(afGetLoopMode(r, AF_DEFAULT_INST, 1) == AF_LOOP_MODE_FORW);
	TASK_CHECK(afGetLoopStartFrame(r, AF_DEFAULT_INST, 1) == live0);
	TASK_CHECK(afGetLoopEndFrame(r, AF_DEFAULT_INST, 1) == live5);
	afCloseFile(r);
	unlink(path.c_str());
	return 0;
}

// ---------------------------------------------------------------------------
// live_handle group: reads, cursor mapping, conversion, rejection.
// ---------------------------------------------------------------------------

static int live_multichannel_virtual_conversion()
{
	std::string path;
	TASK_CHECK(tempPath(path));
	FileSpec spec = { 16, 2, 10, 0, false, true,
		std::vector<MarkerSpec>(), false, LoopSpec(), LoopSpec() };
	TASK_CHECK(writeFile(path, buildFile(spec)));

	AFfilehandle h = afOpenFile(path.c_str(), "r+", NULL);
	TASK_CHECK(h != NULL);
	SpliceSpec sp = { 4, 3, 2 };
	TASK_CHECK(splice(h, spec, sp) == 0);
	int frames = 9;
	TASK_CHECK(afGetFrameCount(h, AF_DEFAULT_TRACK) == frames);
	TASK_CHECK(afGetChannels(h, AF_DEFAULT_TRACK) == 2);

	std::vector<int32_t> got;
	TASK_CHECK(readVirtual32(h, frames, 2, got) == 0);
	std::vector<std::vector<int32_t> > want =
		expectedFrames(spec, std::vector<SpliceSpec>(1, sp));
	for (int f=0; f<frames; ++f)
		for (int c=0; c<2; ++c)
			TASK_CHECK(got[f*2 + c] ==
				virtual32Expected(want[f][c], spec.width));
	afCloseFile(h);
	unlink(path.c_str());
	return 0;
}

static int live_partial_read_cursor()
{
	std::string path;
	TASK_CHECK(tempPath(path));
	FileSpec spec = { 16, 1, 10, 0, false, false,
		std::vector<MarkerSpec>(), false, LoopSpec(), LoopSpec() };
	TASK_CHECK(writeFile(path, buildFile(spec)));

	AFfilehandle h = afOpenFile(path.c_str(), "r+", NULL);
	TASK_CHECK(h != NULL);
	// Consume three frames before editing so a decode pipeline exists.
	int16_t prefix[3];
	TASK_CHECK(afReadFrames(h, AF_DEFAULT_TRACK, prefix, 3) == 3);
	TASK_CHECK(afTellFrame(h, AF_DEFAULT_TRACK) == 3);

	// Erase starting after the cursor: frame 3 itself is removed, so the
	// cursor (at the start of the erased zone) snaps to the splice point.
	SpliceSpec sp = { 3, 2, 1 };
	TASK_CHECK(splice(h, spec, sp) == 0);
	TASK_CHECK(afTellFrame(h, AF_DEFAULT_TRACK) == 4);
	TASK_CHECK(afGetFrameCount(h, AF_DEFAULT_TRACK) == 9);

	// Continued reading must not leak any pre-splice buffered samples.
	std::vector<std::vector<int32_t> > want =
		expectedFrames(spec, std::vector<SpliceSpec>(1, sp));
	int16_t rest[16];
	AFframecount got = afReadFrames(h, AF_DEFAULT_TRACK, rest, 16);
	TASK_CHECK(got == 9 - 4);
	for (int i=0; i<got; ++i)
		TASK_CHECK(rest[i] == want[4 + i][0]);

	// Seeking and reading from the beginning gives the full new audio.
	TASK_CHECK(afSeekFrame(h, AF_DEFAULT_TRACK, 0) == 0);
	int16_t all[16];
	TASK_CHECK(afReadFrames(h, AF_DEFAULT_TRACK, all, 16) == 9);
	for (int f=0; f<9; ++f)
		TASK_CHECK(all[f] == want[f][0]);
	afCloseFile(h);
	unlink(path.c_str());
	return 0;
}

static int live_tail_and_empty()
{
	std::string path;
	TASK_CHECK(tempPath(path));
	FileSpec spec = { 16, 2, 4, 0, false, true,
		std::vector<MarkerSpec>(), false, LoopSpec(), LoopSpec() };
	TASK_CHECK(writeFile(path, buildFile(spec)));

	// Tail insertion on a populated file.
	AFfilehandle h = afOpenFile(path.c_str(), "r+", NULL);
	TASK_CHECK(h != NULL);
	SpliceSpec tail = { 4, 0, 2 };
	TASK_CHECK(splice(h, spec, tail) == 0);
	TASK_CHECK(afGetFrameCount(h, AF_DEFAULT_TRACK) == 6);
	afCloseFile(h);

	// Empty a file, then insert at the tail of zero frames.
	AFfilehandle e = afOpenFile(path.c_str(), "r+", NULL);
	TASK_CHECK(e != NULL);
	SpliceSpec eraseAll = { 0, 6, 0 };
	SpliceSpec seed = { 0, 0, 3 };
	TASK_CHECK(splice(e, spec, eraseAll) == 0);
	TASK_CHECK(afGetFrameCount(e, AF_DEFAULT_TRACK) == 0);
	TASK_CHECK(afTellFrame(e, AF_DEFAULT_TRACK) == 0);
	TASK_CHECK(splice(e, spec, seed) == 0);
	TASK_CHECK(afGetFrameCount(e, AF_DEFAULT_TRACK) == 3);

	std::vector<int32_t> got;
	TASK_CHECK(readVirtual32(e, 3, 2, got) == 0);
	std::vector<std::vector<int32_t> > want =
		expectedFrames(spec, std::vector<SpliceSpec> { eraseAll, seed });
	for (int f=0; f<3; ++f)
		for (int c=0; c<2; ++c)
			TASK_CHECK(got[f*2 + c] ==
				virtual32Expected(want[f][c], spec.width));
	afCloseFile(e);
	unlink(path.c_str());
	return 0;
}

static int live_rejection_preserves_state()
{
	std::string path;
	TASK_CHECK(tempPath(path));
	FileSpec spec = { 16, 1, 6, 0, false, false,
		std::vector<MarkerSpec>(), false, LoopSpec(), LoopSpec() };
	Bytes original = buildFile(spec);
	TASK_CHECK(writeFile(path, original));

	AFerrfunc oldHandler = afSetErrorHandler(NULL);
	AFfilehandle h = afOpenFile(path.c_str(), "r+", NULL);
	TASK_CHECK(h != NULL);
	int16_t prefix[2];
	TASK_CHECK(afReadFrames(h, AF_DEFAULT_TRACK, prefix, 2) == 2);
	AFframecount cursorBefore = afTellFrame(h, AF_DEFAULT_TRACK);

	Bytes insert = insertPCMBytes(spec, 1);
	// Every rejection happens before the file is modified.
	TASK_CHECK(afSpliceFrames(h, AF_DEFAULT_TRACK, -1, 0, NULL, 0) == AF_FAIL);
	TASK_CHECK(afSpliceFrames(h, AF_DEFAULT_TRACK, 0, -1, NULL, 0) == AF_FAIL);
	TASK_CHECK(afSpliceFrames(h, AF_DEFAULT_TRACK, 0, 0, NULL, -1) == AF_FAIL);
	TASK_CHECK(afSpliceFrames(h, AF_DEFAULT_TRACK, 7, 0, NULL, 0) == AF_FAIL);
	TASK_CHECK(afSpliceFrames(h, AF_DEFAULT_TRACK, 4, 3, NULL, 0) == AF_FAIL);
	TASK_CHECK(afSpliceFrames(h, AF_DEFAULT_TRACK, 0, 0,
		insert.data(), 0) == AF_SUCCEED);	// zero insert ignores pointer
	TASK_CHECK(afSpliceFrames(h, AF_DEFAULT_TRACK, 0, 0, NULL, 1) == AF_FAIL);
	// afWriteFrames remains refused in edit mode.
	int16_t junk = 7;
	TASK_CHECK(afWriteFrames(h, AF_DEFAULT_TRACK, &junk, 1) == -1);

	// Handle queries and the read cursor are unchanged.
	TASK_CHECK(afGetFrameCount(h, AF_DEFAULT_TRACK) == 6);
	TASK_CHECK(afTellFrame(h, AF_DEFAULT_TRACK) == cursorBefore);
	afCloseFile(h);
	afSetErrorHandler(oldHandler);

	// The on-disk file is byte-for-byte identical to the original.
	Bytes afterReject;
	TASK_CHECK(readFile(path, afterReject));
	TASK_CHECK(afterReject == original);

	// AIFC is not eligible for edit mode and is never truncated.
	std::string aifcPath;
	TASK_CHECK(tempPath(aifcPath));
	Bytes aifc;
	putTag(aifc, "FORM");
	Bytes aifcBody;
	putTag(aifcBody, "AIFC");
	Bytes fver;
	putU32(fver, 0xA2805140);
	appendChunk(aifcBody, "FVER", fver);
	Bytes comm;
	putU16(comm, 1); putU32(comm, 2); putU16(comm, 16);
	putBytes(comm, kRate48000, 10);
	putTag(comm, "NONE");
	putU8(comm, 15);
	for (int i=0; i<15; ++i) putU8(comm, (uint8_t) 'x');
	appendChunk(aifcBody, "COMM", comm);
	Bytes ssnd;
	putU32(ssnd, 0); putU32(ssnd, 0);
	putU16(ssnd, 1); putU16(ssnd, 2);
	appendChunk(aifcBody, "SSND", ssnd);
	putU32(aifc, (uint32_t) aifcBody.size());
	putBytes(aifc, aifcBody.data(), aifcBody.size());
	Bytes aifcOriginal = aifc;
	TASK_CHECK(writeFile(aifcPath, aifcOriginal));
	oldHandler = afSetErrorHandler(NULL);
	TASK_CHECK(afOpenFile(aifcPath.c_str(), "r+", NULL) == AF_NULL_FILEHANDLE);
	afSetErrorHandler(oldHandler);
	Bytes aifcAfter;
	TASK_CHECK(readFile(aifcPath, aifcAfter));
	TASK_CHECK(aifcAfter == aifcOriginal);
	unlink(path.c_str());
	unlink(aifcPath.c_str());
	return 0;
}

// ---------------------------------------------------------------------------
// Registration.  The table and its size use C linkage so the C test driver
// (task_support/driver.c) can find them.
// ---------------------------------------------------------------------------

} // namespace

extern "C" const struct task_case task_cases[] =
{
	{ "layout", "odd_24bit_payload", layout_odd_24bit_payload },
	{ "layout", "ssnd_offset_unknown_chunks",
		layout_ssnd_offset_and_unknown_chunks },
	{ "layout", "delete_all_and_rebuild", layout_delete_all_and_rebuild },
	{ "layout", "repeated_edits", layout_repeated_edits },

	{ "anchors", "three_zones", anchors_three_zones },
	{ "anchors", "identities_preserved", anchors_identities_preserved },
	{ "anchors", "loop_inversion", anchors_loop_inversion },
	{ "anchors", "persist_reopen", anchors_persist_reopen },

	{ "live_handle", "multichannel_virtual_conversion",
		live_multichannel_virtual_conversion },
	{ "live_handle", "partial_read_cursor", live_partial_read_cursor },
	{ "live_handle", "tail_and_empty", live_tail_and_empty },
	{ "live_handle", "rejection_preserves_state",
		live_rejection_preserves_state },
};

extern "C" const size_t task_case_count =
	sizeof(task_cases) / sizeof(task_cases[0]);

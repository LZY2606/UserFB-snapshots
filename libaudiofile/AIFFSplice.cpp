/*
	Audio File Library
	Copyright (C) 2016, Michael Pruett <michael@68k.org>

	This library is free software; you can redistribute it and/or
	modify it under the terms of the GNU Lesser General Public
	License as published by the Free Software Foundation; either
	version 2.1 of the License, or (at your option) any later version.

	This library is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
	Lesser General Public License for more details.

	You should have received a copy of the GNU Lesser General Public
	License along with this library; if not, write to the
	Free Software Foundation, Inc., 59 Temple Place, Fifth Floor,
	Boston, MA  02111-1307  USA
*/

/*
	AIFFSplice.cpp

	In-place frame editing for named, uncompressed AIFF files opened
	with mode "r+".  Retained samples are never re-encoded and are moved
	with a fixed-size scratch buffer whose size does not depend on the
	length of the recording.
*/

#include "config.h"
#include "AIFF.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "File.h"
#include "FileHandle.h"
#include "Instrument.h"
#include "Marker.h"
#include "Tag.h"
#include "Track.h"
#include "afinternal.h"
#include "audiofile.h"
#include "modules/ModuleState.h"
#include "util.h"

// Fixed scratch buffer used for every within-file byte move.
static const size_t kSpliceBufferBytes = 64 * 1024;

const AIFFChunkDesc *AIFFFile::findChunk(const Tag &tag) const
{
	for (size_t i=0; i<m_chunkLayout.size(); i++)
		if (m_chunkLayout[i].tag == tag)
			return &m_chunkLayout[i];
	return NULL;
}

AIFFChunkDesc *AIFFFile::findChunk(const Tag &tag)
{
	for (size_t i=0; i<m_chunkLayout.size(); i++)
		if (m_chunkLayout[i].tag == tag)
			return &m_chunkLayout[i];
	return NULL;
}

// Map a pre-splice frame coordinate through the splice.
AFframecount AIFFFile::mapPosition(AFframecount x, AFframecount start,
	AFframecount eraseFrames, AFframecount insertFrames)
{
	if (x < start)
		return x;
	if (x < start + eraseFrames)
		return start + insertFrames;
	return x + insertFrames - eraseFrames;
}

static bool markerIDExists(Track *track, int id)
{
	for (int i=0; i<track->markerCount; i++)
		if (track->markers[i].id == id)
			return true;
	return false;
}

static AFframecount markerPosition(Track *track, int id)
{
	for (int i=0; i<track->markerCount; i++)
		if (track->markers[i].id == id)
			return (AFframecount) track->markers[i].position;
	return -1;
}

static uint16_t readBE16(const unsigned char *p)
{
	return (uint16_t) ((p[0] << 8) | p[1]);
}

/*
	Independently walk the MARK chunk bytes to confirm that its declared
	size accounts exactly for its entries, Pascal strings and padding,
	and record the file offset of each marker's 4-byte position field.
*/
static status collectMarkPositionOffsets(File *fh,
	const AIFFChunkDesc &mark, unsigned expectedCount,
	AFfileoffset *positions)
{
	if (mark.size < 2)
		return AF_FAIL;

	unsigned char scratch[256];
	AFfileoffset cursor = mark.offset + 8;
	const AFfileoffset end = mark.offset + 8 + (AFfileoffset) mark.size;

	if (fh->seek(cursor, File::SeekFromBeginning) != cursor ||
		fh->read(scratch, 2) != 2)
		return AF_FAIL;
	unsigned count = readBE16(scratch);
	if (count != expectedCount)
		return AF_FAIL;
	cursor += 2;

	for (unsigned i=0; i<count; i++)
	{
		if (cursor + 7 > end ||
			fh->seek(cursor, File::SeekFromBeginning) != cursor ||
			fh->read(scratch, 7) != 7)
			return AF_FAIL;
		// id (2) then position (4); record the position field offset.
		positions[i] = cursor + 2;
		unsigned char nameLength = scratch[6];
		cursor += 7;

		if (nameLength > 0)
		{
			if (cursor + nameLength > end ||
				fh->read(scratch, nameLength) != (ssize_t) nameLength)
				return AF_FAIL;
			cursor += nameLength;
		}

		// An even Pascal-string length (size byte + name) is padded.
		if ((nameLength % 2) == 0)
			cursor += 1;
	}

	return cursor == end ? AF_SUCCEED : AF_FAIL;
}

status AIFFFile::editEligible()
{
	// Only plain (non-AIFC), named AIFF is editable.
	if (m_fileFormat != AF_FILE_AIFF)
		return AF_FAIL;

	Track *track = getTrack();
	if (!track || !m_seekok)
		return AF_FAIL;

	// Uncompressed, big-endian, two's-complement integer PCM in
	// 8/16/24/32-bit widths only.
	if (track->f.compressionType != AF_COMPRESSION_NONE)
		return AF_FAIL;
	if (track->f.sampleFormat != AF_SAMPFMT_TWOSCOMP)
		return AF_FAIL;
	if (track->f.byteOrder != AF_BYTEORDER_BIGENDIAN)
		return AF_FAIL;
	switch (track->f.sampleWidth)
	{
		case 8: case 16: case 24: case 32:
			break;
		default:
			return AF_FAIL;
	}

	// Exactly one COMM and one SSND, at most one MARK and one INST.
	if (m_commCount != 1 || m_ssndCount != 1 ||
		m_markCount > 1 || m_instCount > 1)
		return AF_FAIL;

	const AIFFChunkDesc *comm = findChunk("COMM");
	const AIFFChunkDesc *ssnd = findChunk("SSND");
	if (!comm || !ssnd || comm->size != 18)
		return AF_FAIL;
	if (m_instCount == 1)
	{
		const AIFFChunkDesc *inst = findChunk("INST");
		if (!inst || inst->size != 20)
			return AF_FAIL;
	}

	if (!m_formConsistent)
		return AF_FAIL;

	off_t actualLength = m_fh->length();
	if (actualLength < 0)
		return AF_FAIL;

	// Every chunk must fit, be correctly padded and sit immediately
	// before the next chunk header; the last chunk ends at EOF.
	for (size_t i=0; i<m_chunkLayout.size(); i++)
	{
		const AIFFChunkDesc &d = m_chunkLayout[i];
		uint64_t occupancy =
			(uint64_t) d.offset + 8 + d.size + (d.padded ? 1 : 0);
		if (occupancy > (uint64_t) actualLength)
			return AF_FAIL;
		if (i + 1 < m_chunkLayout.size())
		{
			if (occupancy != (uint64_t) m_chunkLayout[i+1].offset)
				return AF_FAIL;
		}
		else if (occupancy != (uint64_t) actualLength)
			return AF_FAIL;
	}

	const size_t bytesPerFrame = track->f.bytesPerFrame(false);
	if (bytesPerFrame == 0)
		return AF_FAIL;

	// SSND must hold an integral number of frames; the offset area must
	// fit inside the chunk.
	if (ssnd->size < 8)
		return AF_FAIL;
	uint64_t ssndDataArea = (uint64_t) ssnd->size - 8;
	if ((uint64_t) m_ssndDataOffset > ssndDataArea)
		return AF_FAIL;
	uint64_t payloadBytes = ssndDataArea - m_ssndDataOffset;
	if (payloadBytes % bytesPerFrame != 0)
		return AF_FAIL;
	uint64_t framesFromSSND = payloadBytes / bytesPerFrame;

	// COMM frame count must agree with the actual PCM payload.
	uint64_t framesFromCOMM = (uint64_t) track->totalfframes;
	if (framesFromCOMM != framesFromSSND)
		return AF_FAIL;
	if (framesFromCOMM > (uint64_t) INT32_MAX)
		return AF_FAIL;

	// MARK structure, unique IDs and in-range positions.
	if (m_markCount == 1)
	{
		const AIFFChunkDesc *mark = findChunk("MARK");
		if (!mark || track->markerCount < 0)
			return AF_FAIL;
		AFfileoffset *positionOffsets = (AFfileoffset *) _af_malloc(
			sizeof(AFfileoffset) * (track->markerCount + 1));
		if (!positionOffsets)
			return AF_FAIL;
		status markStatus = collectMarkPositionOffsets(m_fh, *mark,
			(unsigned) track->markerCount, positionOffsets);
		free(positionOffsets);
		if (markStatus != AF_SUCCEED)
			return AF_FAIL;

		for (int i=0; i<track->markerCount; i++)
		{
			for (int j=i+1; j<track->markerCount; j++)
				if (track->markers[i].id == track->markers[j].id)
					return AF_FAIL;
			if ((uint64_t) track->markers[i].position > framesFromCOMM)
				return AF_FAIL;
		}
	}

	// Active instrument loops need existing markers and must not already
	// be inverted.
	if (m_instCount == 1 && m_instruments)
	{
		Instrument *instrument = &m_instruments[0];
		for (int i=0; i<instrument->loopCount; i++)
		{
			Loop *loop = &instrument->loops[i];
			if (loop->mode == AF_LOOP_MODE_NOLOOP)
				continue;
			if (!markerIDExists(track, loop->beginMarker) ||
				!markerIDExists(track, loop->endMarker))
				return AF_FAIL;
			AFframecount begin = markerPosition(track, loop->beginMarker);
			AFframecount end = markerPosition(track, loop->endMarker);
			if (begin < 0 || end < 0 || begin > end)
				return AF_FAIL;
		}
	}

	return AF_SUCCEED;
}

/*
	Move the length bytes beginning at source so they begin at
	destination, using a fixed scratch buffer.  The regions may overlap;
	the copy direction is chosen to preserve the source until read.
*/
static status moveRegion(File *fh, AFfileoffset source,
	AFfileoffset destination, AFfileoffset length)
{
	if (length == 0)
		return AF_SUCCEED;
	if (source < 0 || destination < 0 || length < 0)
		return AF_FAIL;

	unsigned char *buffer = (unsigned char *) _af_malloc(kSpliceBufferBytes);
	if (!buffer)
		return AF_FAIL;

	status result = AF_SUCCEED;

	if (destination >= source)
	{
		// Grow: copy from the end toward the beginning.
		AFfileoffset remaining = length;
		while (remaining > 0)
		{
			size_t n = (remaining > (AFfileoffset) kSpliceBufferBytes) ?
				kSpliceBufferBytes : (size_t) remaining;
			AFfileoffset srcPos = source + remaining - n;
			AFfileoffset dstPos = destination + remaining - n;
			if (fh->seek(srcPos, File::SeekFromBeginning) != srcPos ||
				fh->read(buffer, n) != (ssize_t) n ||
				fh->seek(dstPos, File::SeekFromBeginning) != dstPos ||
				fh->write(buffer, n) != (ssize_t) n)
			{
				result = AF_FAIL;
				break;
			}
			remaining -= n;
		}
	}
	else
	{
		// Shrink: copy from the beginning toward the end.
		AFfileoffset done = 0;
		while (done < length)
		{
			size_t n = (length - done > (AFfileoffset) kSpliceBufferBytes) ?
				kSpliceBufferBytes : (size_t) (length - done);
			AFfileoffset srcPos = source + done;
			AFfileoffset dstPos = destination + done;
			if (fh->seek(srcPos, File::SeekFromBeginning) != srcPos ||
				fh->read(buffer, n) != (ssize_t) n ||
				fh->seek(dstPos, File::SeekFromBeginning) != dstPos ||
				fh->write(buffer, n) != (ssize_t) n)
			{
				result = AF_FAIL;
				break;
			}
			done += n;
		}
	}

	free(buffer);
	return result;
}

static status writeBE32At(File *fh, AFfileoffset offset, uint32_t value)
{
	unsigned char bytes[4] =
	{
		(unsigned char) (value >> 24),
		(unsigned char) (value >> 16),
		(unsigned char) (value >> 8),
		(unsigned char) value
	};
	if (fh->seek(offset, File::SeekFromBeginning) != offset)
		return AF_FAIL;
	return fh->write(bytes, 4) == 4 ? AF_SUCCEED : AF_FAIL;
}

static status writeBE16At(File *fh, AFfileoffset offset, uint16_t value)
{
	unsigned char bytes[2] =
	{
		(unsigned char) (value >> 8),
		(unsigned char) value
	};
	if (fh->seek(offset, File::SeekFromBeginning) != offset)
		return AF_FAIL;
	return fh->write(bytes, 2) == 2 ? AF_SUCCEED : AF_FAIL;
}

/*
	Rewrite FORM/COMM/SSND size fields, the SSND pad, MARK marker
	positions and INST loop play modes in place, then update the cached
	chunk layout so a subsequent splice sees the new structure.
*/
status AIFFFile::rewriteChunkHeaders(int64_t chunkShift,
	uint32_t newFrameCount, uint64_t newSsndSize, bool newSsndPadded,
	AFfileoffset newPayloadEnd, AFfileoffset oldChunkRegion)
{
	Track *track = getTrack();
	off_t newFileLength = m_fh->length();
	if (newFileLength < 0)
		return AF_FAIL;

	// FORM size: everything following the 8-byte FORM header.
	if (writeBE32At(m_fh, 4, (uint32_t) (newFileLength - 8)) != AF_SUCCEED)
		return AF_FAIL;

	AIFFChunkDesc *ssnd = findChunk("SSND");
	if (!ssnd)
		return AF_FAIL;

	// SSND data size; offset and blockSize are deliberately untouched.
	if (writeBE32At(m_fh, ssnd->offset + 4,
			(uint32_t) newSsndSize) != AF_SUCCEED)
		return AF_FAIL;

	// SSND pad byte after the new payload.
	if (newSsndPadded)
	{
		unsigned char zero = 0;
		if (m_fh->seek(newPayloadEnd, File::SeekFromBeginning) !=
				newPayloadEnd ||
			m_fh->write(&zero, 1) != 1)
			return AF_FAIL;
	}

	// COMM frame count: the uint32 frame count follows the 2-byte
	// channel count within the chunk data.
	AIFFChunkDesc *comm = findChunk("COMM");
	if (!comm || writeBE32At(m_fh, comm->offset + 8 + 2,
			newFrameCount) != AF_SUCCEED)
		return AF_FAIL;

	// Refresh the layout first so MARK/INST are located after any move.
	ssnd->size = (uint32_t) newSsndSize;
	ssnd->padded = newSsndPadded;
	for (size_t i=0; i<m_chunkLayout.size(); i++)
	{
		AIFFChunkDesc &d = m_chunkLayout[i];
		if (d.tag != "SSND" && (AFfileoffset) d.offset >= oldChunkRegion)
			d.offset += chunkShift;
	}
	m_formSize = (uint32_t) (newFileLength - 8);

	// MARK positions: overwrite each position field, preserving IDs,
	// names, ordering and padding byte for byte.
	if (m_markCount == 1)
	{
		AIFFChunkDesc *mark = findChunk("MARK");
		if (!mark)
			return AF_FAIL;
		AFfileoffset *positionOffsets = (AFfileoffset *) _af_malloc(
			sizeof(AFfileoffset) * (track->markerCount + 1));
		if (!positionOffsets)
			return AF_FAIL;
		status markStatus = collectMarkPositionOffsets(m_fh, *mark,
			(unsigned) track->markerCount, positionOffsets);
		if (markStatus == AF_SUCCEED)
		{
			for (int i=0; i<track->markerCount; i++)
			{
				if (writeBE32At(m_fh, positionOffsets[i],
						(uint32_t) track->markers[i].position) != AF_SUCCEED)
				{
					markStatus = AF_FAIL;
					break;
				}
			}
		}
		free(positionOffsets);
		if (markStatus != AF_SUCCEED)
			return AF_FAIL;
	}

	// INST play modes: eight bytes of instrument parameters precede the
	// sustain loop mode; the release loop mode starts 14 bytes into the
	// chunk data (after the 6-byte sustain loop).
	if (m_instCount == 1 && m_instruments)
	{
		AIFFChunkDesc *inst = findChunk("INST");
		Instrument *instrument = &m_instruments[0];
		if (!inst)
			return AF_FAIL;
		if (writeBE16At(m_fh, inst->offset + 8 + 8,
				(uint16_t) instrument->loops[0].mode) != AF_SUCCEED)
			return AF_FAIL;
		if (writeBE16At(m_fh, inst->offset + 8 + 14,
				(uint16_t) instrument->loops[1].mode) != AF_SUCCEED)
			return AF_FAIL;
	}

	// Keep the header offset caches authoritative for any later writer.
	if (AIFFChunkDesc *c = findChunk("COMM")) m_COMM_offset = c->offset;
	if (AIFFChunkDesc *c = findChunk("MARK")) m_MARK_offset = c->offset;
	if (AIFFChunkDesc *c = findChunk("INST")) m_INST_offset = c->offset;
	if (AIFFChunkDesc *c = findChunk("AESD")) m_AESD_offset = c->offset;
	if (AIFFChunkDesc *c = findChunk("SSND")) m_SSND_offset = c->offset;

	return AF_SUCCEED;
}

status AIFFFile::remapMarkersAndLoops(AFframecount start,
	AFframecount eraseFrames, AFframecount insertFrames)
{
	Track *track = getTrack();

	// Remap marker positions; IDs, names and ordering are preserved.
	for (int i=0; i<track->markerCount; i++)
	{
		AFframecount x = (AFframecount) track->markers[i].position;
		track->markers[i].position =
			(unsigned long) mapPosition(x, start, eraseFrames, insertFrames);
	}

	// An active loop whose start reaches or passes its end is disabled.
	if (m_instCount == 1 && m_instruments)
	{
		Instrument *instrument = &m_instruments[0];
		for (int i=0; i<instrument->loopCount; i++)
		{
			Loop *loop = &instrument->loops[i];
			if (loop->mode == AF_LOOP_MODE_NOLOOP)
				continue;
			AFframecount begin = markerPosition(track, loop->beginMarker);
			AFframecount end = markerPosition(track, loop->endMarker);
			if (begin < 0 || end < 0 || begin >= end)
				loop->mode = AF_LOOP_MODE_NOLOOP;
		}
	}

	return AF_SUCCEED;
}

status AIFFFile::refreshTrackState(AFframecount start,
	AFframecount eraseFrames, AFframecount insertFrames)
{
	Track *track = getTrack();
	size_t bytesPerFrame = track->f.bytesPerFrame(false);
	AFframecount newTotal =
		track->totalfframes - eraseFrames + insertFrames;

	track->totalfframes = newTotal;
	track->data_size =
		(AFfileoffset) newTotal * (AFfileoffset) bytesPerFrame;
	track->fpos_after_data =
		track->fpos_first_frame + track->data_size;

	// Remap the read cursor through the splice.
	AFframecount mapped = mapPosition(track->nextvframe,
		start, eraseFrames, insertFrames);
	if (mapped > newTotal)
		mapped = newTotal;
	track->nextvframe = mapped;
	track->nextfframe = mapped;
	track->fpos_next_frame =
		track->fpos_first_frame + (AFfileoffset) mapped * bytesPerFrame;
	track->totalvframes = newTotal;

	// Rebuild the decode/conversion pipeline on next use so no buffered
	// pre-splice samples can leak out.
	track->ms->setDirty();

	return AF_SUCCEED;
}

status AIFFFile::spliceFrames(int trackid, AFframecount start,
	AFframecount eraseFrames, const void *insertPCM,
	AFframecount insertFrames)
{
	if (m_access != _AF_RW_ACCESS)
	{
		_af_error(AF_BAD_NOWRITEACC,
			"file must be opened with mode \"r+\" to splice frames");
		return AF_FAIL;
	}

	Track *track = getTrack(trackid);
	if (!track)
		return AF_FAIL;

	// Re-check eligibility so every edit observes the constraints.
	if (editEligible() != AF_SUCCEED)
		return AF_FAIL;

	/* ---- Validate parameters before touching the file. ---- */

	if (start < 0 || eraseFrames < 0 || insertFrames < 0)
	{
		_af_error(AF_BAD_FRAME, "negative splice parameter");
		return AF_FAIL;
	}

	const AFframecount totalFrames = track->totalfframes;
	if (start > totalFrames || eraseFrames > totalFrames - start)
	{
		_af_error(AF_BAD_FRAME, "splice range is out of bounds");
		return AF_FAIL;
	}

	const size_t bytesPerFrame = track->f.bytesPerFrame(false);
	if (bytesPerFrame == 0)
		return AF_FAIL;

	if (insertFrames > (AFframecount) (INT64_MAX / (int64_t) bytesPerFrame))
	{
		_af_error(AF_BAD_SAMPCNT, "splice length overflow");
		return AF_FAIL;
	}
	const int64_t insertBytes =
		(int64_t) insertFrames * (int64_t) bytesPerFrame;
	if (insertFrames > 0 && insertPCM == NULL)
	{
		_af_error(AF_BAD_SAMPCNT,
			"insertion requires a non-null PCM buffer");
		return AF_FAIL;
	}

	AFframecount newTotal = totalFrames - eraseFrames + insertFrames;
	if (newTotal < 0 || newTotal > (AFframecount) UINT32_MAX)
	{
		_af_error(AF_BAD_SAMPCNT, "resulting frame count overflows");
		return AF_FAIL;
	}

	const int64_t frameDelta =
		(int64_t) insertFrames - (int64_t) eraseFrames;
	int64_t delta;
	if (frameDelta > 0 &&
		frameDelta > (int64_t) (INT64_MAX / (int64_t) bytesPerFrame))
	{
		_af_error(AF_BAD_SAMPCNT, "splice byte length overflow");
		return AF_FAIL;
	}
	delta = frameDelta * (int64_t) bytesPerFrame;

	const AIFFChunkDesc *ssnd = findChunk("SSND");
	if (!ssnd)
		return AF_FAIL;

	const AFfileoffset dataStart = track->fpos_first_frame;
	const uint64_t oldPayload = (uint64_t) totalFrames * bytesPerFrame;
	const uint64_t newPayload = (uint64_t) newTotal * bytesPerFrame;
	const bool oldSsndPadded = ssnd->padded;
	const uint64_t newSsndSize =
		8 + (uint64_t) m_ssndDataOffset + newPayload;
	if (newSsndSize > UINT32_MAX)
	{
		_af_error(AF_BAD_SAMPCNT, "SSND chunk size overflow");
		return AF_FAIL;
	}
	const bool newSsndPadded = (newSsndSize % 2) != 0;

	const AFfileoffset eraseByteStart =
		dataStart + (AFfileoffset) start * bytesPerFrame;
	const AFfileoffset tailSource =
		dataStart + (AFfileoffset) (start + eraseFrames) * bytesPerFrame;
	const AFfileoffset retainedLength =
		(AFfileoffset) oldPayload -
		(AFfileoffset) (start + eraseFrames) * bytesPerFrame;
	const AFfileoffset oldPayloadEnd = dataStart + oldPayload;
	const AFfileoffset newPayloadEnd = dataStart + newPayload;
	const AFfileoffset oldChunkRegion =
		oldPayloadEnd + (oldSsndPadded ? 1 : 0);

	off_t oldFileLength = m_fh->length();
	if (oldFileLength < 0)
		return AF_FAIL;
	const AFfileoffset chunkRegionLength = oldFileLength - oldChunkRegion;
	const int64_t chunkShift =
		delta + (newSsndPadded ? 1 : 0) - (oldSsndPadded ? 1 : 0);
	const int64_t newFileLength = (int64_t) oldFileLength + chunkShift;
	if (newFileLength < 0 || newFileLength > (int64_t) UINT32_MAX + 8)
	{
		_af_error(AF_BAD_SAMPCNT, "resulting file length overflows");
		return AF_FAIL;
	}

	// A no-op splice succeeds without I/O.
	if (eraseFrames == 0 && insertFrames == 0)
		return AF_SUCCEED;

	/*
		Move the two relocatable byte ranges -- the retained PCM block
		and the raw region of chunks following SSND -- without a
		full-file copy.  When growing, trailing chunks are evacuated
		first so the forward audio shift cannot overwrite them; when
		shrinking, audio is moved first.  Each move uses a fixed-size
		scratch buffer and picks its copy direction to tolerate overlap.
	*/
	const bool growing = chunkShift > 0;

	if (growing)
	{
		if (chunkRegionLength > 0 && chunkShift != 0 &&
			moveRegion(m_fh, oldChunkRegion,
				oldChunkRegion + chunkShift,
				chunkRegionLength) != AF_SUCCEED)
			return AF_FAIL;
		if (retainedLength > 0 && delta != 0 &&
			moveRegion(m_fh, tailSource, tailSource + delta,
				retainedLength) != AF_SUCCEED)
			return AF_FAIL;
	}
	else
	{
		if (retainedLength > 0 && delta != 0 &&
			moveRegion(m_fh, tailSource, tailSource + delta,
				retainedLength) != AF_SUCCEED)
			return AF_FAIL;
		if (chunkRegionLength > 0 && chunkShift != 0 &&
			moveRegion(m_fh, oldChunkRegion,
				oldChunkRegion + chunkShift,
				chunkRegionLength) != AF_SUCCEED)
			return AF_FAIL;
	}

	/*
		Write the inserted native, big-endian, interleaved PCM into the
		opened gap.
	*/
	if (insertFrames > 0)
	{
		if (m_fh->seek(eraseByteStart, File::SeekFromBeginning) !=
				eraseByteStart ||
			m_fh->write(insertPCM, (size_t) insertBytes) !=
				(ssize_t) insertBytes)
			return AF_FAIL;
	}

	if (newFileLength < oldFileLength)
	{
		if (m_fh->truncate(newFileLength) != 0)
			return AF_FAIL;
	}

	/*
		4. Remap markers and loop modes in memory first so the header
		   rewrite persists the post-splice anchor coordinates.
	*/
	if (remapMarkersAndLoops(start, eraseFrames, insertFrames) != AF_SUCCEED)
		return AF_FAIL;

	/*
		5. Update headers, marker positions, loop modes, the chunk layout
		   and the handle state.
	*/
	if (rewriteChunkHeaders(chunkShift, (uint32_t) newTotal, newSsndSize,
			newSsndPadded, newPayloadEnd, oldChunkRegion) != AF_SUCCEED)
		return AF_FAIL;

	if (refreshTrackState(start, eraseFrames, insertFrames) != AF_SUCCEED)
		return AF_FAIL;

	return AF_SUCCEED;
}

int afSpliceFrames(AFfilehandle file, int track, AFframecount start,
	AFframecount eraseFrames, const void *insertPCM,
	AFframecount insertFrames)
{
	if (!_af_filehandle_ok(file))
		return AF_FAIL;

	return file->spliceFrames(track, start, eraseFrames,
		insertPCM, insertFrames);
}

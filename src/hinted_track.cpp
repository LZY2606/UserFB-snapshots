/*
 * The contents of this file are subject to the Mozilla Public
 * License Version 1.1 (the "License"); you may not use this file
 * except in compliance with the License. You may obtain a copy of
 * the License at http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS
 * IS" basis, WITHOUT WARRANTY OF ANY KIND, either express or
 * implied. See the License for the specific language governing
 * rights and limitations under the License.
 *
 * The Original Code is MPEG4IP.
 *
 * The Initial Developer of the Original Code is Cisco Systems Inc.
 * Portions created by Cisco Systems Inc. are
 * Copyright (C) Cisco Systems Inc. 2001.  All Rights Reserved.
 *
 * Contributor(s):
 *      copy of an RTP hinted video track driven by the media edit list
 */

#include "src/impl.h"

#include <vector>

using namespace mp4v2::impl;

namespace {

///////////////////////////////////////////////////////////////////////////////
// RTP hint sample grammar (ISO/IEC 14496-16), big-endian on disk:
//
//  u16 packetCount, u16 reserved
//  per packet:
//    u32 relativeTransmitTime
//    u8  reserved1(2) P(1) X(1) reserved2(4)
//    u8  M(1) payloadType(7)
//    u16 sequenceNumber
//    u16 reserved3(13) extraFlag(1) bFrameFlag(1) repeatFlag(1)
//    u16 entryCount
//    [extra information when extraFlag set -- rejected]
//    per entry:
//      u8 type 0 null        : 15 pad bytes
//      u8 type 1 immediate   : u8 count, 14 data bytes
//      u8 type 2 sample data : u8 trackRefIndex, u16 length, u32 sampleNumber,
//                              u32 sampleOffset, u16 bytesPerBlock,
//                              u16 samplesPerBlock
//      u8 type 3 sample description -- rejected
///////////////////////////////////////////////////////////////////////////////

struct ParsedEntry {
    uint8_t  type;
    uint8_t  trackRefIndex;
    uint16_t length;
    uint32_t sampleNumber;
    uint32_t sampleOffset;
    uint16_t bytesPerBlock;
    uint16_t samplesPerBlock;
    uint32_t sampleNumberFieldOffset;
};

struct ParsedPacket {
    uint32_t sequenceNumberFieldOffset;
    std::vector<ParsedEntry> entries;
};

struct ParsedHint {
    std::vector<ParsedPacket> packets;
};

struct Segment {
    bool          empty;
    MP4Duration   movieDuration;   // source movie timescale units
    MP4Duration   mediaDuration;   // media timescale units
    MP4SampleId   firstSampleId;   // inclusive, media segments only
    MP4SampleId   lastSampleId;    // inclusive, media segments only
};

static uint16_t read16(const uint8_t* p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t read32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
           | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void write16(uint8_t* p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)(value & 0xff);
}

static void write32(uint8_t* p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)(value & 0xff);
}

// exact, lossless time scale conversion: no rounding, no truncation
static bool exactScale(uint64_t value, uint32_t fromTimeScale,
                       uint32_t toTimeScale, uint64_t& result)
{
    if (fromTimeScale == 0 || toTimeScale == 0) {
        return false;
    }
    if (value != 0 && toTimeScale > UINT64_MAX / value) {
        return false;
    }
    uint64_t product = value * (uint64_t)toTimeScale;
    if (product % fromTimeScale != 0) {
        return false;
    }
    result = product / fromTimeScale;
    return true;
}

static void parseHintSample(const uint8_t* bytes, uint32_t size,
                            ParsedHint& hint)
{
    uint32_t pos = 0;

    if (size < 4) {
        throw new Exception("truncated RTP hint sample",
                            __FILE__, __LINE__, __FUNCTION__);
    }

    uint16_t packetCount = read16(bytes + pos);
    pos += 4; // packetCount + reserved

    for (uint16_t pi = 0; pi < packetCount; ++pi) {
        ParsedPacket packet;

        if (pos + 12 > size) {
            throw new Exception("truncated RTP packet entry",
                                __FILE__, __LINE__, __FUNCTION__);
        }

        pos += 4; // relative transmit time, preserved verbatim
        pos += 1; // reserved1/Pbit/Xbit/reserved2, preserved verbatim
        pos += 1; // Mbit/payloadType, preserved verbatim

        packet.sequenceNumberFieldOffset = pos;
        pos += 2; // source sequence number; renumbered on output

        uint16_t flags = read16(bytes + pos);
        pos += 2;
        bool extraFlag = (flags & 0x0004) != 0;
        if (extraFlag) {
            throw new Exception("RTP packet extra information is unsupported",
                                __FILE__, __LINE__, __FUNCTION__);
        }

        uint16_t entryCount = read16(bytes + pos);
        pos += 2;

        for (uint16_t ei = 0; ei < entryCount; ++ei) {
            if (pos + 1 > size) {
                throw new Exception("truncated RTP data entry",
                                    __FILE__, __LINE__, __FUNCTION__);
            }

            ParsedEntry entry;
            entry.type = bytes[pos++];

            if (entry.type == 0) {
                // null data: 15 pad bytes
                if (pos + 15 > size) {
                    throw new Exception("truncated RTP null data",
                                        __FILE__, __LINE__, __FUNCTION__);
                }
                pos += 15;

            } else if (entry.type == 1) {
                // immediate data: u8 count, 14 fixed data bytes
                if (pos + 15 > size) {
                    throw new Exception("truncated RTP immediate data",
                                        __FILE__, __LINE__, __FUNCTION__);
                }
                pos += 15;

            } else if (entry.type == 2) {
                // sample data reference: 1 type byte plus 15 property bytes
                if (pos + 15 > size) {
                    throw new Exception("truncated RTP sample data",
                                        __FILE__, __LINE__, __FUNCTION__);
                }

                entry.trackRefIndex = bytes[pos++];
                entry.length = read16(bytes + pos);
                pos += 2;
                entry.sampleNumberFieldOffset = pos;
                entry.sampleNumber = read32(bytes + pos);
                pos += 4;
                entry.sampleOffset = read32(bytes + pos);
                pos += 4;
                entry.bytesPerBlock = read16(bytes + pos);
                pos += 2;
                entry.samplesPerBlock = read16(bytes + pos);
                pos += 2;

                if (entry.bytesPerBlock != 1 || entry.samplesPerBlock != 1) {
                    throw new Exception(
                        "non-trivial RTP sample data block layout is unsupported",
                        __FILE__, __LINE__, __FUNCTION__);
                }

            } else {
                throw new Exception(
                    "RTP sample-description data is unsupported",
                    __FILE__, __LINE__, __FUNCTION__);
            }

            packet.entries.push_back(entry);
        }

        hint.packets.push_back(packet);
    }

    // Any bytes past the final data entry are embedded payload blobs that
    // self-reference (trackRefIndex 255) sample entries point into by
    // sample-relative offset; their bounds are validated separately.
    if (pos > size) {
        throw new Exception("truncated RTP hint sample",
                            __FILE__, __LINE__, __FUNCTION__);
    }
}

} // namespace

///////////////////////////////////////////////////////////////////////////////

namespace mp4v2 { namespace impl {

bool CopyHintedTrack(
    MP4FileHandle srcHandle, MP4TrackId srcHintTrackId,
    MP4FileHandle dstHandle, MP4TrackId* pMediaTrackId,
    MP4TrackId* pHintTrackId)
{
    if (pMediaTrackId) {
        *pMediaTrackId = MP4_INVALID_TRACK_ID;
    }
    if (pHintTrackId) {
        *pHintTrackId = MP4_INVALID_TRACK_ID;
    }

    if (!MP4_IS_VALID_FILE_HANDLE(srcHandle)
            || !MP4_IS_VALID_FILE_HANDLE(dstHandle)
            || srcHandle == dstHandle
            || pMediaTrackId == NULL
            || pHintTrackId == NULL) {
        return false;
    }

    MP4File& srcFile = *(MP4File*)srcHandle;
    MP4File& dstFile = *(MP4File*)dstHandle;

    MP4TrackId dstMediaTrackId = MP4_INVALID_TRACK_ID;
    MP4TrackId dstHintTrackId = MP4_INVALID_TRACK_ID;

    try {
        ///////////////////////////////////////////////////////////////////////
        // Phase A: identify the tracks
        ///////////////////////////////////////////////////////////////////////

        MP4Track* srcHintTrack = srcFile.GetTrack(srcHintTrackId);
        if (strcmp(srcHintTrack->GetType(), MP4_HINT_TRACK_TYPE)) {
            return false;
        }

        MP4Atom& hintTrak = srcHintTrack->GetTrakAtom();

        MP4Integer32Property* pTrefCount = NULL;
        MP4Integer32Property* pTrefTrackId = NULL;
        if (!hintTrak.FindProperty(
                    "trak.tref.hint.entryCount",
                    (MP4Property**)&pTrefCount)
                || !hintTrak.FindProperty(
                    "trak.tref.hint.entries[0].trackId",
                    (MP4Property**)&pTrefTrackId)
                || pTrefCount->GetValue() != 1) {
            return false;
        }

        MP4TrackId srcMediaTrackId = pTrefTrackId->GetValue();
        MP4Track* srcMediaTrack = srcFile.GetTrack(srcMediaTrackId);
        if (strcmp(srcMediaTrack->GetType(), MP4_VIDEO_TRACK_TYPE)) {
            return false;
        }

        const char* mediaDataName =
            srcFile.GetTrackMediaDataName(srcMediaTrackId);
        if (mediaDataName == NULL
                || (ATOMID(mediaDataName) != ATOMID("avc1")
                    && ATOMID(mediaDataName) != ATOMID("mp4v"))) {
            return false;
        }

        // exactly one sample description per source track
        if (srcFile.GetTrackIntegerProperty(
                    srcMediaTrackId,
                    "mdia.minf.stbl.stsd.entryCount") != 1
                || srcFile.GetTrackIntegerProperty(
                    srcHintTrackId,
                    "mdia.minf.stbl.stsd.entryCount") != 1) {
            return false;
        }
        // the hint track itself must carry no edit list; a missing media
        // edit list is allowed and means "copy the whole track"
        if (srcHintTrack->GetTrakAtom().FindAtom("trak.edts.elst") != NULL) {
            return false;
        }
        bool mediaHasEdits =
            srcMediaTrack->GetTrakAtom().FindAtom("trak.edts.elst") != NULL;

        // composition offsets are not part of the supported input
        if (srcMediaTrack->GetTrakAtom().FindAtom(
                    "trak.mdia.minf.stbl.ctts") != NULL
                || hintTrak.FindAtom("trak.mdia.minf.stbl.ctts") != NULL) {
            return false;
        }

        uint32_t mediaTimeScale = srcMediaTrack->GetTimeScale();
        uint32_t hintTimeScale = srcHintTrack->GetTimeScale();
        if (mediaTimeScale == 0 || mediaTimeScale != hintTimeScale) {
            return false;
        }

        uint32_t srcMovieTimeScale = srcFile.GetTimeScale();
        uint32_t dstMovieTimeScale = dstFile.GetTimeScale();
        if (srcMovieTimeScale == 0 || dstMovieTimeScale == 0) {
            return false;
        }

        uint32_t mediaSampleCount = srcMediaTrack->GetNumberOfSamples();
        uint32_t hintSampleCount = srcHintTrack->GetNumberOfSamples();
        if (mediaSampleCount == 0
                || mediaSampleCount != hintSampleCount
                || mediaSampleCount > UINT32_MAX - 1) {
            return false;
        }

        ///////////////////////////////////////////////////////////////////////
        // Phase B: per-sample timing must line up between the two tracks
        ///////////////////////////////////////////////////////////////////////

        for (MP4SampleId sid = 1; sid <= mediaSampleCount; ++sid) {
            MP4Timestamp mediaStart = 0, hintStart = 0;
            MP4Duration mediaDuration = 0, hintDuration = 0;
            srcMediaTrack->GetSampleTimes(sid, &mediaStart, &mediaDuration);
            srcHintTrack->GetSampleTimes(sid, &hintStart, &hintDuration);
            if (mediaStart != hintStart || mediaDuration != hintDuration
                    || mediaDuration == 0) {
                return false;
            }
        }

        ///////////////////////////////////////////////////////////////////////
        // Phase C: parse every hint sample; packet sequence numbers are
        // renumbered from zero on output regardless of their source values
        ///////////////////////////////////////////////////////////////////////

        std::vector<ParsedHint> parsedHints(mediaSampleCount);

        for (MP4SampleId sid = 1; sid <= mediaSampleCount; ++sid) {
            uint8_t* rawBytes = NULL;
            uint32_t rawSize = 0;
            srcHintTrack->ReadSample(sid, &rawBytes, &rawSize);

            try {
                parseHintSample(rawBytes, rawSize, parsedHints[sid - 1]);
                MP4Free(rawBytes);
            } catch (...) {
                MP4Free(rawBytes);
                throw;
            }
        }

        ///////////////////////////////////////////////////////////////////////
        // Phase D: build the edit segment plan
        ///////////////////////////////////////////////////////////////////////

        std::vector<Segment> segments;
        uint32_t editCount = mediaHasEdits
            ? srcFile.GetTrackNumberOfEdits(srcMediaTrackId) : 0;

        if (editCount == 0) {
            // no edits: copy the whole track as one implicit segment
            Segment seg;
            seg.empty = false;
            seg.firstSampleId = 1;
            seg.lastSampleId = mediaSampleCount;

            MP4Duration totalMediaDuration = 0;
            for (MP4SampleId sid = 1; sid <= mediaSampleCount; ++sid) {
                MP4Timestamp start = 0;
                MP4Duration duration = 0;
                srcMediaTrack->GetSampleTimes(sid, &start, &duration);
                totalMediaDuration += duration;
            }
            seg.mediaDuration = totalMediaDuration;

            if (!exactScale(totalMediaDuration, mediaTimeScale,
                            srcMovieTimeScale, seg.movieDuration)) {
                return false;
            }
            segments.push_back(seg);
        } else {
            for (MP4EditId editId = 1; editId <= editCount; ++editId) {
                Segment seg;
                seg.empty = false;
                seg.firstSampleId = MP4_INVALID_SAMPLE_ID;
                seg.lastSampleId = MP4_INVALID_SAMPLE_ID;

                MP4Duration srcMovieDuration =
                    srcFile.GetTrackEditDuration(srcMediaTrackId, editId);
                MP4Timestamp mediaTime =
                    srcFile.GetTrackEditMediaStart(srcMediaTrackId, editId);
                bool dwell =
                    srcFile.GetTrackEditDwell(srcMediaTrackId, editId);

                if (srcMovieDuration == 0
                        || srcMovieDuration == MP4_INVALID_DURATION) {
                    return false;
                }
                seg.movieDuration = srcMovieDuration;

                // exact conversion into the media timescale: never round
                if (!exactScale(srcMovieDuration, srcMovieTimeScale,
                                mediaTimeScale, seg.mediaDuration)) {
                    return false;
                }

                // empty edits are either dwells (mediaRate 0, mediaTime -1)
                // or rate 1 edits whose mediaTime is the all-ones sentinel
                bool emptySentinel =
                    (mediaTime == UINT32_MAX || mediaTime == UINT64_MAX);
                if (dwell || emptySentinel) {
                    if (dwell && !emptySentinel
                            && (int64_t)mediaTime != -1) {
                        return false;
                    }
                    seg.empty = true;
                    segments.push_back(seg);
                    continue;
                }

                // rate=1 media segment; reject any other rate
                int64_t rate = (int64_t)srcFile.GetIntegerProperty(
                    srcFile.MakeTrackEditName(srcMediaTrackId, editId,
                                              "mediaRate"));
                if (rate != 1) {
                    return false;
                }

                if (mediaTime > UINT64_MAX - seg.mediaDuration) {
                    return false;
                }
                MP4Timestamp mediaEnd = mediaTime + seg.mediaDuration;
                if (mediaEnd > srcMediaTrack->GetDuration()) {
                    return false;
                }

                // the segment start must land exactly on a sample DTS
                MP4SampleId firstSid =
                    srcMediaTrack->GetSampleIdFromTime(mediaTime, false);
                MP4Timestamp firstStart = 0;
                MP4Duration firstDuration = 0;
                srcMediaTrack->GetSampleTimes(
                    firstSid, &firstStart, &firstDuration);
                if (firstStart != mediaTime
                        || !srcMediaTrack->IsSyncSample(firstSid)) {
                    return false;
                }

                // and the segment end must land exactly on a sample boundary
                MP4Duration accumulated = 0;
                MP4SampleId sid = firstSid;
                while (accumulated < seg.mediaDuration) {
                    MP4Timestamp start = 0;
                    MP4Duration duration = 0;
                    srcMediaTrack->GetSampleTimes(sid, &start, &duration);
                    if (accumulated == 0 && start != mediaTime) {
                        return false;
                    }
                    accumulated += duration;
                    if (accumulated > seg.mediaDuration) {
                        return false;
                    }
                    if (accumulated == seg.mediaDuration) {
                        break;
                    }
                    ++sid;
                    if (sid > mediaSampleCount) {
                        return false;
                    }
                }
                if (accumulated != seg.mediaDuration) {
                    return false;
                }
                seg.firstSampleId = firstSid;
                seg.lastSampleId = sid;

                segments.push_back(seg);
            }
        }

        // at least one media segment must exist
        bool anyMedia = false;
        for (size_t i = 0; i < segments.size(); ++i) {
            if (!segments[i].empty) {
                anyMedia = true;
                break;
            }
        }
        if (!anyMedia) {
            return false;
        }

        ///////////////////////////////////////////////////////////////////////
        // Phase E: validate all sample references, occurrence by occurrence
        ///////////////////////////////////////////////////////////////////////

        MP4SampleId copiedMediaCount = 0;
        MP4SampleId outputHintCount = 0;

        for (size_t si = 0; si < segments.size(); ++si) {
            const Segment& seg = segments[si];

            if (seg.empty) {
                if (outputHintCount == UINT32_MAX) {
                    return false;
                }
                ++outputHintCount;
                continue;
            }

            for (MP4SampleId sid = seg.firstSampleId;
                    sid <= seg.lastSampleId; ++sid) {
                ++copiedMediaCount;
                if (copiedMediaCount == UINT32_MAX
                        || outputHintCount == UINT32_MAX) {
                    return false;
                }
                ++outputHintCount;
                const ParsedHint& hint = parsedHints[sid - 1];

                for (size_t pi = 0; pi < hint.packets.size(); ++pi) {
                    const ParsedPacket& packet = hint.packets[pi];
                    for (size_t ei = 0; ei < packet.entries.size(); ++ei) {
                        const ParsedEntry& entry = packet.entries[ei];
                        if (entry.type != 2) {
                            continue;
                        }

                        if (entry.trackRefIndex == 0) {
                            // media reference: must stay inside this segment
                            if (entry.sampleNumber < seg.firstSampleId
                                    || entry.sampleNumber > seg.lastSampleId
                                    || entry.sampleNumber == 0) {
                                return false;
                            }

                            uint32_t mediaSampleSize =
                                srcMediaTrack->GetSampleSize(entry.sampleNumber);
                            if ((uint64_t)entry.sampleOffset + entry.length
                                    > mediaSampleSize) {
                                return false;
                            }
                        } else if (entry.trackRefIndex == 255) {
                            // self reference: only the current hint sample,
                            // never a cross-sample reference
                            if (entry.sampleNumber != sid) {
                                return false;
                            }

                            uint32_t hintSampleSize =
                                srcHintTrack->GetSampleSize(sid);
                            if ((uint64_t)entry.sampleOffset + entry.length
                                    > hintSampleSize) {
                                return false;
                            }
                        } else {
                            return false;
                        }
                    }
                }
            }
        }

        ///////////////////////////////////////////////////////////////////////
        // Phase F: destination time math, including capacity checks
        ///////////////////////////////////////////////////////////////////////

        uint64_t totalMovieDurationSrc = 0;
        for (size_t i = 0; i < segments.size(); ++i) {
            if (segments[i].movieDuration > UINT64_MAX - totalMovieDurationSrc) {
                return false;
            }
            totalMovieDurationSrc += segments[i].movieDuration;
        }

        uint64_t totalMovieDurationDst = 0;
        if (!exactScale(totalMovieDurationSrc, srcMovieTimeScale,
                        dstMovieTimeScale, totalMovieDurationDst)) {
            return false;
        }

        MP4Duration newMediaDuration = 0;
        MP4Duration newHintDuration = 0;
        for (size_t i = 0; i < segments.size(); ++i) {
            const Segment& seg = segments[i];
            if (!seg.empty) {
                newMediaDuration += seg.mediaDuration;
            }
            newHintDuration += seg.mediaDuration;
        }

        // newly generated atoms use version 0 (32-bit) duration fields
        if (totalMovieDurationDst > UINT32_MAX
                || newMediaDuration > UINT32_MAX
                || newHintDuration > UINT32_MAX) {
            return false;
        }

        for (size_t i = 0; i < segments.size(); ++i) {
            uint64_t dstEditDuration = 0;
            if (!exactScale(segments[i].movieDuration, srcMovieTimeScale,
                            dstMovieTimeScale, dstEditDuration)
                    || dstEditDuration > UINT32_MAX) {
                return false;
            }
        }

        // All validation passed. From here on nothing in the source file or
        // the pre-existing destination tracks is modified until construction
        // succeeds; a failure rolls the two new tracks back.
        //
        ///////////////////////////////////////////////////////////////////////
        // Phase G: create the destination tracks
        ///////////////////////////////////////////////////////////////////////

        dstMediaTrackId = MP4CloneTrack(
            srcHandle, srcMediaTrackId, dstHandle,
            MP4_INVALID_TRACK_ID);
        if (dstMediaTrackId == MP4_INVALID_TRACK_ID) {
            return false;
        }

        try {
            dstHintTrackId = dstFile.AddHintTrack(dstMediaTrackId);

            MP4RtpHintTrack* pSrcHint =
                (MP4RtpHintTrack*)srcFile.GetTrack(srcHintTrackId);
            MP4RtpHintTrack* pDstHint =
                (MP4RtpHintTrack*)dstFile.GetTrack(dstHintTrackId);

            char* payloadName = NULL;
            char* encodingParms = NULL;
            uint8_t payloadNumber = 0;
            uint16_t maxPayloadSize = 0;
            pSrcHint->GetPayload(&payloadName, &payloadNumber,
                                 &maxPayloadSize, &encodingParms);
            if (payloadName == NULL || payloadName[0] == '\0') {
                MP4Free(payloadName);
                MP4Free(encodingParms);
                throw new Exception("hint track has no RTP payload",
                                    __FILE__, __LINE__, __FUNCTION__);
            }
            pDstHint->SetPayload(payloadName, payloadNumber, maxPayloadSize,
                                 encodingParms, true, true);
            MP4Free(payloadName);
            MP4Free(encodingParms);

            ///////////////////////////////////////////////////////////////
            // Phase H: walk the segments, copying media and hint samples
            ///////////////////////////////////////////////////////////////

            MP4SampleId runningMediaId = 0;
            MP4SampleId runningHintId = 0;
            uint32_t runningSequence = 0;
            static const uint8_t emptyHintBytes[4] = { 0, 0, 0, 0 };

            for (size_t si = 0; si < segments.size(); ++si) {
                const Segment& seg = segments[si];

                if (seg.empty) {
                    ++runningHintId;
                    dstFile.WriteSample(
                        dstHintTrackId, emptyHintBytes,
                        sizeof(emptyHintBytes), seg.mediaDuration, 0, true);
                    continue;
                }

                for (MP4SampleId sid = seg.firstSampleId;
                        sid <= seg.lastSampleId; ++sid) {
                    ++runningMediaId;
                    ++runningHintId;

                    // media payload is copied independently per occurrence
                    MP4File::CopySample(
                        &srcFile, srcMediaTrackId, sid,
                        &dstFile, dstMediaTrackId, MP4_INVALID_DURATION);

                    // rebuild this hint sample with rebound references
                    uint8_t* rawBytes = NULL;
                    uint32_t rawSize = 0;
                    srcHintTrack->ReadSample(sid, &rawBytes, &rawSize);

                    std::vector<uint8_t> hintBytes(rawBytes, rawBytes + rawSize);
                    MP4Free(rawBytes);

                    const ParsedHint& hint = parsedHints[sid - 1];
                    for (size_t pi = 0; pi < hint.packets.size(); ++pi) {
                        const ParsedPacket& packet = hint.packets[pi];
                        write16(&hintBytes[packet.sequenceNumberFieldOffset],
                                (uint16_t)(runningSequence++ & 0xffff));

                        for (size_t ei = 0; ei < packet.entries.size(); ++ei) {
                            const ParsedEntry& entry = packet.entries[ei];
                            if (entry.type != 2) {
                                continue;
                            }

                            uint32_t targetSampleId = 0;
                            if (entry.trackRefIndex == 0) {
                                targetSampleId =
                                    runningMediaId
                                    - (sid - seg.firstSampleId);
                                targetSampleId +=
                                    (entry.sampleNumber - seg.firstSampleId);
                            } else { // 255: self reference
                                targetSampleId = runningHintId;
                            }
                            write32(
                                &hintBytes[entry.sampleNumberFieldOffset],
                                targetSampleId);
                            // the sample-relative offset and length are
                            // unchanged: the hint byte layout is copied
                            // verbatim, only the sample id is rebound
                        }
                    }

                    MP4Timestamp hintStart = 0;
                    MP4Duration hintDuration = 0;
                    srcHintTrack->GetSampleTimes(
                        sid, &hintStart, &hintDuration);
                    bool sync = srcHintTrack->IsSyncSample(sid);

                    dstFile.WriteSample(
                        dstHintTrackId, &hintBytes[0],
                        (uint32_t)hintBytes.size(),
                        hintDuration, 0, sync);
                }
            }

            ///////////////////////////////////////////////////////////////
            // Phase I: rebuild the media edit list on the destination
            ///////////////////////////////////////////////////////////////

            if (editCount != 0) {
                MP4Timestamp dstMediaTime = 0;
                for (size_t si = 0; si < segments.size(); ++si) {
                    const Segment& seg = segments[si];

                    MP4EditId newEdit = dstFile.AddTrackEdit(
                        dstMediaTrackId, MP4_INVALID_EDIT_ID);
                    if (newEdit == MP4_INVALID_EDIT_ID) {
                        throw new Exception("failed to add track edit",
                                            __FILE__, __LINE__, __FUNCTION__);
                    }

                    uint64_t dstEditDuration = 0;
                    if (!exactScale(seg.movieDuration, srcMovieTimeScale,
                                    dstMovieTimeScale, dstEditDuration)) {
                        throw new Exception("edit duration conversion failed",
                                            __FILE__, __LINE__, __FUNCTION__);
                    }
                    dstFile.SetTrackEditDuration(
                        dstMediaTrackId, newEdit, dstEditDuration);

                    if (seg.empty) {
                        // encode the empty segment the same way the library
                        // does: rate 1 with the all-ones mediaTime sentinel
                        dstFile.SetTrackEditMediaStart(
                            dstMediaTrackId, newEdit, (uint64_t)(int64_t)-1);
                        dstFile.SetTrackEditDwell(
                            dstMediaTrackId, newEdit, false);
                    } else {
                        dstFile.SetTrackEditMediaStart(
                            dstMediaTrackId, newEdit, dstMediaTime);
                        dstFile.SetTrackEditDwell(
                            dstMediaTrackId, newEdit, false);
                        dstMediaTime += seg.mediaDuration;
                    }
                }
            }

            ///////////////////////////////////////////////////////////////
            // Phase J: force the two timelines to their exact durations
            ///////////////////////////////////////////////////////////////

            dstFile.SetTrackIntegerProperty(
                dstMediaTrackId, "mdia.mdhd.duration", newMediaDuration);
            dstFile.SetTrackIntegerProperty(
                dstHintTrackId, "mdia.mdhd.duration", newHintDuration);

            dstFile.SetTrackIntegerProperty(
                dstMediaTrackId, "tkhd.duration", totalMovieDurationDst);
            dstFile.SetTrackIntegerProperty(
                dstHintTrackId, "tkhd.duration", totalMovieDurationDst);

            if (dstFile.GetDuration() < totalMovieDurationDst) {
                dstFile.SetDuration(totalMovieDurationDst);
            }
        } catch (...) {
            // do not leave a half pair of tracks behind
            if (dstHintTrackId != MP4_INVALID_TRACK_ID) {
                dstFile.DeleteTrack(dstHintTrackId);
            }
            dstFile.DeleteTrack(dstMediaTrackId);
            throw;
        }

        *pMediaTrackId = dstMediaTrackId;
        *pHintTrackId = dstHintTrackId;
        return true;

    } catch (Exception* x) {
        mp4v2::impl::log.errorf(*x);
        delete x;
    } catch (...) {
        mp4v2::impl::log.errorf("%s: failed", __FUNCTION__);
    }

    // construction-time failure: roll back anything that was created
    if (MP4_IS_VALID_FILE_HANDLE(dstHandle)) {
        try {
            if (dstHintTrackId != MP4_INVALID_TRACK_ID) {
                ((MP4File*)dstHandle)->DeleteTrack(dstHintTrackId);
            }
            if (dstMediaTrackId != MP4_INVALID_TRACK_ID) {
                ((MP4File*)dstHandle)->DeleteTrack(dstMediaTrackId);
            }
        } catch (...) {
            // nothing else we can do
        }
    }

    return false;
}

}} // namespace mp4v2::impl

///////////////////////////////////////////////////////////////////////////////

extern "C"

bool MP4CopyHintedTrack(
    MP4FileHandle srcFile,
    MP4TrackId    srcHintTrackId,
    MP4FileHandle dstFile,
    MP4TrackId*   pMediaTrackId,
    MP4TrackId*   pHintTrackId)
{
    return mp4v2::impl::CopyHintedTrack(
        srcFile, srcHintTrackId, dstFile, pMediaTrackId, pHintTrackId);
}

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
 * The Original Code is MP4v2.
 */

#include "src/impl.h"
#include "src/mp4hintedcopy.h"

#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace mp4v2 {
namespace impl {

///////////////////////////////////////////////////////////////////////////////

namespace {

// ---- parsed, file-neutral representation of a source hint sample --------

struct RawEntry {
    uint8_t  type;                 // 0 null, 1 immediate, 2 sample
    // immediate (type 1)
    uint8_t  immCount;
    uint8_t  immBytes[14];
    // sample reference (type 2)
    uint8_t  trackRefIndex;
    uint16_t length;
    uint32_t sampleNumber;
    uint32_t sampleOffset;
    uint16_t bytesPerBlock;
    uint16_t samplesPerBlock;
};

struct RawPacket {
    int32_t               xmitOffset;
    bool                  mbit;
    bool                  pbit;
    bool                  xbit;
    bool                  bframe;
    uint8_t               payloadType;
    std::vector<RawEntry> entries;
};

struct RawHint {
    std::vector<RawPacket> packets;
    bool                   sync;
    uint8_t*               rawBytes;   // backing bytes of the hint sample
    uint32_t              rawSize;

    RawHint() : sync(false), rawBytes(NULL), rawSize(0) { }
};

struct EditSegment {
    bool          isGap;
    // movie-timescale units (source and destination)
    uint64_t      srcMovieDuration;
    uint64_t      dstMovieDuration;
    // track-timescale units
    uint64_t      trackDuration;
    // media segments only
    uint32_t      firstSample;      // 1-based source media sample id
    uint32_t      lastSample;       // inclusive
    uint64_t      mediaStart;       // source media time (track units)
    uint64_t      outMediaStart;    // output media time (track units)
    MP4SampleId   outFirstSample;   // first output media sample id
};

// exact integer scaling: fails on non-integral conversion or 64 bit overflow
bool scaleExact(uint64_t value, uint32_t fromScale, uint32_t toScale,
                uint64_t* result)
{
    if (fromScale == 0 || toScale == 0) {
        return false;
    }
    if (fromScale == toScale) {
        *result = value;
        return true;
    }
    if (value != 0 && value > UINT64_MAX / (uint64_t)toScale) {
        return false;
    }
    uint64_t scaled = value * (uint64_t)toScale;
    if (scaled % (uint64_t)fromScale != 0) {
        return false;
    }
    *result = scaled / (uint64_t)fromScale;
    return true;
}

bool addChecked(uint64_t a, uint64_t b, uint64_t* sum)
{
    if (b > UINT64_MAX - a) {
        return false;
    }
    *sum = a + b;
    return true;
}

uint8_t moviePropertyWidth(MP4File& file, const char* name)
{
    MP4Property* p = NULL;
    MP4Atom* mvhd = file.FindAtom("moov.mvhd");
    if (mvhd && mvhd->FindProperty(name, (MP4Property**)&p)
            && p && p->GetType() == Integer64Property) {
        return 64;
    }
    return 32;
}

bool fitsWidth(uint64_t value, uint8_t width)
{
    return width == 64 || value <= (uint64_t)UINT32_MAX;
}

// mediaTime value that marks an empty (no-media) edit in an elst
uint64_t editEmptySentinel(MP4File& file, MP4TrackId trackId)
{
    MP4Atom* pElst = file.FindTrackAtom(trackId, "edts.elst");
    MP4Property* pVersion = NULL;
    if (pElst
            && pElst->FindProperty("trak.edts.elst.version",
                                   (MP4Property**)&pVersion)
            && ((MP4Integer8Property*)pVersion)->GetValue() == 1) {
        return UINT64_MAX;
    }
    return 0xFFFFFFFFULL;
}

MP4Property* findPacketProperty(MP4RtpPacket& packet, const char* name)
{
    MP4Property* p = NULL;
    if (!packet.FindProperty(name, (MP4Property**)&p)) {
        throw new Exception("missing RTP packet property",
                            __FILE__, __LINE__, __FUNCTION__);
    }
    return p;
}

MP4Property* findEntryProperty(MP4RtpData& data, const char* name)
{
    MP4Property* p = NULL;
    if (!data.FindProperty(name, (MP4Property**)&p)) {
        throw new Exception("missing RTP data entry property",
                            __FILE__, __LINE__, __FUNCTION__);
    }
    return p;
}

uint64_t entryValue(MP4RtpData& data, const char* name)
{
    MP4Property* p = findEntryProperty(data, name);
    return ((MP4IntegerProperty*)p)->GetValue();
}

// parse one source hint sample into file-neutral structures
RawHint parseHintSample(MP4RtpHintTrack& hintTrack, MP4SampleId hintSampleId)
{
    MP4File& file = hintTrack.GetFile();

    bool sync = false;
    uint8_t* rawBytes = NULL;
    uint32_t rawSize = 0;
    file.ReadSample(hintTrack.GetId(), hintSampleId,
                    &rawBytes, &rawSize, NULL, NULL, NULL, &sync);

    RawHint raw;
    raw.rawBytes = rawBytes;
    raw.rawSize = rawSize;
    raw.sync = sync;

    file.EnableMemoryBuffer(rawBytes, rawSize);
    MP4RtpHint* pHint = NULL;
    try {
        pHint = new MP4RtpHint(hintTrack);
        pHint->Read(file);

        uint16_t numPackets = pHint->GetNumberOfPackets();
        raw.packets.resize(numPackets);

        for (uint16_t pi = 0; pi < numPackets; pi++) {
            MP4RtpPacket* pPacket = pHint->GetPacket(pi);
            RawPacket& rp = raw.packets[pi];

            rp.xmitOffset = pPacket->GetTransmitOffset();
            rp.mbit = pPacket->GetMBit();
            rp.pbit = pPacket->GetPBit();
            rp.xbit = pPacket->GetXBit();
            rp.bframe = pPacket->IsBFrame();
            rp.payloadType = pPacket->GetPayload();

            MP4Property* pExtra = NULL;
            if (pPacket->FindProperty("extraFlag", (MP4Property**)&pExtra)
                    && ((MP4BitfieldProperty*)pExtra)->GetValue() != 0) {
                throw new Exception(
                    "unsupported RTP packet extra information",
                    __FILE__, __LINE__, __FUNCTION__);
            }
            MP4Property* pRepeat = NULL;
            if (pPacket->FindProperty("repeatFlag",
                                      (MP4Property**)&pRepeat)
                    && ((MP4BitfieldProperty*)pRepeat)->GetValue() != 0) {
                throw new Exception("unsupported repeated RTP packet",
                                    __FILE__, __LINE__, __FUNCTION__);
            }

            uint16_t numEntries = pPacket->GetDataEntryCount();
            for (uint16_t ei = 0; ei < numEntries; ei++) {
                MP4RtpData* pData = pPacket->GetDataEntry(ei);

                RawEntry entry;
                std::memset(&entry, 0, sizeof(entry));
                entry.type = (uint8_t)
                    ((MP4Integer8Property*)findEntryProperty(
                         *pData, "type"))->GetValue();

                if (entry.type == 0) {
                    // null data: its 15 pad bytes need no interpretation
                } else if (entry.type == 1) {
                    entry.immCount = (uint8_t)
                        ((MP4Integer8Property*)findEntryProperty(
                             *pData, "count"))->GetValue();
                    if (entry.immCount == 0 || entry.immCount > 14) {
                        throw new Exception("bad RTP immediate data count",
                                            __FILE__, __LINE__, __FUNCTION__);
                    }
                    MP4BytesProperty* pBytes =
                        (MP4BytesProperty*)findEntryProperty(*pData, "data");
                    pBytes->CopyValue(entry.immBytes);
                } else if (entry.type == 2) {
                    entry.trackRefIndex = (uint8_t)entryValue(
                        *pData, "trackRefIndex");
                    entry.length = (uint16_t)entryValue(*pData, "length");
                    entry.sampleNumber = (uint32_t)entryValue(
                        *pData, "sampleNumber");
                    entry.sampleOffset = (uint32_t)entryValue(
                        *pData, "sampleOffset");
                    entry.bytesPerBlock = (uint16_t)entryValue(
                        *pData, "bytesPerBlock");
                    entry.samplesPerBlock = (uint16_t)entryValue(
                        *pData, "samplesPerBlock");

                    if (entry.bytesPerBlock != 1
                            || entry.samplesPerBlock != 1) {
                        throw new Exception(
                            "non-trivial RTP sample block layout",
                            __FILE__, __LINE__, __FUNCTION__);
                    }
                    if (entry.trackRefIndex != 0
                            && entry.trackRefIndex != 0xFF) {
                        throw new Exception(
                            "RTP sample data references another track",
                            __FILE__, __LINE__, __FUNCTION__);
                    }
                } else {
                    throw new Exception("unsupported RTP data entry type",
                                        __FILE__, __LINE__, __FUNCTION__);
                }

                rp.entries.push_back(entry);
            }
        }
    } catch (...) {
        delete pHint;
        file.DisableMemoryBuffer();
        throw;
    }
    delete pHint;
    file.DisableMemoryBuffer();

    return raw;
}

void freeRawHints(std::vector<RawHint>& hints)
{
    for (size_t i = 0; i < hints.size(); i++) {
        if (hints[i].rawBytes) {
            MP4Free(hints[i].rawBytes);
            hints[i].rawBytes = NULL;
        }
    }
}

} // anonymous namespace

///////////////////////////////////////////////////////////////////////////////

bool CopyHintedTrack(
    MP4File*     srcFile,
    MP4TrackId   srcHintTrackId,
    MP4File*     dstFile,
    MP4TrackId*  pDstMediaTrackId,
    MP4TrackId*  pDstHintTrackId )
{
    *pDstMediaTrackId = MP4_INVALID_TRACK_ID;
    *pDstHintTrackId = MP4_INVALID_TRACK_ID;

    if (srcFile == NULL || dstFile == NULL || srcFile == dstFile) {
        return false;
    }
    if (!dstFile->IsWriteMode()) {
        return false;
    }

    MP4TrackId createdMedia = MP4_INVALID_TRACK_ID;
    MP4TrackId createdHint = MP4_INVALID_TRACK_ID;
    bool committed = false;

    std::vector<RawHint> rawHints;

    try {
        // -- identify the hinted pair ------------------------------------

        MP4Track* pAnyHint = srcFile->GetTrack(srcHintTrackId);
        if (strcmp(pAnyHint->GetType(), MP4_HINT_TRACK_TYPE)) {
            return false;
        }
        MP4RtpHintTrack* pSrcHint = (MP4RtpHintTrack*)pAnyHint;
        MP4Track* pSrcMediaTrack = pSrcHint->GetRefTrack();
        if (pSrcMediaTrack == NULL) {
            return false;
        }
        MP4TrackId srcMediaTrackId = pSrcMediaTrack->GetId();

        if (strcmp(pSrcMediaTrack->GetType(), MP4_VIDEO_TRACK_TYPE)) {
            return false;
        }
        const char* dataName =
            srcFile->GetTrackMediaDataName(srcMediaTrackId);
        if (dataName == NULL
                || (strcmp(dataName, "avc1") && strcmp(dataName, "mp4v"))) {
            return false;
        }

        // -- structural constraints --------------------------------------

        if (srcFile->GetTrackIntegerProperty(
                    srcMediaTrackId,
                    "mdia.minf.stbl.stsd.entryCount") != 1) {
            return false;
        }
        if (srcFile->GetTrackIntegerProperty(
                    srcHintTrackId,
                    "mdia.minf.stbl.stsd.entryCount") != 1) {
            return false;
        }
        if (srcFile->FindTrackAtom(srcHintTrackId, "trak.edts") != NULL) {
            return false;   // hint track carries no edit list of its own
        }
        if (srcFile->FindTrackAtom(srcMediaTrackId,
                                   "mdia.minf.stbl.ctts") != NULL) {
            return false;   // composition offsets are not supported
        }
        // the hint track must reference exactly one (hint) media track
        if (srcFile->GetTrackIntegerProperty(
                    srcHintTrackId,
                    "tref.hint.entryCount") != 1) {
            return false;
        }

        uint32_t trackTimeScale =
            srcFile->GetTrackTimeScale(srcMediaTrackId);
        if (trackTimeScale == 0
                || srcFile->GetTrackTimeScale(srcHintTrackId)
                   != trackTimeScale) {
            return false;
        }

        uint32_t srcMovieScale = srcFile->GetTimeScale();
        uint32_t dstMovieScale = dstFile->GetTimeScale();
        if (srcMovieScale == 0 || dstMovieScale == 0) {
            return false;
        }

        uint32_t numMediaSamples =
            srcFile->GetTrackNumberOfSamples(srcMediaTrackId);
        uint32_t numHintSamples =
            srcFile->GetTrackNumberOfSamples(srcHintTrackId);
        if (numMediaSamples == 0 || numMediaSamples != numHintSamples) {
            return false;
        }

        // per-sample timing of media and hint track must be identical
        std::vector<uint64_t> sampleDts(numMediaSamples + 1);
        std::vector<uint64_t> sampleDur(numMediaSamples + 1);
        std::map<uint64_t, uint32_t> dtsToSample;
        uint64_t mediaTotalDuration = 0;

        for (uint32_t sid = 1; sid <= numMediaSamples; sid++) {
            uint64_t mDts = srcFile->GetSampleTime(srcMediaTrackId, sid);
            uint64_t mDur = srcFile->GetSampleDuration(srcMediaTrackId, sid);
            uint64_t hDts = srcFile->GetSampleTime(srcHintTrackId, sid);
            uint64_t hDur = srcFile->GetSampleDuration(srcHintTrackId, sid);
            if (mDts != hDts || mDur != hDur || mDur == 0) {
                return false;
            }
            if (dtsToSample.count(mDts)) {
                return false;
            }
            dtsToSample[mDts] = sid;
            sampleDts[sid] = mDts;
            sampleDur[sid] = mDur;
            if (!addChecked(mediaTotalDuration, mDur, &mediaTotalDuration)) {
                return false;
            }
        }

        // -- build the edit plan -----------------------------------------

        std::vector<EditSegment> segments;

        MP4Atom* pElst = srcFile->FindTrackAtom(srcMediaTrackId,
                                                "edts.elst");
        uint32_t numEdits = pElst ? srcFile->GetTrackNumberOfEdits(
            srcMediaTrackId) : 0;

        // mediaTime sentinel of an empty edit depends on elst version
        uint64_t emptySentinel = pElst
            ? editEmptySentinel(*srcFile, srcMediaTrackId)
            : UINT64_MAX;

        if (numEdits == 0) {
            // no edit list: play the whole track once
            EditSegment seg;
            std::memset(&seg, 0, sizeof(seg));
            seg.isGap = false;
            seg.trackDuration = mediaTotalDuration;
            seg.firstSample = 1;
            seg.lastSample = numMediaSamples;
            seg.mediaStart = 0;
            if (!scaleExact(mediaTotalDuration, trackTimeScale,
                            srcMovieScale, &seg.srcMovieDuration)
                    || !scaleExact(mediaTotalDuration, trackTimeScale,
                                   dstMovieScale, &seg.dstMovieDuration)) {
                return false;
            }
            segments.push_back(seg);
        } else {
            for (uint32_t e = 1; e <= numEdits; e++) {
                EditSegment seg;
                std::memset(&seg, 0, sizeof(seg));

                char name[128];
                seg.srcMovieDuration =
                    srcFile->GetTrackEditDuration(srcMediaTrackId, e);

                snprintf(name, sizeof(name),
                         "edts.elst.entries[%u].mediaRate", e - 1);
                uint64_t mediaRate =
                    srcFile->GetTrackIntegerProperty(srcMediaTrackId, name);

                snprintf(name, sizeof(name),
                         "edts.elst.entries[%u].mediaTime", e - 1);
                uint64_t mediaTime =
                    srcFile->GetTrackIntegerProperty(srcMediaTrackId, name);

                // segment duration must convert exactly into track units
                // and into the destination movie timescale
                if (!scaleExact(seg.srcMovieDuration, srcMovieScale,
                                trackTimeScale, &seg.trackDuration)
                        || !scaleExact(seg.srcMovieDuration, srcMovieScale,
                                       dstMovieScale,
                                       &seg.dstMovieDuration)
                        || seg.trackDuration == 0) {
                    return false;
                }

                if (mediaTime == emptySentinel) {
                    // empty edit: no media is played
                    seg.isGap = true;
                } else {
                    if (mediaRate != 1) {
                        return false;
                    }
                    seg.isGap = false;

                    std::map<uint64_t, uint32_t>::iterator it =
                        dtsToSample.find(mediaTime);
                    if (it == dtsToSample.end()) {
                        return false;   // start not on a sample boundary
                    }
                    seg.firstSample = it->second;
                    seg.mediaStart = mediaTime;

                    // consume whole samples until the segment is covered
                    uint64_t covered = 0;
                    uint32_t sid = seg.firstSample;
                    while (covered < seg.trackDuration) {
                        if (sid > numMediaSamples
                                || sampleDur[sid]
                                   > seg.trackDuration - covered) {
                            return false;   // end cuts through a sample
                        }
                        // the samples inside a segment must be contiguous
                        // in media time
                        if (sampleDts[sid] != mediaTime + covered) {
                            return false;
                        }
                        covered += sampleDur[sid];
                        sid++;
                    }
                    seg.lastSample = sid - 1;

                    // segment start must be a sync sample
                    if (!srcFile->GetSampleSync(srcMediaTrackId,
                                                seg.firstSample)) {
                        return false;
                    }
                    // segment end must land on a sample DTS boundary
                    uint64_t endMedia = mediaTime + covered;
                    if (endMedia > mediaTotalDuration) {
                        return false;
                    }
                    if (endMedia < mediaTotalDuration
                            && dtsToSample.find(endMedia)
                               == dtsToSample.end()) {
                        return false;
                    }
                }

                segments.push_back(seg);
            }
        }

        // at least one segment has to carry media
        bool anyMedia = false;
        for (size_t i = 0; i < segments.size(); i++) {
            if (!segments[i].isGap) {
                anyMedia = true;
                break;
            }
        }
        if (!anyMedia) {
            return false;   // all-blank input is rejected
        }

        // -- parse every source hint sample (structural validation) ------

        rawHints.resize(numHintSamples + 1);
        for (uint32_t sid = 1; sid <= numHintSamples; sid++) {
            rawHints[sid] = parseHintSample(*pSrcHint, sid);
        }

        // -- output layout and reference validation ----------------------

        MP4SampleId nextMediaSample = 1;
        uint64_t outMediaDuration = 0;
        uint64_t outHintDuration = 0;
        uint64_t totalDstMovieDuration = 0;

        for (size_t si = 0; si < segments.size(); si++) {
            EditSegment& seg = segments[si];
            if (!addChecked(totalDstMovieDuration, seg.dstMovieDuration,
                            &totalDstMovieDuration)) {
                return false;
            }

            if (seg.isGap) {
                if (!addChecked(outHintDuration, seg.trackDuration,
                                &outHintDuration)) {
                    return false;
                }
                continue;
            }

            seg.outFirstSample = nextMediaSample;
            seg.outMediaStart = outMediaDuration;

            uint32_t count = seg.lastSample - seg.firstSample + 1;
            for (uint32_t k = 0; k < count; k++) {
                uint32_t srcSid = seg.firstSample + k;
                RawHint& rh = rawHints[srcSid];

                for (size_t pi = 0; pi < rh.packets.size(); pi++) {
                    RawPacket& rp = rh.packets[pi];
                    for (size_t ei = 0; ei < rp.entries.size(); ei++) {
                        RawEntry& re = rp.entries[ei];
                        if (re.type != 2) {
                            continue;
                        }
                        if (re.trackRefIndex == 0) {
                            // media reference: stay inside this occurrence
                            if (re.sampleNumber < seg.firstSample
                                    || re.sampleNumber > seg.lastSample) {
                                return false;
                            }
                            uint32_t refSize = srcFile->GetSampleSize(
                                srcMediaTrackId, re.sampleNumber);
                            if (re.length == 0
                                    || (uint64_t)re.sampleOffset + re.length
                                       > refSize) {
                                return false;
                            }
                        } else {
                            // self reference: only this hint sample, and
                            // the embedded bytes must stay inside it
                            if (re.sampleNumber != srcSid
                                    || (uint64_t)re.sampleOffset + re.length
                                       > rh.rawSize) {
                                return false;
                            }
                        }
                    }
                }

                if (!addChecked(outMediaDuration, sampleDur[srcSid],
                                &outMediaDuration)
                        || !addChecked(outHintDuration, sampleDur[srcSid],
                                       &outHintDuration)) {
                    return false;
                }
                nextMediaSample++;
            }
        }

        // the hint timeline spans the whole edited timeline, gaps included
        uint64_t expectedHintDuration = 0;
        if (!scaleExact(totalDstMovieDuration, dstMovieScale,
                        trackTimeScale, &expectedHintDuration)
                || expectedHintDuration != outHintDuration
                || outHintDuration == 0) {
            return false;
        }

        // every time atom (mvhd/tkhd/mdhd) created in the destination uses
        // the same 32/64 bit width, governed by the file's time-atom flag
        uint8_t timeWidth = moviePropertyWidth(*dstFile,
                                               "moov.mvhd.duration");
        if (!fitsWidth(totalDstMovieDuration, timeWidth)
                || !fitsWidth(outMediaDuration, timeWidth)
                || !fitsWidth(outHintDuration, timeWidth)) {
            return false;
        }

        // stts sample deltas are always 32 bit, so every individual hint
        // sample duration (including each gap) must fit there
        for (size_t si = 0; si < segments.size(); si++) {
            if (!fitsWidth(segments[si].trackDuration, 32)) {
                return false;
            }
        }
        for (uint32_t sid = 1; sid <= numMediaSamples; sid++) {
            if (!fitsWidth(sampleDur[sid], 32)) {
                return false;
            }
        }

        // -- create the destination pair ---------------------------------

        committed = true;

        MP4TrackId dstMedia = MP4CloneTrack(
            (MP4FileHandle)srcFile, srcMediaTrackId,
            (MP4FileHandle)dstFile);
        if (dstMedia == MP4_INVALID_TRACK_ID) {
            throw new Exception("could not create destination media track",
                                __FILE__, __LINE__, __FUNCTION__);
        }
        createdMedia = dstMedia;

        MP4TrackId dstHint = MP4CloneTrack(
            (MP4FileHandle)srcFile, srcHintTrackId,
            (MP4FileHandle)dstFile, dstMedia);
        if (dstHint == MP4_INVALID_TRACK_ID) {
            throw new Exception("could not create destination hint track",
                                __FILE__, __LINE__, __FUNCTION__);
        }
        createdHint = dstHint;

        // -- copy media samples, one contiguous run per occurrence -------

        for (size_t si = 0; si < segments.size(); si++) {
            EditSegment& seg = segments[si];
            if (seg.isGap) {
                continue;
            }
            uint32_t count = seg.lastSample - seg.firstSample + 1;
            for (uint32_t k = 0; k < count; k++) {
                uint32_t srcSid = seg.firstSample + k;
                if (!MP4CopySample((MP4FileHandle)srcFile, srcMediaTrackId,
                                   srcSid, (MP4FileHandle)dstFile, dstMedia,
                                   sampleDur[srcSid])) {
                    throw new Exception("media sample copy failed",
                                        __FILE__, __LINE__, __FUNCTION__);
                }
            }
        }

        // -- build the destination hint samples --------------------------

        MP4RtpHintTrack* pDstHint =
            (MP4RtpHintTrack*)dstFile->GetTrack(dstHint);

        uint32_t sequence = 0;
        MP4SampleId dstHintSampleId = 0;

        for (size_t si = 0; si < segments.size(); si++) {
            EditSegment& seg = segments[si];

            if (seg.isGap) {
                // zero-packet hint sample covering the blank playback time
                dstHintSampleId++;
                MP4RtpHint* pHint = new MP4RtpHint(*pDstHint);
                uint8_t* pBytes = NULL;
                uint64_t numBytes = 0;
                try {
                    dstFile->EnableMemoryBuffer();
                    pHint->Write(*dstFile);
                    dstFile->DisableMemoryBuffer(&pBytes, &numBytes);
                    dstFile->WriteSample(dstHint, pBytes,
                                         (uint32_t)numBytes,
                                         seg.trackDuration, 0, true);
                    MP4Free(pBytes);
                    delete pHint;
                } catch (...) {
                    MP4Free(pBytes);
                    delete pHint;
                    throw;
                }
                continue;
            }

            uint32_t count = seg.lastSample - seg.firstSample + 1;
            for (uint32_t k = 0; k < count; k++) {
                uint32_t srcSid = seg.firstSample + k;
                RawHint& rh = rawHints[srcSid];
                dstHintSampleId++;

                MP4RtpHint* pHint = new MP4RtpHint(*pDstHint);
                uint8_t* pBytes = NULL;
                uint64_t numBytes = 0;
                try {
                    for (size_t pi = 0; pi < rh.packets.size(); pi++) {
                        RawPacket& rp = rh.packets[pi];
                        MP4RtpPacket* pPacket = pHint->AddPacket();
                        pPacket->Set(rp.payloadType,
                                     sequence & 0xFFFF, rp.mbit);
                        sequence = (sequence + 1) & 0xFFFF;
                        pPacket->SetTransmitOffset(rp.xmitOffset);
                        pPacket->SetBFrame(rp.bframe);
                        if (rp.pbit) {
                            MP4BitfieldProperty* pP =
                                (MP4BitfieldProperty*)findPacketProperty(
                                    *pPacket, "Pbit");
                            pP->SetValue(1);
                        }
                        if (rp.xbit) {
                            MP4BitfieldProperty* pX =
                                (MP4BitfieldProperty*)findPacketProperty(
                                    *pPacket, "Xbit");
                            pX->SetValue(1);
                        }

                        for (size_t ei = 0; ei < rp.entries.size(); ei++) {
                            RawEntry& re = rp.entries[ei];

                            if (re.type == 0) {
                                MP4RtpNullData* pData =
                                    new MP4RtpNullData(*pPacket);
                                pPacket->AddData(pData);
                            } else if (re.type == 1) {
                                MP4RtpImmediateData* pData =
                                    new MP4RtpImmediateData(*pPacket);
                                pData->Set(re.immBytes, re.immCount);
                                pPacket->AddData(pData);
                            } else {
                                MP4RtpSampleData* pData =
                                    new MP4RtpSampleData(*pPacket);
                                if (re.trackRefIndex == 0) {
                                    MP4SampleId newId =
                                        seg.outFirstSample
                                        + (re.sampleNumber
                                           - seg.firstSample);
                                    pData->SetReferenceSample(
                                        newId, re.sampleOffset, re.length);
                                } else {
                                    // keep the bytes embedded in this hint
                                    // sample; never flatten to immediate
                                    uint8_t* pEmbedded =
                                        (uint8_t*)MP4Malloc(re.length);
                                    std::memcpy(pEmbedded,
                                                &rh.rawBytes[re.sampleOffset],
                                                re.length);
                                    pData->SetEmbeddedImmediate(
                                        dstHintSampleId,
                                        pEmbedded, re.length);
                                }
                                pPacket->AddData(pData);
                            }
                        }
                    }

                    dstFile->EnableMemoryBuffer();
                    pHint->Write(*dstFile);
                    dstFile->DisableMemoryBuffer(&pBytes, &numBytes);
                    dstFile->WriteSample(dstHint, pBytes,
                                         (uint32_t)numBytes,
                                         sampleDur[srcSid], 0, rh.sync);
                    MP4Free(pBytes);
                    delete pHint;
                } catch (...) {
                    MP4Free(pBytes);
                    delete pHint;
                    throw;
                }
            }
        }

        // -- edit list on the destination media track --------------------
        // With no source edit list the whole track is copied and plays
        // naturally, so the destination needs no edit list either.

        if (numEdits > 0) {
            for (size_t si = 0; si < segments.size(); si++) {
                EditSegment& seg = segments[si];
                MP4EditId editId = dstFile->AddTrackEdit(dstMedia);
                if (editId == MP4_INVALID_EDIT_ID) {
                    throw new Exception("could not add destination edit",
                                        __FILE__, __LINE__, __FUNCTION__);
                }
                // elst segment durations are in movie timescale
                dstFile->SetTrackEditDuration(dstMedia, editId,
                                              seg.dstMovieDuration);
                if (seg.isGap) {
                    dstFile->SetTrackEditMediaStart(
                        dstMedia, editId,
                        editEmptySentinel(*dstFile, dstMedia));
                } else {
                    dstFile->SetTrackEditMediaStart(dstMedia, editId,
                                                    seg.outMediaStart);
                }
                dstFile->SetTrackEditDwell(dstMedia, editId, false);
            }
        }

        // -- finalize durations ------------------------------------------

        dstFile->SetTrackIntegerProperty(dstMedia, "tkhd.duration",
                                         totalDstMovieDuration);
        dstFile->SetTrackIntegerProperty(dstHint, "tkhd.duration",
                                         totalDstMovieDuration);
        dstFile->SetTrackIntegerProperty(dstMedia, "mdia.mdhd.duration",
                                         outMediaDuration);
        dstFile->SetTrackIntegerProperty(dstHint, "mdia.mdhd.duration",
                                         outHintDuration);

        if (dstFile->GetDuration() < totalDstMovieDuration) {
            dstFile->SetIntegerProperty("moov.mvhd.duration",
                                        totalDstMovieDuration);
        }

        freeRawHints(rawHints);

        *pDstMediaTrackId = dstMedia;
        *pDstHintTrackId = dstHint;
        return true;

    } catch (Exception* x) {
        log.errorf(*x);
        delete x;
    } catch (...) {
        log.errorf("%s: failed", __FUNCTION__);
    }

    freeRawHints(rawHints);

    // failure after tracks were created: remove the whole pair so no
    // half pair is ever left behind
    if (committed) {
        try {
            if (createdHint != MP4_INVALID_TRACK_ID) {
                dstFile->DeleteTrack(createdHint);
            }
            if (createdMedia != MP4_INVALID_TRACK_ID) {
                dstFile->DeleteTrack(createdMedia);
            }
        } catch (...) {
            // best effort cleanup
        }
    }

    *pDstMediaTrackId = MP4_INVALID_TRACK_ID;
    *pDstHintTrackId = MP4_INVALID_TRACK_ID;
    return false;
}

///////////////////////////////////////////////////////////////////////////////

} // namespace mp4v2::impl
} // namespace mp4v2

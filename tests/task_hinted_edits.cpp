/*
 * Task tests for MP4CopyHintedTrack: edit-list driven reordering of an
 * RTP hinted avc1/mp4v video track pair.
 *
 * Registered through task_support/check.h; run via task_support/run.sh.
 */

#include "task_support/check.h"

#include <mp4v2/mp4v2.h>
#include "src/impl.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace mp4v2::impl;

namespace {

const MP4TrackId BAD_TRACK = MP4_INVALID_TRACK_ID;

// ---- small byte/word writers (RTP hint samples are big endian) ---------

void put16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back((uint8_t)(v >> 8));
    b.push_back((uint8_t)(v & 0xFF));
}

void put32(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back((uint8_t)(v >> 24));
    b.push_back((uint8_t)(v >> 16));
    b.push_back((uint8_t)(v >> 8));
    b.push_back((uint8_t)(v & 0xFF));
}

// append one RTP packet (entryCount already known) with no extra block
void appendPacketHeader(std::vector<uint8_t>& b, int32_t xmit,
                        bool mbit, uint8_t payload, uint16_t seq,
                        bool bframe, uint16_t entries)
{
    put32(b, (uint32_t)xmit);
    uint8_t flags0 = 0;                       // reserved1/P/X/reserved2
    b.push_back(flags0);
    b.push_back((uint8_t)((mbit ? 0x80 : 0) | (payload & 0x7F)));
    put16(b, seq);
    put16(b, bframe ? 0x0002 : 0);           // reserved3/extra/bframe/repeat
    put16(b, entries);
}

void appendSampleEntry(std::vector<uint8_t>& b, uint8_t trackRefIndex,
                       uint16_t length, uint32_t sampleNumber,
                       uint32_t sampleOffset, uint16_t block,
                       uint16_t perBlock)
{
    b.push_back(2);
    b.push_back(trackRefIndex);
    put16(b, length);
    put32(b, sampleNumber);
    put32(b, sampleOffset);
    put16(b, block);
    put16(b, perBlock);
}

std::vector<uint8_t> makeHintBytes(uint16_t packets,
                                   const std::vector<uint8_t>& body)
{
    std::vector<uint8_t> b;
    put16(b, packets);
    put16(b, 0);                 // reserved
    b.insert(b.end(), body.begin(), body.end());
    return b;
}

// ---- source file layout -------------------------------------------------

const uint32_t NUM_MEDIA = 6;
const uint32_t TRACK_TS = 1000;
const uint32_t SAMPLE_DUR = 100;
const uint8_t  PAYLOAD_TYPE = 96;

struct SourceSpec {
    uint32_t movieTs;
    uint32_t dstMovieTs;
    bool     avc1;
    // when true, hint for media sample 3 references sample 6 (which lives
    // outside every edit occurrence that contains sample 3)
    bool     badMediaRef;
    // when true, one hint carries a crafted self reference pointing at a
    // different hint sample
    bool     badSelfRef;
    // when true, one hint carries a non-trivial block layout entry
    bool     badBlock;
    // when true, one hint carries a sample-description (type 3) entry
    bool     badSampleDesc;
    // when true, one hint's sample entry names another track (ref index 1)
    bool     badOtherTrack;
    // when true, the edit list is nothing but empty edits
    bool     allBlank;
};

// media sample payloads are unique so references can be identified after
// remapping to a different occurrence
void mediaBytes(uint32_t sid, uint8_t out[2])
{
    out[0] = (uint8_t)(0xA0 + sid);
    out[1] = (uint8_t)(0xB0 + sid);
}

// movie duration for a track-timescale duration, exact conversion required
uint64_t toMovie(uint64_t trackDur, uint32_t movieTs, uint32_t trackTs)
{
    return trackDur * (uint64_t)movieTs / (uint64_t)trackTs;
}

MP4TrackId buildSource(const char* path, const SourceSpec& spec)
{
    std::remove(path);
    MP4FileHandle f = MP4Create(path);
    if (f == MP4_INVALID_FILE_HANDLE) {
        return BAD_TRACK;
    }
    MP4SetTimeScale(f, spec.movieTs);

    MP4TrackId m;
    if (spec.avc1) {
        m = MP4AddH264VideoTrack(f, TRACK_TS, SAMPLE_DUR, 16, 16,
                                 0x42, 0x00, 0x1E, 3);
        if (m == BAD_TRACK) {
            MP4Close(f);
            return BAD_TRACK;
        }
        const uint8_t sps[] = {0x67, 0x42, 0x00, 0x1E};
        const uint8_t pps[] = {0x68, 0xCE, 0x38, 0x80};
        MP4AddH264SequenceParameterSet(f, m, sps, sizeof(sps));
        MP4AddH264PictureParameterSet(f, m, pps, sizeof(pps));
    } else {
        m = MP4AddVideoTrack(f, TRACK_TS, SAMPLE_DUR, 16, 16,
                             MP4_MPEG4_VIDEO_TYPE);
        if (m == BAD_TRACK) {
            MP4Close(f);
            return BAD_TRACK;
        }
        const uint8_t config[] = {0x12, 0x34, 0x56};
        if (!MP4SetTrackESConfiguration(f, m, config, sizeof(config))) {
            MP4Close(f);
            return BAD_TRACK;
        }
    }

    // samples 1,3,4,5,6 are sync; sample 2 is not (and no edit segment is
    // allowed to start on sample 2)
    for (uint32_t i = 1; i <= NUM_MEDIA; i++) {
        uint8_t bytes[2];
        mediaBytes(i, bytes);
        bool sync = (i != 2);
        if (!MP4WriteSample(f, m, bytes, 2, SAMPLE_DUR, 0, sync)) {
            MP4Close(f);
            return BAD_TRACK;
        }
    }

    MP4TrackId h = MP4AddHintTrack(f, m);
    if (h == BAD_TRACK) {
        MP4Close(f);
        return BAD_TRACK;
    }
    uint8_t payload = PAYLOAD_TYPE;
    if (!MP4SetHintTrackRtpPayload(f, h, spec.avc1 ? "H264" : "MP4V-ES",
                                   &payload, 1400)) {
        MP4Close(f);
        return BAD_TRACK;
    }

    for (uint32_t i = 1; i <= NUM_MEDIA; i++) {
        bool sync = (i != 2);

        if (spec.badSelfRef && i == 2) {
            // hand-crafted hint whose self reference names another sample
            std::vector<uint8_t> body;
            appendPacketHeader(body, -7, true, PAYLOAD_TYPE, 0, false, 1);
            appendSampleEntry(body, 0xFF, 1, 99u, 0, 1, 1);
            std::vector<uint8_t> raw = makeHintBytes(1, body);
            if (!MP4WriteSample(f, h, &raw[0], (uint32_t)raw.size(),
                                SAMPLE_DUR, 0, sync)) {
                MP4Close(f);
                return BAD_TRACK;
            }
            continue;
        }
        if (spec.badBlock && i == 2) {
            std::vector<uint8_t> body;
            appendPacketHeader(body, 0, false, PAYLOAD_TYPE, 0, false, 1);
            appendSampleEntry(body, 0, 2, 1u, 0, 2, 2);
            std::vector<uint8_t> raw = makeHintBytes(1, body);
            if (!MP4WriteSample(f, h, &raw[0], (uint32_t)raw.size(),
                                SAMPLE_DUR, 0, sync)) {
                MP4Close(f);
                return BAD_TRACK;
            }
            continue;
        }
        if (spec.badSampleDesc && i == 2) {
            std::vector<uint8_t> body;
            appendPacketHeader(body, 0, false, PAYLOAD_TYPE, 0, false, 1);
            body.push_back(3);               // sample-description entry
            body.push_back(0);               // track ref index
            put16(body, 1);                  // length
            put32(body, 0);                  // sample description index
            put32(body, 0);                  // offset
            put32(body, 0);                  // reserved
            std::vector<uint8_t> raw = makeHintBytes(1, body);
            if (!MP4WriteSample(f, h, &raw[0], (uint32_t)raw.size(),
                                SAMPLE_DUR, 0, sync)) {
                MP4Close(f);
                return BAD_TRACK;
            }
            continue;
        }
        if (spec.badOtherTrack && i == 2) {
            std::vector<uint8_t> body;
            appendPacketHeader(body, 0, false, PAYLOAD_TYPE, 0, false, 1);
            appendSampleEntry(body, 1, 1, 1u, 0, 1, 1);  // ref index 1
            std::vector<uint8_t> raw = makeHintBytes(1, body);
            if (!MP4WriteSample(f, h, &raw[0], (uint32_t)raw.size(),
                                SAMPLE_DUR, 0, sync)) {
                MP4Close(f);
                return BAD_TRACK;
            }
            continue;
        }

        if (!MP4AddRtpHint(f, h)) {
            MP4Close(f);
            return BAD_TRACK;
        }

        // packet 0: ES configuration -> a self (255) embedded reference
        if (!spec.avc1) {
            if (!MP4AddRtpESConfigurationPacket(f, h)) {
                MP4Close(f);
                return BAD_TRACK;
            }
        }

        // packet 1: an immediate marker plus a cross-sample media reference
        if (!MP4AddRtpPacket(f, h, true, -3)) {
            MP4Close(f);
            return BAD_TRACK;
        }
        const uint8_t imm[] = {'I', (uint8_t)i};
        if (!MP4AddRtpImmediateData(f, h, imm, 2)) {
            MP4Close(f);
            return BAD_TRACK;
        }
        uint32_t refSample;
        if (spec.badMediaRef && i == 3) {
            refSample = 6;                   // outside the [3,4] occurrence
        } else if (i & 1u) {
            refSample = i + 1;               // cross-sample, same segment
        } else {
            refSample = i - 1;               // cross-sample, same segment
        }
        if (!MP4AddRtpSampleData(f, h, refSample, 0, 1)) {
            MP4Close(f);
            return BAD_TRACK;
        }

        if (!MP4WriteRtpHint(f, h, SAMPLE_DUR, sync)) {
            MP4Close(f);
            return BAD_TRACK;
        }
    }

    // edit list: leading gap, media [3,4], middle gap, [5,6], [1,2],
    // then [3,4] again (repeated, and the pair order is reversed relative
    // to the natural sample order)
    struct E { uint64_t trackDur; uint64_t mediaStart; bool gap; };
    std::vector<E> edits;
    if (spec.allBlank) {
        edits.push_back((E){150, 0, true});
        edits.push_back((E){100, 0, true});
    } else {
        edits.push_back((E){150, 0, true});
        edits.push_back((E){200, 200, false});   // samples 3,4
        edits.push_back((E){100, 0, true});
        edits.push_back((E){200, 400, false});   // samples 5,6
        edits.push_back((E){200, 0, false});     // samples 1,2
        edits.push_back((E){200, 200, false});   // samples 3,4 again
    }

    for (size_t i = 0; i < edits.size(); i++) {
        uint64_t movieDur = toMovie(edits[i].trackDur, spec.movieTs, TRACK_TS);
        MP4EditId e = MP4AddTrackEdit(
            f, m, MP4_INVALID_EDIT_ID,
            edits[i].gap ? UINT64_MAX : edits[i].mediaStart,
            movieDur, false);
        if (e == MP4_INVALID_EDIT_ID) {
            MP4Close(f);
            return BAD_TRACK;
        }
    }

    MP4Close(f);
    return h;
}

// ---- output timeline expectations ---------------------------------------

struct ExpectedHintSlot {
    uint64_t dts;
    uint64_t dur;
    bool     gap;
    uint32_t srcMediaSid;     // for media slots
};

std::vector<ExpectedHintSlot> expectedSlots()
{
    std::vector<ExpectedHintSlot> v;
    uint64_t t = 0;
    const uint64_t gap1 = 150, gap2 = 100;
    v.push_back((ExpectedHintSlot){t, gap1, true, 0}); t += gap1;
    uint32_t segA[] = {3, 4};
    for (size_t i = 0; i < 2; i++) {
        v.push_back((ExpectedHintSlot){t, SAMPLE_DUR, false, segA[i]});
        t += SAMPLE_DUR;
    }
    v.push_back((ExpectedHintSlot){t, gap2, true, 0}); t += gap2;
    uint32_t segB[] = {5, 6};
    for (size_t i = 0; i < 2; i++) {
        v.push_back((ExpectedHintSlot){t, SAMPLE_DUR, false, segB[i]});
        t += SAMPLE_DUR;
    }
    uint32_t segC[] = {1, 2};
    for (size_t i = 0; i < 2; i++) {
        v.push_back((ExpectedHintSlot){t, SAMPLE_DUR, false, segC[i]});
        t += SAMPLE_DUR;
    }
    uint32_t segD[] = {3, 4};
    for (size_t i = 0; i < 2; i++) {
        v.push_back((ExpectedHintSlot){t, SAMPLE_DUR, false, segD[i]});
        t += SAMPLE_DUR;
    }
    return v;
}

// read the on-disk sampleNumber of the first type-2 sample entry of a hint
uint32_t firstStoredSampleNumber(MP4FileHandle f, MP4TrackId hint,
                                 MP4SampleId sid, uint8_t wantRefIndex)
{
    MP4File& file = *(MP4File*)f;
    MP4RtpHintTrack* ht =
        (MP4RtpHintTrack*)file.GetTrack(hint);

    uint8_t* raw = NULL;
    uint32_t rawSize = 0;
    file.ReadSample(ht->GetId(), sid, &raw, &rawSize);
    file.EnableMemoryBuffer(raw, rawSize);
    uint32_t found = 0;
    MP4RtpHint* pHint = NULL;
    try {
        pHint = new MP4RtpHint(*ht);
        pHint->Read(file);
        for (uint16_t p = 0; p < pHint->GetNumberOfPackets() && !found; p++) {
            MP4RtpPacket* pkt = pHint->GetPacket(p);
            for (uint16_t e = 0; e < pkt->GetDataEntryCount(); e++) {
                MP4RtpData* d = pkt->GetDataEntry(e);
                MP4Property* tp = NULL;
                d->FindProperty("type", (MP4Property**)&tp);
                if (((MP4Integer8Property*)tp)->GetValue() != 2) {
                    continue;
                }
                MP4Property* rp = NULL;
                d->FindProperty("trackRefIndex", (MP4Property**)&rp);
                if (((MP4Integer8Property*)rp)->GetValue() != wantRefIndex) {
                    continue;
                }
                MP4Property* sn = NULL;
                d->FindProperty("sampleNumber", (MP4Property**)&sn);
                found = (uint32_t)((MP4Integer32Property*)sn)->GetValue();
                break;
            }
        }
        delete pHint;
    } catch (...) {
        delete pHint;
        file.DisableMemoryBuffer();
        MP4Free(raw);
        throw;
    }
    file.DisableMemoryBuffer();
    MP4Free(raw);
    return found;
}

// read the int32 transmit offset and marker bit of the last packet
// (the public MP4GetRtpPacketTransmitOffset truncates to uint16_t)
void lastPacketMeta(MP4FileHandle f, MP4TrackId hint, MP4SampleId sid,
                    int32_t* xmit, bool* mbit)
{
    MP4File& file = *(MP4File*)f;
    MP4RtpHintTrack* ht = (MP4RtpHintTrack*)file.GetTrack(hint);
    uint8_t* raw = NULL;
    uint32_t rawSize = 0;
    file.ReadSample(hint, sid, &raw, &rawSize);
    file.EnableMemoryBuffer(raw, rawSize);
    MP4RtpHint* pHint = NULL;
    try {
        pHint = new MP4RtpHint(*ht);
        pHint->Read(file);
        MP4RtpPacket* pkt = pHint->GetPacket(
            pHint->GetNumberOfPackets() - 1);
        *xmit = pkt->GetTransmitOffset();
        *mbit = pkt->GetMBit();
        delete pHint;
    } catch (...) {
        delete pHint;
        file.DisableMemoryBuffer();
        MP4Free(raw);
        throw;
    }
    file.DisableMemoryBuffer();
    MP4Free(raw);
}

// ---- full verification of a good output file ----------------------------

bool verifyOutput(const char* path, uint32_t dstMovieTs, bool avc1,
                  uint32_t expectedPreExistingTracks,
                  MP4TrackId media, MP4TrackId hint)
{
    MP4FileHandle f = MP4Read(path);
    if (f == MP4_INVALID_FILE_HANDLE) {
        return false;
    }
    bool ok = true;
    if (media == BAD_TRACK || hint == BAD_TRACK) {
        // resolve the freshly created pair by type (used by the example,
        // whose output contains only the two new tracks)
        media = MP4FindTrackId(f, 0, MP4_VIDEO_TRACK_TYPE);
        hint = MP4FindTrackId(f, 0, MP4_HINT_TRACK_TYPE);
    }
    if (media == BAD_TRACK || hint == BAD_TRACK) {
        MP4Close(f);
        return false;
    }

    if (MP4GetNumberOfTracks(f) != expectedPreExistingTracks + 2) {
        ok = false;
    }
    if (MP4GetHintTrackReferenceTrackId(f, hint) != media) {
        ok = false;
    }

    const std::vector<ExpectedHintSlot> slots = expectedSlots();
    const uint32_t numMediaCopies = 8;
    const uint32_t numHintSamples = (uint32_t)slots.size();

    if (MP4GetTrackNumberOfSamples(f, media) != numMediaCopies) {
        ok = false;
    }
    if (MP4GetTrackNumberOfSamples(f, hint) != numHintSamples) {
        ok = false;
    }

    // dual timeline: tkhd equal; media mdhd excludes gaps, hint mdhd
    // includes them
    uint64_t mediaTkhd = 0, hintTkhd = 0, mediaMdhd = 0, hintMdhd = 0;
    MP4GetTrackIntegerProperty(f, media, "tkhd.duration", &mediaTkhd);
    MP4GetTrackIntegerProperty(f, hint, "tkhd.duration", &hintTkhd);
    MP4GetTrackIntegerProperty(f, media, "mdia.mdhd.duration", &mediaMdhd);
    MP4GetTrackIntegerProperty(f, hint, "mdia.mdhd.duration", &hintMdhd);
    if (mediaTkhd != hintTkhd) {
        ok = false;
    }
    if (mediaMdhd != numMediaCopies * SAMPLE_DUR) {       // 800
        ok = false;
    }
    if (hintMdhd != 1050) {                               // gaps included
        ok = false;
    }
    uint64_t wantTkhd = 0;
    if (hintMdhd != 0) {
        wantTkhd = hintMdhd * (uint64_t)dstMovieTs / TRACK_TS;
    }
    if (mediaTkhd != wantTkhd) {
        ok = false;
    }

    // edit list on the media track: 6 entries, two gaps, media starts
    // pointing at the freshly copied contiguous regions
    if (MP4GetTrackNumberOfEdits(f, media) != 6) {
        ok = false;
    }
    if (MP4GetTrackNumberOfEdits(f, hint) != 0) {
        ok = false;
    }
    const uint64_t outMediaStarts[4] = {0, 200, 400, 600};
    uint32_t mediaEdit = 0;
    uint32_t gapEdits = 0;
    for (uint32_t e = 1; e <= 6; e++) {
        char prop[96];
        std::snprintf(prop, sizeof(prop),
                      "edts.elst.entries[%u].mediaTime", e - 1);
        uint64_t mediaTime = 0;
        if (!MP4GetTrackIntegerProperty(f, media, prop, &mediaTime)) {
            ok = false;
            continue;
        }
        if (mediaTime == 0xFFFFFFFFULL || mediaTime == UINT64_MAX) {
            gapEdits++;
        } else {
            if (mediaEdit >= 4 || mediaTime != outMediaStarts[mediaEdit]) {
                ok = false;
            }
            mediaEdit++;
        }
    }
    if (gapEdits != 2 || mediaEdit != 4) {
        ok = false;
    }

    // hint sample timing and packet payloads
    uint16_t prevSeq = 0;
    bool havePrevSeq = false;
    uint32_t totalPackets = 0;
    uint32_t firstStoredSeq = 0xFFFF;
    // RTP timestamps read out of the actual hint reader must advance with
    // the playback timeline, i.e. include the gap duration
    uint32_t rtpStart = MP4GetRtpTimestampStart(f, hint);
    uint32_t prevRtpTs = 0;
    bool havePrevRtp = false;
    uint64_t prevSampleDts = 0;

    for (uint32_t si = 1; si <= numHintSamples; si++) {
        const ExpectedHintSlot& slot = slots[si - 1];

        if (MP4GetSampleTime(f, hint, si) != slot.dts) {
            ok = false;
        }
        if (MP4GetSampleDuration(f, hint, si) != slot.dur) {
            ok = false;
        }

        // the RTP timestamp exposed by the real reader must equal the
        // playback start (DTS) plus the library's random start offset, so
        // gap time shifts every later packet naturally
        {
            uint16_t probe = 0;
            if (MP4ReadRtpHint(f, hint, si, &probe) && probe > 0) {
                uint8_t* hp = NULL;
                uint32_t hn = 0;
                if (MP4ReadRtpPacket(f, hint, 0, &hp, &hn, 0, true, true)
                        && hn >= 12) {
                    uint32_t rtpTs = (uint32_t)((hp[4] << 24)
                        | (hp[5] << 16) | (hp[6] << 8) | hp[7]);
                    uint32_t want = rtpStart + (uint32_t)slot.dts;
                    if (rtpTs != want) {
                        ok = false;
                    }
                    if (havePrevRtp) {
                        // delta between successive media packets equals the
                        // DTS delta (gap + intervening sample durations)
                        if (rtpTs - prevRtpTs
                                != (uint32_t)(slot.dts - prevSampleDts)) {
                            ok = false;
                        }
                    }
                    prevRtpTs = rtpTs;
                    prevSampleDts = slot.dts;
                    havePrevRtp = true;
                    MP4Free(hp);
                }
            }
        }

        uint16_t numPackets = 0xFFFF;
        if (!MP4ReadRtpHint(f, hint, si, &numPackets)) {
            ok = false;
            continue;
        }

        if (slot.gap) {
            if (numPackets != 0) {
                ok = false;
            }
            continue;
        }

        // mp4v hints carry an ES config packet; avc1 hints start at the
        // media packet
        const uint16_t expectedPackets = avc1 ? 1 : 2;
        if (numPackets != expectedPackets) {
            ok = false;
        }
        totalPackets += numPackets;

        for (uint16_t p = 0; p < numPackets; p++) {
            uint8_t* pkt = NULL;
            uint32_t n = 0;
            if (!MP4ReadRtpPacket(f, hint, p, &pkt, &n, 0, true, true)) {
                ok = false;
                continue;
            }
            if (n < 12) {
                ok = false;
            } else {
                uint16_t seq = (uint16_t)((pkt[2] << 8) | pkt[3]);
                if (!havePrevSeq) {
                    havePrevSeq = true;
                } else {
                    if ((uint16_t)(seq - prevSeq) != 1) {
                        ok = false;   // consecutive 16 bit sequence numbers
                    }
                }
                prevSeq = seq;
                if (firstStoredSeq == 0xFFFF) {
                    firstStoredSeq = seq;
                }
                if ((pkt[1] & 0x7F) != PAYLOAD_TYPE) {
                    ok = false;
                }
            }
            MP4Free(pkt);
        }

        // the media packet is the last one; check its payload: immediate
        // marker byte 'I' followed by the referenced media sample's byte
        uint8_t* pkt = NULL;
        uint32_t n = 0;
        if (MP4ReadRtpPacket(f, hint, numPackets - 1,
                             &pkt, &n, 0, false, true)) {
            if (n < 3 || pkt[0] != 'I'
                    || pkt[1] != (uint8_t)slot.srcMediaSid) {
                ok = false;
            }
            // last payload byte identifies the referenced media content
            uint8_t expect[2];
            uint32_t refSid = (slot.srcMediaSid & 1u)
                ? slot.srcMediaSid + 1 : slot.srcMediaSid - 1;
            mediaBytes(refSid, expect);
            if (pkt[n - 1] != expect[0]) {
                ok = false;
            }
            MP4Free(pkt);
        } else {
            ok = false;
        }

        // marker bit and transmit offset preserved on the media packet
        if (numPackets > 0) {
            int32_t xmit = 0;
            bool mbit = false;
            lastPacketMeta(f, hint, si, &xmit, &mbit);
            if (xmit != -3 || !mbit) {
                ok = false;
            }
        }
    }

    if (totalPackets == 0) {
        ok = false;
    }

    // the on-disk sequence numbering starts at zero (library readers may
    // still add a random RTP start offset at playback time)
    uint32_t storedFirst = 0xFFFF;
    for (uint32_t si = 1; si <= numHintSamples && storedFirst == 0xFFFF; si++) {
        uint16_t n = 0;
        if (MP4ReadRtpHint(f, hint, si, &n) && n > 0) {
            MP4File& file = *(MP4File*)f;
            MP4RtpHintTrack* ht =
                (MP4RtpHintTrack*)file.GetTrack(hint);
            uint8_t* raw = NULL;
            uint32_t rawSize = 0;
            file.ReadSample(hint, si, &raw, &rawSize);
            file.EnableMemoryBuffer(raw, rawSize);
            MP4RtpHint* pH = NULL;
            try {
                pH = new MP4RtpHint(*ht);
                pH->Read(file);
                storedFirst = pH->GetPacket(0)->GetSequenceNumber();
                delete pH;
            } catch (...) {
                delete pH;
                ok = false;
            }
            file.DisableMemoryBuffer();
            MP4Free(raw);
        }
    }
    if (storedFirst != 0) {
        ok = false;
    }

    // per-occurrence remapping: both copies of segment [3,4] must point at
    // their own media copies (output samples 1..2 and 7..8), not always at
    // the first occurrence
    uint32_t firstOccurrence = firstStoredSampleNumber(f, hint, 2, 0);
    uint32_t secondOccurrence = firstStoredSampleNumber(f, hint, 9, 0);
    if (firstOccurrence < 1 || secondOccurrence < 1
            || firstOccurrence == secondOccurrence
            || secondOccurrence != firstOccurrence + 6) {
        ok = false;
    }

    // self (255) references keep their embedded bytes and name the new
    // hint sample; reading them through the public entry still yields the
    // ES configuration bytes
    if (!avc1) {
        bool sawSelf = false;
        for (uint32_t si = 1; si <= numHintSamples; si++) {
            uint16_t n = 0;
            if (!MP4ReadRtpHint(f, hint, si, &n) || n < 2) {
                continue;
            }
            uint32_t selfId = firstStoredSampleNumber(f, hint, si, 0xFF);
            if (selfId != si) {
                ok = false;
            } else {
                sawSelf = true;
            }
            uint8_t* pkt = NULL;
            uint32_t pn = 0;
            if (MP4ReadRtpPacket(f, hint, 0, &pkt, &pn, 0, false, true)) {
                if (pn != 3 || pkt[0] != 0x12 || pkt[1] != 0x34
                        || pkt[2] != 0x56) {
                    ok = false;
                }
                MP4Free(pkt);
            } else {
                ok = false;
            }
        }
        if (!sawSelf) {
            ok = false;
        }
    }

    // sync flags survive: the copy of non-sync source sample 2 lands at
    // output media sample 6 (segment [1,2] starts at output sample 5)
    int8_t sync2 = MP4GetSampleSync(f, media, 6);
    if (sync2 != 0) {
        ok = false;
    }
    if (MP4GetSampleSync(f, media, 1) != 1) {
        ok = false;
    }

    MP4Close(f);
    return ok;
}

// ---- group: occurrence ---------------------------------------------------

int occurrence_mp4v()
{
    const char* src = ".task-build/case-occ-mp4v-src.mp4";
    const char* dst = ".task-build/case-occ-mp4v-dst.mp4";
    SourceSpec spec;
    std::memset(&spec, 0, sizeof(spec));
    spec.movieTs = 1000;
    spec.dstMovieTs = 1000;
    spec.avc1 = false;

    TASK_CHECK(buildSource(src, spec) != BAD_TRACK);

    MP4FileHandle sf = MP4Read(src);
    TASK_CHECK(sf != MP4_INVALID_FILE_HANDLE);
    MP4TrackId sh = MP4FindTrackId(sf, 0, MP4_HINT_TRACK_TYPE);
    TASK_CHECK(sh != BAD_TRACK);

    std::remove(dst);
    MP4FileHandle df = MP4Create(dst);
    TASK_CHECK(df != MP4_INVALID_FILE_HANDLE);
    MP4TrackId mo = BAD_TRACK, ho = BAD_TRACK;
    TASK_CHECK(MP4CopyHintedTrack(sf, sh, df, &mo, &ho));
    TASK_CHECK(mo != BAD_TRACK && ho != BAD_TRACK);
    MP4Close(df);
    MP4Close(sf);

    TASK_CHECK(verifyOutput(dst, 1000, false, 0, mo, ho));
    return 0;
}

int occurrence_avc1()
{
    const char* src = ".task-build/case-occ-avc1-src.mp4";
    const char* dst = ".task-build/case-occ-avc1-dst.mp4";
    SourceSpec spec;
    std::memset(&spec, 0, sizeof(spec));
    spec.movieTs = 1000;
    spec.dstMovieTs = 1000;
    spec.avc1 = true;

    TASK_CHECK(buildSource(src, spec) != BAD_TRACK);

    MP4FileHandle sf = MP4Read(src);
    TASK_CHECK(sf != MP4_INVALID_FILE_HANDLE);
    MP4TrackId sh = MP4FindTrackId(sf, 0, MP4_HINT_TRACK_TYPE);
    TASK_CHECK(sh != BAD_TRACK);

    std::remove(dst);
    MP4FileHandle df = MP4Create(dst);
    TASK_CHECK(df != MP4_INVALID_FILE_HANDLE);
    MP4TrackId mo = BAD_TRACK, ho = BAD_TRACK;
    TASK_CHECK(MP4CopyHintedTrack(sf, sh, df, &mo, &ho));
    TASK_CHECK(mo != BAD_TRACK && ho != BAD_TRACK);
    MP4Close(df);
    MP4Close(sf);

    TASK_CHECK(verifyOutput(dst, 1000, true, 0, mo, ho));
    return 0;
}

// destination already holds an unrelated track
int occurrence_preexisting()
{
    const char* src = ".task-build/case-pre-src.mp4";
    const char* dst = ".task-build/case-pre-dst.mp4";
    SourceSpec spec;
    std::memset(&spec, 0, sizeof(spec));
    spec.movieTs = 1000;
    spec.dstMovieTs = 1000;
    spec.avc1 = false;

    TASK_CHECK(buildSource(src, spec) != BAD_TRACK);

    std::remove(dst);
    MP4FileHandle df = MP4Create(dst);
    TASK_CHECK(df != MP4_INVALID_FILE_HANDLE);
    MP4TrackId extra = MP4AddVideoTrack(df, 1000, 100, 8, 8,
                                        MP4_MPEG4_VIDEO_TYPE);
    TASK_CHECK(extra != BAD_TRACK);
    uint8_t b[] = {1, 2};
    TASK_CHECK(MP4WriteSample(df, extra, b, 2, 100, 0, true));

    MP4FileHandle sf = MP4Read(src);
    TASK_CHECK(sf != MP4_INVALID_FILE_HANDLE);
    MP4TrackId sh = MP4FindTrackId(sf, 0, MP4_HINT_TRACK_TYPE);
    TASK_CHECK(sh != BAD_TRACK);
    MP4TrackId mo = BAD_TRACK, ho = BAD_TRACK;
    TASK_CHECK(MP4CopyHintedTrack(sf, sh, df, &mo, &ho));
    TASK_CHECK(mo != BAD_TRACK && ho != BAD_TRACK);
    MP4Close(df);
    MP4Close(sf);

    TASK_CHECK(verifyOutput(dst, 1000, false, 1, mo, ho));
    return 0;
}

// no edit list: the whole track is copied and the destination has no elst
int occurrence_no_edits()
{
    const char* src = ".task-build/case-noedit-src.mp4";
    const char* dst = ".task-build/case-noedit-dst.mp4";
    std::remove(src);
    MP4FileHandle f = MP4Create(src);
    TASK_CHECK(f != MP4_INVALID_FILE_HANDLE);
    MP4SetTimeScale(f, 1000);
    MP4TrackId m = MP4AddVideoTrack(f, TRACK_TS, SAMPLE_DUR, 16, 16,
                                    MP4_MPEG4_VIDEO_TYPE);
    TASK_CHECK(m != BAD_TRACK);
    for (uint32_t i = 1; i <= 4; i++) {
        uint8_t bytes[2];
        mediaBytes(i, bytes);
        TASK_CHECK(MP4WriteSample(f, m, bytes, 2, SAMPLE_DUR, 0, true));
    }
    MP4TrackId h = MP4AddHintTrack(f, m);
    TASK_CHECK(h != BAD_TRACK);
    uint8_t payload = PAYLOAD_TYPE;
    TASK_CHECK(MP4SetHintTrackRtpPayload(f, h, "MP4V-ES", &payload, 1400));
    for (uint32_t i = 1; i <= 4; i++) {
        TASK_CHECK(MP4AddRtpHint(f, h));
        TASK_CHECK(MP4AddRtpPacket(f, h, true, 0));
        const uint8_t imm[] = {'Z', (uint8_t)i};
        TASK_CHECK(MP4AddRtpImmediateData(f, h, imm, 2));
        TASK_CHECK(MP4WriteRtpHint(f, h, SAMPLE_DUR, true));
    }
    MP4Close(f);

    MP4FileHandle sf = MP4Read(src);
    TASK_CHECK(sf != MP4_INVALID_FILE_HANDLE);
    MP4TrackId sh = MP4FindTrackId(sf, 0, MP4_HINT_TRACK_TYPE);
    std::remove(dst);
    MP4FileHandle df = MP4Create(dst);
    TASK_CHECK(df != MP4_INVALID_FILE_HANDLE);
    MP4TrackId mo = BAD_TRACK, ho = BAD_TRACK;
    TASK_CHECK(MP4CopyHintedTrack(sf, sh, df, &mo, &ho));
    MP4Close(df);
    MP4Close(sf);

    MP4FileHandle v = MP4Read(dst);
    TASK_CHECK(v != MP4_INVALID_FILE_HANDLE);
    TASK_CHECK(MP4GetTrackNumberOfSamples(v, mo) == 4);
    TASK_CHECK(MP4GetTrackNumberOfSamples(v, ho) == 4);
    TASK_CHECK(MP4GetTrackNumberOfEdits(v, mo) == 0);
    TASK_CHECK(MP4GetTrackNumberOfEdits(v, ho) == 0);
    uint64_t mm = 0, hm = 0;
    TASK_CHECK(MP4GetTrackIntegerProperty(v, mo, "mdia.mdhd.duration", &mm));
    TASK_CHECK(MP4GetTrackIntegerProperty(v, ho, "mdia.mdhd.duration", &hm));
    TASK_CHECK(mm == 400 && hm == 400);
    MP4Close(v);
    return 0;
}

// ---- group: clock --------------------------------------------------------

int clock_exact_scales()
{
    const char* src = ".task-build/case-clock-src.mp4";
    const char* dst = ".task-build/case-clock-dst.mp4";
    SourceSpec spec;
    std::memset(&spec, 0, sizeof(spec));
    spec.movieTs = 600;       // exact 3:5 ratio with the 1000 track clock
    spec.dstMovieTs = 480;    // exact against both
    spec.avc1 = false;

    TASK_CHECK(buildSource(src, spec) != BAD_TRACK);

    MP4FileHandle sf = MP4Read(src);
    TASK_CHECK(sf != MP4_INVALID_FILE_HANDLE);
    MP4TrackId sh = MP4FindTrackId(sf, 0, MP4_HINT_TRACK_TYPE);
    std::remove(dst);
    MP4FileHandle df = MP4Create(dst);
    TASK_CHECK(df != MP4_INVALID_FILE_HANDLE);
    MP4SetTimeScale(df, 480);
    MP4TrackId mo = BAD_TRACK, ho = BAD_TRACK;
    TASK_CHECK(MP4CopyHintedTrack(sf, sh, df, &mo, &ho));
    MP4Close(df);
    MP4Close(sf);

    TASK_CHECK(verifyOutput(dst, 480, false, 0, mo, ho));

    // the destination movie timescale really is the non-millisecond one
    MP4FileHandle v = MP4Read(dst);
    TASK_CHECK(v != MP4_INVALID_FILE_HANDLE);
    TASK_CHECK(MP4GetTimeScale(v) == 480);
    // 1050 track units -> 504 movie units exactly
    uint64_t tkhd = 0;
    TASK_CHECK(MP4GetTrackIntegerProperty(v, ho, "tkhd.duration", &tkhd));
    TASK_CHECK(tkhd == 504);
    MP4Close(v);
    return 0;
}

int clock_nondividing()
{
    const char* src = ".task-build/case-clockbad-src.mp4";
    const char* dst = ".task-build/case-clockbad-dst.mp4";
    SourceSpec spec;
    std::memset(&spec, 0, sizeof(spec));
    spec.movieTs = 1000;
    spec.dstMovieTs = 481;    // 150 * 481 / 1000 is not integral
    spec.avc1 = false;

    TASK_CHECK(buildSource(src, spec) != BAD_TRACK);

    std::remove(dst);
    MP4FileHandle df = MP4Create(dst);
    TASK_CHECK(df != MP4_INVALID_FILE_HANDLE);
    MP4SetTimeScale(df, 481);
    MP4TrackId extra = MP4AddVideoTrack(df, 1000, 100, 8, 8,
                                        MP4_MPEG4_VIDEO_TYPE);
    TASK_CHECK(extra != BAD_TRACK);

    MP4FileHandle sf = MP4Read(src);
    TASK_CHECK(sf != MP4_INVALID_FILE_HANDLE);
    MP4TrackId sh = MP4FindTrackId(sf, 0, MP4_HINT_TRACK_TYPE);
    MP4TrackId mo = 999, ho = 999;
    TASK_CHECK(!MP4CopyHintedTrack(sf, sh, df, &mo, &ho));
    TASK_CHECK(mo == BAD_TRACK && ho == BAD_TRACK);
    // preflight failure leaves the pre-existing track alone
    TASK_CHECK(MP4GetNumberOfTracks(df) == 1);
    MP4Close(df);
    MP4Close(sf);
    return 0;
}

// ---- group: reject_compat ------------------------------------------------

int reject_bad_media_ref()
{
    const char* src = ".task-build/case-badmedia-src.mp4";
    const char* dst = ".task-build/case-badmedia-dst.mp4";
    SourceSpec spec;
    std::memset(&spec, 0, sizeof(spec));
    spec.movieTs = 1000;
    spec.dstMovieTs = 1000;
    spec.avc1 = false;
    spec.badMediaRef = true;

    TASK_CHECK(buildSource(src, spec) != BAD_TRACK);

    std::remove(dst);
    MP4FileHandle df = MP4Create(dst);
    TASK_CHECK(df != MP4_INVALID_FILE_HANDLE);
    MP4TrackId extra = MP4AddVideoTrack(df, 1000, 100, 8, 8,
                                        MP4_MPEG4_VIDEO_TYPE);
    TASK_CHECK(extra != BAD_TRACK);

    MP4FileHandle sf = MP4Read(src);
    TASK_CHECK(sf != MP4_INVALID_FILE_HANDLE);
    MP4TrackId sh = MP4FindTrackId(sf, 0, MP4_HINT_TRACK_TYPE);
    MP4TrackId mo = 999, ho = 999;
    TASK_CHECK(!MP4CopyHintedTrack(sf, sh, df, &mo, &ho));
    TASK_CHECK(mo == BAD_TRACK && ho == BAD_TRACK);
    TASK_CHECK(MP4GetNumberOfTracks(df) == 1);
    MP4Close(df);
    MP4Close(sf);
    return 0;
}

int reject_bad_self_ref()
{
    const char* src = ".task-build/case-badself-src.mp4";
    const char* dst = ".task-build/case-badself-dst.mp4";
    SourceSpec spec;
    std::memset(&spec, 0, sizeof(spec));
    spec.movieTs = 1000;
    spec.dstMovieTs = 1000;
    spec.avc1 = false;
    spec.badSelfRef = true;

    TASK_CHECK(buildSource(src, spec) != BAD_TRACK);
    std::remove(dst);
    MP4FileHandle df = MP4Create(dst);
    TASK_CHECK(df != MP4_INVALID_FILE_HANDLE);
    MP4FileHandle sf = MP4Read(src);
    TASK_CHECK(sf != MP4_INVALID_FILE_HANDLE);
    MP4TrackId sh = MP4FindTrackId(sf, 0, MP4_HINT_TRACK_TYPE);
    MP4TrackId mo = 999, ho = 999;
    TASK_CHECK(!MP4CopyHintedTrack(sf, sh, df, &mo, &ho));
    TASK_CHECK(mo == BAD_TRACK && ho == BAD_TRACK);
    TASK_CHECK(MP4GetNumberOfTracks(df) == 0);
    MP4Close(df);
    MP4Close(sf);
    return 0;
}

int reject_bad_block()
{
    const char* src = ".task-build/case-badblock-src.mp4";
    const char* dst = ".task-build/case-badblock-dst.mp4";
    SourceSpec spec;
    std::memset(&spec, 0, sizeof(spec));
    spec.movieTs = 1000;
    spec.dstMovieTs = 1000;
    spec.badBlock = true;

    TASK_CHECK(buildSource(src, spec) != BAD_TRACK);
    std::remove(dst);
    MP4FileHandle df = MP4Create(dst);
    TASK_CHECK(df != MP4_INVALID_FILE_HANDLE);
    MP4FileHandle sf = MP4Read(src);
    TASK_CHECK(sf != MP4_INVALID_FILE_HANDLE);
    MP4TrackId sh = MP4FindTrackId(sf, 0, MP4_HINT_TRACK_TYPE);
    MP4TrackId mo = 999, ho = 999;
    TASK_CHECK(!MP4CopyHintedTrack(sf, sh, df, &mo, &ho));
    TASK_CHECK(mo == BAD_TRACK && ho == BAD_TRACK);
    MP4Close(df);
    MP4Close(sf);
    return 0;
}

int reject_sample_desc()
{
    const char* src = ".task-build/case-badsd-src.mp4";
    const char* dst = ".task-build/case-badsd-dst.mp4";
    SourceSpec spec;
    std::memset(&spec, 0, sizeof(spec));
    spec.movieTs = 1000;
    spec.dstMovieTs = 1000;
    spec.badSampleDesc = true;

    TASK_CHECK(buildSource(src, spec) != BAD_TRACK);
    std::remove(dst);
    MP4FileHandle df = MP4Create(dst);
    TASK_CHECK(df != MP4_INVALID_FILE_HANDLE);
    MP4FileHandle sf = MP4Read(src);
    TASK_CHECK(sf != MP4_INVALID_FILE_HANDLE);
    MP4TrackId sh = MP4FindTrackId(sf, 0, MP4_HINT_TRACK_TYPE);
    MP4TrackId mo = 999, ho = 999;
    TASK_CHECK(!MP4CopyHintedTrack(sf, sh, df, &mo, &ho));
    TASK_CHECK(mo == BAD_TRACK && ho == BAD_TRACK);
    MP4Close(df);
    MP4Close(sf);
    return 0;
}

int reject_other_track_ref()
{
    const char* src = ".task-build/case-badother-src.mp4";
    const char* dst = ".task-build/case-badother-dst.mp4";
    SourceSpec spec;
    std::memset(&spec, 0, sizeof(spec));
    spec.movieTs = 1000;
    spec.dstMovieTs = 1000;
    spec.badOtherTrack = true;

    TASK_CHECK(buildSource(src, spec) != BAD_TRACK);
    std::remove(dst);
    MP4FileHandle df = MP4Create(dst);
    TASK_CHECK(df != MP4_INVALID_FILE_HANDLE);
    MP4FileHandle sf = MP4Read(src);
    TASK_CHECK(sf != MP4_INVALID_FILE_HANDLE);
    MP4TrackId sh = MP4FindTrackId(sf, 0, MP4_HINT_TRACK_TYPE);
    MP4TrackId mo = 999, ho = 999;
    TASK_CHECK(!MP4CopyHintedTrack(sf, sh, df, &mo, &ho));
    TASK_CHECK(mo == BAD_TRACK && ho == BAD_TRACK);
    TASK_CHECK(MP4GetNumberOfTracks(df) == 0);
    MP4Close(df);
    MP4Close(sf);
    return 0;
}

int reject_all_blank()
{
    const char* src = ".task-build/case-blank-src.mp4";
    const char* dst = ".task-build/case-blank-dst.mp4";
    SourceSpec spec;
    std::memset(&spec, 0, sizeof(spec));
    spec.movieTs = 1000;
    spec.dstMovieTs = 1000;
    spec.allBlank = true;

    TASK_CHECK(buildSource(src, spec) != BAD_TRACK);
    std::remove(dst);
    MP4FileHandle df = MP4Create(dst);
    TASK_CHECK(df != MP4_INVALID_FILE_HANDLE);
    MP4FileHandle sf = MP4Read(src);
    TASK_CHECK(sf != MP4_INVALID_FILE_HANDLE);
    MP4TrackId sh = MP4FindTrackId(sf, 0, MP4_HINT_TRACK_TYPE);
    MP4TrackId mo = 999, ho = 999;
    TASK_CHECK(!MP4CopyHintedTrack(sf, sh, df, &mo, &ho));
    TASK_CHECK(mo == BAD_TRACK && ho == BAD_TRACK);
    MP4Close(df);
    MP4Close(sf);
    return 0;
}

// legacy single-track copy paths keep working
int compat_legacy_copy()
{
    const char* src = ".task-build/case-legacy-src.mp4";
    const char* dst = ".task-build/case-legacy-dst.mp4";
    SourceSpec spec;
    std::memset(&spec, 0, sizeof(spec));
    spec.movieTs = 1000;
    spec.dstMovieTs = 1000;
    spec.avc1 = false;

    TASK_CHECK(buildSource(src, spec) != BAD_TRACK);

    MP4FileHandle sf = MP4Read(src);
    TASK_CHECK(sf != MP4_INVALID_FILE_HANDLE);
    MP4TrackId sm = MP4FindTrackId(sf, 0, MP4_VIDEO_TRACK_TYPE);
    MP4TrackId sh = MP4FindTrackId(sf, 0, MP4_HINT_TRACK_TYPE);

    std::remove(dst);
    MP4FileHandle df = MP4Create(dst);
    TASK_CHECK(df != MP4_INVALID_FILE_HANDLE);

    MP4TrackId dm = MP4CloneTrack(sf, sm, df);
    TASK_CHECK(dm != BAD_TRACK);
    MP4TrackId dh = MP4CloneTrack(sf, sh, df, dm);
    TASK_CHECK(dh != BAD_TRACK);
    TASK_CHECK(MP4GetHintTrackReferenceTrackId(df, dh) == dm);

    for (uint32_t i = 1; i <= NUM_MEDIA; i++) {
        TASK_CHECK(MP4CopySample(sf, sm, i, df, dm, SAMPLE_DUR));
        TASK_CHECK(MP4CopySample(sf, sh, i, df, dh, SAMPLE_DUR));
    }
    MP4Close(df);
    MP4Close(sf);

    MP4FileHandle v = MP4Read(dst);
    TASK_CHECK(v != MP4_INVALID_FILE_HANDLE);
    TASK_CHECK(MP4GetTrackNumberOfSamples(v, dm) == NUM_MEDIA);
    TASK_CHECK(MP4GetTrackNumberOfSamples(v, dh) == NUM_MEDIA);
    MP4Close(v);
    return 0;
}

// the shipped example really drives the new API end to end
int compat_example_roundtrip()
{
    const char* src = ".task-build/case-example-src.mp4";
    const char* dst = ".task-build/case-example-dst.mp4";
    SourceSpec spec;
    std::memset(&spec, 0, sizeof(spec));
    spec.movieTs = 1000;
    spec.dstMovieTs = 1000;
    spec.avc1 = false;

    TASK_CHECK(buildSource(src, spec) != BAD_TRACK);
    std::remove(dst);

    std::string cmd = std::string(".task-build/example ") + src + " " + dst;
    int rc = std::system(cmd.c_str());
    TASK_CHECK(rc == 0);

    TASK_CHECK(verifyOutput(dst, 1000, false, 0, BAD_TRACK, BAD_TRACK));
    return 0;
}

} // namespace

const struct task_case task_cases[] = {
    {"occurrence", "mp4v_reorder",       occurrence_mp4v},
    {"occurrence", "avc1_reorder",       occurrence_avc1},
    {"occurrence", "preexisting_track",  occurrence_preexisting},
    {"occurrence", "no_edit_list",       occurrence_no_edits},
    {"clock",      "exact_scales",       clock_exact_scales},
    {"clock",      "nondividing_reject", clock_nondividing},
    {"reject_compat", "bad_media_ref",   reject_bad_media_ref},
    {"reject_compat", "bad_self_ref",    reject_bad_self_ref},
    {"reject_compat", "bad_block_layout",reject_bad_block},
    {"reject_compat", "sample_desc_ref", reject_sample_desc},
    {"reject_compat", "other_track_ref", reject_other_track_ref},
    {"reject_compat", "all_blank",       reject_all_blank},
    {"reject_compat", "legacy_copy",     compat_legacy_copy},
    {"reject_compat", "example_roundtrip",compat_example_roundtrip},
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);

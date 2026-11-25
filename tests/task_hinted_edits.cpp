/*
 * Tests for MP4CopyHintedTrack(): edit-list driven copying of an RTP
 * hinted video track. The test file builds small, self-contained mp4 files
 * through the public mp4v2 API (including hand-built RTP hint samples where
 * precise control over hint entries is needed), closes them, reopens the
 * generated output, and verifies the result through the real hint reading
 * entry points.
 */

#include <mp4v2/mp4v2.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

// task_support/driver.c is built with the C++ compiler in this harness, so
// these declarations share ordinary C++ linkage with it.
#include "task_support/check.h"

namespace {

// --------------------------------------------------------------------------
// small big-endian byte buffer used to hand-craft RTP hint samples
// --------------------------------------------------------------------------

struct ByteBuf {
    std::vector<uint8_t> b;

    void put8(uint8_t v) { b.push_back(v); }
    void put16(uint16_t v)
    {
        b.push_back((uint8_t)(v >> 8));
        b.push_back((uint8_t)(v & 0xff));
    }
    void put32(uint32_t v)
    {
        b.push_back((uint8_t)(v >> 24));
        b.push_back((uint8_t)(v >> 16));
        b.push_back((uint8_t)(v >> 8));
        b.push_back((uint8_t)(v & 0xff));
    }
    void bytes(const void* p, size_t n)
    {
        const uint8_t* q = (const uint8_t*)p;
        b.insert(b.end(), q, q + n);
    }
    const uint8_t* data() const { return b.empty() ? NULL : &b[0]; }
    uint32_t size() const { return (uint32_t)b.size(); }
};

// hint entry builders, mirroring the on-disk grammar of ISO/IEC 14496-16

void putNullEntry(ByteBuf& out)
{
    out.put8(0);
    for (int i = 0; i < 15; ++i) {
        out.put8(0);
    }
}

void putImmediateEntry(ByteBuf& out, const uint8_t* data, uint8_t count)
{
    out.put8(1);
    out.put8(count);
    for (uint8_t i = 0; i < count; ++i) {
        out.put8(data[i]);
    }
    for (uint8_t i = count; i < 14; ++i) {
        out.put8(0);
    }
}

void putSampleEntry(ByteBuf& out, uint8_t refIndex, uint16_t length,
                    uint32_t sampleNumber, uint32_t sampleOffset,
                    uint16_t bytesPerBlock = 1,
                    uint16_t samplesPerBlock = 1)
{
    out.put8(2);
    out.put8(refIndex);
    out.put16(length);
    out.put32(sampleNumber);
    out.put32(sampleOffset);
    out.put16(bytesPerBlock);
    out.put16(samplesPerBlock);
}

void putSampleDescriptionEntry(ByteBuf& out, uint8_t refIndex,
                               uint16_t length, uint32_t descrIndex,
                               uint32_t sampleOffset)
{
    out.put8(3);
    out.put8(refIndex);
    out.put16(length);
    out.put32(descrIndex);
    out.put32(sampleOffset);
    out.put32(0);
}

// one RTP packet: transmit offset, marker/payload byte, sequence number,
// followed by already-serialized entries
void putPacketHeader(ByteBuf& out, int32_t transmitOffset, bool marker,
                     uint8_t payloadType, uint16_t sequenceNumber,
                     uint16_t entryCount)
{
    out.put32((uint32_t)transmitOffset);
    out.put8(0); // reserved1/P/X/reserved2
    out.put8((uint8_t)((marker ? 0x80 : 0) | (payloadType & 0x7f)));
    out.put16(sequenceNumber);
    out.put16(0); // reserved3/extra/bframe/repeat flags
    out.put16(entryCount);
}

uint16_t rd16(const uint8_t* p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

uint32_t rd32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
           | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

// --------------------------------------------------------------------------
// scratch directory handling
// --------------------------------------------------------------------------

std::string scratchDir()
{
    // Prefer a repo-local scratch area: the sandbox may forbid creating
    // directories under the system temp directory.
    std::string base = ".task-build/scratch";
    if (mkdir(base.c_str(), 0755) != 0 && errno != EEXIST) {
        return std::string();
    }

    std::string dir = base + "/case-XXXXXX";
    std::vector<char> buf(dir.begin(), dir.end());
    buf.push_back('\0');
    char* made = mkdtemp(&buf[0]);
    if (made == NULL) {
        return std::string();
    }
    return std::string(made);
}

void cleanupDir(const std::string& dir)
{
    std::string p = dir + "/src.mp4";
    remove(p.c_str());
    p = dir + "/dst.mp4";
    remove(p.c_str());
    p = dir + "/copy.mp4";
    remove(p.c_str());
    rmdir(dir.c_str());
}

// --------------------------------------------------------------------------
// source file construction
// --------------------------------------------------------------------------

struct SourceSpec {
    uint32_t movieTimeScale;
    uint32_t trackTimeScale;
    uint32_t sampleDuration;
    uint32_t sampleCount;
    bool     avc1;
};

// Builds a video track (mp4v by default, avc1 when requested) with distinct
// sample payloads and an RTP hint track carrying one hint sample per media
// sample. The hints mix immediate data, null data and media sample
// references (including references to other media samples).
bool buildBasicSource(MP4FileHandle f, const SourceSpec& spec,
                      MP4TrackId* mediaOut, MP4TrackId* hintOut)
{
    MP4SetTimeScale(f, spec.movieTimeScale);

    MP4TrackId media;
    if (spec.avc1) {
        media = MP4AddH264VideoTrack(f, spec.trackTimeScale,
                                     spec.sampleDuration, 16, 16,
                                     0x42, 0x00, 0x0a, 3);
        static const uint8_t sps[] = { 0x67, 0x42, 0x00, 0x0a };
        static const uint8_t pps[] = { 0x68, 0xce, 0x38, 0x80 };
        MP4AddH264SequenceParameterSet(f, media, sps, sizeof(sps));
        MP4AddH264PictureParameterSet(f, media, pps, sizeof(pps));
    } else {
        media = MP4AddVideoTrack(f, spec.trackTimeScale,
                                 spec.sampleDuration, 16, 16,
                                 MP4_MPEG4_VIDEO_TYPE);
        const uint8_t config[] = { 0x12, 0x34, 0x56 };
        if (!MP4SetTrackESConfiguration(f, media, config, sizeof(config))) {
            return false;
        }
    }
    if (media == MP4_INVALID_TRACK_ID) {
        return false;
    }

    for (uint32_t i = 0; i < spec.sampleCount; ++i) {
        uint8_t payload[6];
        payload[0] = (uint8_t)('A' + i);
        payload[1] = (uint8_t)i;
        payload[2] = (uint8_t)(0xa0 + i);
        payload[3] = (uint8_t)(0x0b + i);
        if (spec.avc1) {
            // 4-byte length prefix plus two NAL bytes
            payload[4] = 0x65;
            payload[5] = (uint8_t)i;
        }
        uint32_t size = spec.avc1 ? 6u : 4u;
        bool sync = (i % 2 == 0);
        if (!MP4WriteSample(f, media, payload, size,
                            spec.sampleDuration, 0, sync)) {
            return false;
        }
    }

    MP4TrackId hint = MP4AddHintTrack(f, media);
    if (hint == MP4_INVALID_TRACK_ID) {
        return false;
    }
    uint8_t payloadNumber = 96;
    if (!MP4SetHintTrackRtpPayload(
                f, hint, spec.avc1 ? "H264" : "MP4V-ES",
                &payloadNumber, 1400)) {
        return false;
    }

    for (uint32_t i = 0; i < spec.sampleCount; ++i) {
        MP4SampleId sid = i + 1;

        ByteBuf body;
        // packet 0: immediate byte + reference to our own media sample
        putPacketHeader(body, (int32_t)(i * 7), i == 0, 96,
                        (uint16_t)(i * 2), 2);
        uint8_t imm = (uint8_t)('!' + i);
        putImmediateEntry(body, &imm, 1);
        putSampleEntry(body, 0, 1, sid, 0, 1, 1);

        // packet 1: null + reference to the previous media sample
        MP4SampleId other = (sid == 1) ? 1 : sid - 1;
        putPacketHeader(body, (int32_t)(i * 7 + 3), false, 96,
                        (uint16_t)(i * 2 + 1), 2);
        putNullEntry(body);
        putSampleEntry(body, 0, 1, other, 0, 1, 1);

        ByteBuf full;
        full.put16(2); // packet count
        full.put16(0); // reserved
        full.bytes(body.data(), body.size());

        if (!MP4WriteSample(f, hint, full.data(), full.size(),
                            spec.sampleDuration, 0, true)) {
            return false;
        }
    }

    *mediaOut = media;
    *hintOut = hint;
    return true;
}

// Hand-built main source: leading empty edit, middle empty edit, repeated
// reversed media segments, multi-packet hints with null/immediate/sample and
// self-referencing embedded bytes.
const uint32_t kMainSampleCount = 4;
const uint32_t kMainSampleDuration = 100;

bool buildMainSource(const std::string& path)
{
    MP4FileHandle f = MP4Create(path.c_str());
    if (f == MP4_INVALID_FILE_HANDLE) {
        return false;
    }
    MP4SetTimeScale(f, 1000);

    MP4TrackId media = MP4AddVideoTrack(f, 1000, 100, 16, 16,
                                        MP4_MPEG4_VIDEO_TYPE);
    if (media == MP4_INVALID_TRACK_ID) {
        return false;
    }
    const uint8_t config[] = { 0x12, 0x34, 0x56 };
    if (!MP4SetTrackESConfiguration(f, media, config, sizeof(config))) {
        return false;
    }

    for (uint32_t i = 0; i < kMainSampleCount; ++i) {
        uint8_t payload[4];
        payload[0] = (uint8_t)('A' + i);
        payload[1] = (uint8_t)i;
        payload[2] = (uint8_t)(0xa0 + i);
        payload[3] = (uint8_t)(0x0b + i);
        if (!MP4WriteSample(f, media, payload, sizeof(payload),
                            kMainSampleDuration, 0, (i % 2 == 0))) {
            return false;
        }
    }

    MP4TrackId hint = MP4AddHintTrack(f, media);
    if (hint == MP4_INVALID_TRACK_ID) {
        return false;
    }
    uint8_t payloadNumber = 96;
    if (!MP4SetHintTrackRtpPayload(f, hint, "MP4V-ES",
                                   &payloadNumber, 1400)) {
        return false;
    }

    const uint8_t embedded[] = { 0x77, 0x88, 0x99 };

    for (uint32_t i = 0; i < kMainSampleCount; ++i) {
        MP4SampleId sid = i + 1;
        // the edit list only contains the two-sample segments [1..2] and
        // [3..4], so cross references must stay inside each pair
        MP4SampleId other = (sid % 2 == 1) ? sid : sid - 1;

        ByteBuf body;

        // packet 0: marker set, transmit offset, immediate + media ref
        putPacketHeader(body, (int32_t)(i * 11), true, 96,
                        (uint16_t)(0xfff0 + i), 2);
        uint8_t imm = (uint8_t)('a' + i);
        putImmediateEntry(body, &imm, 1);
        putSampleEntry(body, 0, 1, sid, 1, 1, 1); // byte 1 of own sample

        // packet 1: null + cross media-sample reference within segment
        putPacketHeader(body, (int32_t)(i * 11 + 5), false, 96,
                        (uint16_t)(0xfff0 + i + 1), 2);
        putNullEntry(body);
        putSampleEntry(body, 0, 2, other, 0, 1, 1);

        // packet 2: self reference carrying embedded bytes; the sample
        // entry itself is 16 bytes (one type byte plus 15 property bytes),
        // so the embedded blob starts after the entry
        putPacketHeader(body, 0, false, 96, 0, 1);

        ByteBuf full;
        full.put16(3); // three packets
        full.put16(0);
        uint32_t entriesSize = (uint32_t)body.size();
        full.bytes(body.data(), body.size());

        static const uint32_t kSampleEntrySize = 16;
        uint32_t embeddedOffset =
            4u + entriesSize + kSampleEntrySize;
        putSampleEntry(full, 255, (uint16_t)sizeof(embedded),
                       sid, embeddedOffset, 1, 1);
        full.bytes(embedded, sizeof(embedded));

        if (!MP4WriteSample(f, hint, full.data(), full.size(),
                            kMainSampleDuration, 0, true)) {
            return false;
        }
    }

    // empty(50), media[3..4](200), empty(50), media[1..2](200),
    // media[3..4] again(200)
    if (MP4AddTrackEdit(f, media, MP4_INVALID_EDIT_ID,
                        UINT64_MAX, 50) == MP4_INVALID_EDIT_ID
            || MP4AddTrackEdit(f, media, MP4_INVALID_EDIT_ID,
                               200, 200) == MP4_INVALID_EDIT_ID
            || MP4AddTrackEdit(f, media, MP4_INVALID_EDIT_ID,
                               UINT64_MAX, 50) == MP4_INVALID_EDIT_ID
            || MP4AddTrackEdit(f, media, MP4_INVALID_EDIT_ID,
                               0, 200) == MP4_INVALID_EDIT_ID
            || MP4AddTrackEdit(f, media, MP4_INVALID_EDIT_ID,
                               200, 200) == MP4_INVALID_EDIT_ID) {
        return false;
    }

    MP4Close(f);
    return true;
}

// --------------------------------------------------------------------------
// output verification helpers
// --------------------------------------------------------------------------

// read one assembled RTP packet payload (no RTP header)
bool readPacketPayload(MP4FileHandle f, MP4TrackId hint,
                       uint16_t index, std::vector<uint8_t>& out)
{
    uint8_t* p = NULL;
    uint32_t n = 0;
    if (!MP4ReadRtpPacket(f, hint, index, &p, &n, 0, false, true)) {
        return false;
    }
    out.assign(p, p + n);
    MP4Free(p);
    return true;
}

// read the assembled RTP header (12 bytes)
bool readPacketHeaderBytes(MP4FileHandle f, MP4TrackId hint,
                           uint16_t index, std::vector<uint8_t>& out)
{
    uint8_t* p = NULL;
    uint32_t n = 0;
    if (!MP4ReadRtpPacket(f, hint, index, &p, &n, 0, true, false)
            || n != 12) {
        return false;
    }
    out.assign(p, p + n);
    MP4Free(p);
    return true;
}

// parse a raw hint sample and collect stored sequence numbers plus every
// type-2 sample reference (refIndex, sampleNumber, offset, length)
struct RawRef {
    uint8_t  refIndex;
    uint32_t sampleNumber;
    uint32_t sampleOffset;
    uint16_t length;
};

struct RawHint {
    std::vector<uint16_t> sequences;
    std::vector<RawRef> refs;
    uint16_t packetCount;
};

bool parseRawHint(const std::vector<uint8_t>& bytes, RawHint& out)
{
    uint32_t pos = 0;
    if (bytes.size() < 4) {
        return false;
    }
    out.packetCount = rd16(&bytes[0]);
    pos = 4;
    for (uint16_t pi = 0; pi < out.packetCount; ++pi) {
        if (pos + 12 > bytes.size()) {
            return false;
        }
        pos += 4; // relative transmit time
        pos += 1; // reserved/P/X/reserved bits
        pos += 1; // marker/payload type
        out.sequences.push_back(rd16(&bytes[pos]));
        pos += 2;
        pos += 2; // reserved3/extra/bframe/repeat flags
        uint16_t entryCount = rd16(&bytes[pos]);
        pos += 2;
        for (uint16_t ei = 0; ei < entryCount; ++ei) {
            uint8_t type = bytes[pos++];
            if (type == 0) {
                pos += 15;
            } else if (type == 1) {
                pos += 15;
            } else if (type == 2) {
                if (pos + 15 > bytes.size()) {
                    return false;
                }
                RawRef ref;
                ref.refIndex = bytes[pos++];
                ref.length = rd16(&bytes[pos]);
                pos += 2;
                ref.sampleNumber = rd32(&bytes[pos]);
                pos += 4;
                ref.sampleOffset = rd32(&bytes[pos]);
                pos += 4;
                pos += 4; // block layout, already validated as 1/1
                out.refs.push_back(ref);
            } else {
                return false;
            }
            if (pos > bytes.size()) {
                return false;
            }
        }
    }
    // bytes past the last entry are self-reference embedded payload blobs
    return pos <= bytes.size();
}

bool readRawSample(MP4FileHandle f, MP4TrackId track, MP4SampleId sid,
                   std::vector<uint8_t>& out)
{
    uint8_t* p = NULL;
    uint32_t n = 0;
    if (!MP4ReadSample(f, track, sid, &p, &n)) {
        return false;
    }
    out.assign(p, p + n);
    MP4Free(p);
    return true;
}

// expected media payload for source sample sid (1-based), mp4v variant
std::vector<uint8_t> expectedMedia(uint32_t sid)
{
    uint32_t i = sid - 1;
    uint8_t payload[4];
    payload[0] = (uint8_t)('A' + i);
    payload[1] = (uint8_t)i;
    payload[2] = (uint8_t)(0xa0 + i);
    payload[3] = (uint8_t)(0x0b + i);
    return std::vector<uint8_t>(payload, payload + 4);
}

// build a minimal one-hint, one-reference hinted track and return its ids
bool buildSimpleHinted(MP4FileHandle f, uint32_t movieScale,
                       uint32_t trackScale, uint32_t sampleCount,
                       uint32_t sampleDuration, MP4TrackId* media,
                       MP4TrackId* hint)
{
    MP4SetTimeScale(f, movieScale);
    *media = MP4AddVideoTrack(f, trackScale, sampleDuration, 16, 16,
                              MP4_MPEG4_VIDEO_TYPE);
    if (*media == MP4_INVALID_TRACK_ID) {
        return false;
    }
    for (uint32_t i = 0; i < sampleCount; ++i) {
        uint8_t payload[4] = { (uint8_t)('A' + i), 0, 0, 0 };
        if (!MP4WriteSample(f, *media, payload, sizeof(payload),
                            sampleDuration, 0, true)) {
            return false;
        }
    }
    *hint = MP4AddHintTrack(f, *media);
    if (*hint == MP4_INVALID_TRACK_ID) {
        return false;
    }
    uint8_t pt = 96;
    if (!MP4SetHintTrackRtpPayload(f, *hint, "MP4V-ES", &pt, 1400)) {
        return false;
    }
    for (uint32_t i = 0; i < sampleCount; ++i) {
        ByteBuf full;
        full.put16(1);
        full.put16(0);
        putPacketHeader(full, 0, true, 96, (uint16_t)i, 1);
        putSampleEntry(full, 0, 1, i + 1, 0);
        if (!MP4WriteSample(f, *hint, full.data(), full.size(),
                            sampleDuration, 0, true)) {
            return false;
        }
    }
    return true;
}

// ==========================================================================
// occurrence group
// ==========================================================================

int case_reordered_full()
{
    std::string dir = scratchDir();
    TASK_CHECK(!dir.empty());
    std::string srcPath = dir + "/src.mp4";
    std::string dstPath = dir + "/dst.mp4";

    TASK_CHECK(buildMainSource(srcPath));

    MP4FileHandle src = MP4Read(srcPath.c_str());
    TASK_CHECK(src != MP4_INVALID_FILE_HANDLE);
    MP4TrackId srcHint = MP4FindTrackId(src, 0, MP4_HINT_TRACK_TYPE);
    TASK_CHECK(srcHint != MP4_INVALID_TRACK_ID);

    MP4FileHandle dst = MP4Create(dstPath.c_str());
    TASK_CHECK(dst != MP4_INVALID_FILE_HANDLE);

    MP4TrackId media = MP4_INVALID_TRACK_ID;
    MP4TrackId hint = MP4_INVALID_TRACK_ID;
    TASK_CHECK(MP4CopyHintedTrack(src, srcHint, dst, &media, &hint));
    TASK_CHECK(media != MP4_INVALID_TRACK_ID);
    TASK_CHECK(hint != MP4_INVALID_TRACK_ID);
    MP4Close(dst);
    MP4Close(src);

    // reopen with fresh handles and verify through the real read APIs
    dst = MP4Read(dstPath.c_str());
    TASK_CHECK(dst != MP4_INVALID_FILE_HANDLE);
    TASK_CHECK(MP4GetNumberOfTracks(dst) == 2);
    TASK_CHECK(MP4GetHintTrackReferenceTrackId(dst, hint) == media);

    // sample counts: six copied media samples, eight hint samples
    // (two empty-segment hints + six media hints)
    TASK_CHECK(MP4GetTrackNumberOfSamples(dst, media) == 6);
    TASK_CHECK(MP4GetTrackNumberOfSamples(dst, hint) == 8);

    // dual timeline: tkhd movie durations agree, mdhd differ by the gaps
    TASK_CHECK(MP4GetTrackDuration(dst, media) == 600);
    TASK_CHECK(MP4GetTrackDuration(dst, hint) == 700);
    uint64_t mediaTkhd = 0, hintTkhd = 0;
    TASK_CHECK(MP4GetTrackIntegerProperty(dst, media,
                                          "tkhd.duration", &mediaTkhd));
    TASK_CHECK(MP4GetTrackIntegerProperty(dst, hint,
                                          "tkhd.duration", &hintTkhd));
    TASK_CHECK(mediaTkhd == 700 && hintTkhd == 700);
    TASK_CHECK(MP4GetDuration(dst) == 700);

    // the hint track has no edit list, the media track keeps five edits
    TASK_CHECK(MP4GetTrackNumberOfEdits(dst, hint) == 0);
    TASK_CHECK(MP4GetTrackNumberOfEdits(dst, media) == 5);
    TASK_CHECK(MP4GetTrackEditDuration(dst, media, 1) == 50);
    // empty edits carry the all-ones mediaTime sentinel and no dwell
    TASK_CHECK(!MP4GetTrackEditDwell(dst, media, 1));
    TASK_CHECK(MP4GetTrackEditMediaStart(dst, media, 1) == UINT32_MAX);
    TASK_CHECK(MP4GetTrackEditDuration(dst, media, 2) == 200);
    TASK_CHECK(MP4GetTrackEditMediaStart(dst, media, 2) == 0);
    TASK_CHECK(MP4GetTrackEditDuration(dst, media, 3) == 50);
    TASK_CHECK(!MP4GetTrackEditDwell(dst, media, 3));
    TASK_CHECK(MP4GetTrackEditMediaStart(dst, media, 3) == UINT32_MAX);
    TASK_CHECK(MP4GetTrackEditDuration(dst, media, 4) == 200);
    TASK_CHECK(MP4GetTrackEditMediaStart(dst, media, 4) == 200);
    TASK_CHECK(MP4GetTrackEditDuration(dst, media, 5) == 200);
    TASK_CHECK(MP4GetTrackEditMediaStart(dst, media, 5) == 400);

    // hint sample timeline, gaps included:
    // 0(empty,50) 50 150 250(empty,50) 300 400 500 600
    const MP4Timestamp hintStarts[8] = { 0, 50, 150, 250, 300, 400, 500, 600 };
    const MP4Duration hintDurs[8] = { 50, 100, 100, 50, 100, 100, 100, 100 };
    for (uint32_t i = 0; i < 8; ++i) {
        TASK_CHECK(MP4GetSampleTime(dst, hint, i + 1) == hintStarts[i]);
        TASK_CHECK(MP4GetSampleDuration(dst, hint, i + 1) == hintDurs[i]);
    }

    // media samples are contiguous, no gaps in the media timeline
    for (uint32_t i = 0; i < 6; ++i) {
        TASK_CHECK(MP4GetSampleTime(dst, media, i + 1) == i * 100);
        TASK_CHECK(MP4GetSampleDuration(dst, media, i + 1) == 100);
    }

    // empty hint samples contain zero packets but consume timeline time
    uint16_t packets = 99;
    TASK_CHECK(MP4ReadRtpHint(dst, hint, 1, &packets));
    TASK_CHECK(packets == 0);
    TASK_CHECK(MP4ReadRtpHint(dst, hint, 4, &packets));
    TASK_CHECK(packets == 0);

    // all other hints carry the original three packets
    const uint32_t mediaHintIds[6] = { 2, 3, 5, 6, 7, 8 };
    for (uint32_t i = 0; i < 6; ++i) {
        TASK_CHECK(MP4ReadRtpHint(dst, hint, mediaHintIds[i], &packets));
        TASK_CHECK(packets == 3);
    }

    // stored packet sequence numbers start at zero and increase continuously
    uint32_t expectedSeq = 0;
    for (uint32_t i = 0; i < 8; ++i) {
        std::vector<uint8_t> raw;
        TASK_CHECK(readRawSample(dst, hint, i + 1, raw));
        RawHint parsed;
        TASK_CHECK(parseRawHint(raw, parsed));
        for (size_t k = 0; k < parsed.sequences.size(); ++k) {
            TASK_CHECK(parsed.sequences[k] == (uint16_t)(expectedSeq & 0xffff));
            ++expectedSeq;
        }
    }
    TASK_CHECK(expectedSeq == 18); // six media hints times three packets

    // the RTP read path also sees continuous sequence number deltas, even
    // though its random start offset is unknown to us
    TASK_CHECK(MP4ReadRtpHint(dst, hint, 2, NULL));
    std::vector<uint8_t> h0, h1;
    TASK_CHECK(readPacketHeaderBytes(dst, hint, 0, h0));
    TASK_CHECK(readPacketHeaderBytes(dst, hint, 1, h1));
    uint16_t seq0 = rd16(&h0[2]);
    uint16_t seq1 = rd16(&h1[2]);
    TASK_CHECK((uint16_t)(seq1 - seq0) == 1);
    TASK_CHECK((h0[1] & 0x80) == 0x80); // marker preserved on first packet
    TASK_CHECK((h0[1] & 0x7f) == 96);   // payload type preserved
    // transmit offset preserved verbatim: source hint 3 packet 0 used 22
    TASK_CHECK(MP4GetRtpPacketTransmitOffset(dst, hint, 0) == 22);
    TASK_CHECK(MP4GetRtpPacketTransmitOffset(dst, hint, 1) == 27);

    // RTP timestamps move with the gapped hint timeline: hint 2 starts at
    // movie time 50, hint 3 at 150 -> header timestamp delta of 100
    TASK_CHECK(MP4ReadRtpHint(dst, hint, 2, NULL));
    std::vector<uint8_t> rtp2;
    TASK_CHECK(readPacketHeaderBytes(dst, hint, 0, rtp2));
    TASK_CHECK(MP4ReadRtpHint(dst, hint, 3, NULL));
    std::vector<uint8_t> rtp3;
    TASK_CHECK(readPacketHeaderBytes(dst, hint, 0, rtp3));
    uint32_t ts2 = rd32(&rtp2[4]);
    uint32_t ts3 = rd32(&rtp3[4]);
    TASK_CHECK(ts3 - ts2 == 100);

    // payload verification for the first copied occurrence (source samples
    // 3 and 4 become new media samples 1 and 2)
    TASK_CHECK(MP4ReadRtpHint(dst, hint, 2, NULL));
    std::vector<uint8_t> pay0, pay1, pay2;
    TASK_CHECK(readPacketPayload(dst, hint, 0, pay0));
    TASK_CHECK(pay0.size() == 2);
    TASK_CHECK(pay0[0] == 'c'); // immediate byte of source hint 3
    std::vector<uint8_t> media3 = expectedMedia(3);
    TASK_CHECK(pay0[1] == media3[1]); // referenced byte 1 of media sample 3

    // second packet references the other sample of the same two-sample
    // segment; for source hint 3 that is still sample 3, mapped onto the
    // first new media sample
    TASK_CHECK(readPacketPayload(dst, hint, 1, pay1));
    TASK_CHECK(pay1.size() == 2);
    TASK_CHECK(pay1[0] == media3[0]);

    // third packet is the self reference: embedded bytes survive and are
    // still read through the sample-relative self reference
    TASK_CHECK(readPacketPayload(dst, hint, 2, pay2));
    TASK_CHECK(pay2.size() == 3);
    TASK_CHECK(pay2[0] == 0x77 && pay2[1] == 0x88 && pay2[2] == 0x99);

    // raw reference check: repeated occurrences rebound independently.
    // output hint ids: 2=src3, 3=src4, 5=src1, 6=src2, 7=src3, 8=src4
    // new media ids:    1=src3, 2=src4, 3=src1, 4=src2, 5=src3, 6=src4
    // each hint holds an own-sample reference (refs[0]), an in-segment
    // other-sample reference (refs[1]) and the self reference (refs[2])
    struct Expect {
        MP4SampleId hintId;
        uint32_t    ownTarget;
        uint32_t    otherTarget;
        MP4SampleId selfTarget;
    };
    const Expect expects[6] = {
        { 2, 1, 1, 2 }, { 3, 2, 1, 3 }, { 5, 3, 3, 5 },
        { 6, 4, 3, 6 }, { 7, 5, 5, 7 }, { 8, 6, 5, 8 }
    };
    for (uint32_t i = 0; i < 6; ++i) {
        std::vector<uint8_t> raw;
        TASK_CHECK(readRawSample(dst, hint, expects[i].hintId, raw));
        RawHint parsed;
        TASK_CHECK(parseRawHint(raw, parsed));
        TASK_CHECK(parsed.refs.size() == 3);
        TASK_CHECK(parsed.refs[0].refIndex == 0);
        TASK_CHECK(parsed.refs[0].sampleNumber == expects[i].ownTarget);
        TASK_CHECK(parsed.refs[1].refIndex == 0);
        TASK_CHECK(parsed.refs[1].sampleNumber == expects[i].otherTarget);
        TASK_CHECK(parsed.refs[2].refIndex == 255);
        TASK_CHECK(parsed.refs[2].sampleNumber == expects[i].selfTarget);
    }

    // sync flags carried over (source samples 1 and 3 are sync)
    TASK_CHECK(MP4GetSampleSync(dst, media, 1) == 1); // src3
    TASK_CHECK(MP4GetSampleSync(dst, media, 2) == 0); // src4
    TASK_CHECK(MP4GetSampleSync(dst, media, 3) == 1); // src1
    TASK_CHECK(MP4GetSampleSync(dst, media, 4) == 0); // src2

    MP4Close(dst);
    cleanupDir(dir);
    return 0;
}

int case_no_edits_whole_track()
{
    std::string dir = scratchDir();
    TASK_CHECK(!dir.empty());
    std::string srcPath = dir + "/src.mp4";
    std::string dstPath = dir + "/dst.mp4";

    MP4FileHandle f = MP4Create(srcPath.c_str());
    SourceSpec spec;
    spec.movieTimeScale = 1000;
    spec.trackTimeScale = 1000;
    spec.sampleDuration = 100;
    spec.sampleCount = 3;
    spec.avc1 = false;
    MP4TrackId media = MP4_INVALID_TRACK_ID;
    MP4TrackId hint = MP4_INVALID_TRACK_ID;
    TASK_CHECK(buildBasicSource(f, spec, &media, &hint));
    MP4Close(f);

    MP4FileHandle src = MP4Read(srcPath.c_str());
    MP4FileHandle dst = MP4Create(dstPath.c_str());
    MP4TrackId outMedia = MP4_INVALID_TRACK_ID;
    MP4TrackId outHint = MP4_INVALID_TRACK_ID;
    TASK_CHECK(MP4CopyHintedTrack(src, hint, dst, &outMedia, &outHint));
    MP4Close(dst);
    MP4Close(src);

    dst = MP4Read(dstPath.c_str());
    TASK_CHECK(MP4GetTrackNumberOfSamples(dst, outMedia) == 3);
    TASK_CHECK(MP4GetTrackNumberOfSamples(dst, outHint) == 3);
    TASK_CHECK(MP4GetTrackNumberOfEdits(dst, outMedia) == 0);
    TASK_CHECK(MP4GetTrackDuration(dst, outMedia) == 300);
    TASK_CHECK(MP4GetTrackDuration(dst, outHint) == 300);

    // every packet survives in order
    for (MP4SampleId sid = 1; sid <= 3; ++sid) {
        uint16_t count = 0;
        TASK_CHECK(MP4ReadRtpHint(dst, outHint, sid, &count));
        TASK_CHECK(count == 2);
        std::vector<uint8_t> raw;
        TASK_CHECK(readRawSample(dst, outHint, sid, raw));
        RawHint parsed;
        TASK_CHECK(parseRawHint(raw, parsed));
        TASK_CHECK(parsed.sequences.size() == 2);
        TASK_CHECK(parsed.sequences[0] == (uint16_t)((sid - 1) * 2));
        TASK_CHECK(parsed.sequences[1] == (uint16_t)((sid - 1) * 2 + 1));
    }
    MP4Close(dst);

    cleanupDir(dir);
    return 0;
}

int case_existing_destination_tracks()
{
    std::string dir = scratchDir();
    TASK_CHECK(!dir.empty());
    std::string srcPath = dir + "/src.mp4";
    std::string dstPath = dir + "/dst.mp4";
    TASK_CHECK(buildMainSource(srcPath));

    MP4FileHandle dst = MP4Create(dstPath.c_str());
    // pre-existing unrelated track
    MP4TrackId pre = MP4AddVideoTrack(dst, 1000, 100, 8, 8,
                                      MP4_MPEG4_VIDEO_TYPE);
    TASK_CHECK(pre != MP4_INVALID_TRACK_ID);
    const uint8_t preBytes[] = { 1, 2, 3, 4 };
    TASK_CHECK(MP4WriteSample(dst, pre, preBytes, sizeof(preBytes),
                              100, 0, true));

    MP4FileHandle src = MP4Read(srcPath.c_str());
    MP4TrackId srcHint = MP4FindTrackId(src, 0, MP4_HINT_TRACK_TYPE);
    MP4TrackId media = MP4_INVALID_TRACK_ID;
    MP4TrackId hint = MP4_INVALID_TRACK_ID;
    TASK_CHECK(MP4CopyHintedTrack(src, srcHint, dst, &media, &hint));
    TASK_CHECK(MP4GetNumberOfTracks(dst) == 3);
    MP4Close(dst);
    MP4Close(src);

    dst = MP4Read(dstPath.c_str());
    TASK_CHECK(MP4GetNumberOfTracks(dst) == 3);
    TASK_CHECK(MP4GetTrackNumberOfSamples(dst, pre) == 1);
    TASK_CHECK(MP4GetHintTrackReferenceTrackId(dst, hint) == media);
    TASK_CHECK(MP4GetTrackNumberOfSamples(dst, media) == 6);
    MP4Close(dst);

    cleanupDir(dir);
    return 0;
}

int case_avc1_source()
{
    std::string dir = scratchDir();
    TASK_CHECK(!dir.empty());
    std::string srcPath = dir + "/src.mp4";
    std::string dstPath = dir + "/dst.mp4";

    MP4FileHandle f = MP4Create(srcPath.c_str());
    SourceSpec spec;
    spec.movieTimeScale = 1000;
    spec.trackTimeScale = 1000;
    spec.sampleDuration = 100;
    spec.sampleCount = 2;
    spec.avc1 = true;
    MP4TrackId media = MP4_INVALID_TRACK_ID;
    MP4TrackId hint = MP4_INVALID_TRACK_ID;
    TASK_CHECK(buildBasicSource(f, spec, &media, &hint));
    MP4Close(f);

    MP4FileHandle src = MP4Read(srcPath.c_str());
    MP4FileHandle dst = MP4Create(dstPath.c_str());
    MP4TrackId outMedia = MP4_INVALID_TRACK_ID;
    MP4TrackId outHint = MP4_INVALID_TRACK_ID;
    TASK_CHECK(MP4CopyHintedTrack(src, hint, dst, &outMedia, &outHint));
    MP4Close(dst);
    MP4Close(src);

    dst = MP4Read(dstPath.c_str());
    TASK_CHECK(MP4GetTrackMediaDataName(dst, outMedia) != NULL);
    TASK_CHECK(!strcmp(MP4GetTrackMediaDataName(dst, outMedia), "avc1"));
    TASK_CHECK(MP4GetTrackNumberOfSamples(dst, outMedia) == 2);
    TASK_CHECK(MP4GetTrackNumberOfSamples(dst, outHint) == 2);
    // avcC configuration survived
    TASK_CHECK(MP4HaveTrackAtom(dst, outMedia,
                                "mdia.minf.stbl.stsd.avc1.avcC"));
    uint16_t count = 0;
    TASK_CHECK(MP4ReadRtpHint(dst, outHint, 1, &count));
    TASK_CHECK(count == 2);
    MP4Close(dst);

    cleanupDir(dir);
    return 0;
}

int case_example_program()
{
    std::string dir = scratchDir();
    TASK_CHECK(!dir.empty());
    std::string srcPath = dir + "/src.mp4";
    std::string dstPath = dir + "/dst.mp4";
    TASK_CHECK(buildMainSource(srcPath));

    std::string cmd = std::string(".task-build/example \"") + srcPath
                      + "\" \"" + dstPath + "\"";
    int rc = system(cmd.c_str());
    TASK_CHECK(rc == 0);

    MP4FileHandle dst = MP4Read(dstPath.c_str());
    TASK_CHECK(dst != MP4_INVALID_FILE_HANDLE);
    MP4TrackId hint = MP4FindTrackId(dst, 0, MP4_HINT_TRACK_TYPE);
    MP4TrackId media = MP4FindTrackId(dst, 0, MP4_VIDEO_TRACK_TYPE);
    TASK_CHECK(hint != MP4_INVALID_TRACK_ID);
    TASK_CHECK(media != MP4_INVALID_TRACK_ID);
    TASK_CHECK(MP4GetTrackNumberOfSamples(dst, media) == 6);
    TASK_CHECK(MP4GetTrackNumberOfSamples(dst, hint) == 8);
    TASK_CHECK(MP4GetHintTrackReferenceTrackId(dst, hint) == media);
    uint16_t count = 0;
    TASK_CHECK(MP4ReadRtpHint(dst, hint, 2, &count));
    TASK_CHECK(count == 3);
    MP4Close(dst);

    cleanupDir(dir);
    return 0;
}

// ==========================================================================
// clock group
// ==========================================================================

int case_timescale_exact_conversion()
{
    std::string dir = scratchDir();
    TASK_CHECK(!dir.empty());
    std::string srcPath = dir + "/src.mp4";
    std::string dstPath = dir + "/dst.mp4";

    // movie timescale 600, track timescale 1000: 100 media units equal
    // exactly 60 movie units
    MP4FileHandle f = MP4Create(srcPath.c_str());
    MP4TrackId media = MP4_INVALID_TRACK_ID;
    MP4TrackId hint = MP4_INVALID_TRACK_ID;
    TASK_CHECK(buildSimpleHinted(f, 600, 1000, 4, 100, &media, &hint));
    // 30 movie units empty (= 50 media units), then media[1..2]
    TASK_CHECK(MP4AddTrackEdit(f, media, MP4_INVALID_EDIT_ID,
                               UINT64_MAX, 30) != MP4_INVALID_EDIT_ID);
    TASK_CHECK(MP4AddTrackEdit(f, media, MP4_INVALID_EDIT_ID,
                               0, 120) != MP4_INVALID_EDIT_ID);
    MP4Close(f);

    MP4FileHandle src = MP4Read(srcPath.c_str());
    MP4FileHandle dst = MP4Create(dstPath.c_str());
    MP4TrackId outMedia = MP4_INVALID_TRACK_ID;
    MP4TrackId outHint = MP4_INVALID_TRACK_ID;
    TASK_CHECK(MP4CopyHintedTrack(src, hint, dst, &outMedia, &outHint));
    MP4Close(dst);
    MP4Close(src);

    dst = MP4Read(dstPath.c_str());
    // movie duration 150 units at 600 -> 250 units at the destination's
    // default 1000 movie timescale; media mdhd 200, hint mdhd 250
    TASK_CHECK(MP4GetTrackDuration(dst, outMedia) == 200);
    TASK_CHECK(MP4GetTrackDuration(dst, outHint) == 250);
    uint64_t tkhd = 0;
    TASK_CHECK(MP4GetTrackIntegerProperty(dst, outMedia,
                                          "tkhd.duration", &tkhd));
    TASK_CHECK(tkhd == 250);
    TASK_CHECK(MP4GetTrackEditDuration(dst, outMedia, 1) == 50);
    TASK_CHECK(MP4GetTrackEditDuration(dst, outMedia, 2) == 200);
    TASK_CHECK(MP4GetTrackEditMediaStart(dst, outMedia, 2) == 0);
    // hint gap honored: empty sample 50 media units then two 100-unit hints
    TASK_CHECK(MP4GetSampleTime(dst, outHint, 1) == 0);
    TASK_CHECK(MP4GetSampleDuration(dst, outHint, 1) == 50);
    TASK_CHECK(MP4GetSampleTime(dst, outHint, 2) == 50);
    TASK_CHECK(MP4GetSampleTime(dst, outHint, 3) == 150);
    MP4Close(dst);

    cleanupDir(dir);
    return 0;
}

int case_timescale_indivisible_rejected()
{
    std::string dir = scratchDir();
    TASK_CHECK(!dir.empty());
    std::string srcPath = dir + "/src.mp4";
    std::string dstPath = dir + "/dst.mp4";

    MP4FileHandle f = MP4Create(srcPath.c_str());
    MP4TrackId media = MP4_INVALID_TRACK_ID;
    MP4TrackId hint = MP4_INVALID_TRACK_ID;
    TASK_CHECK(buildSimpleHinted(f, 600, 1000, 2, 100, &media, &hint));
    // 100 movie units at 600 -> 166.67 media units at 1000, not integral
    TASK_CHECK(MP4AddTrackEdit(f, media, MP4_INVALID_EDIT_ID,
                               0, 100) != MP4_INVALID_EDIT_ID);
    MP4Close(f);

    MP4FileHandle src = MP4Read(srcPath.c_str());
    MP4FileHandle dst = MP4Create(dstPath.c_str());
    MP4TrackId pre = MP4AddVideoTrack(dst, 1000, 100, 8, 8,
                                      MP4_MPEG4_VIDEO_TYPE);
    TASK_CHECK(pre != MP4_INVALID_TRACK_ID);
    MP4TrackId outMedia = MP4_INVALID_TRACK_ID;
    MP4TrackId outHint = MP4_INVALID_TRACK_ID;
    TASK_CHECK(!MP4CopyHintedTrack(src, hint, dst, &outMedia, &outHint));
    TASK_CHECK(outMedia == MP4_INVALID_TRACK_ID);
    TASK_CHECK(outHint == MP4_INVALID_TRACK_ID);
    // preflight rejection must not have created any track
    TASK_CHECK(MP4GetNumberOfTracks(dst) == 1);
    MP4Close(dst);
    MP4Close(src);

    cleanupDir(dir);
    return 0;
}

int case_dst_movie_timescale()
{
    std::string dir = scratchDir();
    TASK_CHECK(!dir.empty());
    std::string srcPath = dir + "/src.mp4";
    std::string dstPath = dir + "/dst.mp4";
    TASK_CHECK(buildMainSource(srcPath));

    MP4FileHandle src = MP4Read(srcPath.c_str());
    MP4TrackId srcHint = MP4FindTrackId(src, 0, MP4_HINT_TRACK_TYPE);
    MP4FileHandle dst = MP4Create(dstPath.c_str());
    TASK_CHECK(MP4SetTimeScale(dst, 30000));
    MP4TrackId media = MP4_INVALID_TRACK_ID;
    MP4TrackId hint = MP4_INVALID_TRACK_ID;
    TASK_CHECK(MP4CopyHintedTrack(src, srcHint, dst, &media, &hint));
    MP4Close(dst);
    MP4Close(src);

    dst = MP4Read(dstPath.c_str());
    // 700 src movie units at 1000 -> 21000 dst movie units at 30000
    uint64_t tkhd = 0;
    TASK_CHECK(MP4GetTrackIntegerProperty(dst, media,
                                          "tkhd.duration", &tkhd));
    TASK_CHECK(tkhd == 21000);
    TASK_CHECK(MP4GetTrackDuration(dst, media) == 600);
    TASK_CHECK(MP4GetTrackDuration(dst, hint) == 700);
    TASK_CHECK(MP4GetTrackEditDuration(dst, media, 1) == 1500);
    TASK_CHECK(MP4GetTrackEditDuration(dst, media, 2) == 6000);
    TASK_CHECK(MP4GetTrackEditMediaStart(dst, media, 2) == 0);
    MP4Close(dst);

    cleanupDir(dir);
    return 0;
}

int case_all_empty_rejected()
{
    std::string dir = scratchDir();
    TASK_CHECK(!dir.empty());
    std::string srcPath = dir + "/src.mp4";
    std::string dstPath = dir + "/dst.mp4";

    MP4FileHandle f = MP4Create(srcPath.c_str());
    MP4SetTimeScale(f, 1000);
    MP4TrackId media = MP4AddVideoTrack(f, 1000, 100, 16, 16,
                                        MP4_MPEG4_VIDEO_TYPE);
    uint8_t payload[4] = { 'A', 0, 0, 0 };
    TASK_CHECK(MP4WriteSample(f, media, payload, sizeof(payload),
                              100, 0, true));
    MP4TrackId hint = MP4AddHintTrack(f, media);
    uint8_t pt = 96;
    TASK_CHECK(MP4SetHintTrackRtpPayload(f, hint, "MP4V-ES", &pt, 1400));
    ByteBuf full;
    full.put16(0);
    full.put16(0);
    TASK_CHECK(MP4WriteSample(f, hint, full.data(), full.size(),
                              100, 0, true));
    TASK_CHECK(MP4AddTrackEdit(f, media, MP4_INVALID_EDIT_ID,
                               UINT64_MAX, 50) != MP4_INVALID_EDIT_ID);
    TASK_CHECK(MP4AddTrackEdit(f, media, MP4_INVALID_EDIT_ID,
                               UINT64_MAX, 50) != MP4_INVALID_EDIT_ID);
    MP4Close(f);

    MP4FileHandle src = MP4Read(srcPath.c_str());
    MP4FileHandle dst = MP4Create(dstPath.c_str());
    MP4TrackId outMedia = MP4_INVALID_TRACK_ID;
    MP4TrackId outHint = MP4_INVALID_TRACK_ID;
    TASK_CHECK(!MP4CopyHintedTrack(src, hint, dst, &outMedia, &outHint));
    TASK_CHECK(outMedia == MP4_INVALID_TRACK_ID);
    TASK_CHECK(outHint == MP4_INVALID_TRACK_ID);
    TASK_CHECK(MP4GetNumberOfTracks(dst) == 0);
    MP4Close(dst);
    MP4Close(src);

    cleanupDir(dir);
    return 0;
}

// ==========================================================================
// reject_compat group
// ==========================================================================

int case_bad_cross_segment_reference()
{
    std::string dir = scratchDir();
    TASK_CHECK(!dir.empty());
    std::string srcPath = dir + "/src.mp4";
    std::string dstPath = dir + "/dst.mp4";

    MP4FileHandle f = MP4Create(srcPath.c_str());
    MP4SetTimeScale(f, 1000);
    MP4TrackId media = MP4AddVideoTrack(f, 1000, 100, 16, 16,
                                        MP4_MPEG4_VIDEO_TYPE);
    for (uint32_t i = 0; i < 4; ++i) {
        uint8_t payload[4] = { (uint8_t)('A' + i), 0, 0, 0 };
        TASK_CHECK(MP4WriteSample(f, media, payload, sizeof(payload),
                                  100, 0, (i % 2 == 0)));
    }
    MP4TrackId hint = MP4AddHintTrack(f, media);
    uint8_t pt = 96;
    TASK_CHECK(MP4SetHintTrackRtpPayload(f, hint, "MP4V-ES", &pt, 1400));

    // hint for sample 3 references sample 1; an edit covering only [3..4]
    // makes that reference leave the segment
    for (uint32_t i = 0; i < 4; ++i) {
        ByteBuf full;
        full.put16(1);
        full.put16(0);
        putPacketHeader(full, 0, true, 96, (uint16_t)i, 1);
        MP4SampleId ref = (i == 2) ? 1u : (i + 1);
        putSampleEntry(full, 0, 1, ref, 0);
        TASK_CHECK(MP4WriteSample(f, hint, full.data(), full.size(),
                                  100, 0, true));
    }
    TASK_CHECK(MP4AddTrackEdit(f, media, MP4_INVALID_EDIT_ID,
                               200, 200) != MP4_INVALID_EDIT_ID);
    MP4Close(f);

    MP4FileHandle src = MP4Read(srcPath.c_str());
    MP4FileHandle dst = MP4Create(dstPath.c_str());
    MP4TrackId pre = MP4AddVideoTrack(dst, 1000, 100, 8, 8,
                                      MP4_MPEG4_VIDEO_TYPE);
    MP4TrackId outMedia = MP4_INVALID_TRACK_ID;
    MP4TrackId outHint = MP4_INVALID_TRACK_ID;
    TASK_CHECK(!MP4CopyHintedTrack(src, hint, dst, &outMedia, &outHint));
    TASK_CHECK(outMedia == MP4_INVALID_TRACK_ID);
    TASK_CHECK(outHint == MP4_INVALID_TRACK_ID);
    TASK_CHECK(MP4GetNumberOfTracks(dst) == 1);
    TASK_CHECK(MP4GetTrackNumberOfSamples(dst, pre) == 0);
    MP4Close(dst);
    MP4Close(src);

    cleanupDir(dir);
    return 0;
}

int case_bad_self_reference()
{
    std::string dir = scratchDir();
    TASK_CHECK(!dir.empty());
    std::string srcPath = dir + "/src.mp4";
    std::string dstPath = dir + "/dst.mp4";

    MP4FileHandle f = MP4Create(srcPath.c_str());
    MP4SetTimeScale(f, 1000);
    MP4TrackId media = MP4AddVideoTrack(f, 1000, 100, 16, 16,
                                        MP4_MPEG4_VIDEO_TYPE);
    for (uint32_t i = 0; i < 2; ++i) {
        uint8_t payload[4] = { (uint8_t)('A' + i), 0, 0, 0 };
        TASK_CHECK(MP4WriteSample(f, media, payload, sizeof(payload),
                                  100, 0, true));
    }
    MP4TrackId hint = MP4AddHintTrack(f, media);
    uint8_t pt = 96;
    TASK_CHECK(MP4SetHintTrackRtpPayload(f, hint, "MP4V-ES", &pt, 1400));
    for (uint32_t i = 0; i < 2; ++i) {
        ByteBuf full;
        full.put16(1);
        full.put16(0);
        putPacketHeader(full, 0, true, 96, (uint16_t)i, 1);
        // hint sample 2 points back at hint sample 1: cross-sample self ref
        putSampleEntry(full, 255, 1, 1, 0);
        TASK_CHECK(MP4WriteSample(f, hint, full.data(), full.size(),
                                  100, 0, true));
    }
    MP4Close(f);

    MP4FileHandle src = MP4Read(srcPath.c_str());
    MP4FileHandle dst = MP4Create(dstPath.c_str());
    MP4TrackId outMedia = MP4_INVALID_TRACK_ID;
    MP4TrackId outHint = MP4_INVALID_TRACK_ID;
    TASK_CHECK(!MP4CopyHintedTrack(src, hint, dst, &outMedia, &outHint));
    TASK_CHECK(MP4GetNumberOfTracks(dst) == 0);
    MP4Close(dst);
    MP4Close(src);

    cleanupDir(dir);
    return 0;
}

int case_edit_not_on_sample_boundary()
{
    std::string dir = scratchDir();
    TASK_CHECK(!dir.empty());
    std::string srcPath = dir + "/src.mp4";
    std::string dstPath = dir + "/dst.mp4";

    MP4FileHandle f = MP4Create(srcPath.c_str());
    MP4TrackId media = MP4_INVALID_TRACK_ID;
    MP4TrackId hint = MP4_INVALID_TRACK_ID;
    TASK_CHECK(buildSimpleHinted(f, 1000, 1000, 4, 100, &media, &hint));
    // mediaTime 50 lands in the middle of sample 1
    TASK_CHECK(MP4AddTrackEdit(f, media, MP4_INVALID_EDIT_ID,
                               50, 100) != MP4_INVALID_EDIT_ID);
    MP4Close(f);

    MP4FileHandle src = MP4Read(srcPath.c_str());
    MP4FileHandle dst = MP4Create(dstPath.c_str());
    MP4TrackId outMedia = MP4_INVALID_TRACK_ID;
    MP4TrackId outHint = MP4_INVALID_TRACK_ID;
    TASK_CHECK(!MP4CopyHintedTrack(src, hint, dst, &outMedia, &outHint));
    TASK_CHECK(MP4GetNumberOfTracks(dst) == 0);
    MP4Close(dst);
    MP4Close(src);

    cleanupDir(dir);
    return 0;
}

int case_edit_start_not_sync()
{
    std::string dir = scratchDir();
    TASK_CHECK(!dir.empty());
    std::string srcPath = dir + "/src.mp4";
    std::string dstPath = dir + "/dst.mp4";

    // samples 1 and 3 are sync, samples 2 and 4 are not; an edit starting on
    // sample 2 must be rejected
    MP4FileHandle f = MP4Create(srcPath.c_str());
    MP4SetTimeScale(f, 1000);
    MP4TrackId media = MP4AddVideoTrack(f, 1000, 100, 16, 16, MP4_MPEG4_VIDEO_TYPE);
    for (uint32_t i = 0; i < 4; ++i) {
        uint8_t payload[4] = { (uint8_t)('A' + i), 0, 0, 0 };
        TASK_CHECK(MP4WriteSample(f, media, payload, sizeof(payload),
                                  100, 0, (i % 2 == 0)));
    }
    MP4TrackId hint = MP4AddHintTrack(f, media);
    uint8_t ptn = 96;
    TASK_CHECK(MP4SetHintTrackRtpPayload(f, hint, "MP4V-ES", &ptn, 1400));
    for (uint32_t i = 0; i < 4; ++i) {
        ByteBuf full;
        full.put16(1);
        full.put16(0);
        putPacketHeader(full, 0, true, 96, (uint16_t)i, 1);
        putSampleEntry(full, 0, 1, i + 1, 0);
        TASK_CHECK(MP4WriteSample(f, hint, full.data(), full.size(),
                                  100, 0, true));
    }
    // mediaTime 100 is sample 2, which is not a sync sample
    TASK_CHECK(MP4AddTrackEdit(f, media, MP4_INVALID_EDIT_ID,
                               100, 200) != MP4_INVALID_EDIT_ID);
    MP4Close(f);

    MP4FileHandle src = MP4Read(srcPath.c_str());
    MP4FileHandle dst = MP4Create(dstPath.c_str());
    MP4TrackId outMedia = MP4_INVALID_TRACK_ID;
    MP4TrackId outHint = MP4_INVALID_TRACK_ID;
    TASK_CHECK(!MP4CopyHintedTrack(src, hint, dst, &outMedia, &outHint));
    TASK_CHECK(MP4GetNumberOfTracks(dst) == 0);
    MP4Close(dst);
    MP4Close(src);

    cleanupDir(dir);
    return 0;
}

int case_unsupported_hint_structures()
{
    std::string dir = scratchDir();
    TASK_CHECK(!dir.empty());

    // two malformed hints: non-trivial block layout and a
    // sample-description entry
    for (int variant = 0; variant < 2; ++variant) {
        std::string srcPath = dir + "/src" + std::to_string(variant) + ".mp4";
        std::string dstPath = dir + "/dst" + std::to_string(variant) + ".mp4";

        MP4FileHandle f = MP4Create(srcPath.c_str());
        MP4SetTimeScale(f, 1000);
        MP4TrackId media = MP4AddVideoTrack(f, 1000, 100, 16, 16,
                                            MP4_MPEG4_VIDEO_TYPE);
        uint8_t payload[4] = { 'A', 0, 0, 0 };
        TASK_CHECK(MP4WriteSample(f, media, payload, sizeof(payload),
                                  100, 0, true));
        MP4TrackId hint = MP4AddHintTrack(f, media);
        uint8_t ptn = 96;
        TASK_CHECK(MP4SetHintTrackRtpPayload(f, hint, "MP4V-ES",
                                             &ptn, 1400));
        ByteBuf full;
        full.put16(1);
        full.put16(0);
        putPacketHeader(full, 0, true, 96, 0, 1);
        if (variant == 0) {
            putSampleEntry(full, 0, 2, 1, 0, 2, 1); // bytesPerBlock 2
        } else {
            putSampleDescriptionEntry(full, 0, 1, 0, 0);
        }
        TASK_CHECK(MP4WriteSample(f, hint, full.data(), full.size(),
                                  100, 0, true));
        MP4Close(f);

        MP4FileHandle src = MP4Read(srcPath.c_str());
        MP4FileHandle dst = MP4Create(dstPath.c_str());
        MP4TrackId outMedia = MP4_INVALID_TRACK_ID;
        MP4TrackId outHint = MP4_INVALID_TRACK_ID;
        TASK_CHECK(!MP4CopyHintedTrack(src, hint, dst,
                                       &outMedia, &outHint));
        TASK_CHECK(MP4GetNumberOfTracks(dst) == 0);
        MP4Close(dst);
        MP4Close(src);
        remove(srcPath.c_str());
        remove(dstPath.c_str());
    }

    rmdir(dir.c_str());
    return 0;
}

int case_legacy_single_track_copy()
{
    std::string dir = scratchDir();
    TASK_CHECK(!dir.empty());
    std::string srcPath = dir + "/src.mp4";
    std::string dstPath = dir + "/dst.mp4";

    MP4FileHandle f = MP4Create(srcPath.c_str());
    MP4SetTimeScale(f, 1000);
    MP4TrackId media = MP4AddVideoTrack(f, 1000, 100, 16, 16,
                                        MP4_MPEG4_VIDEO_TYPE);
    const uint8_t config[] = { 0x12, 0x34, 0x56 };
    TASK_CHECK(MP4SetTrackESConfiguration(f, media, config, sizeof(config)));
    for (uint32_t i = 0; i < 3; ++i) {
        uint8_t payload[4] = { (uint8_t)('A' + i), (uint8_t)i, 0, 0 };
        TASK_CHECK(MP4WriteSample(f, media, payload, sizeof(payload),
                                  100, 0, true));
    }
    MP4TrackId hint = MP4AddHintTrack(f, media);
    uint8_t ptn = 96;
    TASK_CHECK(MP4SetHintTrackRtpPayload(f, hint, "MP4V-ES", &ptn, 1400));
    for (uint32_t i = 0; i < 3; ++i) {
        TASK_CHECK(MP4AddRtpHint(f, hint));
        TASK_CHECK(MP4AddRtpPacket(f, hint, true, -3));
        const uint8_t imm[] = { '!' };
        TASK_CHECK(MP4AddRtpImmediateData(f, hint, imm, 1));
        TASK_CHECK(MP4AddRtpSampleData(f, hint, i + 1, 0, 1));
        TASK_CHECK(MP4WriteRtpHint(f, hint, 100, true));
    }
    MP4Close(f);

    // the legacy single-track APIs keep working unchanged
    MP4FileHandle src = MP4Read(srcPath.c_str());
    MP4FileHandle dst = MP4Create(dstPath.c_str());
    MP4TrackId dstMedia = MP4CloneTrack(src, media, dst);
    TASK_CHECK(dstMedia != MP4_INVALID_TRACK_ID);
    MP4TrackId dstHint = MP4CloneTrack(src, hint, dst, dstMedia);
    TASK_CHECK(dstHint != MP4_INVALID_TRACK_ID);
    for (MP4SampleId sid = 1; sid <= 3; ++sid) {
        TASK_CHECK(MP4CopySample(src, media, sid, dst, dstMedia));
        TASK_CHECK(MP4CopySample(src, hint, sid, dst, dstHint));
    }
    MP4Close(dst);
    MP4Close(src);

    dst = MP4Read(dstPath.c_str());
    TASK_CHECK(MP4GetTrackNumberOfSamples(dst, dstMedia) == 3);
    TASK_CHECK(MP4GetTrackNumberOfSamples(dst, dstHint) == 3);
    TASK_CHECK(MP4GetHintTrackReferenceTrackId(dst, dstHint) == dstMedia);
    uint16_t count = 0;
    TASK_CHECK(MP4ReadRtpHint(dst, dstHint, 2, &count));
    TASK_CHECK(count == 1);
    uint8_t* p = NULL;
    uint32_t n = 0;
    TASK_CHECK(MP4ReadRtpPacket(dst, dstHint, 0, &p, &n, 0, false, true));
    TASK_CHECK(n == 2 && p[0] == '!' && p[1] == 'B');
    MP4Free(p);
    MP4Close(dst);

    // MP4CopyTrack remains available as well
    std::string copyPath = dir + "/copy.mp4";
    src = MP4Read(srcPath.c_str());
    MP4FileHandle dst2 = MP4Create(copyPath.c_str());
    MP4TrackId copied = MP4CopyTrack(src, media, dst2, false);
    TASK_CHECK(copied != MP4_INVALID_TRACK_ID);
    TASK_CHECK(MP4GetTrackNumberOfSamples(dst2, copied) == 3);
    MP4Close(dst2);
    MP4Close(src);

    cleanupDir(dir);
    return 0;
}

int case_other_track_ref_index_rejected()
{
    std::string dir = scratchDir();
    TASK_CHECK(!dir.empty());
    std::string srcPath = dir + "/src.mp4";
    std::string dstPath = dir + "/dst.mp4";

    MP4FileHandle f = MP4Create(srcPath.c_str());
    MP4SetTimeScale(f, 1000);
    MP4TrackId media = MP4AddVideoTrack(f, 1000, 100, 16, 16,
                                        MP4_MPEG4_VIDEO_TYPE);
    uint8_t payload[4] = { 'A', 0, 0, 0 };
    TASK_CHECK(MP4WriteSample(f, media, payload, sizeof(payload),
                              100, 0, true));
    MP4TrackId hint = MP4AddHintTrack(f, media);
    uint8_t ptn = 96;
    TASK_CHECK(MP4SetHintTrackRtpPayload(f, hint, "MP4V-ES", &ptn, 1400));
    ByteBuf full;
    full.put16(1);
    full.put16(0);
    putPacketHeader(full, 0, true, 96, 0, 1);
    putSampleEntry(full, 7, 1, 1, 0); // unsupported trackRefIndex
    TASK_CHECK(MP4WriteSample(f, hint, full.data(), full.size(),
                              100, 0, true));
    MP4Close(f);

    MP4FileHandle src = MP4Read(srcPath.c_str());
    MP4FileHandle dst = MP4Create(dstPath.c_str());
    MP4TrackId outMedia = MP4_INVALID_TRACK_ID;
    MP4TrackId outHint = MP4_INVALID_TRACK_ID;
    TASK_CHECK(!MP4CopyHintedTrack(src, hint, dst, &outMedia, &outHint));
    TASK_CHECK(MP4GetNumberOfTracks(dst) == 0);
    MP4Close(dst);
    MP4Close(src);

    cleanupDir(dir);
    return 0;
}

// a byte-offset/length pair that runs past the referenced sample must be
// rejected before any destination track is created
int case_bad_sample_offset_rejected()
{
    std::string dir = scratchDir();
    TASK_CHECK(!dir.empty());
    std::string srcPath = dir + "/src.mp4";
    std::string dstPath = dir + "/dst.mp4";

    MP4FileHandle f = MP4Create(srcPath.c_str());
    MP4SetTimeScale(f, 1000);
    MP4TrackId media = MP4AddVideoTrack(f, 1000, 100, 16, 16,
                                        MP4_MPEG4_VIDEO_TYPE);
    uint8_t payload[4] = { 'A', 0, 0, 0 };
    TASK_CHECK(MP4WriteSample(f, media, payload, sizeof(payload),
                              100, 0, true));
    MP4TrackId hint = MP4AddHintTrack(f, media);
    uint8_t ptn = 96;
    TASK_CHECK(MP4SetHintTrackRtpPayload(f, hint, "MP4V-ES", &ptn, 1400));
    ByteBuf full;
    full.put16(1);
    full.put16(0);
    putPacketHeader(full, 0, true, 96, 0, 1);
    // media sample is only 4 bytes but the reference claims 5 at offset 3
    putSampleEntry(full, 0, 5, 1, 3);
    TASK_CHECK(MP4WriteSample(f, hint, full.data(), full.size(),
                              100, 0, true));
    MP4Close(f);

    MP4FileHandle src = MP4Read(srcPath.c_str());
    MP4FileHandle dst = MP4Create(dstPath.c_str());
    MP4TrackId outMedia = MP4_INVALID_TRACK_ID;
    MP4TrackId outHint = MP4_INVALID_TRACK_ID;
    TASK_CHECK(!MP4CopyHintedTrack(src, hint, dst, &outMedia, &outHint));
    TASK_CHECK(outMedia == MP4_INVALID_TRACK_ID);
    TASK_CHECK(outHint == MP4_INVALID_TRACK_ID);
    TASK_CHECK(MP4GetNumberOfTracks(dst) == 0);
    MP4Close(dst);
    MP4Close(src);

    cleanupDir(dir);
    return 0;
}

// a destination movie timescale that cannot represent the source edit
// durations integrally is rejected
int case_dst_timescale_indivisible_rejected()
{
    std::string dir = scratchDir();
    TASK_CHECK(!dir.empty());
    std::string srcPath = dir + "/src.mp4";
    std::string dstPath = dir + "/dst.mp4";
    TASK_CHECK(buildMainSource(srcPath));

    MP4FileHandle src = MP4Read(srcPath.c_str());
    MP4TrackId srcHint = MP4FindTrackId(src, 0, MP4_HINT_TRACK_TYPE);
    MP4FileHandle dst = MP4Create(dstPath.c_str());
    // 700 source movie units at 1000 -> 700*7/1000 = 4.9 at timescale 7
    TASK_CHECK(MP4SetTimeScale(dst, 7));
    MP4TrackId media = MP4_INVALID_TRACK_ID;
    MP4TrackId hint = MP4_INVALID_TRACK_ID;
    TASK_CHECK(!MP4CopyHintedTrack(src, srcHint, dst, &media, &hint));
    TASK_CHECK(MP4GetNumberOfTracks(dst) == 0);
    MP4Close(dst);
    MP4Close(src);

    cleanupDir(dir);
    return 0;
}

// the extra-information flag on an RTP packet is an unsupported structure
int case_rtp_extra_information_rejected()
{
    std::string dir = scratchDir();
    TASK_CHECK(!dir.empty());
    std::string srcPath = dir + "/src.mp4";
    std::string dstPath = dir + "/dst.mp4";

    MP4FileHandle f = MP4Create(srcPath.c_str());
    MP4SetTimeScale(f, 1000);
    MP4TrackId media = MP4AddVideoTrack(f, 1000, 100, 16, 16,
                                        MP4_MPEG4_VIDEO_TYPE);
    uint8_t payload[4] = { 'A', 0, 0, 0 };
    TASK_CHECK(MP4WriteSample(f, media, payload, sizeof(payload),
                              100, 0, true));
    MP4TrackId hint = MP4AddHintTrack(f, media);
    uint8_t ptn = 96;
    TASK_CHECK(MP4SetHintTrackRtpPayload(f, hint, "MP4V-ES", &ptn, 1400));

    ByteBuf full;
    full.put16(1); // one packet
    full.put16(0);
    full.put32(0);            // transmit offset
    full.put8(0);             // reserved/P/X/reserved
    full.put8(96);            // marker/payload type
    full.put16(0);            // sequence number
    full.put16(0x0004);       // extra-information flag set
    full.put16(0);            // no data entries
    full.put32(0x12345678);   // bogus extra information payload
    TASK_CHECK(MP4WriteSample(f, hint, full.data(), full.size(),
                              100, 0, true));
    MP4Close(f);

    MP4FileHandle src = MP4Read(srcPath.c_str());
    MP4FileHandle dst = MP4Create(dstPath.c_str());
    MP4TrackId outMedia = MP4_INVALID_TRACK_ID;
    MP4TrackId outHint = MP4_INVALID_TRACK_ID;
    TASK_CHECK(!MP4CopyHintedTrack(src, hint, dst, &outMedia, &outHint));
    TASK_CHECK(MP4GetNumberOfTracks(dst) == 0);
    MP4Close(dst);
    MP4Close(src);

    cleanupDir(dir);
    return 0;
}

} // namespace

// --------------------------------------------------------------------------
// case registration consumed by task_support/driver.c
// --------------------------------------------------------------------------

const struct task_case task_cases[] = {
    { "occurrence", "reordered_full",        case_reordered_full },
    { "occurrence", "no_edits_whole_track",  case_no_edits_whole_track },
    { "occurrence", "existing_dst_tracks",   case_existing_destination_tracks },
    { "occurrence", "avc1_source",           case_avc1_source },
    { "occurrence", "example_program",       case_example_program },
    { "clock",      "exact_conversion",      case_timescale_exact_conversion },
    { "clock",      "indivisible_rejected",  case_timescale_indivisible_rejected },
    { "clock",      "dst_movie_timescale",   case_dst_movie_timescale },
    { "clock",      "all_empty_rejected",    case_all_empty_rejected },
    { "reject_compat", "bad_cross_ref",      case_bad_cross_segment_reference },
    { "reject_compat", "bad_self_ref",       case_bad_self_reference },
    { "reject_compat", "bad_boundary",       case_edit_not_on_sample_boundary },
    { "reject_compat", "start_not_sync",     case_edit_start_not_sync },
    { "reject_compat", "unsupported_hints",  case_unsupported_hint_structures },
    { "reject_compat", "legacy_copy",        case_legacy_single_track_copy },
    { "reject_compat", "other_ref_index",    case_other_track_ref_index_rejected },
    { "reject_compat", "bad_sample_offset",  case_bad_sample_offset_rejected },
    { "reject_compat", "dst_scale_indiv",    case_dst_timescale_indivisible_rejected },
    { "reject_compat", "rtp_extra_info",     case_rtp_extra_information_rejected },
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);

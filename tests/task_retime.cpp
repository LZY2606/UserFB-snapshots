// Tests for MP4RescaleTrackTimeScale: endpoint-based track time scale
// conversion that keeps sample bytes and playback positions in place.
//
// Groups:
//   quantize - conversion math: absolute endpoints, run merging/splitting,
//              wide (64-bit) endpoints, unrepresentable results
//   state    - warm caches, unflushed chunks, appending, edits, close/reopen
//   reject   - unsupported layouts/content leave the track untouched
//   compat   - the legacy MP4SetTrackTimeScale field-only behavior

#include <mp4v2/mp4v2.h>

#include "task_support/check.h"

#include <cstdio>
#include <cstring>

namespace {

// floor(t * N / S), the endpoint quantization the API is specified to use.
// Test values are chosen so the intermediate product fits in 64 bits.
uint64_t Q(uint64_t t, uint32_t N, uint32_t S)
{
    return (t / S) * N + ((t % S) * N) / S;
}

void makePath(char* buf, size_t size, const char* name)
{
    std::snprintf(buf, size, ".task-build/task_retime_%s.mp4", name);
}

const char* testPath(const char* name)
{
    static char buf[256];
    makePath(buf, sizeof(buf), name);
    std::remove(buf);
    return buf;
}

bool writeSample(MP4FileHandle f, MP4TrackId t, uint8_t tag, uint32_t size,
                 MP4Duration duration, MP4Duration offset = 0,
                 bool isSync = true)
{
    uint8_t bytes[64];
    if (size > sizeof(bytes))
        return false;
    for (uint32_t i = 0; i < size; i++)
        bytes[i] = (uint8_t)(tag + i);
    return MP4WriteSample(f, t, bytes, size, duration, offset, isSync);
}

// check sample bytes and, optionally, timing against expectations
bool checkSample(MP4FileHandle f, MP4TrackId t, MP4SampleId sid, uint8_t tag,
                 uint32_t size, MP4Timestamp start, MP4Duration duration,
                 MP4Duration offset)
{
    uint8_t* bytes = NULL;
    uint32_t numBytes = 0;
    MP4Timestamp s = 0;
    MP4Duration d = 0, c = 0;
    if (!MP4ReadSample(f, t, sid, &bytes, &numBytes, &s, &d, &c))
        return false;
    bool ok = numBytes == size && s == start && d == duration && c == offset;
    for (uint32_t i = 0; ok && i < size; i++)
        ok = bytes[i] == (uint8_t)(tag + i);
    MP4Free(bytes);
    return ok;
}

bool getInt(MP4FileHandle f, const char* path, uint64_t* value)
{
    return MP4GetIntegerProperty(f, path, value);
}

///////////////////////////////////////////////////////////////////////////////
// quantize group
///////////////////////////////////////////////////////////////////////////////

// A single stts run is split into multiple runs by a 1000 -> 900 conversion;
// every endpoint is checked against Q(t) = floor(t * 900 / 1000).
int q_stts_split()
{
    const char* path = testPath("split");
    MP4FileHandle f = MP4Create(path);
    TASK_CHECK(f);
    MP4TrackId t = MP4AddAudioTrack(f, 1000, MP4_INVALID_DURATION);
    TASK_CHECK(t != MP4_INVALID_TRACK_ID);

    for (uint8_t i = 0; i < 7; i++)
        TASK_CHECK(writeSample(f, t, (uint8_t)(0x10 + i), 4, 3));

    uint64_t v;
    TASK_CHECK(getInt(f, "moov.trak[0].mdia.minf.stbl.stts.entryCount", &v) && v == 1);
    TASK_CHECK(getInt(f, "moov.trak[0].mdia.minf.stbl.stts.entries[0].sampleCount", &v) && v == 7);

    TASK_CHECK(MP4RescaleTrackTimeScale(f, t, 900));
    TASK_CHECK(MP4GetTrackTimeScale(f, t) == 900);

    // per-sample endpoints: d = 3k, u = 3
    const MP4Duration durs[7]   = { 2, 3, 3, 2, 3, 3, 2 };
    const MP4Timestamp starts[7] = { 0, 2, 5, 8, 10, 13, 16 };
    for (MP4SampleId s = 1; s <= 7; s++) {
        TASK_CHECK(Q(3 * (s - 1), 900, 1000) == starts[s - 1]);
        TASK_CHECK(Q(3 * (s - 1), 900, 1000) + durs[s - 1] == Q(3 * s, 900, 1000));
        TASK_CHECK(MP4GetSampleTime(f, t, s) == starts[s - 1]);
        TASK_CHECK(MP4GetSampleDuration(f, t, s) == durs[s - 1]);
    }
    TASK_CHECK(MP4GetTrackDuration(f, t) == Q(21, 900, 1000));

    // the single run was split into merged runs of identical deltas
    const uint32_t runCounts[5] = { 1, 2, 1, 2, 1 };
    const uint32_t runDeltas[5] = { 2, 3, 2, 3, 2 };
    TASK_CHECK(getInt(f, "moov.trak[0].mdia.minf.stbl.stts.entryCount", &v) && v == 5);
    for (uint32_t i = 0; i < 5; i++) {
        char prop[128];
        std::snprintf(prop, sizeof(prop),
                      "moov.trak[0].mdia.minf.stbl.stts.entries[%u].sampleCount", i);
        TASK_CHECK(getInt(f, prop, &v) && v == runCounts[i]);
        std::snprintf(prop, sizeof(prop),
                      "moov.trak[0].mdia.minf.stbl.stts.entries[%u].sampleDelta", i);
        TASK_CHECK(getInt(f, prop, &v) && v == runDeltas[i]);
    }
    MP4Close(f);

    // after close/reopen the same values come back (no double conversion)
    f = MP4Read(path);
    TASK_CHECK(f);
    TASK_CHECK(MP4GetTrackTimeScale(f, t) == 900);
    TASK_CHECK(MP4GetTrackDuration(f, t) == 18);
    for (MP4SampleId s = 1; s <= 7; s++) {
        TASK_CHECK(MP4GetSampleTime(f, t, s) == starts[s - 1]);
        TASK_CHECK(MP4GetSampleDuration(f, t, s) == durs[s - 1]);
        TASK_CHECK(checkSample(f, t, s, (uint8_t)(0x10 + s - 1), 4,
                               starts[s - 1], durs[s - 1], 0));
    }
    MP4Close(f);
    return 0;
}

// Two tracks with different time scales in one file; variable durations and
// non-zero composition offsets on the converted track; the other track, the
// chunk layout, sync marks and the movie/track headers must not move.
int q_endpoints_two_scales()
{
    const char* path = testPath("var");
    MP4FileHandle f = MP4Create(path);
    TASK_CHECK(f);
    MP4TrackId v = MP4AddH264VideoTrack(f, 1000, MP4_INVALID_DURATION,
                                        176, 144, 0x64, 0, 0x1f, 3);
    TASK_CHECK(v != MP4_INVALID_TRACK_ID);
    MP4TrackId a = MP4AddAudioTrack(f, 48000, 1024);
    TASK_CHECK(a != MP4_INVALID_TRACK_ID);

    TASK_CHECK(MP4SetTrackDurationPerChunk(f, v, 30));

    const MP4Duration durs[4] = { 7, 11, 13, 3 };
    const MP4Duration offs[4] = { 2, 0, 5, 1 };
    const bool syncs[4] = { true, false, false, true };
    for (uint8_t i = 0; i < 4; i++)
        TASK_CHECK(writeSample(f, v, (uint8_t)(0x20 + i), 8,
                               durs[i], offs[i], syncs[i]));
    for (uint8_t i = 0; i < 3; i++)
        TASK_CHECK(writeSample(f, a, (uint8_t)(0x40 + i), 4, 1024));

    // first video chunk (3 samples, 31 >= 30) is already flushed
    uint64_t chunkOffset, mvhdScale, mvhdDur, tkhdV, tkhdA;
    TASK_CHECK(getInt(f, "moov.trak[0].mdia.minf.stbl.stco.entryCount", &chunkOffset) && chunkOffset == 1);
    TASK_CHECK(getInt(f, "moov.trak[0].mdia.minf.stbl.stco.entries[0].chunkOffset", &chunkOffset));
    TASK_CHECK(getInt(f, "moov.mvhd.timeScale", &mvhdScale));
    TASK_CHECK(getInt(f, "moov.mvhd.duration", &mvhdDur));
    TASK_CHECK(getInt(f, "moov.trak[0].tkhd.duration", &tkhdV));
    TASK_CHECK(getInt(f, "moov.trak[1].tkhd.duration", &tkhdA));

    TASK_CHECK(MP4RescaleTrackTimeScale(f, v, 900));
    TASK_CHECK(MP4GetTrackTimeScale(f, v) == 900);

    // per-sample endpoints
    uint64_t d = 0;
    for (MP4SampleId s = 1; s <= 4; s++) {
        TASK_CHECK(MP4GetSampleTime(f, v, s) == Q(d, 900, 1000));
        TASK_CHECK(MP4GetSampleDuration(f, v, s) ==
                   Q(d + durs[s - 1], 900, 1000) - Q(d, 900, 1000));
        TASK_CHECK(MP4GetSampleRenderingOffset(f, v, s) ==
                   Q(d + offs[s - 1], 900, 1000) - Q(d, 900, 1000));
        d += durs[s - 1];
    }
    TASK_CHECK(MP4GetTrackDuration(f, v) == Q(34, 900, 1000));

    // explicit expected values
    const MP4Timestamp eStarts[4] = { 0, 6, 16, 27 };
    const MP4Duration eDurs[4] = { 6, 10, 11, 3 };
    const MP4Duration eOffs[4] = { 1, 0, 4, 1 };
    for (MP4SampleId s = 1; s <= 4; s++) {
        TASK_CHECK(MP4GetSampleTime(f, v, s) == eStarts[s - 1]);
        TASK_CHECK(MP4GetSampleDuration(f, v, s) == eDurs[s - 1]);
        TASK_CHECK(MP4GetSampleRenderingOffset(f, v, s) == eOffs[s - 1]);
    }

    // ctts runs rebuilt and merged
    uint64_t val;
    TASK_CHECK(getInt(f, "moov.trak[0].mdia.minf.stbl.ctts.entryCount", &val) && val == 4);
    for (uint32_t i = 0; i < 4; i++) {
        char prop[128];
        std::snprintf(prop, sizeof(prop),
                      "moov.trak[0].mdia.minf.stbl.ctts.entries[%u].sampleOffset", i);
        TASK_CHECK(getInt(f, prop, &val) && val == eOffs[i]);
    }

    // sync marks, chunk addresses and the flushed chunk layout untouched
    TASK_CHECK(getInt(f, "moov.trak[0].mdia.minf.stbl.stss.entries[0].sampleNumber", &val) && val == 1);
    TASK_CHECK(getInt(f, "moov.trak[0].mdia.minf.stbl.stss.entries[1].sampleNumber", &val) && val == 4);
    TASK_CHECK(getInt(f, "moov.trak[0].mdia.minf.stbl.stco.entries[0].chunkOffset", &val) && val == chunkOffset);
    TASK_CHECK(getInt(f, "moov.trak[0].mdia.minf.stbl.stco.entryCount", &val) && val == 1);

    // duration-based chunking threshold follows the new scale
    MP4Duration perChunk = 0;
    TASK_CHECK(MP4GetTrackDurationPerChunk(f, v, &perChunk) && perChunk == Q(30, 900, 1000));

    // the other track and the headers are unchanged
    TASK_CHECK(MP4GetTrackTimeScale(f, a) == 48000);
    TASK_CHECK(MP4GetTrackDuration(f, a) == 3072);
    TASK_CHECK(MP4GetSampleTime(f, a, 3) == 2048);
    TASK_CHECK(MP4GetSampleDuration(f, a, 2) == 1024);
    TASK_CHECK(getInt(f, "moov.mvhd.timeScale", &val) && val == mvhdScale);
    TASK_CHECK(getInt(f, "moov.mvhd.duration", &val) && val == mvhdDur);
    TASK_CHECK(getInt(f, "moov.trak[0].tkhd.duration", &val) && val == tkhdV);
    TASK_CHECK(getInt(f, "moov.trak[1].tkhd.duration", &val) && val == tkhdA);
    MP4Close(f);

    // everything persists through close/reopen, sample bytes included
    f = MP4Read(path);
    TASK_CHECK(f);
    for (MP4SampleId s = 1; s <= 4; s++)
        TASK_CHECK(checkSample(f, v, s, (uint8_t)(0x20 + s - 1), 8,
                               eStarts[s - 1], eDurs[s - 1], eOffs[s - 1]));
    for (MP4SampleId s = 1; s <= 3; s++)
        TASK_CHECK(checkSample(f, a, s, (uint8_t)(0x40 + s - 1), 4,
                               (s - 1) * 1024, 1024, 0));
    TASK_CHECK(getInt(f, "moov.trak[0].tkhd.duration", &val) && val == tkhdV);
    MP4Close(f);
    return 0;
}

// Upscaling 1000 -> 48000 is exact for every endpoint.
int q_scale_up_exact()
{
    const char* path = testPath("up");
    MP4FileHandle f = MP4Create(path);
    TASK_CHECK(f);
    MP4TrackId t = MP4AddAudioTrack(f, 1000, MP4_INVALID_DURATION);
    TASK_CHECK(t != MP4_INVALID_TRACK_ID);

    const MP4Duration durs[3] = { 1, 2, 3 };
    const MP4Duration offs[3] = { 0, 1, 0 };
    for (uint8_t i = 0; i < 3; i++)
        TASK_CHECK(writeSample(f, t, (uint8_t)(0x50 + i), 4, durs[i], offs[i]));

    TASK_CHECK(MP4RescaleTrackTimeScale(f, t, 48000));
    uint64_t d = 0;
    for (MP4SampleId s = 1; s <= 3; s++) {
        TASK_CHECK(MP4GetSampleTime(f, t, s) == d * 48);
        TASK_CHECK(MP4GetSampleDuration(f, t, s) == durs[s - 1] * 48);
        TASK_CHECK(MP4GetSampleRenderingOffset(f, t, s) == offs[s - 1] * 48);
        d += durs[s - 1];
    }
    TASK_CHECK(MP4GetTrackDuration(f, t) == 6 * 48);
    MP4Close(f);

    f = MP4Read(path);
    TASK_CHECK(f);
    TASK_CHECK(MP4GetSampleTime(f, t, 3) == 3 * 48);
    TASK_CHECK(MP4GetSampleDuration(f, t, 3) == 3 * 48);
    MP4Close(f);
    return 0;
}

// 64-bit endpoints: cumulative decoding times beyond 32 bits in a
// version-1 mdhd file, converted without overflow.
int q_wide_endpoints()
{
    const char* path = testPath("wide");
    MP4FileHandle f = MP4Create(path, MP4_CREATE_64BIT_TIME);
    TASK_CHECK(f);
    MP4TrackId t = MP4AddAudioTrack(f, 1000, MP4_INVALID_DURATION);
    TASK_CHECK(t != MP4_INVALID_TRACK_ID);

    const MP4Duration big = 0x40000000ULL; // 2^30 per sample, 2^32 total
    for (uint8_t i = 0; i < 4; i++)
        TASK_CHECK(writeSample(f, t, (uint8_t)(0x60 + i), 4, big));
    TASK_CHECK(MP4GetTrackDuration(f, t) == 0x100000000ULL);

    TASK_CHECK(MP4RescaleTrackTimeScale(f, t, 2000));
    for (MP4SampleId s = 1; s <= 4; s++) {
        TASK_CHECK(MP4GetSampleTime(f, t, s) == (uint64_t)(s - 1) * 0x80000000ULL);
        TASK_CHECK(MP4GetSampleDuration(f, t, s) == 0x80000000ULL);
    }
    TASK_CHECK(MP4GetTrackDuration(f, t) == 0x200000000ULL);
    MP4Close(f);

    f = MP4Read(path);
    TASK_CHECK(f);
    TASK_CHECK(MP4GetTrackTimeScale(f, t) == 2000);
    TASK_CHECK(MP4GetTrackDuration(f, t) == 0x200000000ULL);
    TASK_CHECK(MP4GetSampleTime(f, t, 4) == 3ULL * 0x80000000ULL);
    TASK_CHECK(checkSample(f, t, 4, 0x63, 4, 3ULL * 0x80000000ULL,
                           0x80000000ULL, 0));
    MP4Close(f);
    return 0;
}

// A conversion to the current time scale is the identity on valid content.
int q_same_scale_identity()
{
    const char* path = testPath("same");
    MP4FileHandle f = MP4Create(path);
    TASK_CHECK(f);
    MP4TrackId t = MP4AddAudioTrack(f, 1000, MP4_INVALID_DURATION);
    TASK_CHECK(t != MP4_INVALID_TRACK_ID);
    TASK_CHECK(writeSample(f, t, 0x70, 4, 5, 1));
    TASK_CHECK(writeSample(f, t, 0x71, 4, 10, 0));

    TASK_CHECK(MP4RescaleTrackTimeScale(f, t, 1000));
    TASK_CHECK(MP4GetTrackTimeScale(f, t) == 1000);
    TASK_CHECK(MP4GetSampleTime(f, t, 2) == 5);
    TASK_CHECK(MP4GetSampleDuration(f, t, 1) == 5);
    TASK_CHECK(MP4GetSampleDuration(f, t, 2) == 10);
    TASK_CHECK(MP4GetSampleRenderingOffset(f, t, 1) == 1);
    TASK_CHECK(MP4GetTrackDuration(f, t) == 15);
    MP4Close(f);
    return 0;
}

///////////////////////////////////////////////////////////////////////////////
// state group
///////////////////////////////////////////////////////////////////////////////

// Warmed per-sample and time-to-sample queries must use the new tables
// immediately; appending after the conversion uses the new scale; the
// result survives close/reopen exactly once.
int s_warm_cache_append()
{
    const char* path = testPath("warm");
    MP4FileHandle f = MP4Create(path);
    TASK_CHECK(f);
    MP4TrackId t = MP4AddAudioTrack(f, 1000, MP4_INVALID_DURATION);
    TASK_CHECK(t != MP4_INVALID_TRACK_ID);

    const MP4Duration durs[3] = { 10, 20, 30 };
    for (uint8_t i = 0; i < 3; i++)
        TASK_CHECK(writeSample(f, t, (uint8_t)(0x80 + i), 4, durs[i]));

    // warm the stts caches and the time-to-sample path
    TASK_CHECK(MP4GetSampleTime(f, t, 3) == 30);
    TASK_CHECK(MP4GetSampleDuration(f, t, 2) == 20);
    TASK_CHECK(MP4GetSampleIdFromTime(f, t, 25) == 2);

    TASK_CHECK(MP4RescaleTrackTimeScale(f, t, 900));

    // queries immediately reflect the new tables
    TASK_CHECK(MP4GetSampleTime(f, t, 3) == 27);
    TASK_CHECK(MP4GetSampleTime(f, t, 2) == 9);
    TASK_CHECK(MP4GetSampleDuration(f, t, 2) == 18);
    TASK_CHECK(MP4GetSampleDuration(f, t, 3) == 27);
    TASK_CHECK(MP4GetSampleIdFromTime(f, t, 9) == 2);
    TASK_CHECK(MP4GetSampleIdFromTime(f, t, 27) == 3);

    // appending continues in the new scale, explicit duration and offset
    TASK_CHECK(writeSample(f, t, 0x83, 4, 9, 2));
    TASK_CHECK(writeSample(f, t, 0x84, 4, 9));
    TASK_CHECK(MP4GetSampleTime(f, t, 4) == 54);
    TASK_CHECK(MP4GetSampleRenderingOffset(f, t, 4) == 2);
    TASK_CHECK(MP4GetTrackDuration(f, t) == 72);
    MP4Close(f);

    f = MP4Read(path);
    TASK_CHECK(f);
    const MP4Timestamp starts[5] = { 0, 9, 27, 54, 63 };
    const MP4Duration ndurs[5] = { 9, 18, 27, 9, 9 };
    for (MP4SampleId s = 1; s <= 5; s++)
        TASK_CHECK(checkSample(f, t, s, (uint8_t)(0x80 + s - 1), 4,
                               starts[s - 1], ndurs[s - 1], s == 4 ? 2 : 0));
    TASK_CHECK(MP4GetTrackDuration(f, t) == 72);
    MP4Close(f);
    return 0;
}

// A plain self-contained file opened with MP4Modify can be rescaled in
// place; the new tables are persisted once.
int s_modify_mode()
{
    const char* path = testPath("modify");
    MP4FileHandle f = MP4Create(path);
    TASK_CHECK(f);
    MP4TrackId v = MP4AddH264VideoTrack(f, 1000, MP4_INVALID_DURATION,
                                        176, 144, 0x64, 0, 0x1f, 3);
    TASK_CHECK(v != MP4_INVALID_TRACK_ID);
    const MP4Duration durs[3] = { 33, 33, 34 };
    const MP4Duration offs[3] = { 3, 0, 1 };
    for (uint8_t i = 0; i < 3; i++)
        TASK_CHECK(writeSample(f, v, (uint8_t)(0x90 + i), 8, durs[i], offs[i]));
    MP4Close(f);

    f = MP4Modify(path);
    TASK_CHECK(f);
    TASK_CHECK(MP4GetTrackTimeScale(f, v) == 1000);
    TASK_CHECK(MP4RescaleTrackTimeScale(f, v, 900));
    TASK_CHECK(MP4GetTrackTimeScale(f, v) == 900);
    uint64_t d = 0;
    for (MP4SampleId s = 1; s <= 3; s++) {
        TASK_CHECK(checkSample(f, v, s, (uint8_t)(0x90 + s - 1), 8,
                               Q(d, 900, 1000),
                               Q(d + durs[s - 1], 900, 1000) - Q(d, 900, 1000),
                               Q(d + offs[s - 1], 900, 1000) - Q(d, 900, 1000)));
        d += durs[s - 1];
    }
    TASK_CHECK(MP4GetTrackDuration(f, v) == Q(100, 900, 1000));
    MP4Close(f);

    f = MP4Read(path);
    TASK_CHECK(f);
    TASK_CHECK(MP4GetTrackTimeScale(f, v) == 900);
    TASK_CHECK(MP4GetTrackDuration(f, v) == 90);
    TASK_CHECK(MP4GetSampleTime(f, v, 3) == Q(66, 900, 1000));
    TASK_CHECK(MP4GetSampleDuration(f, v, 3) == Q(100, 900, 1000) - Q(66, 900, 1000));
    TASK_CHECK(MP4GetSampleRenderingOffset(f, v, 1) == Q(3, 900, 1000));
    MP4Close(f);
    return 0;
}

// Samples still sitting in the in-memory chunk buffer keep working with the
// new scale: the duration-based chunking threshold becomes max(1, Q(old)).
int s_unflushed_chunk()
{
    const char* path = testPath("chunk");
    MP4FileHandle f = MP4Create(path);
    TASK_CHECK(f);
    MP4TrackId t = MP4AddAudioTrack(f, 1000, MP4_INVALID_DURATION);
    TASK_CHECK(t != MP4_INVALID_TRACK_ID);
    TASK_CHECK(MP4SetTrackDurationPerChunk(f, t, 1000));

    // two samples stay in the chunk buffer (200 < 1000)
    TASK_CHECK(writeSample(f, t, 0xA0, 4, 100));
    TASK_CHECK(writeSample(f, t, 0xA1, 4, 100));
    uint64_t val;
    TASK_CHECK(getInt(f, "moov.trak[0].mdia.minf.stbl.stco.entryCount", &val) && val == 0);

    TASK_CHECK(MP4RescaleTrackTimeScale(f, t, 900));

    // threshold converted to the new scale
    MP4Duration perChunk = 0;
    TASK_CHECK(MP4GetTrackDurationPerChunk(f, t, &perChunk) && perChunk == 900);

    // buffered 180 (2 x 90 converted) + 8 x 100 (new scale) = 900 fills
    // the first chunk at sample 10
    for (uint8_t i = 2; i < 12; i++)
        TASK_CHECK(writeSample(f, t, (uint8_t)(0xA0 + i), 4, 100));
    TASK_CHECK(MP4GetTrackDuration(f, t) == 1180);
    MP4Close(f);

    f = MP4Read(path);
    TASK_CHECK(f);
    TASK_CHECK(getInt(f, "moov.trak[0].mdia.minf.stbl.stco.entryCount", &val) && val == 2);
    TASK_CHECK(getInt(f, "moov.trak[0].mdia.minf.stbl.stsc.entries[0].samplesPerChunk", &val) && val == 10);
    TASK_CHECK(getInt(f, "moov.trak[0].mdia.minf.stbl.stsc.entries[1].samplesPerChunk", &val) && val == 2);
    for (MP4SampleId s = 1; s <= 12; s++) {
        // samples 1-2 were converted to 90; later samples were written
        // with explicit durations in the new scale
        const MP4Timestamp start = s <= 2 ? (s - 1) * 90 : 180 + (s - 3) * 100;
        const MP4Duration dur = s <= 2 ? 90 : 100;
        TASK_CHECK(checkSample(f, t, s, (uint8_t)(0xA0 + s - 1), 4, start, dur, 0));
    }
    MP4Close(f);
    return 0;
}

// Empty (media_time == -1) edits are preserved, rate=1 media segments get
// converted media times, segment durations stay put, and edit-time lookups
// use the new tables right away.
int s_edits()
{
    const char* path = testPath("edits");
    MP4FileHandle f = MP4Create(path);
    TASK_CHECK(f);
    MP4TrackId t = MP4AddAudioTrack(f, 1000, MP4_INVALID_DURATION);
    TASK_CHECK(t != MP4_INVALID_TRACK_ID);
    for (uint8_t i = 0; i < 10; i++)
        TASK_CHECK(writeSample(f, t, (uint8_t)(0xB0 + i), 4, 100));

    TASK_CHECK(MP4AddTrackEdit(f, t, MP4_INVALID_EDIT_ID, 0, 400, false) == 1);
    TASK_CHECK(MP4AddTrackEdit(f, t, MP4_INVALID_EDIT_ID, 0, 200, false) == 2);
    TASK_CHECK(MP4SetTrackEditMediaStart(f, t, 2, (MP4Timestamp)-1)); // empty edit
    TASK_CHECK(MP4AddTrackEdit(f, t, MP4_INVALID_EDIT_ID, 500, 500, false) == 3);

    TASK_CHECK(MP4GetTrackNumberOfEdits(f, t) == 3);
    TASK_CHECK(MP4GetTrackEditMediaStart(f, t, 3) == 500);
    TASK_CHECK(MP4GetSampleIdFromEditTime(f, t, 650) == 6);

    TASK_CHECK(MP4RescaleTrackTimeScale(f, t, 900));

    TASK_CHECK(MP4GetTrackEditMediaStart(f, t, 1) == 0);
    TASK_CHECK(MP4GetTrackEditMediaStart(f, t, 2) == 0xFFFFFFFFULL); // still empty
    TASK_CHECK(MP4GetTrackEditMediaStart(f, t, 3) == 450);
    // segment durations are not converted
    TASK_CHECK(MP4GetTrackEditDuration(f, t, 1) == 400);
    TASK_CHECK(MP4GetTrackEditDuration(f, t, 2) == 200);
    TASK_CHECK(MP4GetTrackEditDuration(f, t, 3) == 500);
    // edit-time lookup uses the new media times and sample tables
    TASK_CHECK(MP4GetSampleIdFromEditTime(f, t, 650) == 6);
    MP4Close(f);

    f = MP4Read(path);
    TASK_CHECK(f);
    TASK_CHECK(MP4GetTrackEditMediaStart(f, t, 2) == 0xFFFFFFFFULL);
    TASK_CHECK(MP4GetTrackEditMediaStart(f, t, 3) == 450);
    TASK_CHECK(MP4GetSampleIdFromEditTime(f, t, 650) == 6);
    MP4Close(f);
    return 0;
}

// The duration-based chunking threshold never drops below one tick:
// max(1, Q(old threshold)).
int s_chunk_threshold_floor()
{
    const char* path = testPath("floor");
    MP4FileHandle f = MP4Create(path);
    TASK_CHECK(f);
    MP4TrackId t = MP4AddAudioTrack(f, 1000, MP4_INVALID_DURATION);
    TASK_CHECK(t != MP4_INVALID_TRACK_ID);
    TASK_CHECK(MP4SetTrackDurationPerChunk(f, t, 1));

    TASK_CHECK(MP4RescaleTrackTimeScale(f, t, 900)); // Q(1) == 0
    MP4Duration perChunk = 0;
    TASK_CHECK(MP4GetTrackDurationPerChunk(f, t, &perChunk) && perChunk == 1);

    // chunking still works: every sample fills its own chunk immediately
    TASK_CHECK(writeSample(f, t, 0xD0, 4, 5));
    TASK_CHECK(writeSample(f, t, 0xD1, 4, 5));
    MP4Close(f);

    f = MP4Read(path);
    TASK_CHECK(f);
    uint64_t val;
    TASK_CHECK(getInt(f, "moov.trak[0].mdia.minf.stbl.stco.entryCount", &val) && val == 2);
    TASK_CHECK(checkSample(f, t, 1, 0xD0, 4, 0, 5, 0));
    TASK_CHECK(checkSample(f, t, 2, 0xD1, 4, 5, 5, 0));
    MP4Close(f);
    return 0;
}

// A fixed sample duration that no written sample has consumed yet is
// converted along with the scale.
int s_fixed_duration_pending()
{
    const char* path = testPath("fixed");
    MP4FileHandle f = MP4Create(path);
    TASK_CHECK(f);
    MP4TrackId t = MP4AddAudioTrack(f, 1000, 40);
    TASK_CHECK(t != MP4_INVALID_TRACK_ID);
    TASK_CHECK(MP4GetTrackFixedSampleDuration(f, t) == 40);

    TASK_CHECK(MP4RescaleTrackTimeScale(f, t, 900));
    TASK_CHECK(MP4GetTrackFixedSampleDuration(f, t) == 36);

    TASK_CHECK(writeSample(f, t, 0xC0, 4, MP4_INVALID_DURATION));
    TASK_CHECK(MP4GetSampleDuration(f, t, 1) == 36);
    TASK_CHECK(MP4GetTrackDuration(f, t) == 36);
    MP4Close(f);

    f = MP4Read(path);
    TASK_CHECK(f);
    TASK_CHECK(MP4GetSampleDuration(f, t, 1) == 36);
    MP4Close(f);
    return 0;
}

///////////////////////////////////////////////////////////////////////////////
// reject group
///////////////////////////////////////////////////////////////////////////////

// A zero new scale is rejected; the track is unchanged and writing can
// continue.
int r_zero_scale()
{
    const char* path = testPath("rzero");
    MP4FileHandle f = MP4Create(path);
    TASK_CHECK(f);
    MP4TrackId t = MP4AddAudioTrack(f, 1000, MP4_INVALID_DURATION);
    TASK_CHECK(t != MP4_INVALID_TRACK_ID);
    TASK_CHECK(writeSample(f, t, 0x01, 4, 10));
    TASK_CHECK(writeSample(f, t, 0x02, 4, 10));

    TASK_CHECK(!MP4RescaleTrackTimeScale(f, t, 0));
    TASK_CHECK(!MP4RescaleTrackTimeScale(f, MP4_INVALID_TRACK_ID, 900));
    TASK_CHECK(MP4GetTrackTimeScale(f, t) == 1000);
    TASK_CHECK(MP4GetSampleDuration(f, t, 1) == 10);
    TASK_CHECK(MP4GetTrackDuration(f, t) == 20);

    TASK_CHECK(writeSample(f, t, 0x03, 4, 10));
    TASK_CHECK(MP4GetTrackDuration(f, t) == 30);
    MP4Close(f);
    return 0;
}

// Zero-duration samples are rejected; the pending write buffer is not
// flushed by the failed call and writing can continue.
int r_zero_duration_sample()
{
    const char* path = testPath("rzerodur");
    MP4FileHandle f = MP4Create(path);
    TASK_CHECK(f);
    MP4TrackId t = MP4AddAudioTrack(f, 1000, MP4_INVALID_DURATION);
    TASK_CHECK(t != MP4_INVALID_TRACK_ID);
    TASK_CHECK(writeSample(f, t, 0x11, 4, 0));
    TASK_CHECK(writeSample(f, t, 0x12, 4, 10));

    uint64_t val;
    TASK_CHECK(getInt(f, "moov.trak[0].mdia.minf.stbl.stco.entryCount", &val) && val == 0);

    TASK_CHECK(!MP4RescaleTrackTimeScale(f, t, 900));
    TASK_CHECK(MP4GetTrackTimeScale(f, t) == 1000);
    TASK_CHECK(MP4GetSampleDuration(f, t, 1) == 0);
    TASK_CHECK(MP4GetSampleDuration(f, t, 2) == 10);
    // the chunk buffer was not drained by the rejected call
    TASK_CHECK(getInt(f, "moov.trak[0].mdia.minf.stbl.stco.entryCount", &val) && val == 0);

    // writing continues after the rejection
    TASK_CHECK(writeSample(f, t, 0x13, 4, 10));
    TASK_CHECK(MP4GetTrackDuration(f, t) == 20);
    MP4Close(f);

    f = MP4Read(path);
    TASK_CHECK(f);
    TASK_CHECK(checkSample(f, t, 1, 0x11, 4, 0, 0, 0));
    TASK_CHECK(checkSample(f, t, 3, 0x13, 4, 10, 10, 0));
    MP4Close(f);
    return 0;
}

// A sample whose converted duration would become zero is rejected.
int r_zero_result_duration()
{
    const char* path = testPath("rzerores");
    MP4FileHandle f = MP4Create(path);
    TASK_CHECK(f);
    MP4TrackId t = MP4AddAudioTrack(f, 1000, MP4_INVALID_DURATION);
    TASK_CHECK(t != MP4_INVALID_TRACK_ID);
    TASK_CHECK(writeSample(f, t, 0x21, 4, 1));

    TASK_CHECK(!MP4RescaleTrackTimeScale(f, t, 900)); // Q(1) == 0
    TASK_CHECK(MP4GetTrackTimeScale(f, t) == 1000);
    TASK_CHECK(MP4GetSampleDuration(f, t, 1) == 1);
    TASK_CHECK(MP4GetTrackDuration(f, t) == 1);

    TASK_CHECK(writeSample(f, t, 0x22, 4, 5));
    TASK_CHECK(MP4GetTrackDuration(f, t) == 6);
    MP4Close(f);
    return 0;
}

// Results that do not fit the original field width are rejected (32-bit
// mdhd/stts here); the track keeps its old values and accepts more writes.
int r_unrepresentable_result()
{
    const char* path = testPath("rwide");
    MP4FileHandle f = MP4Create(path); // 32-bit mdhd
    TASK_CHECK(f);
    MP4TrackId t = MP4AddAudioTrack(f, 1000, MP4_INVALID_DURATION);
    TASK_CHECK(t != MP4_INVALID_TRACK_ID);
    TASK_CHECK(writeSample(f, t, 0x31, 4, 0x80000000ULL));
    TASK_CHECK(MP4GetTrackDuration(f, t) == 0x80000000ULL);

    TASK_CHECK(!MP4RescaleTrackTimeScale(f, t, 2000)); // would need 2^32
    TASK_CHECK(MP4GetTrackTimeScale(f, t) == 1000);
    TASK_CHECK(MP4GetTrackDuration(f, t) == 0x80000000ULL);
    TASK_CHECK(MP4GetSampleDuration(f, t, 1) == 0x80000000ULL);

    TASK_CHECK(writeSample(f, t, 0x32, 4, 10));
    MP4Close(f);
    return 0;
}

// A corrupt time table (stts not covering the sample count) is rejected;
// after repairing the table the same conversion succeeds.
int r_bad_table()
{
    const char* path = testPath("rbadtab");
    MP4FileHandle f = MP4Create(path);
    TASK_CHECK(f);
    MP4TrackId t = MP4AddAudioTrack(f, 1000, MP4_INVALID_DURATION);
    TASK_CHECK(t != MP4_INVALID_TRACK_ID);
    for (uint8_t i = 0; i < 3; i++)
        TASK_CHECK(writeSample(f, t, (uint8_t)(0x41 + i), 4, 10));

    const char* countProp =
        "moov.trak[0].mdia.minf.stbl.stts.entries[0].sampleCount";
    TASK_CHECK(MP4SetIntegerProperty(f, countProp, 2)); // desync from stsz
    TASK_CHECK(!MP4RescaleTrackTimeScale(f, t, 900));
    TASK_CHECK(MP4GetTrackTimeScale(f, t) == 1000);

    TASK_CHECK(MP4SetIntegerProperty(f, countProp, 3)); // repair
    TASK_CHECK(MP4RescaleTrackTimeScale(f, t, 900));
    TASK_CHECK(MP4GetSampleDuration(f, t, 1) == 9);
    TASK_CHECK(MP4GetTrackDuration(f, t) == 27);
    MP4Close(f);
    return 0;
}

// A non-unit edit rate on a media segment is out of scope.
int r_non_unit_edit_rate()
{
    const char* path = testPath("rrate");
    MP4FileHandle f = MP4Create(path);
    TASK_CHECK(f);
    MP4TrackId t = MP4AddAudioTrack(f, 1000, MP4_INVALID_DURATION);
    TASK_CHECK(t != MP4_INVALID_TRACK_ID);
    for (uint8_t i = 0; i < 5; i++)
        TASK_CHECK(writeSample(f, t, (uint8_t)(0x51 + i), 4, 100));

    // dwell edit: media_time 0 with rate 0
    TASK_CHECK(MP4AddTrackEdit(f, t, MP4_INVALID_EDIT_ID, 0, 200, true) == 1);
    TASK_CHECK(MP4GetTrackEditDwell(f, t, 1));

    TASK_CHECK(!MP4RescaleTrackTimeScale(f, t, 900));
    TASK_CHECK(MP4GetTrackTimeScale(f, t) == 1000);
    TASK_CHECK(MP4GetSampleDuration(f, t, 5) == 100);
    MP4Close(f);
    return 0;
}

// Unsupported track layouts (non-avc1/mp4a sample description) and
// read-only handles are rejected.
int r_unsupported_layout()
{
    const char* path = testPath("rlayout");
    MP4FileHandle f = MP4Create(path);
    TASK_CHECK(f);
    MP4TrackId v = MP4AddVideoTrack(f, 1000, MP4_INVALID_DURATION, 176, 144);
    TASK_CHECK(v != MP4_INVALID_TRACK_ID);
    TASK_CHECK(writeSample(f, v, 0x61, 8, 100));
    MP4TrackId a = MP4AddAudioTrack(f, 1000, MP4_INVALID_DURATION);
    TASK_CHECK(a != MP4_INVALID_TRACK_ID);
    TASK_CHECK(writeSample(f, a, 0x62, 4, 100));

    // mp4v sample description is not supported
    TASK_CHECK(!MP4RescaleTrackTimeScale(f, v, 900));
    TASK_CHECK(MP4GetTrackTimeScale(f, v) == 1000);
    // the mp4a track in the same file is fine
    TASK_CHECK(MP4RescaleTrackTimeScale(f, a, 900));
    MP4Close(f);

    // a read-only handle cannot be rescaled
    f = MP4Read(path);
    TASK_CHECK(f);
    TASK_CHECK(!MP4RescaleTrackTimeScale(f, a, 1000));
    TASK_CHECK(MP4GetTrackTimeScale(f, a) == 900);
    MP4Close(f);
    return 0;
}

// Content is validated even when the requested scale equals the current
// one: a zero-duration sample is rejected, a healthy track converts.
int r_same_scale_validates()
{
    const char* path = testPath("rsame");
    MP4FileHandle f = MP4Create(path);
    TASK_CHECK(f);
    MP4TrackId bad = MP4AddAudioTrack(f, 1000, MP4_INVALID_DURATION);
    TASK_CHECK(bad != MP4_INVALID_TRACK_ID);
    TASK_CHECK(writeSample(f, bad, 0x71, 4, 0));
    MP4TrackId good = MP4AddAudioTrack(f, 1000, MP4_INVALID_DURATION);
    TASK_CHECK(good != MP4_INVALID_TRACK_ID);
    TASK_CHECK(writeSample(f, good, 0x72, 4, 10));

    TASK_CHECK(!MP4RescaleTrackTimeScale(f, bad, 1000));
    TASK_CHECK(MP4RescaleTrackTimeScale(f, good, 1000));
    TASK_CHECK(MP4GetSampleDuration(f, good, 1) == 10);
    MP4Close(f);
    return 0;
}

///////////////////////////////////////////////////////////////////////////////
// compat group
///////////////////////////////////////////////////////////////////////////////

// The legacy setter keeps changing only the time scale field; tick values,
// sample bytes and offsets are preserved as before.
int c_legacy_setter()
{
    const char* path = testPath("clegacy");
    MP4FileHandle f = MP4Create(path);
    TASK_CHECK(f);
    MP4TrackId t = MP4AddAudioTrack(f, 1000, MP4_INVALID_DURATION);
    TASK_CHECK(t != MP4_INVALID_TRACK_ID);
    TASK_CHECK(MP4SetTrackDurationPerChunk(f, t, 1000));
    for (uint8_t i = 0; i < 3; i++)
        TASK_CHECK(writeSample(f, t, (uint8_t)(0x81 + i), 4, 11, 3));

    TASK_CHECK(MP4GetTrackDuration(f, t) == 33);
    TASK_CHECK(MP4GetSampleTime(f, t, 3) == 22);
    TASK_CHECK(MP4SetTrackTimeScale(f, t, 900));
    TASK_CHECK(MP4GetTrackTimeScale(f, t) == 900);
    // tick values are not converted by the legacy setter
    TASK_CHECK(MP4GetSampleDuration(f, t, 2) == 11);
    TASK_CHECK(MP4GetSampleRenderingOffset(f, t, 2) == 3);
    TASK_CHECK(MP4GetSampleTime(f, t, 3) == 22);
    TASK_CHECK(MP4GetTrackDuration(f, t) == 33);
    MP4Close(f);

    f = MP4Read(path);
    TASK_CHECK(f);
    TASK_CHECK(MP4GetTrackDuration(f, t) == 33);
    for (MP4SampleId s = 1; s <= 3; s++)
        TASK_CHECK(checkSample(f, t, s, (uint8_t)(0x81 + s - 1), 4,
                               (s - 1) * 11, 11, 3));
    MP4Close(f);
    return 0;
}

// The legacy setter and the rescaling call compose: after a field-only
// change the rescale treats the current field value as the old scale.
int c_legacy_then_rescale()
{
    const char* path = testPath("ccompose");
    MP4FileHandle f = MP4Create(path);
    TASK_CHECK(f);
    MP4TrackId t = MP4AddAudioTrack(f, 1000, MP4_INVALID_DURATION);
    TASK_CHECK(t != MP4_INVALID_TRACK_ID);
    TASK_CHECK(writeSample(f, t, 0x91, 4, 10, 1));
    TASK_CHECK(writeSample(f, t, 0x92, 4, 10, 1));

    TASK_CHECK(MP4SetTrackTimeScale(f, t, 500)); // field only, ticks kept
    TASK_CHECK(MP4GetSampleDuration(f, t, 1) == 10);

    TASK_CHECK(MP4RescaleTrackTimeScale(f, t, 1000)); // 500 -> 1000 doubles
    TASK_CHECK(MP4GetTrackTimeScale(f, t) == 1000);
    TASK_CHECK(MP4GetSampleDuration(f, t, 1) == 20);
    TASK_CHECK(MP4GetSampleTime(f, t, 2) == 20);
    TASK_CHECK(MP4GetSampleRenderingOffset(f, t, 2) == 2);
    TASK_CHECK(MP4GetTrackDuration(f, t) == 40);
    MP4Close(f);

    f = MP4Read(path);
    TASK_CHECK(f);
    TASK_CHECK(checkSample(f, t, 1, 0x91, 4, 0, 20, 2));
    TASK_CHECK(checkSample(f, t, 2, 0x92, 4, 20, 20, 2));
    MP4Close(f);
    return 0;
}

// The legacy setter still rejects a zero scale and leaves the field alone.
int c_legacy_zero_rejected()
{
    const char* path = testPath("czero");
    MP4FileHandle f = MP4Create(path);
    TASK_CHECK(f);
    MP4TrackId t = MP4AddAudioTrack(f, 1000, MP4_INVALID_DURATION);
    TASK_CHECK(t != MP4_INVALID_TRACK_ID);
    TASK_CHECK(writeSample(f, t, 0xA1, 4, 10));

    TASK_CHECK(!MP4SetTrackTimeScale(f, t, 0));
    TASK_CHECK(MP4GetTrackTimeScale(f, t) == 1000);
    TASK_CHECK(MP4GetSampleDuration(f, t, 1) == 10);
    MP4Close(f);
    return 0;
}

} // anonymous namespace

const struct task_case task_cases[] = {
    { "quantize", "stts_split_1000_to_900", q_stts_split },
    { "quantize", "endpoints_two_scales",   q_endpoints_two_scales },
    { "quantize", "scale_up_exact",         q_scale_up_exact },
    { "quantize", "wide_endpoints",         q_wide_endpoints },
    { "quantize", "same_scale_identity",    q_same_scale_identity },
    { "state",    "warm_cache_append",      s_warm_cache_append },
    { "state",    "modify_mode",            s_modify_mode },
    { "state",    "unflushed_chunk",        s_unflushed_chunk },
    { "state",    "edits",                  s_edits },
    { "state",    "fixed_duration_pending", s_fixed_duration_pending },
    { "state",    "chunk_threshold_floor",  s_chunk_threshold_floor },
    { "reject",   "zero_scale",             r_zero_scale },
    { "reject",   "zero_duration_sample",   r_zero_duration_sample },
    { "reject",   "zero_result_duration",   r_zero_result_duration },
    { "reject",   "unrepresentable_result", r_unrepresentable_result },
    { "reject",   "bad_table",              r_bad_table },
    { "reject",   "non_unit_edit_rate",     r_non_unit_edit_rate },
    { "reject",   "unsupported_layout",     r_unsupported_layout },
    { "reject",   "same_scale_validates",   r_same_scale_validates },
    { "compat",   "legacy_setter",          c_legacy_setter },
    { "compat",   "legacy_then_rescale",    c_legacy_then_rescale },
    { "compat",   "legacy_zero_rejected",   c_legacy_zero_rejected },
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);

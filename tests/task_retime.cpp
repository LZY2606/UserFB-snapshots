/* Task tests for MP4RescaleTrackTimeScale.
 *
 * Registers four groups through task_support:
 *   quantize - endpoint math, run rebuilding, edits, 64 bit endpoints
 *   state    - warm caches, unflushed chunk, append, reopen, MP4Modify
 *   reject   - unsupported / invalid arguments and content
 *   compat   - MP4SetTrackTimeScale legacy field-only behaviour
 */

#include <mp4v2/mp4v2.h>

#include "task_support/check.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using std::string;
using std::vector;

/* ------------------------------------------------------------------ */
/* small helpers                                                      */
/* ------------------------------------------------------------------ */

/* reference Q(t) = floor(t * N / S) with a safe 128 bit intermediate */
static uint64_t refQ(uint64_t t, uint32_t N, uint32_t S)
{
    return (uint64_t)(
        ((unsigned __int128)t * (unsigned __int128)N)
        / (unsigned __int128)S);
}

static uint64_t iprop(MP4FileHandle h, const char* path)
{
    uint64_t value = 0;
    MP4GetIntegerProperty(h, path, &value);
    return value;
}

static string trakPath(unsigned trakIndex, const char* tail)
{
    char buf[256];
    std::snprintf(buf, sizeof(buf), "moov.trak[%u].%s", trakIndex, tail);
    return string(buf);
}

static uint64_t trakProp(MP4FileHandle h, unsigned idx, const char* tail)
{
    return iprop(h, trakPath(idx, tail).c_str());
}

struct Run {
    uint32_t count;
    uint32_t value;
};

static vector<Run> mergeRuns(const vector<uint32_t>& values)
{
    vector<Run> runs;
    for (size_t i = 0; i < values.size(); ++i) {
        if (!runs.empty() && runs.back().value == values[i]) {
            runs.back().count++;
        } else {
            Run r;
            r.count = 1;
            r.value = values[i];
            runs.push_back(r);
        }
    }
    return runs;
}

static bool checkRuns(
    MP4FileHandle h, unsigned trakIndex, const char* table,
    const vector<uint32_t>& values)
{
    vector<Run> expected = mergeRuns(values);

    string countPath = string("mdia.minf.stbl.") + table + ".entryCount";
    if (trakProp(h, trakIndex, countPath.c_str()) != expected.size()) {
        std::printf("run check: %s entryCount %llu != expected %zu\n",
                    table,
                    (unsigned long long)trakProp(
                        h, trakIndex, countPath.c_str()),
                    expected.size());
        return false;
    }

    for (size_t i = 0; i < expected.size(); ++i) {
        char tail[160];
        std::snprintf(tail, sizeof(tail),
                      "mdia.minf.stbl.%s.entries[%zu].sampleCount",
                      table, i);
        if (trakProp(h, trakIndex, tail) != expected[i].count) {
            std::printf("run check: %s run %zu count mismatch\n",
                        table, i);
            return false;
        }
        const char* valueField =
            (std::strcmp(table, "stts") == 0)
                ? "sampleDelta" : "sampleOffset";
        std::snprintf(tail, sizeof(tail),
                      "mdia.minf.stbl.%s.entries[%zu].%s",
                      table, i, valueField);
        if (trakProp(h, trakIndex, tail) != expected[i].value) {
            std::printf("run check: %s run %zu value mismatch "
                         "got %llu want %u\n",
                        table, i,
                        (unsigned long long)trakProp(h, trakIndex, tail),
                        expected[i].value);
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* shared rich fixture                                                */
/* ------------------------------------------------------------------ */

static const unsigned RICH_N = 7;
static const uint32_t RICH_U[RICH_N] = { 11, 11, 11, 11, 11, 20, 13 };
static const uint32_t RICH_C[RICH_N] = { 3, 3, 7, 7, 7, 0, 12 };

static uint8_t richByte(unsigned i, unsigned j)
{
    return (uint8_t)(0x40 + i * 7 + j);
}

/* track 1 (trak[0]): scale 1000, variable durations/offsets, edits
 * track 2 (trak[1]): scale 8000, untouched reference track */
static void buildRichFixture(const char* path)
{
    MP4FileHandle h = MP4Create(path);

    MP4TrackId a = MP4AddAudioTrack(h, 1000, MP4_INVALID_DURATION);
    MP4SetTrackDurationPerChunk(h, a, 1000000);
    for (unsigned i = 0; i < RICH_N; ++i) {
        uint8_t bytes[4];
        for (unsigned j = 0; j < 4; ++j)
            bytes[j] = richByte(i, j);
        MP4WriteSample(h, a, bytes, 4, RICH_U[i], RICH_C[i], true);
    }
    /* empty edit (media_time = -1) then a normal media segment */
    MP4AddTrackEdit(h, a, MP4_INVALID_EDIT_ID,
                    MP4_INVALID_TIMESTAMP, 500, false);
    MP4AddTrackEdit(h, a, MP4_INVALID_EDIT_ID,
                    100, 2000, false);

    MP4TrackId b = MP4AddAudioTrack(h, 8000, MP4_INVALID_DURATION);
    static const uint32_t BU[3] = { 200, 400, 200 };
    for (unsigned i = 0; i < 3; ++i) {
        uint8_t bytes[3] = { (uint8_t)(0x90 + i), 2, 3 };
        MP4WriteSample(h, b, bytes, 3, BU[i], 0, true);
    }

    MP4Close(h);
}

/* verify track a against the endpoint formula at scale N */
static bool verifyRichA(MP4FileHandle h, MP4TrackId t, uint32_t N)
{
    uint64_t d[RICH_N + 1];
    d[0] = 0;
    for (unsigned i = 0; i < RICH_N; ++i)
        d[i + 1] = d[i] + RICH_U[i];

    vector<uint32_t> expDur;
    vector<uint32_t> expOff;

    for (unsigned i = 0; i < RICH_N; ++i) {
        uint64_t expTime = refQ(d[i], N, 1000);
        uint64_t expDur64 = refQ(d[i + 1], N, 1000) - expTime;
        uint64_t expOff64 = refQ(d[i] + RICH_C[i], N, 1000) - expTime;

        if (MP4GetSampleTime(h, t, i + 1) != expTime) {
            std::printf("rich: sample %u time mismatch\n", i + 1);
            return false;
        }
        if (MP4GetSampleDuration(h, t, i + 1) != expDur64) {
            std::printf("rich: sample %u duration mismatch\n", i + 1);
            return false;
        }
        if (MP4GetSampleRenderingOffset(h, t, i + 1) != expOff64) {
            std::printf("rich: sample %u offset mismatch\n", i + 1);
            return false;
        }

        uint8_t* bytes = NULL;
        uint32_t nbytes = 0;
        MP4Timestamp st = 0;
        MP4Duration du = 0, off = 0;
        bool sync = false;
        if (!MP4ReadSample(h, t, i + 1, &bytes, &nbytes,
                           &st, &du, &off, &sync)) {
            std::printf("rich: read sample %u failed\n", i + 1);
            return false;
        }
        bool bytesOk = (nbytes == 4);
        for (unsigned j = 0; bytesOk && j < 4; ++j)
            bytesOk = (bytes[j] == richByte(i, j));
        MP4Free(bytes);
        if (!bytesOk) {
            std::printf("rich: sample %u bytes changed\n", i + 1);
            return false;
        }
        if (st != expTime || du != expDur64 || off != expOff64) {
            std::printf("rich: sample %u readback times mismatch\n", i + 1);
            return false;
        }

        expDur.push_back((uint32_t)expDur64);
        expOff.push_back((uint32_t)expOff64);
    }

    if (!checkRuns(h, 0, "stts", expDur))
        return false;
    if (!checkRuns(h, 0, "ctts", expOff))
        return false;

    if (MP4GetTrackDuration(h, t) != refQ(d[RICH_N], N, 1000)) {
        std::printf("rich: mdhd duration mismatch %llu want %llu\n",
                    (unsigned long long)MP4GetTrackDuration(h, t),
                    (unsigned long long)refQ(d[RICH_N], N, 1000));
        return false;
    }

    if (MP4GetTrackNumberOfEdits(h, t) != 2)
        return false;
    if (MP4GetTrackEditMediaStart(h, t, 1) != 0xFFFFFFFFULL) {
        std::printf("rich: empty edit media time not preserved: %llu\n",
                    (unsigned long long)MP4GetTrackEditMediaStart(h, t, 1));
        return false;
    }
    if (MP4GetTrackEditMediaStart(h, t, 2) != refQ(100, N, 1000))
        return false;
    if (MP4GetTrackEditDuration(h, t, 1) != 500 ||
        MP4GetTrackEditDuration(h, t, 2) != 2000)
        return false;

    return true;
}

static bool verifyRichB(MP4FileHandle h, MP4TrackId b)
{
    return MP4GetTrackTimeScale(h, b) == 8000
        && MP4GetTrackDuration(h, b) == 800
        && MP4GetSampleTime(h, b, 2) == 200
        && MP4GetSampleDuration(h, b, 2) == 400
        && MP4GetSampleTime(h, b, 3) == 600;
}

/* snapshot / compare all chunk offsets so chunk addresses are untouched */
static vector<uint64_t> snapshotChunkOffsets(MP4FileHandle h, unsigned idx)
{
    vector<uint64_t> offsets;
    uint64_t count = trakProp(
        h, idx, "mdia.minf.stbl.stco.entryCount");
    for (uint64_t i = 0; i < count; ++i) {
        char tail[128];
        std::snprintf(tail, sizeof(tail),
                      "mdia.minf.stbl.stco.entries[%llu].chunkOffset",
                      (unsigned long long)i);
        offsets.push_back(trakProp(h, idx, tail));
    }
    return offsets;
}

/* ------------------------------------------------------------------ */
/* quantize group                                                     */
/* ------------------------------------------------------------------ */

static int quantize_rich_modify_reopen(void)
{
    const char* path = ".task-build/retime_rich.mp4";
    buildRichFixture(path);

    /* snapshot values that must not change */
    MP4FileHandle h = MP4Read(path);
    TASK_CHECK(h != MP4_INVALID_FILE_HANDLE);
    vector<uint64_t> chunksBefore = snapshotChunkOffsets(h, 0);
    uint64_t mvhdTsBefore = iprop(h, "moov.mvhd.timeScale");
    uint64_t mvhdDurBefore = iprop(h, "moov.mvhd.duration");
    uint64_t tkhdDurBefore = trakProp(h, 0, "tkhd.duration");
    MP4TrackId b = 2;
    TASK_CHECK(verifyRichB(h, b));
    MP4Close(h);

    /* MP4Modify path: rescale 1000 -> 900 */
    h = MP4Modify(path);
    TASK_CHECK(h != MP4_INVALID_FILE_HANDLE);
    TASK_CHECK(MP4RescaleTrackTimeScale(h, 1, 900));
    TASK_CHECK(MP4GetTrackTimeScale(h, 1) == 900);

    /* unchanged movie/track-header durations and movie time base */
    TASK_CHECK(iprop(h, "moov.mvhd.timeScale") == mvhdTsBefore);
    TASK_CHECK(iprop(h, "moov.mvhd.duration") == mvhdDurBefore);
    TASK_CHECK(trakProp(h, 0, "tkhd.duration") == tkhdDurBefore);
    TASK_CHECK(verifyRichB(h, b));
    TASK_CHECK(snapshotChunkOffsets(h, 0) == chunksBefore);
    MP4Close(h);

    /* close + reopen: same result, no second conversion at flush */
    h = MP4Read(path);
    TASK_CHECK(h != MP4_INVALID_FILE_HANDLE);
    TASK_CHECK(MP4GetTrackTimeScale(h, 1) == 900);
    TASK_CHECK(verifyRichA(h, 1, 900));
    TASK_CHECK(verifyRichB(h, b));
    TASK_CHECK(iprop(h, "moov.mvhd.timeScale") == mvhdTsBefore);
    TASK_CHECK(iprop(h, "moov.mvhd.duration") == mvhdDurBefore);
    TASK_CHECK(trakProp(h, 0, "tkhd.duration") == tkhdDurBefore);
    TASK_CHECK(snapshotChunkOffsets(h, 0) == chunksBefore);
    MP4Close(h);

    return 0;
}

static int quantize_splits_same_stts_run(void)
{
    const char* path = ".task-build/retime_split.mp4";
    MP4FileHandle h = MP4Create(path);
    MP4TrackId t = MP4AddAudioTrack(h, 1000, MP4_INVALID_DURATION);
    /* five equal deltas -> a single stts run (5,11) */
    for (unsigned i = 0; i < 5; ++i) {
        uint8_t byte = (uint8_t)(i + 1);
        MP4WriteSample(h, t, &byte, 1, 11, 3, true);
    }
    MP4Close(h);

    h = MP4Modify(path);
    TASK_CHECK(MP4RescaleTrackTimeScale(h, t, 900));
    MP4Close(h);

    h = MP4Read(path);
    TASK_CHECK(MP4GetTrackTimeScale(h, t) == 900);
    /* Q(11j)=floor(9.9j): deltas 9,10,10,10,10 -> run (1,9),(4,10) */
    TASK_CHECK(trakProp(h, 0,
        "mdia.minf.stbl.stts.entryCount") == 2);
    TASK_CHECK(trakProp(h, 0,
        "mdia.minf.stbl.stts.entries[0].sampleCount") == 1);
    TASK_CHECK(trakProp(h, 0,
        "mdia.minf.stbl.stts.entries[0].sampleDelta") == 9);
    TASK_CHECK(trakProp(h, 0,
        "mdia.minf.stbl.stts.entries[1].sampleCount") == 4);
    TASK_CHECK(trakProp(h, 0,
        "mdia.minf.stbl.stts.entries[1].sampleDelta") == 10);
    TASK_CHECK(MP4GetSampleTime(h, t, 1) == 0);
    TASK_CHECK(MP4GetSampleTime(h, t, 2) == 9);
    TASK_CHECK(MP4GetSampleTime(h, t, 5) == 39);
    TASK_CHECK(MP4GetSampleDuration(h, t, 5) == 10);
    /* offsets 3 -> per sample 2,3,3? compute by formula instead */
    for (unsigned i = 0; i < 5; ++i) {
        uint64_t d = 11ULL * i;
        uint64_t want = refQ(d + 3, 900, 1000) - refQ(d, 900, 1000);
        TASK_CHECK(MP4GetSampleRenderingOffset(h, t, i + 1) == want);
    }
    MP4Close(h);
    return 0;
}

static int quantize_other_ratio(void)
{
    const char* path = ".task-build/retime_ratio.mp4";
    MP4FileHandle h = MP4Create(path);
    MP4TrackId t = MP4AddAudioTrack(h, 44100, MP4_INVALID_DURATION);
    static const uint32_t U[6] = { 1024, 1024, 800, 1024, 1, 5000 };
    static const uint32_t C[6] = { 0, 512, 1024, 33, 700, 0 };
    for (unsigned i = 0; i < 6; ++i) {
        uint8_t b = (uint8_t)i;
        MP4WriteSample(h, t, &b, 1, U[i], C[i], true);
    }
    MP4Close(h);

    h = MP4Modify(path);
    TASK_CHECK(MP4RescaleTrackTimeScale(h, t, 48000));
    MP4Close(h);

    h = MP4Read(path);
    uint64_t d = 0;
    vector<uint32_t> du, co;
    for (unsigned i = 0; i < 6; ++i) {
        uint64_t qd = refQ(d, 48000, 44100);
        uint64_t qend = refQ(d + U[i], 48000, 44100);
        uint64_t qco = refQ(d + C[i], 48000, 44100) - qd;
        TASK_CHECK(MP4GetSampleTime(h, t, i + 1) == qd);
        TASK_CHECK(MP4GetSampleDuration(h, t, i + 1) == qend - qd);
        TASK_CHECK(MP4GetSampleRenderingOffset(h, t, i + 1) == qco);
        du.push_back((uint32_t)(qend - qd));
        co.push_back((uint32_t)qco);
        d += U[i];
    }
    TASK_CHECK(checkRuns(h, 0, "stts", du));
    TASK_CHECK(checkRuns(h, 0, "ctts", co));
    TASK_CHECK(MP4GetTrackDuration(h, t) == refQ(d, 48000, 44100));
    MP4Close(h);
    return 0;
}

static int quantize_64bit_endpoints(void)
{
    const char* path = ".task-build/retime_64bit.mp4";
    MP4FileHandle h = MP4Create(path, MP4_CREATE_64BIT_TIME);
    MP4TrackId t = MP4AddAudioTrack(h, 1000, MP4_INVALID_DURATION);
    MP4SetTrackDurationPerChunk(h, t, 0xFFFFFFFFULL);
    const uint32_t U = 1500000000u;
    for (unsigned i = 0; i < 4; ++i) {
        uint8_t b = (uint8_t)i;
        MP4WriteSample(h, t, &b, 1, U, 0, true);
    }
    MP4Close(h);

    h = MP4Modify(path);
    TASK_CHECK(MP4RescaleTrackTimeScale(h, t, 2000)); /* exact 2x */
    MP4Close(h);

    h = MP4Read(path);
    TASK_CHECK(MP4GetSampleTime(h, t, 2) == 3000000000ULL);
    TASK_CHECK(MP4GetSampleTime(h, t, 3) == 6000000000ULL);
    TASK_CHECK(MP4GetSampleTime(h, t, 4) == 9000000000ULL);
    TASK_CHECK(MP4GetSampleDuration(h, t, 1) == 3000000000ULL);
    TASK_CHECK(MP4GetTrackDuration(h, t) == 12000000000ULL);
    MP4Close(h);
    return 0;
}

static int quantize_overflow_safe_fit(void)
{
    /* d*N wraps a naive uint64 multiply while the endpoint differences
     * still fit a 32 bit stts delta: must succeed via wide intermediates. */
    const char* path = ".task-build/retime_widemul.mp4";
    MP4FileHandle h = MP4Create(path, MP4_CREATE_64BIT_TIME);
    const uint32_t S = 4000000000u;
    const uint32_t U = 4000000000u;
    const uint32_t N = 4100000000u;
    MP4TrackId t = MP4AddAudioTrack(h, S, MP4_INVALID_DURATION);
    uint8_t b = 7;
    for (unsigned i = 0; i < 3; ++i)
        MP4WriteSample(h, t, &b, 1, U, 0, true);
    MP4Close(h);

    h = MP4Modify(path);
    TASK_CHECK(MP4RescaleTrackTimeScale(h, t, N));
    MP4Close(h);

    h = MP4Read(path);
    TASK_CHECK(MP4GetTrackTimeScale(h, t) == N);
    for (unsigned i = 1; i <= 3; ++i) {
        uint64_t d = (uint64_t)(i - 1) * U;
        uint64_t wantTime = refQ(d, N, S);
        uint64_t wantDur = refQ(d + U, N, S) - wantTime;
        TASK_CHECK(MP4GetSampleTime(h, t, i) == wantTime);
        TASK_CHECK(MP4GetSampleDuration(h, t, i) == wantDur);
        TASK_CHECK(wantDur >= 1 && wantDur <= 0xFFFFFFFFULL);
    }
    MP4Close(h);
    return 0;
}

/* ------------------------------------------------------------------ */
/* state group                                                        */
/* ------------------------------------------------------------------ */

static int state_warm_caches_and_queries(void)
{
    const char* path = ".task-build/retime_warm.mp4";
    MP4FileHandle h = MP4Create(path);
    MP4TrackId t = MP4AddAudioTrack(h, 1000, MP4_INVALID_DURATION);
    static const uint32_t U[4] = { 11, 11, 20, 13 };
    static const uint32_t C[4] = { 3, 7, 3, 0 };
    for (unsigned i = 0; i < 4; ++i) {
        uint8_t b = (uint8_t)i;
        MP4WriteSample(h, t, &b, 1, U[i], C[i], true);
    }
    /* empty edit then a media segment starting at media time 0 */
    MP4AddTrackEdit(h, t, MP4_INVALID_EDIT_ID,
                    MP4_INVALID_TIMESTAMP, 100, false);
    MP4AddTrackEdit(h, t, MP4_INVALID_EDIT_ID, 0, 500, false);

    /* warm the sequential stts/ctts caches in non-monotonic order */
    TASK_CHECK(MP4GetSampleTime(h, t, 4) == 42);
    TASK_CHECK(MP4GetSampleTime(h, t, 2) == 11);
    TASK_CHECK(MP4GetSampleTime(h, t, 3) == 22);
    TASK_CHECK(MP4GetSampleRenderingOffset(h, t, 4) == 0);
    TASK_CHECK(MP4GetSampleRenderingOffset(h, t, 2) == 7);
    TASK_CHECK(MP4GetSampleIdFromTime(h, t, 25, false) == 3);

    TASK_CHECK(MP4RescaleTrackTimeScale(h, t, 900));

    /* same handles, warm caches must have been invalidated */
    TASK_CHECK(MP4GetSampleTime(h, t, 2) == refQ(11, 900, 1000));
    TASK_CHECK(MP4GetSampleTime(h, t, 3) == refQ(22, 900, 1000));
    TASK_CHECK(MP4GetSampleTime(h, t, 4) == refQ(42, 900, 1000));
    TASK_CHECK(MP4GetSampleDuration(h, t, 3) ==
               refQ(42, 900, 1000) - refQ(22, 900, 1000));
    TASK_CHECK(MP4GetSampleRenderingOffset(h, t, 2) ==
               refQ(11 + 7, 900, 1000) - refQ(11, 900, 1000));
    /* new time base find-sample queries */
    TASK_CHECK(MP4GetSampleIdFromTime(h, t, 8, false) == 1);
    TASK_CHECK(MP4GetSampleIdFromTime(h, t, 9, false) == 2);

    /* edit query lands in the media segment (edit offset 5) */
    MP4Timestamp est = 0;
    MP4Duration edu = 0;
    MP4SampleId sid = MP4GetSampleIdFromEditTime(
        h, t, 105, &est, &edu);
    TASK_CHECK(sid == 1);
    TASK_CHECK(edu == MP4GetSampleDuration(h, t, 1));

    MP4Close(h);

    /* and again through MP4Modify + reopen. A further 900 -> 1800 pass
     * rescales the already quantized endpoints (exact 2x here); the spec
     * does not require a lossy chain to equal one direct conversion. */
    h = MP4Modify(path);
    TASK_CHECK(MP4GetSampleTime(h, t, 4) == refQ(42, 900, 1000));
    TASK_CHECK(MP4RescaleTrackTimeScale(h, t, 1800));
    TASK_CHECK(MP4GetSampleTime(h, t, 4) == 2 * refQ(42, 900, 1000));
    MP4Close(h);
    h = MP4Read(path);
    TASK_CHECK(MP4GetTrackTimeScale(h, t) == 1800);
    TASK_CHECK(MP4GetSampleTime(h, t, 2) == 2 * refQ(11, 900, 1000));
    MP4Close(h);
    return 0;
}

static int state_unflushed_chunk_append_reopen(void)
{
    const char* path = ".task-build/retime_chunk.mp4";
    MP4FileHandle h = MP4Create(path);
    MP4TrackId t = MP4AddAudioTrack(h, 1000, MP4_INVALID_DURATION);
    /* threshold 25 old ticks -> max(1,Q(25)) = 22 at 900 */
    MP4SetTrackDurationPerChunk(h, t, 25);

    uint8_t payload[3] = { 9, 8, 7 };
    for (unsigned i = 0; i < 3; ++i)
        MP4WriteSample(h, t, payload, 3, 6, 2, true);
    /* 18 < 25: all three samples are still in the memory chunk buffer */
    TASK_CHECK(trakProp(h, 0,
        "mdia.minf.stbl.stco.entryCount") == 0);

    TASK_CHECK(MP4RescaleTrackTimeScale(h, t, 900));

    /* rescale itself must not drain the pending chunk buffer */
    TASK_CHECK(trakProp(h, 0,
        "mdia.minf.stbl.stco.entryCount") == 0);

    MP4Duration perChunk = 0;
    TASK_CHECK(MP4GetTrackDurationPerChunk(h, t, &perChunk));
    TASK_CHECK(perChunk == 22);

    /* buffered durations recomputed: new deltas 5,5,6 -> 16.
     * append one new-time-base duration 7 -> 23 >= 22 and flushes */
    TASK_CHECK(MP4WriteSample(h, t, payload, 3, 7, 1, true));
    MP4Close(h);

    h = MP4Read(path);
    TASK_CHECK(iprop(h,
        "moov.trak[0].mdia.minf.stbl.stsz.sampleCount") == 4);
    TASK_CHECK(trakProp(h, 0,
        "mdia.minf.stbl.stco.entryCount") == 1);
    TASK_CHECK(trakProp(h, 0,
        "mdia.minf.stbl.stsc.entries[0].samplesPerChunk") == 4);
    TASK_CHECK(MP4GetSampleTime(h, t, 2) == 5);
    TASK_CHECK(MP4GetSampleTime(h, t, 3) == 10);
    TASK_CHECK(MP4GetSampleTime(h, t, 4) == 16);
    TASK_CHECK(MP4GetSampleDuration(h, t, 3) == 6);
    TASK_CHECK(MP4GetSampleDuration(h, t, 4) == 7);
    TASK_CHECK(MP4GetSampleRenderingOffset(h, t, 4) == 1);

    uint8_t* bytes = NULL;
    uint32_t n = 0;
    TASK_CHECK(MP4ReadSample(h, t, 4, &bytes, &n, NULL, NULL, NULL, NULL));
    TASK_CHECK(n == 3 && bytes[0] == 9 && bytes[2] == 7);
    MP4Free(bytes);
    MP4Close(h);
    return 0;
}

static int state_fixed_duration_converts(void)
{
    /* empty track carrying the INVALID_DURATION sentinel: accepted */
    const char* p0 = ".task-build/retime_empty.mp4";
    MP4FileHandle h0 = MP4Create(p0);
    MP4TrackId t0 = MP4AddAudioTrack(h0, 1000, MP4_INVALID_DURATION);
    TASK_CHECK(MP4RescaleTrackTimeScale(h0, t0, 500));
    TASK_CHECK(MP4GetTrackTimeScale(h0, t0) == 500);
    MP4Close(h0);

    const char* path = ".task-build/retime_fixed.mp4";
    MP4FileHandle h = MP4Create(path);
    MP4TrackId t = MP4AddAudioTrack(h, 1000, 100); /* fixed 100 */
    TASK_CHECK(MP4RescaleTrackTimeScale(h, t, 900)); /* -> Q(100)=90 */
    uint8_t b[2] = { 5, 6 };
    TASK_CHECK(MP4WriteSample(h, t, b, 2, MP4_INVALID_DURATION, 0, true));
    MP4Close(h);

    h = MP4Read(path);
    TASK_CHECK(MP4GetSampleDuration(h, t, 1) == 90);
    TASK_CHECK(MP4GetTrackDuration(h, t) == 90);
    MP4Close(h);
    return 0;
}

/* ------------------------------------------------------------------ */
/* reject group                                                       */
/* ------------------------------------------------------------------ */

static int reject_modify_leaves_file(void)
{
    const char* path = ".task-build/retime_modify_reject.mp4";
    {
        MP4FileHandle h = MP4Create(path);
        MP4TrackId t = MP4AddAudioTrack(h, 1000, MP4_INVALID_DURATION);
        uint8_t p[4] = { 4, 3, 2, 1 };
        for (unsigned i = 0; i < 4; ++i)
            MP4WriteSample(h, t, p, 4, 11, 3, true);
        MP4Close(h);
    }

    MP4FileHandle h = MP4Modify(path);
    TASK_CHECK(h != MP4_INVALID_FILE_HANDLE);
    TASK_CHECK(!MP4RescaleTrackTimeScale(h, 1, 0));
    TASK_CHECK(MP4GetTrackTimeScale(h, 1) == 1000);
    TASK_CHECK(MP4GetSampleTime(h, 1, 4) == 33);
    MP4Close(h);

    h = MP4Read(path);
    TASK_CHECK(h != MP4_INVALID_FILE_HANDLE);
    TASK_CHECK(MP4GetTrackTimeScale(h, 1) == 1000);
    TASK_CHECK(MP4GetTrackDuration(h, 1) == 44);
    for (unsigned i = 1; i <= 4; ++i) {
        uint8_t* bytes = NULL;
        uint32_t n = 0;
        MP4Timestamp d;
        MP4Duration u, c;
        TASK_CHECK(MP4ReadSample(h, 1, i, &bytes, &n, &d, &u, &c, NULL));
        bool ok = (n == 4 && bytes[0] == 4 && bytes[3] == 1
                   && d == (i - 1) * 11 && u == 11 && c == 3);
        MP4Free(bytes);
        TASK_CHECK(ok);
    }
    MP4Close(h);
    return 0;
}

static int reject_bad_arguments(void)
{
    const char* path = ".task-build/retime_args.mp4";
    MP4FileHandle h = MP4Create(path);
    MP4TrackId t = MP4AddAudioTrack(h, 1000, MP4_INVALID_DURATION);
    uint8_t b[2] = { 1, 2 };
    MP4WriteSample(h, t, b, 2, 10, 0, true);

    TASK_CHECK(!MP4RescaleTrackTimeScale(NULL, t, 900));
    TASK_CHECK(!MP4RescaleTrackTimeScale(h, t, 0));
    TASK_CHECK(!MP4RescaleTrackTimeScale(h, MP4_INVALID_TRACK_ID, 900));
    TASK_CHECK(!MP4RescaleTrackTimeScale(h, t + 99, 900));
    /* nothing changed */
    TASK_CHECK(MP4GetTrackTimeScale(h, t) == 1000);
    TASK_CHECK(MP4GetSampleDuration(h, t, 1) == 10);
    MP4Close(h);

    /* read-only handle */
    h = MP4Read(path);
    TASK_CHECK(h != MP4_INVALID_FILE_HANDLE);
    TASK_CHECK(!MP4RescaleTrackTimeScale(h, t, 900));
    MP4Close(h);
    return 0;
}

static int reject_keeps_buffer_and_continues(void)
{
    const char* path = ".task-build/retime_continue.mp4";
    MP4FileHandle h = MP4Create(path);
    MP4TrackId t = MP4AddAudioTrack(h, 1000, MP4_INVALID_DURATION);
    MP4SetTrackDurationPerChunk(h, t, 1000000);
    uint8_t payload[4] = { 1, 2, 3, 4 };
    for (unsigned i = 0; i < 3; ++i)
        TASK_CHECK(MP4WriteSample(h, t, payload, 4, 11, 3, true));

    /* warm cache, then reject */
    TASK_CHECK(MP4GetSampleTime(h, t, 2) == 11);
    TASK_CHECK(!MP4RescaleTrackTimeScale(h, t, 0));
    TASK_CHECK(MP4GetSampleTime(h, t, 2) == 11);

    /* rejection must not drain the write buffer either */
    TASK_CHECK(trakProp(h, 0,
        "mdia.minf.stbl.stco.entryCount") == 0);

    /* write buffer must not have been drained: one chunk after close */
    TASK_CHECK(MP4WriteSample(h, t, payload, 4, 11, 3, true));
    MP4Close(h);

    h = MP4Read(path);
    TASK_CHECK(iprop(h,
        "moov.trak[0].mdia.minf.stbl.stsz.sampleCount") == 4);
    TASK_CHECK(trakProp(h, 0,
        "mdia.minf.stbl.stco.entryCount") == 1);
    for (unsigned i = 1; i <= 4; ++i) {
        uint8_t* bytes = NULL;
        uint32_t n = 0;
        MP4Timestamp d;
        MP4Duration u, c;
        TASK_CHECK(MP4ReadSample(h, t, i, &bytes, &n, &d, &u, &c, NULL));
        bool ok = (n == 4 && bytes[0] == 1 && bytes[3] == 4
                   && d == (i - 1) * 11 && u == 11 && c == 3);
        MP4Free(bytes);
        TASK_CHECK(ok);
    }
    MP4Close(h);
    return 0;
}

static int reject_zero_and_collapsed_durations(void)
{
    const char* p1 = ".task-build/retime_zero.mp4";
    MP4FileHandle h = MP4Create(p1);
    MP4TrackId t = MP4AddAudioTrack(h, 1000, MP4_INVALID_DURATION);
    uint8_t b[2] = { 1, 2 };
    MP4WriteSample(h, t, b, 2, 10, 0, true);
    MP4WriteSample(h, t, b, 2, 0, 0, true);
    MP4WriteSample(h, t, b, 2, 10, 0, true);
    TASK_CHECK(!MP4RescaleTrackTimeScale(h, t, 900));
    /* same scale must also validate content */
    TASK_CHECK(!MP4RescaleTrackTimeScale(h, t, 1000));
    TASK_CHECK(MP4GetTrackTimeScale(h, t) == 1000);
    MP4Close(h);

    const char* p2 = ".task-build/retime_collapse.mp4";
    h = MP4Create(p2);
    t = MP4AddAudioTrack(h, 1000, MP4_INVALID_DURATION);
    for (unsigned i = 0; i < 3; ++i)
        MP4WriteSample(h, t, b, 2, 1, 0, true);
    TASK_CHECK(!MP4RescaleTrackTimeScale(h, t, 1)); /* Q(1)=0 */
    TASK_CHECK(MP4GetTrackTimeScale(h, t) == 1000);
    MP4Close(h);
    return 0;
}

static int reject_unrepresentable(void)
{
    const char* path = ".task-build/retime_overflow.mp4";
    MP4FileHandle h = MP4Create(path, MP4_CREATE_64BIT_TIME);
    MP4TrackId t = MP4AddAudioTrack(h, 1, MP4_INVALID_DURATION);
    MP4SetTrackDurationPerChunk(h, t, 0xFFFFFFFFULL);
    const uint32_t U = 3000000000u;
    uint8_t b = 1;
    for (unsigned i = 0; i < 3; ++i)
        MP4WriteSample(h, t, &b, 1, U, 0, true);
    TASK_CHECK(MP4GetSampleTime(h, t, 3) == 6000000000ULL);

    /* large scale times huge endpoints: naive 64 bit multiply wraps */
    TASK_CHECK(!MP4RescaleTrackTimeScale(h, t, 0xFFFFFFFFu));
    TASK_CHECK(MP4GetTrackTimeScale(h, t) == 1);
    TASK_CHECK(MP4GetSampleTime(h, t, 3) == 6000000000ULL);
    TASK_CHECK(MP4GetSampleDuration(h, t, 2) == U);
    MP4Close(h);

    /* file still reopens with original tables */
    h = MP4Read(path);
    TASK_CHECK(MP4GetTrackTimeScale(h, t) == 1);
    TASK_CHECK(MP4GetSampleTime(h, t, 3) == 6000000000ULL);
    TASK_CHECK(MP4GetSampleDuration(h, t, 1) == U);
    MP4Close(h);
    return 0;
}

static int reject_unsupported_tracks(void)
{
    const char* p1 = ".task-build/retime_mp4v.mp4";
    MP4FileHandle h = MP4Create(p1);
    MP4TrackId v = MP4AddVideoTrack(
        h, 30000, 1001, 16, 16, MP4_MPEG4_VIDEO_TYPE);
    TASK_CHECK(v != MP4_INVALID_TRACK_ID);
    uint8_t b[4] = { 0, 0, 1, 0 };
    MP4WriteSample(h, v, b, 4, 1001, 2000, true);
    TASK_CHECK(!MP4RescaleTrackTimeScale(h, v, 15000));
    TASK_CHECK(!MP4RescaleTrackTimeScale(h, v, 30000)); /* same scale too */
    MP4Close(h);

    /* hint tracks are out of scope as well */
    const char* p2 = ".task-build/retime_hint.mp4";
    h = MP4Create(p2);
    MP4TrackId refAudio = MP4AddAudioTrack(h, 8000, MP4_INVALID_DURATION);
    MP4TrackId hint = MP4AddHintTrack(h, refAudio);
    TASK_CHECK(hint != MP4_INVALID_TRACK_ID);
    TASK_CHECK(!MP4RescaleTrackTimeScale(h, hint, 500));
    MP4Close(h);
    return 0;
}

static int reject_bad_edit_rate(void)
{
    const char* path = ".task-build/retime_rate.mp4";
    MP4FileHandle h = MP4Create(path);
    MP4TrackId t = MP4AddAudioTrack(h, 1000, MP4_INVALID_DURATION);
    uint8_t b[2] = { 1, 2 };
    MP4WriteSample(h, t, b, 2, 10, 0, true);
    /* dwell edit -> mediaRate 0, not unit rate */
    MP4EditId e = MP4AddTrackEdit(
        h, t, MP4_INVALID_EDIT_ID, 0, 100, true);
    TASK_CHECK(e != MP4_INVALID_EDIT_ID);
    TASK_CHECK(!MP4RescaleTrackTimeScale(h, t, 900));
    TASK_CHECK(MP4GetTrackTimeScale(h, t) == 1000);
    TASK_CHECK(MP4GetSampleDuration(h, t, 1) == 10);
    MP4Close(h);
    return 0;
}

static int reject_bad_tables(void)
{
    const char* path = ".task-build/retime_badtable.mp4";
    MP4FileHandle h = MP4Create(path);
    MP4TrackId t = MP4AddAudioTrack(h, 1000, MP4_INVALID_DURATION);
    uint8_t b[2] = { 1, 2 };
    for (unsigned i = 0; i < 3; ++i)
        MP4WriteSample(h, t, b, 2, 10, 3, true);

    /* inflate an stts run count past the stsz sample count */
    TASK_CHECK(MP4SetIntegerProperty(
        h, "moov.trak[0].mdia.minf.stbl.stts.entries[0].sampleCount",
        999));
    TASK_CHECK(!MP4RescaleTrackTimeScale(h, t, 900));
    TASK_CHECK(MP4GetTrackTimeScale(h, t) == 1000);
    MP4Close(h);

    /* ctts coverage mismatch */
    const char* p2 = ".task-build/retime_badctts.mp4";
    h = MP4Create(p2);
    t = MP4AddAudioTrack(h, 1000, MP4_INVALID_DURATION);
    for (unsigned i = 0; i < 3; ++i)
        MP4WriteSample(h, t, b, 2, 10, 3, true);
    TASK_CHECK(MP4SetIntegerProperty(
        h, "moov.trak[0].mdia.minf.stbl.ctts.entries[0].sampleCount",
        999));
    TASK_CHECK(!MP4RescaleTrackTimeScale(h, t, 900));
    MP4Close(h);
    return 0;
}

/* ------------------------------------------------------------------ */
/* compat group: legacy setter behaviour stays tick-preserving        */
/* ------------------------------------------------------------------ */

static int compat_legacy_setter_preserves_ticks(void)
{
    const char* path = ".task-build/retime_legacy.mp4";
    MP4FileHandle f = MP4Create(path);
    MP4TrackId t = MP4AddAudioTrack(f, 1000, MP4_INVALID_DURATION);
    TASK_CHECK(t != MP4_INVALID_TRACK_ID);
    const uint8_t payloads[3][4] = {
        { 1, 2, 3, 4 }, { 5, 6, 7, 8 }, { 9, 10, 11, 12 }
    };
    TASK_CHECK(MP4SetTrackDurationPerChunk(f, t, 1000));
    for (unsigned i = 0; i < 3; ++i)
        TASK_CHECK(MP4WriteSample(
            f, t, payloads[i], 4, 11, 3, true));
    TASK_CHECK(MP4GetTrackDuration(f, t) == 33);
    TASK_CHECK(MP4GetSampleTime(f, t, 3) == 22);

    TASK_CHECK(MP4SetTrackTimeScale(f, t, 900));
    TASK_CHECK(MP4GetTrackTimeScale(f, t) == 900);
    /* legacy setter changes the field only: tick values preserved */
    TASK_CHECK(MP4GetTrackDuration(f, t) == 33);
    TASK_CHECK(MP4GetSampleTime(f, t, 3) == 22);
    TASK_CHECK(MP4GetSampleDuration(f, t, 2) == 11);
    TASK_CHECK(MP4GetSampleRenderingOffset(f, t, 2) == 3);
    MP4Close(f);

    f = MP4Read(path);
    TASK_CHECK(MP4GetTrackTimeScale(f, t) == 900);
    TASK_CHECK(MP4GetTrackDuration(f, t) == 33);
    for (unsigned i = 0; i < 3; ++i) {
        uint8_t* p = NULL;
        uint32_t n = 0;
        MP4Timestamp d = 0;
        MP4Duration u = 0, c = 0;
        TASK_CHECK(MP4ReadSample(
            f, t, i + 1, &p, &n, &d, &u, &c, NULL));
        bool good = (n == 4 && !std::memcmp(p, payloads[i], 4)
                     && d == i * 11 && u == 11 && c == 3);
        MP4Free(p);
        TASK_CHECK(good);
    }
    MP4Close(f);
    return 0;
}

static int compat_legacy_setter_zero_still_false(void)
{
    const char* path = ".task-build/retime_legacy0.mp4";
    MP4FileHandle h = MP4Create(path);
    MP4TrackId t = MP4AddAudioTrack(h, 1000, MP4_INVALID_DURATION);
    TASK_CHECK(!MP4SetTrackTimeScale(h, t, 0));
    TASK_CHECK(MP4GetTrackTimeScale(h, t) == 1000);
    MP4Close(h);
    return 0;
}

/* ------------------------------------------------------------------ */
/* case registry                                                      */
/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
    { "quantize", "rich_modify_reopen",      quantize_rich_modify_reopen },
    { "quantize", "splits_same_stts_run",    quantize_splits_same_stts_run },
    { "quantize", "other_ratio",             quantize_other_ratio },
    { "quantize", "64bit_endpoints",         quantize_64bit_endpoints },
    { "quantize", "overflow_safe_fit",       quantize_overflow_safe_fit },

    { "state", "warm_caches_and_queries",    state_warm_caches_and_queries },
    { "state", "unflushed_chunk_append",     state_unflushed_chunk_append_reopen },
    { "state", "fixed_duration_converts",    state_fixed_duration_converts },

    { "reject", "bad_arguments",             reject_bad_arguments },
    { "reject", "modify_leaves_file",        reject_modify_leaves_file },
    { "reject", "keeps_buffer_and_continues", reject_keeps_buffer_and_continues },
    { "reject", "zero_and_collapsed",        reject_zero_and_collapsed_durations },
    { "reject", "unrepresentable",           reject_unrepresentable },
    { "reject", "unsupported_tracks",        reject_unsupported_tracks },
    { "reject", "bad_edit_rate",             reject_bad_edit_rate },
    { "reject", "bad_tables",                reject_bad_tables },

    { "compat", "legacy_setter_preserves_ticks",
      compat_legacy_setter_preserves_ticks },
    { "compat", "legacy_setter_zero",
      compat_legacy_setter_zero_still_false },
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);

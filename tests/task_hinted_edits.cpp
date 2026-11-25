/*
 * Task tests for MP4CopyHintedTrack and the legacy track copy paths.
 *
 * Registers three groups with task_support/driver.c:
 *   occurrence    - segment occurrence rebasing and instruction preservation
 *   clock         - exact timescale conversion and timetable checks
 *   reject_compat - preflight rejection and old copy/clone compatibility
 */

#include <mp4v2/mp4v2.h>

#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

#include <unistd.h>
#include <sys/wait.h>

#include "task_support/check.h"

typedef std::vector<uint8_t> Bytes;

/* ------------------------------------------------------------------ */
/* small filesystem / process helpers                                  */
/* ------------------------------------------------------------------ */

static std::string WorkDir()
{
    const char* t = std::getenv("TMPDIR");
    if (t != NULL && *t != '\0') {
        return std::string(t);
    }
    return std::string(".task-build");
}

static std::string TPath(const char* name)
{
    std::string d = WorkDir();
    if (!d.empty() && d[d.size() - 1] != '/') {
        d += "/";
    }
    d += name;
    return d;
}

static void RemoveFile(const std::string& path)
{
    std::remove(path.c_str());
}

static std::string ExampleExe()
{
    const char* e = std::getenv("TASK_EXAMPLE_BIN");
    if (e != NULL && *e != '\0' && access(e, X_OK) == 0) {
        return std::string(e);
    }
    const char* candidates[] = {
        ".task-build/example",
        "./.task-build/example",
        "example",
        NULL
    };
    for (int i = 0; candidates[i] != NULL; ++i) {
        if (access(candidates[i], X_OK) == 0) {
            return std::string(candidates[i]);
        }
    }
    return std::string();
}

static int RunExample(const std::string& input, const std::string& output)
{
    std::string exe = ExampleExe();
    TASK_CHECK(!exe.empty());
    std::string cmd = "\"" + exe + "\" \"" + input + "\" \"" + output + "\"";
    int rc = system(cmd.c_str());
    TASK_CHECK(rc != -1);
    TASK_CHECK(WIFEXITED(rc));
    TASK_CHECK(WEXITSTATUS(rc) == 0);
    return 0;
}

/* ------------------------------------------------------------------ */
/* raw RTP hint sample construction                                    */
/* ------------------------------------------------------------------ */

static void Put16(Bytes& b, uint16_t v)
{
    b.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
    b.push_back(static_cast<uint8_t>(v & 0xff));
}

static void Put32(Bytes& b, uint32_t v)
{
    for (int shift = 24; shift >= 0; shift -= 8) {
        b.push_back(static_cast<uint8_t>((v >> shift) & 0xff));
    }
}

static uint16_t GetBE16(const uint8_t* p)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

static uint32_t GetBE32(const uint8_t* p)
{
    return (static_cast<uint32_t>(p[0]) << 24)
         | (static_cast<uint32_t>(p[1]) << 16)
         | (static_cast<uint32_t>(p[2]) << 8)
         |  static_cast<uint32_t>(p[3]);
}

static Bytes RecordNull()
{
    return Bytes(16, 0);
}

static Bytes RecordImmediate(const Bytes& data)
{
    Bytes r(16, 0);
    r[0] = 1;
    r[1] = static_cast<uint8_t>(data.size() > 14 ? 14 : data.size());
    for (size_t i = 0; i < data.size() && i < 14; ++i) {
        r[2 + i] = data[i];
    }
    return r;
}

static Bytes RecordSample(uint8_t trackRefIndex, uint16_t length,
                          uint32_t sampleNumber, uint32_t sampleOffset,
                          uint16_t bytesPerBlock = 1,
                          uint16_t samplesPerBlock = 1)
{
    Bytes r;
    r.reserve(16);
    r.push_back(2);
    r.push_back(trackRefIndex);
    Put16(r, length);
    Put32(r, sampleNumber);
    Put32(r, sampleOffset);
    Put16(r, bytesPerBlock);
    Put16(r, samplesPerBlock);
    return r;
}

static Bytes RecordSampleDescription()
{
    Bytes r(16, 0);
    r[0] = 3;
    return r;
}

static Bytes RawPacket(uint8_t markerPayloadByte, int32_t transmitOffset,
                       uint16_t sequenceNumber, uint16_t flagBits,
                       const std::vector<Bytes>& records)
{
    Bytes p;
    Put32(p, static_cast<uint32_t>(transmitOffset));
    p.push_back(0);                    /* P/X/reserved bits */
    p.push_back(markerPayloadByte);    /* M + payload type */
    Put16(p, sequenceNumber);
    Put16(p, flagBits);
    Put16(p, static_cast<uint16_t>(records.size()));
    for (size_t i = 0; i < records.size(); ++i) {
        p.insert(p.end(), records[i].begin(), records[i].end());
    }
    return p;
}

static Bytes HintSample(const std::vector<Bytes>& packets,
                        const Bytes& tail = Bytes())
{
    Bytes s;
    Put16(s, static_cast<uint16_t>(packets.size()));
    Put16(s, 0);
    for (size_t i = 0; i < packets.size(); ++i) {
        s.insert(s.end(), packets[i].begin(), packets[i].end());
    }
    s.insert(s.end(), tail.begin(), tail.end());
    return s;
}

struct RawRef {
    uint8_t  trackRefIndex;
    uint16_t length;
    uint32_t sampleNumber;
    uint32_t sampleOffset;
    uint16_t bytesPerBlock;
    uint16_t samplesPerBlock;
    uint8_t  entryType;
};

static bool CollectRawRefs(const Bytes& sample, std::vector<RawRef>& refs)
{
    if (sample.size() < 4) {
        return false;
    }
    uint16_t count = GetBE16(&sample[0]);
    size_t pos = 4;
    for (uint16_t p = 0; p < count; ++p) {
        if (pos + 12 > sample.size()) {
            return false;
        }
        uint16_t entries = GetBE16(&sample[pos + 10]);
        pos += 12;
        for (uint16_t e = 0; e < entries; ++e) {
            if (pos + 16 > sample.size()) {
                return false;
            }
            RawRef r;
            r.entryType = sample[pos];
            r.trackRefIndex = sample[pos + 1];
            r.length = GetBE16(&sample[pos + 2]);
            r.sampleNumber = GetBE32(&sample[pos + 4]);
            r.sampleOffset = GetBE32(&sample[pos + 8]);
            r.bytesPerBlock = GetBE16(&sample[pos + 12]);
            r.samplesPerBlock = GetBE16(&sample[pos + 14]);
            refs.push_back(r);
            pos += 16;
        }
    }
    return pos <= sample.size();
}

static bool ReadAllSample(MP4FileHandle f, MP4TrackId t,
                          MP4SampleId id, Bytes& out)
{
    uint8_t* p = NULL;
    uint32_t n = 0;
    if (!MP4ReadSample(f, t, id, &p, &n, NULL, NULL, NULL, NULL)) {
        if (p != NULL) MP4Free(p);
        return false;
    }
    out.assign(p, p + n);
    MP4Free(p);
    return true;
}

/* ------------------------------------------------------------------ */
/* generic raw hinted file builder                                     */
/* ------------------------------------------------------------------ */

struct EditSpec {
    bool     empty;
    uint64_t mediaTime;
    uint64_t movieDuration;
};

struct RawPairSpec {
    std::string path;
    uint32_t movieTimeScale;
    uint32_t trackTimeScale;
    uint32_t sampleCount;
    MP4Duration sampleDuration;
    bool allSync;
    std::vector<bool> sync;
    std::vector<Bytes> hintSamples;
    std::vector<EditSpec> edits;
    bool hintEdit;

    RawPairSpec()
        : movieTimeScale(1000), trackTimeScale(1000), sampleCount(2),
          sampleDuration(100), allSync(true), hintEdit(false)
    {
    }
};

static bool BuildRawPair(const RawPairSpec& spec)
{
    RemoveFile(spec.path);
    MP4FileHandle f = MP4Create(spec.path.c_str());
    if (f == MP4_INVALID_FILE_HANDLE) {
        return false;
    }
    MP4SetTimeScale(f, spec.movieTimeScale);

    MP4TrackId m = MP4AddVideoTrack(f, spec.trackTimeScale,
                                    static_cast<MP4Duration>(spec.sampleDuration),
                                    16, 16, MP4_MPEG4_VIDEO_TYPE);
    if (m == MP4_INVALID_TRACK_ID) {
        MP4Close(f);
        return false;
    }
    static const uint8_t cfg[] = {0x77, 0x88, 0x99};
    MP4SetTrackESConfiguration(f, m, cfg, sizeof(cfg));

    for (uint32_t i = 1; i <= spec.sampleCount; ++i) {
        Bytes payload;
        payload.push_back(static_cast<uint8_t>(0x40 + i));
        payload.push_back(static_cast<uint8_t>(i & 0xff));
        payload.push_back(0x5a);
        bool sync = spec.allSync
                 || (i - 1 < spec.sync.size() && spec.sync[i - 1]);
        if (!MP4WriteSample(f, m, payload.empty() ? NULL : &payload[0],
                            static_cast<uint32_t>(payload.size()),
                            spec.sampleDuration, 0, sync)) {
            MP4Close(f);
            return false;
        }
    }

    MP4TrackId h = MP4AddHintTrack(f, m);
    if (h == MP4_INVALID_TRACK_ID) {
        MP4Close(f);
        return false;
    }
    uint8_t payloadType = 97;
    MP4SetHintTrackRtpPayload(f, h, "MP4V-ES", &payloadType, 1400);

    for (uint32_t i = 0; i < spec.sampleCount; ++i) {
        Bytes sample;
        if (i < spec.hintSamples.size() && !spec.hintSamples[i].empty()) {
            sample = spec.hintSamples[i];
        } else {
            Put16(sample, 0);
            Put16(sample, 0);
        }
        if (!MP4WriteSample(f, h, sample.empty() ? NULL : &sample[0],
                            static_cast<uint32_t>(sample.size()),
                            spec.sampleDuration, 0, true)) {
            MP4Close(f);
            return false;
        }
    }

    for (size_t i = 0; i < spec.edits.size(); ++i) {
        const EditSpec& e = spec.edits[i];
        MP4Timestamp start = e.empty
            ? static_cast<MP4Timestamp>(UINT64_MAX)
            : static_cast<MP4Timestamp>(e.mediaTime);
        if (MP4AddTrackEdit(f, m, MP4_INVALID_EDIT_ID, start,
                            static_cast<MP4Duration>(e.movieDuration),
                            false) == MP4_INVALID_EDIT_ID) {
            MP4Close(f);
            return false;
        }
    }

    if (spec.hintEdit) {
        if (MP4AddTrackEdit(f, h, MP4_INVALID_EDIT_ID,
                            static_cast<MP4Timestamp>(UINT64_MAX),
                            50, false) == MP4_INVALID_EDIT_ID) {
            MP4Close(f);
            return false;
        }
    }

    MP4Close(f);
    return true;
}

/* Call the API expecting rejection and assert no destination tracks remain. */
static int ExpectReject(const std::string& srcPath, const char* tag,
                        int existingTracks)
{
    MP4FileHandle src = MP4Read(srcPath.c_str());
    TASK_CHECK(src != MP4_INVALID_FILE_HANDLE);
    MP4TrackId sh = MP4FindTrackId(src, 0, MP4_HINT_TRACK_TYPE);
    TASK_CHECK(sh != MP4_INVALID_TRACK_ID);

    std::string dstPath = TPath((std::string("task_reject_") + tag + ".mp4").c_str());
    RemoveFile(dstPath);
    MP4FileHandle dst = MP4Create(dstPath.c_str());
    TASK_CHECK(dst != MP4_INVALID_FILE_HANDLE);

    MP4TrackId keptId = MP4_INVALID_TRACK_ID;
    if (existingTracks > 0) {
        keptId = MP4AddVideoTrack(dst, 1000, 100, 8, 8,
                                  MP4_MPEG4_VIDEO_TYPE);
        TASK_CHECK(keptId != MP4_INVALID_TRACK_ID);
        uint8_t b[2] = {1, 2};
        TASK_CHECK(MP4WriteSample(dst, keptId, b, 2, 100, 0, true));
    }

    MP4TrackId mo = 999;
    MP4TrackId ho = 999;
    bool ok = MP4CopyHintedTrack(src, sh, dst, &mo, &ho);
    TASK_CHECK(!ok);
    TASK_CHECK(mo == MP4_INVALID_TRACK_ID);
    TASK_CHECK(ho == MP4_INVALID_TRACK_ID);
    TASK_CHECK(static_cast<int>(MP4GetNumberOfTracks(dst))
               == existingTracks);
    if (existingTracks > 0) {
        TASK_CHECK(MP4GetTrackNumberOfSamples(dst, keptId) == 1);
    }

    MP4Close(dst);
    MP4Close(src);

    MP4FileHandle re = MP4Read(dstPath.c_str());
    TASK_CHECK(re != MP4_INVALID_FILE_HANDLE);
    TASK_CHECK(static_cast<int>(MP4GetNumberOfTracks(re))
               == existingTracks);
    MP4Close(re);
    RemoveFile(dstPath);
    return 0;
}

/* ------------------------------------------------------------------ */
/* rich occurrence fixture                                             */
/* ------------------------------------------------------------------ */

static std::string BuildRichSource()
{
    std::string path = TPath("task_rich_source.mp4");
    RemoveFile(path);

    MP4FileHandle f = MP4Create(path.c_str());
    if (f == MP4_INVALID_FILE_HANDLE) {
        return std::string();
    }
    MP4SetTimeScale(f, 1000);

    MP4TrackId m = MP4AddVideoTrack(f, 1000, 100, 16, 16,
                                    MP4_MPEG4_VIDEO_TYPE);
    const uint8_t cfg[4] = {0xCA, 0xFE, 0xBA, 0xBE};
    MP4SetTrackESConfiguration(f, m, cfg, sizeof(cfg));

    for (unsigned i = 1; i <= 6; ++i) {
        Bytes b;
        b.push_back(static_cast<uint8_t>('A' + i - 1));
        b.push_back(static_cast<uint8_t>(i));
        b.push_back(static_cast<uint8_t>(0xA0 + i));
        b.push_back(static_cast<uint8_t>(0xB0 + i));
        b.push_back(static_cast<uint8_t>(0xC0 + i));
        b.push_back(static_cast<uint8_t>(0xD0 + i));
        bool sync = (i % 2 == 1); /* 1,3,5 */
        MP4WriteSample(f, m, &b[0], static_cast<uint32_t>(b.size()),
                       100, 0, sync);
    }

    MP4TrackId h = MP4AddHintTrack(f, m);
    uint8_t pt = 96;
    MP4SetHintTrackRtpPayload(f, h, "MP4V-ES", &pt, 1400);

    const int32_t xmit[6] = {0, -3, 7, 100, -100, 1};
    const MP4SampleId mate[6] = {2, 1, 4, 3, 6, 5};

    for (unsigned i = 1; i <= 6; ++i) {
        MP4AddRtpHint(f, h);

        /* Hint samples 1 and 3 carry a self referenced ES config packet. */
        if (i == 1 || i == 3) {
            MP4AddRtpESConfigurationPacket(f, h);
        }

        MP4AddRtpPacket(f, h, false, xmit[i - 1]);
        Bytes imm;
        imm.push_back(static_cast<uint8_t>(0x40 + i));
        if (i % 2 == 0) {
            imm.push_back(0x7e);
        }
        MP4AddRtpImmediateData(f, h, &imm[0],
                               static_cast<uint32_t>(imm.size()));
        /* An ordinary media reference with a non zero offset. */
        MP4AddRtpSampleData(f, h, i, 2, 2);

        MP4AddRtpPacket(f, h, true, 0);
        uint8_t bang = '!';
        MP4AddRtpImmediateData(f, h, &bang, 1);
        MP4AddRtpSampleData(f, h, mate[i - 1], 1, 3);

        MP4WriteRtpHint(f, h, 100, true);
    }

    /* leading empty, descending repeated segments, middle empty, repeat */
    MP4AddTrackEdit(f, m, MP4_INVALID_EDIT_ID,
                    static_cast<MP4Timestamp>(UINT64_MAX), 50);
    MP4AddTrackEdit(f, m, MP4_INVALID_EDIT_ID, 400, 200); /* 5,6 */
    MP4AddTrackEdit(f, m, MP4_INVALID_EDIT_ID, 200, 200); /* 3,4 */
    MP4AddTrackEdit(f, m, MP4_INVALID_EDIT_ID, 0, 200);   /* 1,2 */
    MP4AddTrackEdit(f, m, MP4_INVALID_EDIT_ID,
                    static_cast<MP4Timestamp>(UINT64_MAX), 100);
    MP4AddTrackEdit(f, m, MP4_INVALID_EDIT_ID, 200, 200); /* 3,4 */
    MP4AddTrackEdit(f, m, MP4_INVALID_EDIT_ID, 400, 200); /* 5,6 */

    MP4Close(f);
    return path;
}

/*
 * Verify a rich output.  hintMap[outHintId] is the source hint sample to
 * compare against, 0 means an expected zero packet sample.
 */
static int VerifyRichOutput(const std::string& srcPath,
                            const std::string& outPath)
{
    MP4FileHandle src = MP4Read(srcPath.c_str());
    TASK_CHECK(src != MP4_INVALID_FILE_HANDLE);
    MP4TrackId sh = MP4FindTrackId(src, 0, MP4_HINT_TRACK_TYPE);

    MP4FileHandle out = MP4Read(outPath.c_str());
    TASK_CHECK(out != MP4_INVALID_FILE_HANDLE);
    MP4TrackId om = MP4FindTrackId(out, 0, MP4_VIDEO_TRACK_TYPE);
    MP4TrackId oh = MP4FindTrackId(out, 0, MP4_HINT_TRACK_TYPE);
    TASK_CHECK(om != MP4_INVALID_TRACK_ID);
    TASK_CHECK(oh != MP4_INVALID_TRACK_ID);

    TASK_CHECK(MP4GetNumberOfTracks(out) == 2);
    TASK_CHECK(MP4GetHintTrackReferenceTrackId(out, oh) == om);
    TASK_CHECK(MP4GetTrackNumberOfSamples(out, om) == 10);
    TASK_CHECK(MP4GetTrackNumberOfSamples(out, oh) == 12);
    /* Source sync flags 1,3,5 repeat per copied block -> odd ids sync. */
    for (MP4SampleId k = 1; k <= 10; ++k) {
        TASK_CHECK(MP4GetSampleSync(out, om, k) == (k % 2 == 1 ? 1 : 0));
    }
    TASK_CHECK(MP4GetTrackNumberOfEdits(out, om) == 7);
    TASK_CHECK(MP4GetTrackNumberOfEdits(out, oh) == 0);

    static const uint64_t editStart[8] = {
        0, 0xFFFFFFFFULL, 0, 200, 400, 0xFFFFFFFFULL, 600, 800
    };
    static const MP4Duration editDur[8] = {
        0, 50, 200, 200, 200, 100, 200, 200
    };
    for (MP4EditId e = 1; e <= 7; ++e) {
        TASK_CHECK(static_cast<uint64_t>(
                       MP4GetTrackEditMediaStart(out, om, e))
                   == editStart[e]);
        TASK_CHECK(MP4GetTrackEditDuration(out, om, e) == editDur[e]);
    }

    uint64_t tkhdM = 0, tkhdH = 0, mdhdM = 0, mdhdH = 0;
    TASK_CHECK(MP4GetTrackIntegerProperty(out, om, "tkhd.duration", &tkhdM));
    TASK_CHECK(MP4GetTrackIntegerProperty(out, oh, "tkhd.duration", &tkhdH));
    TASK_CHECK(MP4GetTrackIntegerProperty(out, om, "mdia.mdhd.duration", &mdhdM));
    TASK_CHECK(MP4GetTrackIntegerProperty(out, oh, "mdia.mdhd.duration", &mdhdH));
    TASK_CHECK(tkhdM == 1150 && tkhdH == 1150);
    TASK_CHECK(mdhdM == 1000);
    TASK_CHECK(mdhdH == 1150);

    static const int hintMap[13] = {
        -1, 0, 5, 6, 3, 4, 1, 2, 0, 3, 4, 5, 6
    };
    static const MP4Timestamp expectTime[13] = {
        0, 0, 50, 150, 250, 350, 450, 550, 650, 750, 850, 950, 1050
    };
    static const MP4Duration expectDur[13] = {
        0, 50, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100
    };
    /* Source range start and copied block first id for each output hint. */
    static const uint32_t blockFirst[13] = {
        0, 0, 5, 5, 3, 3, 1, 1, 0, 3, 3, 5, 5
    };
    static const uint32_t newFirst[13] = {
        0, 0, 1, 1, 3, 3, 5, 5, 0, 7, 7, 9, 9
    };

    uint16_t previousSeq = 0;
    bool haveSeq = false;

    for (MP4SampleId id = 1; id <= 12; ++id) {
        TASK_CHECK(MP4GetSampleTime(out, oh, id) == expectTime[id]);
        TASK_CHECK(MP4GetSampleDuration(out, oh, id) == expectDur[id]);

        Bytes raw;
        TASK_CHECK(ReadAllSample(out, oh, id, raw));
        std::vector<RawRef> refs;
        TASK_CHECK(CollectRawRefs(raw, refs));

        uint16_t outCount = 0;
        TASK_CHECK(MP4ReadRtpHint(out, oh, id, &outCount));

        if (hintMap[id] == 0) {
            TASK_CHECK(outCount == 0);
            TASK_CHECK(refs.empty());
            continue;
        }

        MP4SampleId srcId = static_cast<MP4SampleId>(hintMap[id]);

        /* Compare every rewritten reference against its source reference. */
        Bytes srcRaw;
        TASK_CHECK(ReadAllSample(src, sh, srcId, srcRaw));
        std::vector<RawRef> srcRefs;
        TASK_CHECK(CollectRawRefs(srcRaw, srcRefs));
        TASK_CHECK(srcRefs.size() == refs.size());
        for (size_t r = 0; r < refs.size(); ++r) {
            TASK_CHECK(refs[r].entryType == srcRefs[r].entryType);
            TASK_CHECK(refs[r].trackRefIndex == srcRefs[r].trackRefIndex);
            TASK_CHECK(refs[r].length == srcRefs[r].length);
            TASK_CHECK(refs[r].sampleOffset == srcRefs[r].sampleOffset);
            if (refs[r].trackRefIndex == 255) {
                TASK_CHECK(refs[r].sampleNumber == id);
            } else if (refs[r].trackRefIndex == 0) {
                uint32_t expected = newFirst[id]
                    + (srcRefs[r].sampleNumber - blockFirst[id]);
                TASK_CHECK(refs[r].sampleNumber == expected);
            }
        }

        uint16_t srcCount = 0;
        TASK_CHECK(MP4ReadRtpHint(src, sh, srcId, &srcCount));
        TASK_CHECK(outCount == srcCount);

        for (uint16_t p = 0; p < outCount; ++p) {
            uint8_t* ob = NULL;
            uint32_t on = 0;
            TASK_CHECK(MP4ReadRtpPacket(out, oh, p, &ob, &on, 0,
                                        true, true));
            TASK_CHECK(ob != NULL && on >= 12);

            /* Dense global sequence continuity with 16 bit wrap. */
            uint16_t seq = GetBE16(ob + 2);
            if (haveSeq) {
                TASK_CHECK(static_cast<uint16_t>(seq - previousSeq) == 1);
            }
            previousSeq = seq;
            haveSeq = true;

            if (id == 2 && p == 0) {
                /* Stored sequence numbers start at zero. */
                Bytes raw2;
                TASK_CHECK(ReadAllSample(out, oh, 2, raw2));
                TASK_CHECK(raw2.size() > 12);
                TASK_CHECK(raw2[10] == 0 && raw2[11] == 0);
            }

            uint8_t* sb = NULL;
            uint32_t sn = 0;
            TASK_CHECK(MP4ReadRtpPacket(src, sh, p, &sb, &sn, 0,
                                        true, true));
            TASK_CHECK(sb != NULL && sn == on);
            TASK_CHECK(std::memcmp(ob + 1, sb + 1, 1) == 0); /* M + PT */
            TASK_CHECK(std::memcmp(ob + 12, sb + 12, on - 12) == 0);
            TASK_CHECK(MP4GetRtpPacketTransmitOffset(out, oh, p)
                       == MP4GetRtpPacketTransmitOffset(src, sh, p));
            MP4Free(ob);
            MP4Free(sb);
        }
    }

    MP4Close(src);
    MP4Close(out);
    return 0;
}

/* ------------------------------------------------------------------ */
/* test cases                                                          */
/* ------------------------------------------------------------------ */

static int case_occurrence_rich()
{
    std::string srcPath = BuildRichSource();
    TASK_CHECK(!srcPath.empty());

    std::string outPath = TPath("task_rich_direct.mp4");
    RemoveFile(outPath);

    MP4FileHandle src = MP4Read(srcPath.c_str());
    TASK_CHECK(src != MP4_INVALID_FILE_HANDLE);
    MP4TrackId sh = MP4FindTrackId(src, 0, MP4_HINT_TRACK_TYPE);

    MP4FileHandle dst = MP4Create(outPath.c_str());
    TASK_CHECK(dst != MP4_INVALID_FILE_HANDLE);

    MP4TrackId mo = MP4_INVALID_TRACK_ID;
    MP4TrackId ho = MP4_INVALID_TRACK_ID;
    TASK_CHECK(MP4CopyHintedTrack(src, sh, dst, &mo, &ho));
    TASK_CHECK(mo != MP4_INVALID_TRACK_ID);
    TASK_CHECK(ho != MP4_INVALID_TRACK_ID);
    MP4Close(dst);
    MP4Close(src);

    TASK_CHECK(VerifyRichOutput(srcPath, outPath) == 0);

    /* The example binary consumes a test generated input. */
    std::string exampleOut = TPath("task_rich_example.mp4");
    RemoveFile(exampleOut);
    TASK_CHECK(RunExample(srcPath, exampleOut) == 0);
    TASK_CHECK(VerifyRichOutput(srcPath, exampleOut) == 0);

    RemoveFile(outPath);
    RemoveFile(exampleOut);
    RemoveFile(srcPath);
    return 0;
}

/* Raw null / immediate / self embedded instructions and reversed order. */
static int case_occurrence_raw_self()
{
    /*
     * Two samples.  Hint sample 1 carries an immediate plus a self reference
     * to four embedded tail bytes.  Hint sample 2 carries a null instruction
     * and a media reference.  The edit list plays sample 2 then sample 1.
     */
    Bytes tail;
    tail.push_back(0x01);
    tail.push_back(0x02);
    tail.push_back(0x03);
    tail.push_back(0x04);

    std::vector<Bytes> packets1;
    {
        std::vector<Bytes> recs;
        Bytes imm;
        imm.push_back(0xAA);
        recs.push_back(RecordImmediate(imm));
        /* One packet: 4 byte sample header + 12 byte packet + 2 records. */
        uint32_t base = 4 + 12 + 2 * 16;
        recs.push_back(RecordSample(255, 4, 1, base));
        packets1.push_back(RawPacket(0xE0, 5, 0, 0, recs));
    }
    Bytes hint1 = HintSample(packets1, tail);

    Bytes hint2;
    {
        std::vector<Bytes> recs;
        recs.push_back(RecordNull());
        /* h2 belongs to the first occurrence which is the [2,2] segment. */
        recs.push_back(RecordSample(0, 2, 2, 0));
        std::vector<Bytes> packets;
        packets.push_back(RawPacket(0x60, -8, 7, 0, recs));
        hint2 = HintSample(packets);
    }

    RawPairSpec spec;
    spec.path = TPath("task_raw_self.mp4");
    spec.sampleCount = 2;
    spec.hintSamples.resize(2);
    spec.hintSamples[0] = hint1;
    spec.hintSamples[1] = hint2;
    EditSpec e1; e1.empty = false; e1.mediaTime = 100; e1.movieDuration = 100;
    EditSpec e2; e2.empty = false; e2.mediaTime = 0; e2.movieDuration = 100;
    spec.edits.push_back(e1);
    spec.edits.push_back(e2);
    TASK_CHECK(BuildRawPair(spec));

    std::string outPath = TPath("task_raw_self_out.mp4");
    MP4FileHandle src = MP4Read(spec.path.c_str());
    MP4TrackId sh = MP4FindTrackId(src, 0, MP4_HINT_TRACK_TYPE);
    MP4FileHandle dst = MP4Create(outPath.c_str());
    MP4TrackId mo = 0, ho = 0;
    TASK_CHECK(MP4CopyHintedTrack(src, sh, dst, &mo, &ho));
    MP4Close(dst);
    MP4Close(src);

    MP4FileHandle out = MP4Read(outPath.c_str());
    MP4TrackId oh = MP4FindTrackId(out, 0, MP4_HINT_TRACK_TYPE);
    MP4TrackId om = MP4FindTrackId(out, 0, MP4_VIDEO_TRACK_TYPE);
    TASK_CHECK(MP4GetTrackNumberOfSamples(out, om) == 2);
    TASK_CHECK(MP4GetTrackNumberOfSamples(out, oh) == 2);

    /* Output hint 1 is source hint 2 (null + media ref to new sample 2). */
    uint16_t count = 99;
    TASK_CHECK(MP4ReadRtpHint(out, oh, 1, &count));
    TASK_CHECK(count == 1);
    uint8_t* pb = NULL;
    uint32_t pn = 0;
    TASK_CHECK(MP4ReadRtpPacket(out, oh, 0, &pb, &pn, 0, false, true));
    /* null contributes nothing, media sample 2 first two payload bytes. */
    TASK_CHECK(pn == 2 && pb[0] == 0x42 && pb[1] == 0x02);
    MP4Free(pb);

    /* Output hint 2 is source hint 1; self id rebased to 2, bytes intact. */
    TASK_CHECK(MP4ReadRtpHint(out, oh, 2, &count));
    TASK_CHECK(count == 1);
    pb = NULL; pn = 0;
    TASK_CHECK(MP4ReadRtpPacket(out, oh, 0, &pb, &pn, 0, false, true));
    TASK_CHECK(pn == 5);
    TASK_CHECK(pb[0] == 0xAA);
    TASK_CHECK(pb[1] == 1 && pb[2] == 2 && pb[3] == 3 && pb[4] == 4);
    MP4Free(pb);

    /* The self instruction must remain a type 2 sample entry. */
    Bytes raw;
    TASK_CHECK(ReadAllSample(out, oh, 2, raw));
    std::vector<RawRef> refs;
    TASK_CHECK(CollectRawRefs(raw, refs));
    bool sawSelf = false;
    for (size_t i = 0; i < refs.size(); ++i) {
        if (refs[i].trackRefIndex == 255) {
            sawSelf = true;
            TASK_CHECK(refs[i].entryType == 2);
            TASK_CHECK(refs[i].sampleNumber == 2);
        }
        if (refs[i].trackRefIndex == 0) {
            /* Source hint 1 had no media reference; this is hint 2's. */
            TASK_CHECK(refs[i].sampleNumber == 2);
        }
    }
    TASK_CHECK(sawSelf);

    MP4Close(out);
    RemoveFile(spec.path);
    RemoveFile(outPath);
    return 0;
}

/* No edit list means copy the whole track once. */
static int case_occurrence_no_edits()
{
    RawPairSpec spec;
    spec.path = TPath("task_noedits.mp4");
    spec.sampleCount = 3;
    spec.allSync = true;
    for (uint32_t i = 0; i < 3; ++i) {
        std::vector<Bytes> recs;
        Bytes imm;
        imm.push_back(static_cast<uint8_t>(0xD0 + i));
        recs.push_back(RecordImmediate(imm));
        std::vector<Bytes> packets;
        packets.push_back(RawPacket(0x60, 0, static_cast<uint16_t>(i), 0, recs));
        spec.hintSamples.push_back(HintSample(packets));
    }
    TASK_CHECK(BuildRawPair(spec));

    std::string outPath = TPath("task_noedits_out.mp4");
    MP4FileHandle src = MP4Read(spec.path.c_str());
    MP4TrackId sh = MP4FindTrackId(src, 0, MP4_HINT_TRACK_TYPE);
    MP4FileHandle dst = MP4Create(outPath.c_str());
    MP4TrackId mo = 0, ho = 0;
    TASK_CHECK(MP4CopyHintedTrack(src, sh, dst, &mo, &ho));
    MP4Close(dst);
    MP4Close(src);

    MP4FileHandle re = MP4Read(outPath.c_str());
    MP4TrackId om = MP4FindTrackId(re, 0, MP4_VIDEO_TRACK_TYPE);
    MP4TrackId oh = MP4FindTrackId(re, 0, MP4_HINT_TRACK_TYPE);
    TASK_CHECK(MP4GetTrackNumberOfSamples(re, om) == 3);
    TASK_CHECK(MP4GetTrackNumberOfSamples(re, oh) == 3);
    TASK_CHECK(MP4GetTrackNumberOfEdits(re, om) == 0);
    TASK_CHECK(MP4GetTrackNumberOfEdits(re, oh) == 0);
    for (MP4SampleId i = 1; i <= 3; ++i) {
        TASK_CHECK(MP4GetSampleTime(re, oh, i) == (i - 1) * 100);
    }
    uint64_t tkhdN = 0, tkhdH = 0;
    MP4GetTrackIntegerProperty(re, om, "tkhd.duration", &tkhdN);
    MP4GetTrackIntegerProperty(re, oh, "tkhd.duration", &tkhdH);
    TASK_CHECK(tkhdN == 300 && tkhdH == 300);
    MP4Close(re);
    RemoveFile(spec.path);
    RemoveFile(outPath);
    return 0;
}

/* Exact movie/media timescale conversion. */
static int RunClockCheck(uint32_t dstMovieScale, uint64_t expectTkhd)
{
    std::string srcPath = TPath("task_clock_source.mp4");
    RemoveFile(srcPath);
    MP4FileHandle f = MP4Create(srcPath.c_str());
    TASK_CHECK(f != MP4_INVALID_FILE_HANDLE);
    MP4SetTimeScale(f, 600);
    MP4TrackId m = MP4AddVideoTrack(f, 1200, 200, 16, 16,
                                    MP4_MPEG4_VIDEO_TYPE);
    for (unsigned i = 1; i <= 4; ++i) {
        uint8_t b[3] = {static_cast<uint8_t>(i), 0x10, 0x20};
        MP4WriteSample(f, m, b, 3, 200, 0, true);
    }
    MP4TrackId h = MP4AddHintTrack(f, m);
    uint8_t pt = 96;
    MP4SetHintTrackRtpPayload(f, h, "MP4V-ES", &pt, 1400);
    for (unsigned i = 1; i <= 4; ++i) {
        MP4AddRtpHint(f, h);
        MP4AddRtpPacket(f, h, true, 0);
        uint8_t x = static_cast<uint8_t>(i);
        MP4AddRtpImmediateData(f, h, &x, 1);
        MP4WriteRtpHint(f, h, 200, true);
    }
    MP4AddTrackEdit(f, m, MP4_INVALID_EDIT_ID,
                    static_cast<MP4Timestamp>(UINT64_MAX), 30);
    MP4AddTrackEdit(f, m, MP4_INVALID_EDIT_ID, 200, 300); /* s2..4 */
    MP4AddTrackEdit(f, m, MP4_INVALID_EDIT_ID, 0, 200);   /* s1..2 */
    MP4Close(f);

    MP4FileHandle src = MP4Read(srcPath.c_str());
    MP4TrackId sh = MP4FindTrackId(src, 0, MP4_HINT_TRACK_TYPE);
    std::string outPath = TPath(
        dstMovieScale == 600 ? "task_clock600.mp4" : "task_clock300.mp4");
    RemoveFile(outPath);
    MP4FileHandle dst = MP4Create(outPath.c_str());
    MP4SetTimeScale(dst, dstMovieScale);
    MP4TrackId mo = 0, ho = 0;
    TASK_CHECK(MP4CopyHintedTrack(src, sh, dst, &mo, &ho));
    MP4Close(dst);
    MP4Close(src);

    MP4FileHandle re = MP4Read(outPath.c_str());
    MP4TrackId om = MP4FindTrackId(re, 0, MP4_VIDEO_TRACK_TYPE);
    MP4TrackId oh = MP4FindTrackId(re, 0, MP4_HINT_TRACK_TYPE);
    TASK_CHECK(MP4GetTrackNumberOfSamples(re, om) == 5);
    TASK_CHECK(MP4GetTrackNumberOfSamples(re, oh) == 6);

    static const MP4Timestamp expectTime[7] = {
        0, 0, 60, 260, 460, 660, 860
    };
    for (MP4SampleId i = 1; i <= 6; ++i) {
        TASK_CHECK(MP4GetSampleTime(re, oh, i) == expectTime[i]);
    }
    TASK_CHECK(MP4GetSampleDuration(re, oh, 1) == 60);

    uint64_t tkhdM = 0, tkhdH = 0, mdhdM = 0, mdhdH = 0;
    MP4GetTrackIntegerProperty(re, om, "tkhd.duration", &tkhdM);
    MP4GetTrackIntegerProperty(re, oh, "tkhd.duration", &tkhdH);
    MP4GetTrackIntegerProperty(re, om, "mdia.mdhd.duration", &mdhdM);
    MP4GetTrackIntegerProperty(re, oh, "mdia.mdhd.duration", &mdhdH);
    TASK_CHECK(tkhdM == expectTkhd && tkhdH == expectTkhd);
    TASK_CHECK(mdhdM == 1000);
    TASK_CHECK(mdhdH == 1060);

    /* First hint sample is a zero packet sample of 60 media ticks. */
    uint16_t pc = 77;
    TASK_CHECK(MP4ReadRtpHint(re, oh, 1, &pc) && pc == 0);

    MP4Close(re);
    RemoveFile(outPath);
    RemoveFile(srcPath);
    return 0;
}

static int case_clock_scaled()
{
    TASK_CHECK(RunClockCheck(600, 530) == 0);
    TASK_CHECK(RunClockCheck(300, 265) == 0);
    return 0;
}

/* ---------------- reject / compatibility cases -------------------- */

static int case_reject_media_refs()
{
    /* Reference outside the single selected segment [3,4]. */
    {
        Bytes ref = HintSample(std::vector<Bytes>(1,
            RawPacket(0x60, 0, 0, 0,
                      std::vector<Bytes>(1, RecordSample(0, 1, 1, 0)))));
        RawPairSpec spec;
        spec.path = TPath("task_bad_outseg.mp4");
        spec.sampleCount = 4;
        spec.hintSamples.resize(4);
        spec.hintSamples[2] = ref; /* hint sample 3 */
        EditSpec e; e.empty = false; e.mediaTime = 200; e.movieDuration = 200;
        spec.edits.push_back(e);
        TASK_CHECK(BuildRawPair(spec));
        TASK_CHECK(ExpectReject(spec.path, "outseg", 1) == 0);
        RemoveFile(spec.path);
    }
    /* Reference sample id out of range. */
    {
        Bytes ref = HintSample(std::vector<Bytes>(1,
            RawPacket(0x60, 0, 0, 0,
                      std::vector<Bytes>(1, RecordSample(0, 1, 99, 0)))));
        RawPairSpec spec;
        spec.path = TPath("task_bad_range.mp4");
        spec.sampleCount = 2;
        spec.hintSamples.push_back(ref);
        EditSpec e; e.empty = false; e.mediaTime = 0; e.movieDuration = 200;
        spec.edits.push_back(e);
        TASK_CHECK(BuildRawPair(spec));
        TASK_CHECK(ExpectReject(spec.path, "range", 0) == 0);
        RemoveFile(spec.path);
    }
    /* Reference slice past the end of the media sample. */
    {
        Bytes ref = HintSample(std::vector<Bytes>(1,
            RawPacket(0x60, 0, 0, 0,
                      std::vector<Bytes>(1,
                          RecordSample(0, 8, 1, 100)))));
        RawPairSpec spec;
        spec.path = TPath("task_bad_bounds.mp4");
        spec.sampleCount = 1;
        spec.hintSamples.push_back(ref);
        TASK_CHECK(BuildRawPair(spec));
        TASK_CHECK(ExpectReject(spec.path, "bounds", 0) == 0);
        RemoveFile(spec.path);
    }
    return 0;
}

static int case_reject_hint_constructs()
{
    /* Cross hint sample self reference. */
    {
        Bytes ref = HintSample(std::vector<Bytes>(1,
            RawPacket(0x60, 0, 0, 0,
                      std::vector<Bytes>(1,
                          RecordSample(255, 2, 2, 4)))));
        RawPairSpec spec;
        spec.path = TPath("task_bad_crossself.mp4");
        spec.sampleCount = 2;
        spec.hintSamples.push_back(ref);
        TASK_CHECK(BuildRawPair(spec));
        TASK_CHECK(ExpectReject(spec.path, "crossself", 0) == 0);
        RemoveFile(spec.path);
    }
    /* Unsupported track reference index. */
    {
        Bytes ref = HintSample(std::vector<Bytes>(1,
            RawPacket(0x60, 0, 0, 0,
                      std::vector<Bytes>(1, RecordSample(3, 1, 1, 0)))));
        RawPairSpec spec;
        spec.path = TPath("task_bad_refindex.mp4");
        spec.sampleCount = 1;
        spec.hintSamples.push_back(ref);
        TASK_CHECK(BuildRawPair(spec));
        TASK_CHECK(ExpectReject(spec.path, "refindex", 0) == 0);
        RemoveFile(spec.path);
    }
    /* Sample description instruction (type 3). */
    {
        Bytes ref = HintSample(std::vector<Bytes>(1,
            RawPacket(0x60, 0, 0, 0,
                      std::vector<Bytes>(1, RecordSampleDescription()))));
        RawPairSpec spec;
        spec.path = TPath("task_bad_typedesc.mp4");
        spec.sampleCount = 1;
        spec.hintSamples.push_back(ref);
        TASK_CHECK(BuildRawPair(spec));
        TASK_CHECK(ExpectReject(spec.path, "typedesc", 0) == 0);
        RemoveFile(spec.path);
    }
    /* Packet extra information flag. */
    {
        Bytes ref = HintSample(std::vector<Bytes>(1,
            RawPacket(0x60, 0, 0, 0x0004, std::vector<Bytes>())));
        RawPairSpec spec;
        spec.path = TPath("task_bad_extra.mp4");
        spec.sampleCount = 1;
        spec.hintSamples.push_back(ref);
        TASK_CHECK(BuildRawPair(spec));
        TASK_CHECK(ExpectReject(spec.path, "extra", 0) == 0);
        RemoveFile(spec.path);
    }
    /* Non trivial block layout. */
    {
        Bytes ref = HintSample(std::vector<Bytes>(1,
            RawPacket(0x60, 0, 0, 0,
                      std::vector<Bytes>(1,
                          RecordSample(0, 1, 1, 0, 2, 1)))));
        RawPairSpec spec;
        spec.path = TPath("task_bad_block.mp4");
        spec.sampleCount = 1;
        spec.hintSamples.push_back(ref);
        TASK_CHECK(BuildRawPair(spec));
        TASK_CHECK(ExpectReject(spec.path, "block", 0) == 0);
        RemoveFile(spec.path);
    }
    /* Hint track carrying its own edit list. */
    {
        RawPairSpec spec;
        spec.path = TPath("task_bad_hintedit.mp4");
        spec.sampleCount = 1;
        spec.hintEdit = true;
        TASK_CHECK(BuildRawPair(spec));
        TASK_CHECK(ExpectReject(spec.path, "hintedit", 0) == 0);
        RemoveFile(spec.path);
    }
    return 0;
}

static int case_reject_clock_empty_sync()
{
    /* Non integral movie -> media timescale conversion. */
    {
        RawPairSpec spec;
        spec.path = TPath("task_bad_div.mp4");
        spec.movieTimeScale = 30;
        spec.trackTimeScale = 100;
        spec.sampleCount = 3;
        spec.sampleDuration = 10;
        EditSpec e; e.empty = false; e.mediaTime = 0; e.movieDuration = 1;
        spec.edits.push_back(e);
        TASK_CHECK(BuildRawPair(spec));
        TASK_CHECK(ExpectReject(spec.path, "div", 0) == 0);
        RemoveFile(spec.path);
    }
    /* All empty edit list. */
    {
        RawPairSpec spec;
        spec.path = TPath("task_allempty.mp4");
        spec.sampleCount = 2;
        EditSpec a; a.empty = true; a.movieDuration = 50;
        EditSpec b; b.empty = true; b.movieDuration = 70;
        spec.edits.push_back(a);
        spec.edits.push_back(b);
        TASK_CHECK(BuildRawPair(spec));
        TASK_CHECK(ExpectReject(spec.path, "allempty", 0) == 0);
        RemoveFile(spec.path);
    }
    /* Segment start that is not a sync sample. */
    {
        RawPairSpec spec;
        spec.path = TPath("task_bad_sync.mp4");
        spec.sampleCount = 3;
        spec.allSync = false;
        spec.sync.resize(3);
        spec.sync[0] = true;
        spec.sync[1] = false;
        spec.sync[2] = true;
        EditSpec e; e.empty = false; e.mediaTime = 100; e.movieDuration = 200;
        spec.edits.push_back(e);
        TASK_CHECK(BuildRawPair(spec));
        TASK_CHECK(ExpectReject(spec.path, "sync", 0) == 0);
        RemoveFile(spec.path);
    }
    return 0;
}

/* Legacy single track copy/clone paths stay usable. */
static int case_reject_compat_legacy()
{
    std::string srcPath = TPath("task_legacy_src.mp4");
    RemoveFile(srcPath);
    MP4FileHandle f = MP4Create(srcPath.c_str());
    MP4SetTimeScale(f, 1000);
    MP4TrackId m = MP4AddVideoTrack(f, 1000, 100, 16, 16,
                                    MP4_MPEG4_VIDEO_TYPE);
    for (unsigned i = 1; i <= 3; ++i) {
        Bytes b;
        b.push_back(static_cast<uint8_t>('a' + i - 1));
        b.push_back(static_cast<uint8_t>(i));
        MP4WriteSample(f, m, &b[0], 2, 100, 0, true);
    }
    MP4Close(f);

    std::string copyPath = TPath("task_legacy_copy.mp4");
    RemoveFile(copyPath);
    MP4FileHandle src = MP4Read(srcPath.c_str());
    MP4TrackId sm = MP4FindTrackId(src, 0, MP4_VIDEO_TRACK_TYPE);
    MP4FileHandle dst = MP4Create(copyPath.c_str());
    MP4TrackId dm = MP4CopyTrack(src, sm, dst, false);
    TASK_CHECK(dm != MP4_INVALID_TRACK_ID);
    MP4Close(dst);
    MP4Close(src);

    MP4FileHandle re = MP4Read(copyPath.c_str());
    MP4TrackId rm = MP4FindTrackId(re, 0, MP4_VIDEO_TRACK_TYPE);
    TASK_CHECK(MP4GetTrackNumberOfSamples(re, rm) == 3);
    for (MP4SampleId i = 1; i <= 3; ++i) {
        uint8_t* p = NULL;
        uint32_t n = 0;
        TASK_CHECK(MP4ReadSample(re, rm, i, &p, &n, NULL, NULL, NULL, NULL));
        TASK_CHECK(n == 2);
        TASK_CHECK(p[0] == static_cast<uint8_t>('a' + i - 1));
        MP4Free(p);
    }
    MP4Close(re);

    /* MP4CloneTrack still creates an empty compatible skeleton. */
    MP4FileHandle s2 = MP4Read(srcPath.c_str());
    MP4TrackId sm2 = MP4FindTrackId(s2, 0, MP4_VIDEO_TRACK_TYPE);
    std::string clonePath = TPath("task_legacy_clone.mp4");
    RemoveFile(clonePath);
    MP4FileHandle d2 = MP4Create(clonePath.c_str());
    MP4TrackId cm = MP4CloneTrack(s2, sm2, d2);
    TASK_CHECK(cm != MP4_INVALID_TRACK_ID);
    TASK_CHECK(MP4GetTrackNumberOfSamples(d2, cm) == 0);
    MP4Close(d2);
    MP4Close(s2);

    RemoveFile(srcPath);
    RemoveFile(copyPath);
    RemoveFile(clonePath);
    return 0;
}

/* The destination may already contain unrelated tracks. */
static int case_occurrence_dst_existing()
{
    std::string srcPath = BuildRichSource();
    TASK_CHECK(!srcPath.empty());

    std::string dstPath = TPath("task_existing_dst.mp4");
    RemoveFile(dstPath);

    MP4FileHandle dst = MP4Create(dstPath.c_str());
    TASK_CHECK(dst != MP4_INVALID_FILE_HANDLE);
    MP4TrackId keep = MP4AddVideoTrack(dst, 1000, 100, 12, 12,
                                       MP4_MPEG4_VIDEO_TYPE);
    TASK_CHECK(keep != MP4_INVALID_TRACK_ID);
    uint8_t kb[3] = {9, 8, 7};
    TASK_CHECK(MP4WriteSample(dst, keep, kb, 3, 100, 0, true));

    MP4FileHandle src = MP4Read(srcPath.c_str());
    MP4TrackId sh = MP4FindTrackId(src, 0, MP4_HINT_TRACK_TYPE);

    MP4TrackId mo = 0, ho = 0;
    TASK_CHECK(MP4CopyHintedTrack(src, sh, dst, &mo, &ho));
    TASK_CHECK(mo != keep);
    TASK_CHECK(MP4GetNumberOfTracks(dst) == 3);
    MP4Close(dst);
    MP4Close(src);

    MP4FileHandle re = MP4Read(dstPath.c_str());
    TASK_CHECK(MP4GetNumberOfTracks(re) == 3);
    TASK_CHECK(MP4GetTrackNumberOfSamples(re, keep) == 1);
    uint8_t* p = NULL;
    uint32_t n = 0;
    TASK_CHECK(MP4ReadSample(re, keep, 1, &p, &n, NULL, NULL, NULL, NULL));
    TASK_CHECK(n == 3 && p[0] == 9 && p[2] == 7);
    MP4Free(p);
    MP4TrackId oh = MP4FindTrackId(re, 0, MP4_HINT_TRACK_TYPE);
    TASK_CHECK(MP4GetTrackNumberOfSamples(re, oh) == 12);
    MP4Close(re);

    RemoveFile(srcPath);
    RemoveFile(dstPath);
    return 0;
}

/* ------------------------------------------------------------------ */
/* registration                                                        */
/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
    { "occurrence", "rich_reverse_repeat", case_occurrence_rich },
    { "occurrence", "raw_null_self",       case_occurrence_raw_self },
    { "occurrence", "no_edits_whole",      case_occurrence_no_edits },
    { "occurrence", "dst_existing_tracks", case_occurrence_dst_existing },
    { "clock",      "scaled_timebase",     case_clock_scaled },
    { "reject_compat", "bad_media_refs",       case_reject_media_refs },
    { "reject_compat", "bad_hint_constructs",  case_reject_hint_constructs },
    { "reject_compat", "clock_empty_sync",     case_reject_clock_empty_sync },
    { "reject_compat", "legacy_copy_clone",    case_reject_compat_legacy },
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);

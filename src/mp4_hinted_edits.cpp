/*
 * MP4CopyHintedTrack - rebase RTP hint sample references while applying a
 * media track edit list as independent segment occurrences.
 *
 * The implementation only uses the public mp4v2 C API and parses/rewrites the
 * well known RTP hint sample binary layout itself.
 */

#include <mp4v2/mp4v2.h>

#include <vector>
#include <string>
#include <cstdint>
#include <cstddef>
#include <cstdio>

namespace {

/* Exact x * mul / div.  Fails on overflow or a non integral result. */
bool ExactScale(uint64_t x, uint64_t mul, uint64_t div, uint64_t* out)
{
    if (div == 0 || mul == 0 || out == NULL) {
        return false;
    }
    if (x != 0 && mul > UINT64_MAX / x) {
        return false;
    }
    const uint64_t product = x * mul;
    if (product % div != 0) {
        return false;
    }
    *out = product / div;
    return true;
}

bool AddChecked(uint64_t a, uint64_t b, uint64_t* out)
{
    if (b > UINT64_MAX - a) {
        return false;
    }
    *out = a + b;
    return true;
}

uint16_t ReadBE16(const uint8_t* p)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

uint32_t ReadBE32(const uint8_t* p)
{
    return (static_cast<uint32_t>(p[0]) << 24)
         | (static_cast<uint32_t>(p[1]) << 16)
         | (static_cast<uint32_t>(p[2]) << 8)
         |  static_cast<uint32_t>(p[3]);
}

void WriteBE16(std::vector<uint8_t>& buf, size_t offset, uint16_t value)
{
    buf[offset]     = static_cast<uint8_t>(value >> 8);
    buf[offset + 1] = static_cast<uint8_t>(value & 0xff);
}

void WriteBE32(std::vector<uint8_t>& buf, size_t offset, uint32_t value)
{
    buf[offset]     = static_cast<uint8_t>(value >> 24);
    buf[offset + 1] = static_cast<uint8_t>(value >> 16);
    buf[offset + 2] = static_cast<uint8_t>(value >> 8);
    buf[offset + 3] = static_cast<uint8_t>(value & 0xff);
}

struct RefLocation {
    size_t sampleNumberOffset;
    uint8_t  trackRefIndex;
    uint16_t length;
    uint32_t sampleNumber;
    uint32_t sampleOffset;
};

struct ParsedHint {
    uint16_t packetCount;
    std::vector<size_t> sequenceOffsets;
    std::vector<RefLocation> refs;
};

/*
 * Parse one RTP hint sample.  Every data entry in the hint sample is a fixed
 * 16 byte record.  Only null(0), immediate(1) and ordinary sample(2) entries
 * with trivial block layout are accepted.  Packet extra information (the
 * extra flag) and sample-description entries (3) are rejected.
 */
bool ParseHintSample(const uint8_t* data, size_t size, ParsedHint& hint)
{
    if (data == NULL || size < 4) {
        return false;
    }

    hint.packetCount = ReadBE16(data);
    /* bytes 2..3 are reserved */

    size_t pos = 4;

    for (uint16_t packetIndex = 0; packetIndex < hint.packetCount; ++packetIndex) {
        if (pos + 12 > size) {
            return false;
        }

        const uint16_t flagBits = ReadBE16(data + pos + 8);
        /* reserved3:13 extraFlag:1 bFrameFlag:1 repeatFlag:1 */
        if ((flagBits & 0x0004) != 0) {
            /* RTP extra information is outside the supported scope. */
            return false;
        }

        hint.sequenceOffsets.push_back(pos + 6);
        const uint16_t entryCount = ReadBE16(data + pos + 10);
        pos += 12;

        for (uint16_t entryIndex = 0; entryIndex < entryCount; ++entryIndex) {
            if (pos + 16 > size) {
                return false;
            }

            const uint8_t entryType = data[pos];

            if (entryType == 0) {
                /* null data entry: type + 15 pad bytes */
            } else if (entryType == 1) {
                /* immediate data entry: count + 14 fixed data bytes */
                const uint8_t count = data[pos + 1];
                if (count > 14) {
                    return false;
                }
            } else if (entryType == 2) {
                const uint8_t  refIndex    = data[pos + 1];
                const uint16_t length      = ReadBE16(data + pos + 2);
                const uint32_t sampleNum   = ReadBE32(data + pos + 4);
                const uint32_t sampleOff   = ReadBE32(data + pos + 8);
                const uint16_t bytesPerBlock   = ReadBE16(data + pos + 12);
                const uint16_t samplesPerBlock = ReadBE16(data + pos + 14);

                /* Only ordinary contiguous references are supported. */
                if (bytesPerBlock != 1 || samplesPerBlock != 1) {
                    return false;
                }
                if (sampleNum == MP4_INVALID_SAMPLE_ID) {
                    return false;
                }

                RefLocation ref;
                ref.sampleNumberOffset = pos + 4;
                ref.trackRefIndex = refIndex;
                ref.length = length;
                ref.sampleNumber = sampleNum;
                ref.sampleOffset = sampleOff;
                hint.refs.push_back(ref);
            } else {
                /* Type 3 (sample description) and unknown types unsupported. */
                return false;
            }

            pos += 16;
        }
    }

    /*
     * After the fixed records come embedded data bytes referenced by self
     * entries; they are not walked as records.  Any such range is bounds
     * checked against the full sample size by the caller.
     */
    return pos <= size;
}

struct SampleClock {
    MP4Timestamp time;
    MP4Duration  duration;
};

struct Segment {
    bool     empty;
    uint64_t sourceMovieDuration; /* elst segmentDuration, source movie scale */
    uint64_t mediaDuration;       /* exact value in media/hint timescale */
    uint64_t destMovieDuration;   /* exact value in dest movie timescale */
    uint32_t firstSample;         /* non-empty: first source sample id */
    uint32_t lastSample;          /* non-empty: last source sample id */
    uint64_t newMediaTime;        /* mediaStart of the output edit */
    uint32_t newFirstMediaSample; /* mapped id of firstSample after copy */
};

struct Copier {
    MP4FileHandle src;
    MP4TrackId    srcHint;
    MP4TrackId    srcMedia;
    MP4FileHandle dst;
    MP4TrackId    media; /* destination media track */
    MP4TrackId    hint;  /* destination hint track */

    uint32_t mediaCount;
    uint32_t hintCount;
    uint32_t sourceSamples;
    uint32_t mediaTimeScale;
    uint32_t hintTimeScale;
    uint32_t sourceMovieTimeScale;
    uint32_t destMovieTimeScale;

    std::vector<SampleClock> clock; /* indexed by sample id, 1 based */
    std::vector<uint32_t> sampleSizes;
    std::vector<ParsedHint> parsedHints; /* indexed by sample id */
    std::vector<Segment> segments;
    bool haveEdits;

    Copier()
        : src(NULL), srcHint(MP4_INVALID_TRACK_ID),
          srcMedia(MP4_INVALID_TRACK_ID), dst(NULL),
          media(MP4_INVALID_TRACK_ID), hint(MP4_INVALID_TRACK_ID),
          mediaCount(0), hintCount(0), sourceSamples(0),
          mediaTimeScale(0), hintTimeScale(0),
          sourceMovieTimeScale(0), destMovieTimeScale(0),
          haveEdits(false)
    {
    }

    bool ReadSampleBytes(MP4TrackId track, MP4SampleId id,
                         std::vector<uint8_t>& bytes,
                         MP4Duration* duration = NULL,
                         bool* sync = NULL)
    {
        uint8_t* p = NULL;
        uint32_t size = 0;
        MP4Duration dur = MP4_INVALID_DURATION;
        bool isSync = false;

        if (!MP4ReadSample(src, track, id, &p, &size,
                           NULL, &dur, NULL, &isSync)) {
            if (p != NULL) MP4Free(p);
            return false;
        }
        if (p == NULL) {
            return false;
        }
        bytes.assign(p, p + size);
        MP4Free(p);
        if (duration) *duration = dur;
        if (sync) *sync = isSync;
        return true;
    }

    bool GetU64Property(MP4FileHandle file, MP4TrackId track,
                        const char* name, uint64_t* value)
    {
        uint64_t v = 0;
        if (!MP4GetTrackIntegerProperty(file, track, name, &v)) {
            return false;
        }
        *value = v;
        return true;
    }

    /* Read and validate every timetable entry and every hint sample. */
    bool Preflight()
    {
        if (!MP4_IS_VALID_FILE_HANDLE(src) || !MP4_IS_VALID_FILE_HANDLE(dst)) {
            return false;
        }
        if (src == dst || srcHint == MP4_INVALID_TRACK_ID) {
            return false;
        }

        const char* hintType = MP4GetTrackType(src, srcHint);
        if (hintType == NULL || !MP4_IS_HINT_TRACK_TYPE(hintType)) {
            return false;
        }

        srcMedia = MP4GetHintTrackReferenceTrackId(src, srcHint);
        if (srcMedia == MP4_INVALID_TRACK_ID) {
            return false;
        }

        const char* mediaType = MP4GetTrackType(src, srcMedia);
        if (mediaType == NULL || !MP4_IS_VIDEO_TRACK_TYPE(mediaType)) {
            return false;
        }

        const char* dataName = MP4GetTrackMediaDataName(src, srcMedia);
        if (dataName == NULL ||
            (strcmp(dataName, "avc1") != 0 && strcmp(dataName, "mp4v") != 0)) {
            return false;
        }

        /* Exactly one sample description in each track. */
        uint64_t mediaDescriptions = 0;
        uint64_t hintDescriptions = 0;
        if (!GetU64Property(src, srcMedia,
                            "mdia.minf.stbl.stsd.entryCount",
                            &mediaDescriptions)
            || mediaDescriptions != 1) {
            return false;
        }
        if (!GetU64Property(src, srcHint,
                            "mdia.minf.stbl.stsd.entryCount",
                            &hintDescriptions)
            || hintDescriptions != 1) {
            return false;
        }

        mediaTimeScale = MP4GetTrackTimeScale(src, srcMedia);
        hintTimeScale = MP4GetTrackTimeScale(src, srcHint);
        sourceMovieTimeScale = MP4GetTimeScale(src);
        destMovieTimeScale = MP4GetTimeScale(dst);
        if (mediaTimeScale == 0 || hintTimeScale == 0
            || sourceMovieTimeScale == 0 || destMovieTimeScale == 0) {
            return false;
        }
        if (mediaTimeScale != hintTimeScale) {
            return false;
        }

        const uint32_t mediaSamples =
            MP4GetTrackNumberOfSamples(src, srcMedia);
        const uint32_t hintSamples =
            MP4GetTrackNumberOfSamples(src, srcHint);
        if (mediaSamples == 0 || mediaSamples != hintSamples) {
            return false;
        }
        sourceSamples = mediaSamples;

        /* The source hint track must not carry its own edit list. */
        if (MP4GetTrackNumberOfEdits(src, srcHint) != 0) {
            return false;
        }

        clock.assign(sourceSamples + 1, SampleClock());
        sampleSizes.assign(sourceSamples + 1, 0);
        parsedHints.assign(sourceSamples + 1, ParsedHint());

        for (uint32_t i = 1; i <= sourceSamples; ++i) {
            const MP4Timestamp mediaTime = MP4GetSampleTime(src, srcMedia, i);
            const MP4Duration mediaDur = MP4GetSampleDuration(src, srcMedia, i);
            const MP4Duration mediaOffset =
                MP4GetSampleRenderingOffset(src, srcMedia, i);
            const MP4Timestamp hintTime = MP4GetSampleTime(src, srcHint, i);
            const MP4Duration hintDur = MP4GetSampleDuration(src, srcHint, i);

            if (!MP4_IS_VALID_TIMESTAMP(mediaTime)
                || !MP4_IS_VALID_DURATION(mediaDur)
                || !MP4_IS_VALID_DURATION(mediaOffset)
                || !MP4_IS_VALID_TIMESTAMP(hintTime)
                || !MP4_IS_VALID_DURATION(hintDur)) {
                return false;
            }
            if (mediaDur == 0 || mediaDur == MP4_INVALID_DURATION) {
                return false;
            }
            /* No composition offsets in the supported input range. */
            if (mediaOffset != 0) {
                return false;
            }
            /* Identical per sample DTS and duration on both tracks. */
            if (mediaTime != hintTime || mediaDur != hintDur) {
                return false;
            }
            /* Strict DTS contiguity. */
            if (i > 1) {
                uint64_t expected = 0;
                if (!AddChecked(clock[i - 1].time,
                                clock[i - 1].duration, &expected)
                    || mediaTime != expected) {
                    return false;
                }
            }

            clock[i].time = mediaTime;
            clock[i].duration = mediaDur;

            sampleSizes[i] = MP4GetSampleSize(src, srcMedia, i);
            if (sampleSizes[i] == 0) {
                return false;
            }
        }

        /* Hint samples are parsed in a second pass so that forward media
         * references can be bounds checked against every media sample. */
        for (uint32_t i = 1; i <= sourceSamples; ++i) {
            std::vector<uint8_t> hintBytes;
            if (!ReadSampleBytes(srcHint, i, hintBytes)) {
                return false;
            }
            if (!ParseHintSample(hintBytes.empty() ? NULL : &hintBytes[0],
                                 hintBytes.size(), parsedHints[i])) {
                return false;
            }

            for (size_t r = 0; r < parsedHints[i].refs.size(); ++r) {
                const RefLocation& ref = parsedHints[i].refs[r];

                if (ref.trackRefIndex == 0) {
                    /* Media reference: global range/bounds check.  Segment
                     * membership is checked per edit occurrence below. */
                    if (ref.sampleNumber < 1
                        || ref.sampleNumber > sourceSamples) {
                        return false;
                    }
                    uint64_t end = 0;
                    if (!AddChecked(ref.sampleOffset, ref.length, &end)
                        || end > sampleSizes[ref.sampleNumber]) {
                        return false;
                    }
                } else if (ref.trackRefIndex == 255) {
                    /* Self reference may only target this same hint sample. */
                    if (ref.sampleNumber != i) {
                        return false;
                    }
                    uint64_t end = 0;
                    if (!AddChecked(ref.sampleOffset, ref.length, &end)
                        || end > hintBytes.size()) {
                        return false;
                    }
                } else {
                    /* Other tracks are not supported. */
                    return false;
                }
            }
        }

        return BuildEditPlan();
    }

    /* Find the sample id whose DTS equals time. */
    bool SampleAtTime(uint64_t time, uint32_t* sampleId)
    {
        for (uint32_t i = 1; i <= sourceSamples; ++i) {
            if (clock[i].time == time) {
                *sampleId = i;
                return true;
            }
        }
        return false;
    }

    bool BuildEditPlan()
    {
        const uint32_t editCount = MP4GetTrackNumberOfEdits(src, srcMedia);

        if (editCount == 0) {
            /* No edit list: copy the whole track as one occurrence. */
            Segment seg;
            seg.empty = false;
            seg.sourceMovieDuration = 0;
            seg.mediaDuration = 0;
            seg.destMovieDuration = 0;
            seg.firstSample = 1;
            seg.lastSample = sourceSamples;
            seg.newMediaTime = 0;
            seg.newFirstMediaSample = MP4_INVALID_SAMPLE_ID;
            segments.push_back(seg);
            haveEdits = false;
            return ValidateOccurrenceReferences(seg);
        }

        haveEdits = true;

        uint64_t elstVersion = 0;
        if (!GetU64Property(src, srcMedia, "edts.elst.version", &elstVersion)
            || (elstVersion != 0 && elstVersion != 1)) {
            return false;
        }

        bool anyNonEmpty = false;

        for (uint32_t e = 0; e < editCount; ++e) {
            char prefix[64];
            std::snprintf(prefix, sizeof(prefix),
                           "edts.elst.entries[%u].", e);

            uint64_t segmentDuration = 0;
            uint64_t mediaTimeRaw = 0;
            uint64_t rateInteger = 0;
            uint64_t rateFraction = 0;

            std::string durName = std::string(prefix) + "segmentDuration";
            std::string timeName = std::string(prefix) + "mediaTime";
            std::string rateName = std::string(prefix) + "mediaRate";
            std::string fracName = std::string(prefix) + "reserved";

            if (!GetU64Property(src, srcMedia, durName.c_str(), &segmentDuration)
                || !GetU64Property(src, srcMedia, timeName.c_str(), &mediaTimeRaw)
                || !GetU64Property(src, srcMedia, rateName.c_str(), &rateInteger)
                || !GetU64Property(src, srcMedia, fracName.c_str(),
                                  &rateFraction)) {
                return false;
            }

            /* Only rate = 1.0 segments are supported. */
            if (rateInteger != 1 || rateFraction != 0) {
                return false;
            }
            if (segmentDuration == 0) {
                return false;
            }

            bool isEmpty;
            uint64_t mediaTime;
            if (elstVersion == 1) {
                isEmpty = (mediaTimeRaw == UINT64_MAX);
                mediaTime = mediaTimeRaw;
            } else {
                isEmpty = (static_cast<uint32_t>(mediaTimeRaw)
                           == UINT32_MAX);
                mediaTime = static_cast<uint32_t>(mediaTimeRaw);
            }

            Segment seg;
            seg.empty = isEmpty;
            seg.sourceMovieDuration = segmentDuration;
            seg.firstSample = 0;
            seg.lastSample = 0;
            seg.newMediaTime = 0;
            seg.newFirstMediaSample = MP4_INVALID_SAMPLE_ID;

            /* Exact movie timescale -> media timescale conversion. */
            if (!ExactScale(segmentDuration, mediaTimeScale,
                            sourceMovieTimeScale, &seg.mediaDuration)) {
                return false;
            }
            /* Exact source movie timescale -> destination movie scale. */
            if (!ExactScale(segmentDuration, destMovieTimeScale,
                            sourceMovieTimeScale, &seg.destMovieDuration)) {
                return false;
            }

            if (isEmpty) {
                segments.push_back(seg);
                continue;
            }

            anyNonEmpty = true;

            /* End media time must not overflow. */
            uint64_t endMediaTime = 0;
            if (!AddChecked(mediaTime, seg.mediaDuration, &endMediaTime)) {
                return false;
            }

            uint32_t first = 0;
            if (!SampleAtTime(mediaTime, &first)) {
                return false; /* start not on a sample DTS boundary */
            }

            /* Locate the end boundary: a sample DTS or the media end. */
            uint64_t mediaEnd = 0;
            if (sourceSamples == 0
                || !AddChecked(clock[sourceSamples].time,
                               clock[sourceSamples].duration, &mediaEnd)) {
                return false;
            }

            uint32_t last = MP4_INVALID_SAMPLE_ID;
            if (endMediaTime == mediaEnd) {
                last = sourceSamples;
            } else {
                uint32_t boundarySample = 0;
                if (!SampleAtTime(endMediaTime, &boundarySample)
                    || boundarySample == MP4_INVALID_SAMPLE_ID) {
                    return false; /* end not on a sample DTS boundary */
                }
                if (boundarySample <= first) {
                    return false; /* empty or backwards media segment */
                }
                last = boundarySample - 1;
            }

            if (last < first || first < 1 || last > sourceSamples) {
                return false;
            }

            /* Segment start must be a sync sample. */
            if (MP4GetSampleSync(src, srcMedia, first) != 1) {
                return false;
            }

            /* The segment duration must exactly cover the selected samples. */
            uint64_t covered = 0;
            for (uint32_t i = first; i <= last; ++i) {
                if (!AddChecked(covered, clock[i].duration, &covered)) {
                    return false;
                }
            }
            if (covered != seg.mediaDuration) {
                return false;
            }

            seg.firstSample = first;
            seg.lastSample = last;

            if (!ValidateOccurrenceReferences(seg)) {
                return false;
            }

            segments.push_back(seg);
        }

        /* An edit list made only of empty segments has no playable media. */
        if (!anyNonEmpty) {
            return false;
        }

        return true;
    }

    /*
     * Every media reference inside a hint sample belonging to this occurrence
     * must point inside the same contiguous source range.
     */
    bool ValidateOccurrenceReferences(const Segment& seg)
    {
        if (seg.empty) {
            return true;
        }
        for (uint32_t h = seg.firstSample; h <= seg.lastSample; ++h) {
            const std::vector<RefLocation>& refs = parsedHints[h].refs;
            for (size_t i = 0; i < refs.size(); ++i) {
                if (refs[i].trackRefIndex != 0) {
                    continue;
                }
                const uint32_t target = refs[i].sampleNumber;
                if (target < seg.firstSample || target > seg.lastSample) {
                    return false;
                }
            }
        }
        return true;
    }

    bool FailAndCleanup()
    {
        if (hint != MP4_INVALID_TRACK_ID) {
            (void)MP4DeleteTrack(dst, hint);
            hint = MP4_INVALID_TRACK_ID;
        }
        if (media != MP4_INVALID_TRACK_ID) {
            (void)MP4DeleteTrack(dst, media);
            media = MP4_INVALID_TRACK_ID;
        }
        return false;
    }

    bool Execute()
    {
        /* Create the media track skeleton first. */
        media = MP4CloneTrack(src, srcMedia, dst, MP4_INVALID_TRACK_ID);
        if (media == MP4_INVALID_TRACK_ID) {
            media = MP4_INVALID_TRACK_ID;
            return false;
        }

        /* Create the hint track skeleton referencing the new media track. */
        hint = MP4CloneTrack(src, srcHint, dst, media);
        if (hint == MP4_INVALID_TRACK_ID) {
            hint = MP4_INVALID_TRACK_ID;
            return FailAndCleanup();
        }

        uint16_t nextSequence = 0;
        uint64_t blockMediaTime = 0;
        uint64_t totalDestMovieDuration = 0;

        for (size_t s = 0; s < segments.size(); ++s) {
            Segment& seg = segments[s];

            if (!AddChecked(totalDestMovieDuration,
                            seg.destMovieDuration,
                            &totalDestMovieDuration)) {
                return FailAndCleanup();
            }

            if (seg.empty) {
                /* Four zero bytes: packetCount 0 and reserved 0. */
                static const uint8_t zeroHint[4] = {0, 0, 0, 0};
                if (!MP4WriteSample(dst, hint, zeroHint,
                                    static_cast<uint32_t>(sizeof(zeroHint)),
                                    static_cast<MP4Duration>(seg.mediaDuration),
                                    0, true)) {
                    return FailAndCleanup();
                }
                ++hintCount;
                continue;
            }

            seg.newMediaTime = blockMediaTime;
            seg.newFirstMediaSample = mediaCount + 1;
            const uint32_t newFirstHintSample = hintCount + 1;

            /* Independent copy of every media payload in this occurrence. */
            for (uint32_t k = seg.firstSample; k <= seg.lastSample; ++k) {
                if (!MP4CopySample(src, srcMedia, k, dst, media,
                                   MP4_INVALID_DURATION)) {
                    return FailAndCleanup();
                }
                ++mediaCount;
                if (!AddChecked(blockMediaTime, clock[k].duration,
                                &blockMediaTime)) {
                    return FailAndCleanup();
                }
            }

            /* Copy and rebase the corresponding hint samples. */
            for (uint32_t h = seg.firstSample; h <= seg.lastSample; ++h) {
                std::vector<uint8_t> bytes;
                MP4Duration hintDuration = MP4_INVALID_DURATION;
                bool hintSync = true;
                if (!ReadSampleBytes(srcHint, h, bytes,
                                     &hintDuration, &hintSync)) {
                    return FailAndCleanup();
                }

                ParsedHint parsed;
                if (!ParseHintSample(bytes.empty() ? NULL : &bytes[0],
                                     bytes.size(), parsed)) {
                    return FailAndCleanup();
                }

                /* Dense output RTP sequence numbering, 16 bit wrap. */
                for (size_t i = 0; i < parsed.sequenceOffsets.size(); ++i) {
                    WriteBE16(bytes, parsed.sequenceOffsets[i],
                              nextSequence);
                    nextSequence = static_cast<uint16_t>(nextSequence + 1);
                }

                for (size_t i = 0; i < parsed.refs.size(); ++i) {
                    const RefLocation& ref = parsed.refs[i];
                    if (ref.trackRefIndex == 0) {
                        /* Rebase onto this same occurrence's copied block. */
                        const uint32_t newSample =
                            seg.newFirstMediaSample
                            + (ref.sampleNumber - seg.firstSample);
                        WriteBE32(bytes, ref.sampleNumberOffset, newSample);
                    } else if (ref.trackRefIndex == 255) {
                        /* Self reference targets the new hint sample id. */
                        WriteBE32(bytes, ref.sampleNumberOffset,
                                  newFirstHintSample + (h - seg.firstSample));
                    } else {
                        /* Preflight should have rejected anything else. */
                        return FailAndCleanup();
                    }
                }

                if (hintDuration == MP4_INVALID_DURATION) {
                    hintDuration = clock[h].duration;
                }

                if (!MP4WriteSample(dst, hint,
                                    bytes.empty()
                                        ? reinterpret_cast<const uint8_t*>("")
                                        : &bytes[0],
                                    static_cast<uint32_t>(bytes.size()),
                                    hintDuration, 0, hintSync)) {
                    return FailAndCleanup();
                }
                ++hintCount;
            }
        }

        /* Build the output media edit list. */
        if (haveEdits) {
            for (size_t s = 0; s < segments.size(); ++s) {
                const Segment& seg = segments[s];
                const MP4Timestamp mediaStart = seg.empty
                    ? static_cast<MP4Timestamp>(UINT64_MAX)
                    : static_cast<MP4Timestamp>(seg.newMediaTime);
                if (MP4AddTrackEdit(dst, media, MP4_INVALID_EDIT_ID,
                                    mediaStart,
                                    static_cast<MP4Duration>(
                                        seg.destMovieDuration),
                                    false)
                    == MP4_INVALID_EDIT_ID) {
                    return FailAndCleanup();
                }
            }
        }

        /*
         * With an edit list the media tkhd naturally excludes empty edits, so
         * explicitly place both track headers on the full presentation
         * duration.  Without edits both tracks already share the same natural
         * tkhd duration computed by the library.
         */
        if (haveEdits) {
            if (totalDestMovieDuration > INT64_MAX
                || !MP4SetTrackIntegerProperty(
                       dst, media, "tkhd.duration",
                       static_cast<int64_t>(totalDestMovieDuration))
                || !MP4SetTrackIntegerProperty(
                       dst, hint, "tkhd.duration",
                       static_cast<int64_t>(totalDestMovieDuration))) {
                return FailAndCleanup();
            }
        }

        return true;
    }
};

} /* anonymous namespace */

extern "C" bool MP4CopyHintedTrack(
    MP4FileHandle srcFile,
    MP4TrackId    srcHintTrackId,
    MP4FileHandle dstFile,
    MP4TrackId*   pMediaTrackId,
    MP4TrackId*   pHintTrackId)
{
    if (pMediaTrackId != NULL) {
        *pMediaTrackId = MP4_INVALID_TRACK_ID;
    }
    if (pHintTrackId != NULL) {
        *pHintTrackId = MP4_INVALID_TRACK_ID;
    }
    if (pMediaTrackId == NULL || pHintTrackId == NULL) {
        return false;
    }

    Copier copier;
    copier.src = srcFile;
    copier.srcHint = srcHintTrackId;
    copier.dst = dstFile;

    try {
        /* All rejection happens before the destination is modified. */
        if (!copier.Preflight()) {
            *pMediaTrackId = MP4_INVALID_TRACK_ID;
            *pHintTrackId = MP4_INVALID_TRACK_ID;
            return false;
        }

        if (!copier.Execute()) {
            *pMediaTrackId = MP4_INVALID_TRACK_ID;
            *pHintTrackId = MP4_INVALID_TRACK_ID;
            return false;
        }

        *pMediaTrackId = copier.media;
        *pHintTrackId = copier.hint;

        /* Ownership of the new tracks is handed to the caller. */
        copier.media = MP4_INVALID_TRACK_ID;
        copier.hint = MP4_INVALID_TRACK_ID;
        return true;
    }
    catch (...) {
        copier.FailAndCleanup();
        *pMediaTrackId = MP4_INVALID_TRACK_ID;
        *pHintTrackId = MP4_INVALID_TRACK_ID;
        return false;
    }
}

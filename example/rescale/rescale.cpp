/*
 * rescale.cpp - usage example for MP4RescaleTrackTimeScale().
 *
 * MP4RescaleTrackTimeScale() moves an existing track onto a new time base
 * while keeping every sample's original presentation position. Sample
 * bytes, sample order, sync flags and chunk layout are untouched.
 *
 * Typical command line usage:
 *
 *     rescale <file.mp4> <track-id> <new-timescale>
 *
 * which opens a self-contained MP4 file with MP4Modify(), rescales the
 * track and writes the new tables on MP4Close().
 *
 * Run without arguments to create a small demo file "rescale_demo.mp4",
 * rescale its first track from 1000 to 900 and print the endpoints.
 *
 * Build against a CMake static build tree, for example from the
 * repository root (project.h is generated in the build include dir):
 *
 *     c++ -std=c++11 -DMP4V2_USE_STATIC_LIB \
 *         -Iinclude -Ibuild/include \
 *         example/rescale/rescale.cpp \
 *         build/libmp4v2.a -o rescale_example
 */

#include <mp4v2/mp4v2.h>

#include <cstdio>
#include <cstdlib>

static void printSampleTimes(MP4FileHandle file, MP4TrackId track)
{
    const MP4SampleId n = MP4GetTrackNumberOfSamples(file, track);
    const uint32_t scale = MP4GetTrackTimeScale(file, track);

    std::printf("  timescale=%u duration=%llu samples=%u\n",
                scale,
                (unsigned long long)MP4GetTrackDuration(file, track),
                (unsigned)n);

    for (MP4SampleId i = 1; i <= n; ++i) {
        MP4Timestamp start = MP4GetSampleTime(file, track, i);
        MP4Duration duration = MP4GetSampleDuration(file, track, i);
        MP4Duration offset = MP4GetSampleRenderingOffset(file, track, i);

        std::printf("  sample %u: dts=%llu duration=%llu ctts=%llu\n",
                    (unsigned)i,
                    (unsigned long long)start,
                    (unsigned long long)duration,
                    (unsigned long long)offset);
    }
}

/* Command line mode: rescale one track of an existing MP4 file. */
static int rescaleFile(const char* path, MP4TrackId track, uint32_t newScale)
{
    MP4FileHandle file = MP4Modify(path);
    if (file == MP4_INVALID_FILE_HANDLE) {
        std::fprintf(stderr, "cannot open %s for modification\n", path);
        return 1;
    }

    std::printf("before:\n");
    printSampleTimes(file, track);

    if (!MP4RescaleTrackTimeScale(file, track, newScale)) {
        std::fprintf(stderr,
                     "MP4RescaleTrackTimeScale(track=%u, scale=%u) "
                     "rejected the track\n",
                     (unsigned)track, newScale);
        MP4Close(file);
        return 1;
    }

    std::printf("after:\n");
    printSampleTimes(file, track);

    MP4Close(file); /* rebuilt stts/ctts/mdhd/elst are written here */
    return 0;
}

/* No-argument mode: build a small file and rescale it. */
static int runDemo(void)
{
    const char* path = "rescale_demo.mp4";

    MP4FileHandle file = MP4Create(path);
    if (file == MP4_INVALID_FILE_HANDLE) {
        std::fprintf(stderr, "cannot create %s\n", path);
        return 1;
    }

    const MP4TrackId track =
        MP4AddAudioTrack(file, 1000, MP4_INVALID_DURATION);

    /* five equal durations (one stts run), varied composition offsets */
    static const MP4Duration durations[5] = { 11, 11, 11, 11, 11 };
    static const MP4Duration offsets[5] = { 3, 3, 7, 7, 7 };

    for (int i = 0; i < 5; ++i) {
        const uint8_t sample[2] = { (uint8_t)(0x40 + i), 0x80 };
        if (!MP4WriteSample(file, track, sample, sizeof(sample),
                            durations[i], offsets[i], true)) {
            std::fprintf(stderr, "MP4WriteSample failed\n");
            MP4Close(file);
            return 1;
        }
    }

    /* an empty edit (media_time = -1) followed by a media segment */
    MP4AddTrackEdit(file, track, MP4_INVALID_EDIT_ID,
                    MP4_INVALID_TIMESTAMP, 500, false);
    MP4AddTrackEdit(file, track, MP4_INVALID_EDIT_ID,
                    100, 2000, false);

    MP4Close(file);

    const int rc = rescaleFile(path, 1, 900);
    if (rc == 0) {
        std::printf("demo wrote %s\n", path);
    }
    return rc;
}

int main(int argc, char** argv)
{
    if (argc == 1) {
        return runDemo();
    }

    if (argc != 4) {
        std::fprintf(stderr,
                     "usage: %s <file.mp4> <track-id> <new-timescale>\n",
                     argv[0]);
        return 2;
    }

    const char* path = argv[1];
    const MP4TrackId track = (MP4TrackId)std::atoi(argv[2]);
    const uint32_t scale = (uint32_t)std::strtoul(argv[3], NULL, 10);

    return rescaleFile(path, track, scale);
}

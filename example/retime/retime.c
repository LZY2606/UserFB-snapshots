/*
 * Example: convert an existing track to a new time scale in place with
 * MP4RescaleTrackTimeScale(), keeping the media at its original playback
 * position.
 *
 * Unlike MP4SetTrackTimeScale(), which only rewrites the time scale field
 * (changing the real-time meaning of every tick), MP4RescaleTrackTimeScale()
 * rewrites the track's time tables by absolute endpoint rescaling:
 * with Q(t) = floor(t * newScale / oldScale), a sample with decoding time
 * d, duration u and composition offset c gets Q(d), Q(d+u)-Q(d) and
 * Q(d+c)-Q(d). Sample bytes, sync marks and chunk layout are not touched.
 *
 * The call works on files being created and on ordinary self-contained
 * mp4 files opened with MP4Modify(), for non-fragmented tracks with a
 * single avc1 or mp4a sample description. It returns false (leaving the
 * track unchanged) for anything else.
 */

#include <mp4v2/mp4v2.h>
#include <stdio.h>

static void dumpTrack(MP4FileHandle file, MP4TrackId trackId, const char* when)
{
    MP4SampleId numSamples = MP4GetTrackNumberOfSamples(file, trackId);
    MP4SampleId sid;
    printf("%s: timeScale=%u duration=%llu\n", when,
           MP4GetTrackTimeScale(file, trackId),
           (unsigned long long)MP4GetTrackDuration(file, trackId));
    for (sid = 1; sid <= numSamples; sid++) {
        printf("  sample %u: start=%llu duration=%llu offset=%llu\n",
               (unsigned)sid,
               (unsigned long long)MP4GetSampleTime(file, trackId, sid),
               (unsigned long long)MP4GetSampleDuration(file, trackId, sid),
               (unsigned long long)MP4GetSampleRenderingOffset(file, trackId, sid));
    }
}

int main(int argc, char** argv)
{
    const char* path = (argc > 1) ? argv[1] : "retime-example.mp4";
    const uint8_t payload[4] = { 0, 1, 2, 3 };
    MP4FileHandle file;
    MP4TrackId track;
    int i;

    /* create a small file with a 1000 Hz audio track */
    file = MP4Create(path, 0);
    if (!file) {
        fprintf(stderr, "MP4Create(%s) failed\n", path);
        return 1;
    }
    track = MP4AddAudioTrack(file, 1000, MP4_INVALID_DURATION,
                             MP4_MPEG4_AUDIO_TYPE);
    if (track == MP4_INVALID_TRACK_ID) {
        fprintf(stderr, "MP4AddAudioTrack failed\n");
        MP4Close(file, 0);
        return 1;
    }
    for (i = 0; i < 4; i++) {
        /* variable durations 40, 33, 33, 34 ms in track ticks */
        static const MP4Duration durations[4] = { 40, 33, 33, 34 };
        if (!MP4WriteSample(file, track, payload, sizeof(payload),
                            durations[i], 0, true)) {
            fprintf(stderr, "MP4WriteSample failed\n");
            MP4Close(file, 0);
            return 1;
        }
    }
    dumpTrack(file, track, "before rescale");

    /* convert the track to a 90000 Hz time base in place */
    if (!MP4RescaleTrackTimeScale(file, track, 90000)) {
        fprintf(stderr, "MP4RescaleTrackTimeScale failed\n");
        MP4Close(file, 0);
        return 1;
    }
    dumpTrack(file, track, "after rescale");
    MP4Close(file, 0);

    /* the converted tables are persistent */
    file = MP4Read(path);
    if (!file) {
        fprintf(stderr, "MP4Read(%s) failed\n", path);
        return 1;
    }
    dumpTrack(file, track, "after reopen");
    MP4Close(file, 0);
    return 0;
}

/*
 * copy_hinted_edits
 *
 * Copies an RTP-hinted video track pair from the source MP4 to a new,
 * independently playable MP4, reordering the media according to the
 * media track edit list via MP4CopyHintedTrack.
 *
 * Usage: copy_hinted_edits <input.mp4> <output.mp4>
 */

#include <mp4v2/mp4v2.h>

#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv)
{
    if (argc != 3) {
        std::fprintf(stderr,
                     "usage: %s <input.mp4> <output.mp4>\n",
                     argv[0]);
        return 2;
    }

    const char* inputPath = argv[1];
    const char* outputPath = argv[2];

    MP4FileHandle src = MP4Read(inputPath);
    if (src == MP4_INVALID_FILE_HANDLE) {
        std::fprintf(stderr, "cannot open source file: %s\n", inputPath);
        return 1;
    }

    MP4TrackId srcHint = MP4FindTrackId(src, 0, MP4_HINT_TRACK_TYPE);
    if (srcHint == MP4_INVALID_TRACK_ID) {
        std::fprintf(stderr, "no RTP hint track found in %s\n", inputPath);
        MP4Close(src);
        return 1;
    }

    std::remove(outputPath);
    MP4FileHandle dst = MP4Create(outputPath);
    if (dst == MP4_INVALID_FILE_HANDLE) {
        std::fprintf(stderr, "cannot create output file: %s\n", outputPath);
        MP4Close(src);
        return 1;
    }
    MP4SetTimeScale(dst, MP4GetTimeScale(src));

    MP4TrackId mediaOut = MP4_INVALID_TRACK_ID;
    MP4TrackId hintOut = MP4_INVALID_TRACK_ID;

    bool ok = MP4CopyHintedTrack(src, srcHint, dst, &mediaOut, &hintOut);
    if (!ok || mediaOut == MP4_INVALID_TRACK_ID
            || hintOut == MP4_INVALID_TRACK_ID) {
        std::fprintf(stderr,
                     "MP4CopyHintedTrack failed; the hinted edit layout is "
                     "not supported by this input\n");
        MP4Close(dst);
        MP4Close(src);
        std::remove(outputPath);
        return 1;
    }

    MP4Close(dst);
    MP4Close(src);

    std::printf("copied hinted track pair to %s (media track %u, hint "
                "track %u)\n",
                outputPath, (unsigned)mediaOut, (unsigned)hintOut);
    return 0;
}

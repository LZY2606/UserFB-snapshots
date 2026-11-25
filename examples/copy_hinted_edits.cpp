/*
 * copy_hinted_edits - copy the first RTP hint track of an MP4 file together
 * with its referenced media track, applying the media edit list and rebasing
 * hint sample references.
 *
 * Usage:
 *   copy_hinted_edits <input.mp4> <output.mp4>
 */

#include <mp4v2/mp4v2.h>

#include <cstdio>

int main(int argc, char** argv)
{
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <input.mp4> <output.mp4>\n",
                     argv[0] != NULL ? argv[0] : "copy_hinted_edits");
        return 2;
    }

    const char* inputPath = argv[1];
    const char* outputPath = argv[2];

    MP4FileHandle src = MP4Read(inputPath);
    if (src == MP4_INVALID_FILE_HANDLE) {
        std::fprintf(stderr, "error: cannot read %s\n", inputPath);
        return 1;
    }

    MP4TrackId srcHint = MP4FindTrackId(src, 0, MP4_HINT_TRACK_TYPE);
    if (srcHint == MP4_INVALID_TRACK_ID) {
        std::fprintf(stderr, "error: no RTP hint track found in %s\n",
                     inputPath);
        MP4Close(src);
        return 1;
    }

    MP4FileHandle dst = MP4Create(outputPath);
    if (dst == MP4_INVALID_FILE_HANDLE) {
        std::fprintf(stderr, "error: cannot create %s\n", outputPath);
        MP4Close(src);
        return 1;
    }

    MP4TrackId mediaOut = MP4_INVALID_TRACK_ID;
    MP4TrackId hintOut = MP4_INVALID_TRACK_ID;

    bool ok = MP4CopyHintedTrack(src, srcHint, dst, &mediaOut, &hintOut);

    MP4Close(dst);
    MP4Close(src);

    if (!ok) {
        std::fprintf(stderr, "error: MP4CopyHintedTrack failed\n");
        return 1;
    }

    std::printf("copied hinted track: media track id=%u hint track id=%u\n",
                static_cast<unsigned>(mediaOut),
                static_cast<unsigned>(hintOut));
    return 0;
}

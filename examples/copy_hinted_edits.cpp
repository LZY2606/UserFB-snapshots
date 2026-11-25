/*
 * copy_hinted_edits: command line front end for MP4CopyHintedTrack().
 *
 * Usage: copy_hinted_edits <input.mp4> <output.mp4>
 *
 * Finds the first RTP hint track in the input file and writes a pair of
 * independently playable media/hint tracks into the output file, driven by
 * the media track's edit list (empty segments, repeated or reordered
 * media segments). The input file is only read; the output file is created
 * or truncated.
 */

#include <mp4v2/mp4v2.h>

#include <cstdio>

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
        std::fprintf(stderr, "cannot open input file: %s\n", inputPath);
        return 1;
    }

    MP4TrackId srcHint =
        MP4FindTrackId(src, 0, MP4_HINT_TRACK_TYPE);
    if (srcHint == MP4_INVALID_TRACK_ID) {
        std::fprintf(stderr,
                     "input file has no RTP hint track: %s\n", inputPath);
        MP4Close(src);
        return 1;
    }

    MP4FileHandle dst = MP4Create(outputPath);
    if (dst == MP4_INVALID_FILE_HANDLE) {
        std::fprintf(stderr,
                     "cannot create output file: %s\n", outputPath);
        MP4Close(src);
        return 1;
    }

    MP4TrackId mediaTrackId = MP4_INVALID_TRACK_ID;
    MP4TrackId hintTrackId = MP4_INVALID_TRACK_ID;

    bool ok = MP4CopyHintedTrack(src, srcHint, dst,
                                 &mediaTrackId, &hintTrackId);
    if (!ok) {
        std::fprintf(stderr,
                     "MP4CopyHintedTrack rejected the input; no tracks "
                     "were written to %s\n", outputPath);
        MP4Close(dst);
        MP4Close(src);
        return 1;
    }

    std::printf("copied hinted track pair to %s: media track %u, "
                "hint track %u\n",
                outputPath, mediaTrackId, hintTrackId);

    MP4Close(dst);
    MP4Close(src);
    return 0;
}

# mp4v2

This is a fork of mp4v2 from https://code.google.com/archive/p/mp4v2/. A newer, maintained fork of the original mp4v2 code can be found at https://mp4v2.org.

TechSmith has modified this code for its own purposes and openly publishes those changes here in compliance with the Mozilla Public License.

-------
<pre>
All docs are located in doc/ subdirectory. Useful starting points:

Release Notes           -- doc/ReleaseNotes.txt
Building the Source     -- doc/BuildSource.txt
Building the Repository -- doc/BuildRepository.txt

See the COPYING file for license rights and limitations
</pre>

-------

## Copying an RTP hinted track with edit-list reordering

`MP4CopyHintedTrack()` copies an RTP hinted video track pair (a single
`avc1` or `mp4v` media track plus its RTP hint track) from one MP4 file
into another, rearranging the media according to the media track's edit
list so the result is independently playable.

* The source file is only read; the destination may already contain
  other tracks.
* Empty (no-media) edits are kept as playback gaps. They contribute no
  media samples and become zero-packet hint samples whose duration spans
  the gap, so subsequent RTP timestamps land at the right movie time.
* Rate-1 media edits may appear in any order and may repeat. Each
  occurrence copies its media samples independently. Every sample-data
  instruction in the hint track is rebound to the new media copy of that
  exact occurrence; self (track-ref-index 255) embedded references keep
  their embedded bytes and are renamed to the new hint sample instead of
  being flattened to immediate data.
* The output media track keeps an edit list whose media segments point at
  the freshly copied, contiguous media regions; the output hint track has
  no edit list. Both track headers share one movie duration, the media
  `mdhd` counts only copied media time and the hint `mdhd` includes the
  gap time.
* Timescale conversions use exact integer arithmetic; a non-integral
  conversion, an overflow, an all-blank edit list, an unsupported RTP
  instruction (other tracks, sample-description entries, non-trivial
  block layouts, packet extra info), a bad reference, or an edit that does
  not land on whole sync-sample boundaries makes the call fail. On failure
  no destination tracks are created and both output ids are set to
  `MP4_INVALID_TRACK_ID`.

```c
#include <mp4v2/mp4v2.h>

MP4FileHandle src = MP4Read("input.mp4");
MP4TrackId srcHint = MP4FindTrackId(src, 0, MP4_HINT_TRACK_TYPE);

MP4FileHandle dst = MP4Create("output.mp4");
MP4SetTimeScale(dst, MP4GetTimeScale(src));

MP4TrackId mediaId = MP4_INVALID_TRACK_ID;
MP4TrackId hintId  = MP4_INVALID_TRACK_ID;
if (MP4CopyHintedTrack(src, srcHint, dst, &mediaId, &hintId)) {
    /* mediaId/hintId form a self-contained, reordered hinted pair */
}

MP4Close(dst);
MP4Close(src);
```

The standalone program `examples/copy_hinted_edits.cpp` does exactly this
from the command line:

```
copy_hinted_edits input.mp4 output.mp4
```

### Building and testing

```
bash task_support/run.sh target   # builds the library, example and tests
```

The task tests live in `tests/task_hinted_edits.cpp` and are registered in
the `occurrence`, `clock` and `reject_compat` groups. They build their own
small MP4 inputs, close and reopen the outputs, and verify packet
payloads, sequence numbers, the dual media/hint timelines, per-occurrence
reference remapping, self-embedded data, timescale rejection and the
legacy `MP4CloneTrack`/`MP4CopyTrack` paths.

# mp4v2

This is a fork of mp4v2 from https://code.google.com/archive/p/mp4v2/. A newer, maintained fork of the original mp4v2 code can be found at https://mp4v2.org.

TechSmith has modified this code for its own purposes and openly publishes those changes here in compliance with the Mozilla Public License.

-------

## Hinted track edit-list copy

`MP4CopyHintedTrack()` copies an RTP hint track together with its referenced
`avc1`/`mp4v` video track from one open file handle to another and applies the
media track's edit list as independent segment occurrences:

```c
#include <mp4v2/mp4v2.h>

MP4TrackId mediaOut = MP4_INVALID_TRACK_ID;
MP4TrackId hintOut  = MP4_INVALID_TRACK_ID;

bool ok = MP4CopyHintedTrack(src, srcHintTrack, dst, &mediaOut, &hintOut);
```

Every non-empty edit segment occurrence copies its media and hint samples into
a new contiguous block; media references inside the hint samples are rebound
to that occurrence's copies, hint self references are rebound to the new hint
sample ids, and RTP sequence numbers are renumbered densely from zero. Empty
edits stay as empty media edits and become zero-packet hint samples of the
same duration. All validation happens before any destination track is created.

The `examples/copy_hinted_edits.cpp` example is a small command line wrapper:

```sh
c++ -std=c++11 -DMP4V2_USE_STATIC_LIB -Iinclude \
    examples/copy_hinted_edits.cpp build/libmp4v2.a -o copy_hinted_edits
./copy_hinted_edits input.mp4 output.mp4
```

The task test suite (groups `occurrence`, `clock`, `reject_compat`) and the
example are built and run by:

```sh
bash task_support/run.sh target
```

The legacy `MP4CopyTrack()` and `MP4CloneTrack()` APIs are unchanged.

-------
<pre>
All docs are located in doc/ subdirectory. Useful starting points:

Release Notes           -- doc/ReleaseNotes.txt
Building the Source     -- doc/BuildSource.txt
Building the Repository -- doc/BuildRepository.txt

See the COPYING file for license rights and limitations
</pre>

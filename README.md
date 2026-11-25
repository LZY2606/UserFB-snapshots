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

## Hinted track edit copying

`MP4CopyHintedTrack()` (declared in `include/mp4v2/track.h`) copies an RTP
hint track together with its single referenced video media track from one
file into another, following the media track's edit list. Media is copied
once per rate=1 edit segment in playback order; empty edit segments become
zero-packet hint samples with the same playback duration. Hint packet
contents are preserved, with media/self sample references and 16-bit RTP
sequence numbers rebound to the newly copied samples. The source file is
only read, the destination may already contain other tracks, and
unsupported or malformed inputs are rejected before any destination track
is created.

A small command line front end lives in `examples/copy_hinted_edits.cpp`:

```
copy_hinted_edits <input.mp4> <output.mp4>
```

The task test suite and the example are built and run with:

```
bash task_support/run.sh target
```

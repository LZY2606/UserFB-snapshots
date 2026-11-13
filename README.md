# TagLib

[![Build Status](../../actions/workflows/build.yml/badge.svg)](../../actions)

### TagLib Audio Metadata Library

https://taglib.org/

TagLib is a library for reading and editing the metadata of several
popular audio formats. Currently it supports both ID3v1 and [ID3v2][]
for MP3 files, [Ogg Vorbis][] comments and ID3 tags
in [FLAC][], MPC, Speex, WavPack, TrueAudio, WAV, AIFF, MP4, APE,
and ASF files.

TagLib is distributed under the [GNU Lesser General Public License][]
(LGPL) and [Mozilla Public License][] (MPL). Essentially that means that
it may be used in proprietary applications, but if changes are made to
TagLib they must be contributed back to the project. Please review the
licenses if you are considering using TagLib in your project.

  [ID3v2]: https://id3.org/
  [Ogg Vorbis]: https://xiph.org/vorbis/
  [FLAC]: https://xiph.org/flac/
  [GNU Lesser General Public License]: https://www.gnu.org/licenses/lgpl.html
  [Mozilla Public License]: https://www.mozilla.org/MPL/MPL-1.1.html

### ID3v2 unsynchronisation regression harness

The two ID3v2 unsynchronisation read paths have a byte-exact regression in
`support/target_cases.cpp`:

* **ID3v2.3** restores unsynchronisation once over the whole tag body. Frame
  lengths in that body are ordinary big-endian integers of the *restored*
  content, while the tag header size counts the bytes *stored on disk*
  (after escaping).
* **ID3v2.4** restores unsynchronisation per frame. A frame's 4-byte frame
  length is the *on-disk* (escaped) body length used to locate the next
  frame; with a data length indicator (DLI) present, its 4-byte syncsafe
  prefix instead holds the *restored* body length. Tag-level and frame-level
  flags may both be set, in which case the body is restored exactly once.

The test material is generated in C++ by independent big-endian/syncsafe
integer writers and an independent escaping pass; no TagLib render or
`SynchData` helper is used to compute expectations. Run it with:

```sh
bash support/target.sh
```

See `support/README.md` for the group breakdown.


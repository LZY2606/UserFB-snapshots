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

### Multiplexed Ogg Vorbis streams

`TagLib::Vorbis::File` offers two four argument constructors that take an
explicit Ogg serial number in addition to the usual arguments:

```cpp
Vorbis::File(FileName file, bool readProperties,
             Properties::ReadStyle propertiesStyle,
             unsigned int serialNumber);
Vorbis::File(IOStream *stream, bool readProperties,
             Properties::ReadStyle propertiesStyle,
             unsigned int serialNumber);
```

When a serial number is given, every operation is restricted to that one
logical Ogg stream: `tag()`, `packet()`, `firstPageHeader()`,
`lastPageHeader()`, `audioProperties()` and `save()` all ignore the other
streams interleaved in the same physical file. If no Vorbis beginning of
stream page with that serial number exists, `isValid()` returns false.
The three argument constructors keep their previous behavior and do not
automatically pick a stream out of a multiplexed file.

The supported layout is a finite, readable, writable and seekable Ogg
file in which every stream has a unique serial number, the beginning of
stream pages are grouped at the start, and each stream's page numbers run
continuously from zero to its end of stream page. For the edited stream
the identification header occupies its own beginning of stream page; the
comment and setup headers may span pages and may share a page; the setup
header ends exactly at a page boundary and audio starts on the next page
of that stream. Pages of other streams may be interleaved anywhere
between those pages. On save the identification and setup headers, all
audio packets and every audio page's lacing, granule position and
BOS/EOS/continuation flags are preserved; audio pages only receive new
page sequence numbers and CRCs, while every page of every other stream is
left byte for byte unchanged.

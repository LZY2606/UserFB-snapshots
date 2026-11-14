---
layout: page
title: libsndfile: Supported formats
---

# libsndfile : Supported formats

The following table lists the file formats and encodings that libsndfile can
read and write. The file formats are arranged across the top and encodings along
the left edge.

{:.formats-table}
|                    | Microsoft WAV | SGI / Apple AIFF / AIFC |Sun / DEC / NeXT AU / SND | Headerless RAW | Paris Audio File PAF | Commodore Amiga IFF / SVX | Sphere Nist WAV | IRCAM SF | Creative VOC | Soundforge W64 | GNU Octave 2.0 MAT4 | GNU Octave 2.1 MAT5 | Portable Voice Format PVF | Fasttracker 2 XI | HMM Tool Kit HTK | Apple CAF | Sound Designer II SD2 | Free Lossless Audio Codec FLAC |
|:-------------------|:-------------:|:-----------------------:|:------------------------:|:--------------:|:--------------------:|:-------------------------:|:---------------:|:--------:|:------------:|:--------------:|:-------------------:|:-------------------:|:-------------------------:|:----------------:|:----------------:|:---------:|:---------------------:|:------------------------------:|
| Unsigned 8 bit PCM | R/W           | R/W                     |                          | R/W            |                      |                           |                 |          | R/W          | R/W            |                     | R/W                 |                           |                  |                  |           |                       |                                |
| Signed 8 bit PCM   |               | R/W                     | R/W                      | R/W            | R/W                  | R/W                       | R/W             |          |              |                |                     |                     | R/W                       |                  |                  | R/W       | R/W                   | R/W                            |
| Signed 16 bit PCM  | R/W           | R/W                     | R/W                      | R/W            | R/W                  | R/W                       | R/W             | R/W      | R/W          | R/W            | R/W                 | R/W                 | R/W                       |                  | R/W              | R/W       | R/W                   | R/W                            |
| Signed 24 bit PCM  | R/W           | R/W                     | R/W                      | R/W            | R/W                  |                           | R/W             |          |              | R/W            |                     |                     |                           |                  |                  | R/W       | R/W                   | R/W                            |
| Signed 32 bit PCM  | R/W           | R/W                     | R/W                      | R/W            |                      |                           | R/W             | R/W      |              | R/W            | R/W                 | R/W                 | R/W                       |                  |                  | R/W       |                       | R/W                            |
| 32 bit float       | R/W           | R/W                     | R/W                      | R/W            |                      |                           |                 | R/W      |              | R/W            | R/W                 | R/W                 |                           |                  |                  | R/W       |                       |                                |
| 64 bit double      | R/W           | R/W                     | R/W                      | R/W            |                      |                           |                 |          |              | R/W            | R/W                 | R/W                 |                           |                  |                  | R/W       |                       |                                |
| u-law encoding     | R/W           | R/W                     | R/W                      | R/W            |                      |                           | R/W             | R/W      | R/W          | R/W            |                     |                     |                           |                  |                  | R/W       |                       |                                |
| A-law encoding     | R/W           | R/W                     | R/W                      | R/W            |                      |                           | R/W             | R/W      | R/W          | R/W            |                     |                     |                           |                  |                  | R/W       |                       |                                |
| IMA ADPCM          | R/W           |                         |                          |                |                      |                           |                 |          |              | R/W            |                     |                     |                           |                  |                  |           |                       |                                |
| MS ADPCM           | R/W           |                         |                          |                |                      |                           |                 |          |              | R/W            |                     |                     |                           |                  |                  |           |                       |                                |
| GSM 6.10           | R/W           | R/W                     |                          | R/W            |                      |                           |                 |          |              | R/W            |                     |                     |                           |                  |                  |           |                       |                                |
| G721 ADPCM 32kbps  | R/W           |                         | R/W                      |                |                      |                           |                 |          |              |                |                     |                     |                           |                  |                  |           |                       |                                |
| G723 ADPCM 24kbps  |               |                         | R/W                      |                |                      |                           |                 |          |              |                |                     |                     |                           |                  |                  |           |                       |                                |
| G723 ADPCM 40kbps  |               |                         | R/W                      |                |                      |                           |                 |          |              |                |                     |                     |                           |                  |                  |           |                       |                                |
| 12 bit DWVW        |               | R/W                     |                          | R/W            |                      |                           |                 |          |              |                |                     |                     |                           |                  |                  |           |                       |                                |
| 16 bit DWVW        |               | R/W                     |                          | R/W            |                      |                           |                 |          |              |                |                     |                     |                           |                  |                  |           |                       |                                |
| 24 bit DWVW        |               | R/W                     |                          | R/W            |                      |                           |                 |          |              |                |                     |                     |                           |                  |                  |           |                       |                                |
| Ok Dialogic ADPCM  |               |                         |                          | R/W            |                      |                           |                 |          |              |                |                     |                     |                           |                  |                  |           |                       |                                |
| 8 bit DPCM         |               |                         |                          |                |                      |                           |                 |          |              |                |                     |                     |                           | R/W              |                  |           |                       |                                |
| 16 bit DPCM        |               |                         |                          |                |                      |                           |                 |          |              |                |                     |                     |                           | R/W              |                  |           |                       |                                |

From version 1.0.18, libsndfile also reads and writes
[FLAC](https://xiph.org/flac/) and [Ogg/Vorbis](https://xiph.org/vorbis/).

From version 1.0.29, libsndfile can read and write
[Ogg/Opus](https://opus-codec.org/).

From version 1.1.0, libsndfile can read and write MP3.

### Apple CAF / ALAC packet tables

CAF files using Apple Lossless (ALAC) store their audio as a sequence of
packets described by the `pakt` chunk. libsndfile reads both layouts:

* Fixed packet duration: `desc` declares a non-zero `frames per packet` and
  each `pakt` entry is a single byte count. This layout remains readable and
  writable as before; the frame count is derived from the packet table.
* Variable packet duration: `desc` sets both `bytes per packet` and
  `frames per packet` to zero, and every `pakt` entry is a pair of
  seven-bit, big-endian continuation integers holding that packet's byte
  count and frame count.

Variable packet reading is supported for seekable CAF files, ALAC 16 and
24 bit, mono and stereo, with zero priming and remainder frames. Each packet
must declare between 1 and the maximum frame length advertised in the ALAC
magic cookie, up to 4096 frames. The packet count, valid frame count, packet
byte total and the actual `data` payload must agree, and every packet must
decode to exactly the declared number of frames; otherwise opening or
reading fails with an error instead of treating the mismatch as end of file.
Reading allocates scratch space only for one maximum-length packet per
channel, while the packet directory grows with the packet count. Seeking
operates on the same zero-based frame axis as `sf_readf_*`, including packet
boundaries and `SEEK_END`. Writing variable packet files is not supported.

32 bit PCM in FLAC requires libFLAC 1.4.0 or later.

Some of the file formats I am also interested in adding are:

- Kurzweil K2000 sampler files.
- Ogg Speex.

Other file formats may also be added on request.

If you are interested in how to add a new format to a libsndfile, you may find
this [FAQ](new_file_type_howto.md) helpful.

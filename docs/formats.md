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

32 bit PCM in FLAC requires libFLAC 1.4.0 or later.

### CAF/ALAC variable packet durations

In addition to fixed block size CAF/ALAC files, the reader supports seekable
CAF files whose `desc` chunk has both `bytes per packet` and `frames per
packet` set to zero and whose `pakt` chunk stores an alternating
`(byte count, frame count)` pair of big endian 7 bit continuation integers for
every packet. Each packet's byte interval and sample interval are recorded from
the metadata only; opening such a file does not scan or decode the audio to
determine its duration.

The current support range is:

- seekable CAF, including memory based `sf_open_virtual` streams ;
- `SF_FORMAT_ALAC_16` and `SF_FORMAT_ALAC_24`, mono or stereo ;
- zero priming and zero remainder frames ;
- one to the cookie's maximum frame length per packet, capped at 4096.

The packet count, cumulative frame count (`valid frames`), cumulative byte
count against the `data` chunk and the decoder's per packet output count are
all checked. Truncated or overflowing continuation integers, zero lengths,
packet count mismatches and frame or byte range mismatches are rejected as
malformed input instead of being treated as a normal end of file. A packet
whose decoded frame count disagrees with its declaration is withheld and raises
an `sf_error` while the correctly decoded prefix remains available. Seeking,
`sf_readf_short`, `sf_readf_int`, `sf_readf_float` and `sf_readf_double` all
use the same zero based sample axis. Writing variable packet duration CAF is
not provided; the existing fixed block writer and its short tail packets are
unchanged.

Some of the file formats I am also interested in adding are:

- Kurzweil K2000 sampler files.
- Ogg Speex.

Other file formats may also be added on request.

If you are interested in how to add a new format to a libsndfile, you may find
this [FAQ](new_file_type_howto.md) helpful.

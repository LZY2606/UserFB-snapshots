<h4 align="center">Public domain, single file audio decoding libraries for C and C++.</h4>

<p align="center">
    <a href="https://discord.gg/9vpqbjU"><img src="https://img.shields.io/discord/712952679415939085?label=discord&logo=discord&style=flat-square" alt="discord"></a>
</p>

All development of released libraries happens on the master branch. There may exist some decoder-specific branches for work in progress. Check tags for the latest version of a particular library.


Library                                         | Description
----------------------------------------------- | -----------
[dr_flac](dr_flac.h)                            | FLAC audio decoder.
[dr_mp3](dr_mp3.h)                              | MP3 audio decoder. Based off [minimp3](https://github.com/lieff/minimp3).
[dr_wav](dr_wav.h)                              | WAV audio loader and writer.


# Other Libraries
Below are some of my other libraries you may be interested in.

Library                                           | Description
------------------------------------------------- | -----------
[miniaudio](https://github.com/mackron/miniaudio) | A public domain, single file library for audio playback and capture.


# bwf_join

`tools/bwf_join` joins BWF (Broadcast Wave) volumes exported by several
recorders onto one continuous RIFF/WAVE timeline. Every volume carries an
absolute first-frame position in `bext.timeReference`; the inputs share one
sample clock, so no resampling is performed and time positions are never
converted to floating point seconds.

```
tools/bwf_join OUTPUT INPUT...
```

Example: three volumes whose time references overlap. Their order on the
command line does not change the audio or the chosen origin.

```
tools/bwf_join joined.wav recorder_part1.wav recorder_part2.wav recorder_part3.wav
```

The output spans the earliest time reference to the latest end position.
Volumes are merged by their absolute frame positions, not by argument order:

* Overlapping frames must contain byte-identical per-channel PCM in every
  source. Any difference fails the join and reports the absolute frame; the
  sources are never mixed and no source is silently preferred. Identical
  overlap is emitted only once.
* Gaps between volumes are filled with digital silence (`0x80` for 8-bit,
  zero otherwise). No array is allocated for the gap length.
* Only non-empty, single-`data`, little-endian RIFF integer PCM at 8/16/24/32
  bits is accepted, and every input must match in sample rate, channel count
  and bit depth. RF64/Wave64/RIFX, floating point, compressed formats, `wavl`
  and resampling are out of scope.
* The output `bext` keeps the description and other fields of the first input
  on the command line, with `timeReference` set to the earliest start.
* `cue`, the LIST/adtl `labl`/`note`/`ltxt` text and `smpl` are merged with
  the audio. Each input has its own cue ID namespace; new output IDs are
  assigned consecutively from 1 in command-line then cue-table order, and all
  text and loop references are rewritten to the new IDs. Loop and cue
  positions shift by the distance between the volume's origin and the output
  origin. Inputs without cue or smpl chunks are still accepted.
* Duplicate local IDs, dangling references, duplicate `bext`/`cue`/`smpl`,
  out-of-range points, non-integer-frame data, integer overflow, or a result
  that cannot be expressed with RIFF 32-bit lengths all reject the join.

Metadata boundary: only `bext`, `cue`, LIST/adtl `labl`/`note`/`ltxt` and
`smpl` are carried into the output. Unrecognized top-level chunks (for
example `junk`), LIST/INFO strings, `inst`, `acid` and every other chunk type
not listed above are deliberately not copied.

All format, reference and overlap checks finish before `OUTPUT` is created;
if `OUTPUT` already exists the command refuses, and a failed run never leaves
a half-written target file. The implementation streams the audio in
fixed-size blocks, so its memory use grows only with the number of inputs and
the amount of metadata, never with the recording length.

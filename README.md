# Dynamic Universal Music Bibliotheque (libdumb)

     _______         ____    __         ___    ___
    \    _  \       \    /  \  /       \   \  /   /       '   '  '
     |  | \  \       |  |    ||         |   \/   |         .      .
     |  |  |  |      |  |    ||         ||\  /|  |
     |  |  |  |      |  |    ||         || \/ |  |         '  '  '
     |  |  |  |      |  |    ||         ||    |  |         .      .
     |  |_/  /        \  \__//          ||    |  |
    /_______/ynamic    \____/niversal  /__\  /____\usic   /|  .  . ibliotheque
                                                         /  \
                                                        / .  \
                                                       / / \  \
                                                      | <  /   \_
                                                      |  \/ /\   /
                                                       \_  /  > /
                                                         | \ / /
                                                         |  ' /
                                                          \__/



## Introduction

DUMB is a module audio renderer library. It reads module files and
outputs audio that can be dumped to the actual audio playback library.

This is a fork of the original dumb (http://dumb.sf.net/) by Ben Davis.

## Features

- Supports playback of the following module formats. The tracker software or
  library the format is known for is given in parentheses. This does not mean
  that DUMB does not support files created by other trackers provided that they
  output files in one of those formats.

   * IT (Impulse Tracker)
   * XM (Fasttracker II)
   * MOD (Ultimate SoundTracker, ProTracker)
   * STM (Scream Tracker)
   * S3M (Scream Tracker 3)
   * 669 (Composer 669)
   * AMF Asylum Music Format
   * AMF Digital Sound and Music Interface Advanced Music Format
   * DSM Digital Sound Interface Kit module format
   * MTM (MultiTracker)
   * OKT (Oktalyzer)
   * PSM (Protracker Studio)
     Both the older PSM16 and the newer PSM format is supported.
   * PTM (PolyTracker)
   * RIFF AM/AMFF (Galaxy Music System internal format)

- Audio generated can be used in any way; DUMB does not necessarily send it
  straight to a sound output system

- Portable

- Faithful to the original trackers, especially IT; if it plays a module
  wrongly, it is considered a bug

- Accurate support for low-pass resonant filters for IT files

- Very accurate timing and pitching; completely deterministic playback

- Click removal

- Six resampling quality settings: aliasing, aliasing with bandwidth limiting,
  linear interpolation, linear interpolation with bandwidth limiting, cubic
  interpolation, and a compile-time-configurable fast sinc resampler

- Number of samples playing at once can be limited to reduce processor usage,
  but samples will come back in when other louder ones stop

- Option to take longer loading but seek fast to any point before the music
  first loops (seeking time increases beyond this point)

- All notes will be present and correct even if a module's playback is started
  in the middle

- Optional Allegro 4 or Allegro 5 integration support

- Facility to embed music files in other files (e.g. Allegro datafiles)


## IT sample loading notes

When an IT sample has a valid normal loop, only the PCM up to `loop_end` is
retained in memory; if a valid sustain loop reaches further, the larger of the
two ends is kept instead. A sample without a valid normal loop keeps its full
declared length. For stereo samples both channels are interleaved in the
retained buffer, and `IT_SAMPLE.length` reports the retained frame count.

Compressed (IT 2.14 / 2.15) samples are still decoded and validated across
their entire declared length, including blocks past the retained tail, and
uncompressed planar samples are located using the full on-disk frame count.
This means silent tail corruption, an illegal bit width, a truncated block or a
missing delta rejects the whole module through the normal `free`/`unload`
paths rather than silently producing a partially loaded sample. The extra
memory used during decoding is bounded by the retained PCM plus one compressed
block (at most 65535 bytes), independent of how much of the tail is discarded.

The source range of each sample is determined from file positions: every
nonzero instrument, sample-header, pattern and song-message offset, plus every
actual sample-data offset, forms a boundary, and a sample reads at most up to
the next distinct start (or end of file). Two samples sharing a data offset
each decode their own copy of that range.


## Installation

Currently you need to compile libdumb yourself. For more details, please see
the file [COMPILING.md](COMPILING.md).

## License

See [LICENSE](LICENSE) for license details.

## Contributing

Bugs, feature requests and patches can be submitted at https://github.com/kode54/dumb/.

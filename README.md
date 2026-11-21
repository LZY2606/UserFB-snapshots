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


## Deterministic PCM-frame seeking

The ordinary `duh_start_sigrenderer()` entry point starts from a *time*
position and silently fast-forwards. That is not sample-exact: the FIR
resampler, the IT filters and click removal only settle while real audio is
rendered, and random effects (`random` vibrato/tremolo/panbrello waveforms,
sample auto-vibrato, and instrument random volume/pan) draw from the global
`rand()` state, so the result depends on how much is read per call and on any
other song playing at the same time.

The optional seek API renders from a seed instead, producing sample-exact,
reproducible output that can be addressed by output PCM frame:

```c
DUH *duh = dumb_load_any("song.it", 0, 0);

/* Play from the start, deterministically. */
DUH_SIGRENDERER *seeded = duh_it_start_seeded(duh, 2, 44100,
                                              DUMB_RQ_FIR, /*seed*/ 1234);

/* Or build an index of playback states, once. */
DUMB_IT_SEEK_INDEX *index =
    dumb_it_build_seek_index(duh, 2, 44100, DUMB_RQ_FIR, 1234,
                             /*frame_limit*/ 44100 * 60,
                             /*stride*/ 4096);

/* Later, start an independent renderer at an exact output frame. */
unsigned preroll;
DUH_SIGRENDERER *at = dumb_it_seek_index_start(index, /*frame*/ 1234567,
                                               &preroll);

/* ...read with duh_sigrenderer_generate_samples() as usual... */

duh_end_sigrenderer(at);
dumb_it_free_seek_index(index);   /* after all renderers from it */
unload_duh(duh);                  /* last */
```

### Fixed playback parameters

For every seeded or indexed renderer the playback parameters are pinned, and
the `volume`/`delta` arguments to `duh_sigrenderer_generate_samples()` are
ignored:

- **delta**: `(float)(65536.0 / rate)`
- **volume**: `1.0`
- **end of the first pass**: playback stops (zero frames from then on)

`channels` must be 1 or 2; `rate` must be between 8000 and 192000; `quality`
must be `DUMB_RQ_CUBIC` or `DUMB_RQ_FIR`. `frame_limit` and `stride` must be
positive. Invalid arguments, or a frame that cannot be represented, make the
functions return `NULL`/`0`.

Random effects are drawn from the supplied `seed`, held for a whole tracker
tick and advanced only by the tracker, so they are independent of read sizes
and of other renderers. The note-trigger timing of random volume/panning is
unchanged, and NNA voices keep their own smooth-effect state. A module with no
random effects renders exactly the same as `duh_start_sigrenderer()` with the
same delta, volume and quality.

### Index semantics

`dumb_it_build_seek_index()` renders up to `frame_limit` output frames or the
end of the first pass, whichever comes first, and stores a playback snapshot
every `stride` frames. `dumb_it_seek_index_frames()` reports the number of
frames covered. A seek accepts frames `0 .. covered`; `dumb_it_seek_index_start()`
returns `NULL` beyond that.

A seek copies the nearest snapshot at or before the target frame and renders
fewer than `stride` frames of real audio to reach it, so the first sample
matches sequential playback. That preroll amount (always `< stride`) is
reported through `preroll` when non-NULL. Reading past the true end of the song
returns zero frames; reaching the *coverage* limit does not stop playback,
because the song may continue. The index is immutable and may be shared;
renderers started from it are fully independent (interleaved reads and freeing
one or the index early are safe). Memory grows with the number of snapshots,
plus at most one stride of scratch PCM while building or seeking.

### Lifetime

- The `DUH` must outlive the index and all renderers created from it (seeded
  or seeked). Free the renderers, then `dumb_it_free_seek_index()`, then
  `unload_duh()`.

## Installation

Currently you need to compile libdumb yourself. For more details, please see
the file [COMPILING.md](COMPILING.md).

## License

See [LICENSE](LICENSE) for license details.

## Contributing

Bugs, feature requests and patches can be submitted at https://github.com/kode54/dumb/.

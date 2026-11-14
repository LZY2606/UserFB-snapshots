# Changelog

## Unreleased: independent rendering sessions

### Problem: every open module shared one global renderer

The decoder was driven entirely by process-global state.
`ModPlug_SetSettings()` and every `ModPlug_Load()` reconfigured the same
storage, so opening or configuring a second module changed a module that was
already playing.  The shared state fell into four groups:

1. **Mixer configuration** — `CSoundFile` static members in `sndmix.cpp` /
   `sndfile.cpp`: mixing frequency, bits per sample, channel count, sound-setup
   flags, resampling mode, stereo separation, maximum mix channels, volume-ramp
   length, AGC/VU/CPU fields, the master-volume scalar and the mixer callback
   pointer (`gpSndMixHook`).
2. **Effect parameters** — static reverb / megabass / surround depth and delay
   values in `snd_dsp.cpp`.
3. **Persistent DSP history** — file-scope statics in `snd_dsp.cpp`: noise
   reduction filter values, the four reverb delay lines and their filter
   history, megabass low-pass/delay buffers, Dolby surround filters and delay
   line, plus the cross-channel volume offsets (`gnDryROfsVol` and friends),
   reverb-send counter and AGC recover counter.
4. **Per-call scratch** — `MixSoundBuffer`, `MixReverbBuffer` and
   `MixRearBuffer` in `fastmix.cpp`.  These only hold data for the duration of
   a single `CSoundFile::Read()` call, so they remain shared.

### New API

`ModPlug_LoadWithSettings(data, size, settings)` copies the supplied
`ModPlug_Settings` and loads the module into a private `ModPlugSession` that
owns everything in groups 1–3.  The caller's struct, later
`ModPlug_SetSettings()` calls, and the load/read/seek/unload of other handles
cannot reach that session.  `ModPlug_Read()` still performs the rendering; its
byte count and PCM format follow the handle's own settings.
`ModPlug_InitMixerCallback()` / `ModPlug_UnloadMixerCallback()` now update the
target handle's session only.

Legacy `ModPlug_Load()`, `ModPlug_GetSettings()` and `ModPlug_SetSettings()`
keep their old behavior and defaults; they operate on a dedicated legacy
session whose initial values match the original static initializers.

### Implementation outline

- `src/libmodplug/session.h` defines `ModPlugSession` (configuration, callback
  ownership and the full DSP history) and routing macros that map every
  historical global identifier onto the active session.
- A single active-session pointer is swapped around each top-level C API call
  by a scope guard.  Calls are serialized by the API contract; DSP is never
  re-initialized on a switch, no audio is replayed and no whole-song output is
  cached.
- Small reads keep working because all sub-tick progress (channel positions,
  volume-ramp leftovers, delay-line contents) lives in the session and in the
  per-file `CSoundFile`, never in per-call scratch.

### How the tests reject weaker implementations

`tests/session_checks.cpp` always compares interleaved execution against an
exclusive run of the same module, settings, block sizes and seek sequence, and
separately proves that the exclusive reference matches the legacy
`ModPlug_Load` / `ModPlug_SetSettings` decode (which is itself pinned by the
golden PCM files in `task_support/golden`).

- **Settings-only isolation would fail** `settings_copy` and
  `global_defaults_preserved`: the caller zeroes its struct and installs
  hostile global settings after loading, then expects identical PCM, while the
  global defaults must remain unchanged by the explicit load.
- **Re-initializing the effects on every switch would fail**
  `dsp_continuity`: both lanes run the full wet preset (noise reduction,
  reverb, megabass, surround) through irregular 1–1024-sample blocks;
  resetting the filter/delay history when another handle is touched changes
  the samples, so byte-for-byte equality with the exclusive run breaks.
- **Sharing one callback would fail** `independent_callbacks`: stereo and mono
  handles register different callbacks, and the test asserts each callback
  observes only its own reads with its own channel/sample arguments, including
  after one callback is cleared and one handle is unloaded.
- **Sharing clock or output format would fail** `interleaved_formats`,
  `resampling_modes_limits` and `other_handle_lifecycle`, which interleave
  8/16/32-bit, mono/stereo and 11025/22050/44100 Hz lanes (all four
  resampling modes included) while foreign handles are loaded, sought and
  unloaded, comparing byte counts, PCM and order/row after every block.

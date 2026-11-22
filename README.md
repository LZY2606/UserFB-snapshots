[![Linux](https://github.com/Matroska-Org/libmatroska/actions/workflows/linux.yaml/badge.svg)](https://github.com/Matroska-Org/libmatroska/actions/workflows/linux.yaml)
[![Windows](https://github.com/Matroska-Org/libmatroska/actions/workflows/windows.yaml/badge.svg)](https://github.com/Matroska-Org/libmatroska/actions/workflows/windows.yaml)
[![macOS](https://github.com/Matroska-Org/libmatroska/actions/workflows/macos.yaml/badge.svg)](https://github.com/Matroska-Org/libmatroska/actions/workflows/macos.yaml)
[![GCC10](https://github.com/Matroska-Org/libmatroska/actions/workflows/linux-gcc10.yaml/badge.svg)](https://github.com/Matroska-Org/libmatroska/actions/workflows/linux-gcc10.yaml)

# libmatroska
a C++ library to parse and create Matroska files

# Building and installing

## Building and installing the library

libmatroska is based on `cmake`. It requires a C++ compiler compatible
with the C++14 standard as well as
[libebml](https://github.com/Matroska-Org/libebml). This means that
the normal build process consists of the usual steps:

1. Create a build directory: `mkdir build ; cd build`
2. Generate the make file: `cmake ..`
3. Compilation: `make`
4. Installation (run this as root): `make install`

## Customizing what's build with options

By default only a static library is built.

This library supports the usual `cmake` options for specifying
installation paths (e.g. `-DCMAKE_INSTALL_PREFIX=/opt/libmatroska`). The
following additional `cmake` options are supported:

* `-DEbml_DIR=/path/to/libebml` — if `libebml` was installed in a
  non-default location, you have to tell `cmake` where to find
  it. Point this variable to the directory containing the
  `EbmlConfig.cmake` file.
* `-DDISABLE_PKGCONFIG=YES` — don't generate and install the
  `libebml.pc` package configuration module for `pkg-config`
* `-DDISABLE_CMAKE_CONFIG=YES` — don't generate and install the
  package configuration module for `cmake`
* `-DBUILD_SHARED_LIBS=YES` — build the shared library instead of the
  static one (default: no)

## Codec-state-aware cues

By default `KaxCues` only writes a `CueCodecState` for an indexed block
whose own `BlockGroup` carries a `KaxCodecState`. When the codec state
of a track can change in `BlockGroup`s that are not themselves index
points, enable codec state tracking before registering any block and
before rendering any cluster into the cues:

```cpp
KaxCues cues;
cues.SetGlobalTimecodeScale(TimecodeScale);
if (!cues.SetCodecStateTracking(true)) {
  // called too late: blocks were already registered with, or clusters
  // already rendered into, this KaxCues; the previous mode is kept
}

// register the index points, then render the clusters as usual
cues.AddBlockBlob(*blob);
cluster.Render(output, cues, false);
// ...
cues.Render(output, false);
```

With tracking enabled, every successfully written `BlockGroup` updates
the per-track codec state — even groups that have no cue of their own —
following the physical order of the blocks in the file. Each indexed
block gets a `CueCodecState` pointing at the `KaxCodecState` element in
effect for its track (relative to the start of the Segment data, 0 —
i.e. omitted — when the track never had one) and a `CueBlockNumber`
with its 1-based physical index inside its cluster. The source Segment,
Cluster and block objects must stay alive until the final
`cues.Render()`. Clusters may be rendered again (for example relocated
to another output after inserting extra `Info`/`Void` elements); the
cue entries are then updated in place instead of being duplicated. If a
`Cluster.Render()` fails because the output threw, the partial update
is not committed and the render can simply be retried.

On the reading side, the decode position for a track at a given
timecode (in nanoseconds) can be looked up directly from the parsed
cues:

```cpp
const KaxCueTrackPositions *pos = cues.GetDecodePosition(timecode, trackNumber);
if (pos != nullptr) {
  uint64 clusterPos = pos->ClusterPosition();
  uint64 codecState = 0; // CueCodecState defaults to 0 when absent
  const auto *state = static_cast<const KaxCueCodecState *>(
    pos->FindFirstElt(EBML_INFO(KaxCueCodecState)));
  if (state != nullptr)
    codecState = static_cast<uint64>(*state);
}
```

`GetDecodePosition()` returns the latest cue point of the track that is
not after the requested timecode (ties are broken by the physical
position in the file), or `nullptr` when there is no such point.

## Code of conduct

Please note that this project is released with a [Contributor Code of Conduct](CODE_OF_CONDUCT.md). By participating in this project you agree to abide by its terms.

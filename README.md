libebur128
==========

libebur128 is a library that implements the EBU R 128 standard for loudness
normalisation.

All source code is licensed under the MIT license. See COPYING file for
details.

See also [loudness-scanner tool](https://github.com/jiixyj/loudness-scanner).

News
----

v1.2.6 released:
 * Fix dynamic linking on Windows.

v1.2.5 released:
 * Remove `BUILD_STATIC_LIBS` build option. Instead the CMake-supported
   `BUILD_SHARED_LIBS` option is now honored as expected.
 * Various code cleanups, warning fixes and documentation improvements
 * Fix issue related to filter state indexing with high channel enums (#77)
 * Introduce limits for number of channels and maximum supported samplerate to
   avoid integer overflows
 * Fix error return code of `ebur128_set_channel`. The actual behavior is now
   aligned to the documentation (#90).

v1.2.4 released:
 * Fix broken `ebur128_loudness_global_multiple()` function. Since v1.1.0 it
   calculated the relative threshold just from the last state given to it,
   resulting in wrong values.
 * More tests
 * Fix some minor build issues
 * Fix uninitialized memory in `ebur128_init()`, possibly resulting in wrong
   values

v1.2.3 released:
 * Fix uninitialized memory access during true peak scanning (bug #72)

v1.2.2 released (v1.2.1 was mistagged):
 * Fix a null pointer dereference when doing true peak scanning of 192kHz data

v1.2.0 released:

 * New functions for real time loudness/peak monitoring:
   * `ebur128_loudness_window()`
   * `ebur128_set_max_window()`
   * `ebur128_set_max_history()`
   * `ebur128_prev_sample_peak()`
   * `ebur128_prev_true_peak()`
 * New FIR resampler for true peak calculation, removing Speex dependency
 * Add true peak conformance tests
 * Bug fixes

v1.1.0 released:

 * Add `ebur128_relative_threshold()`
 * Add channel definitions from ITU R-REC-BS 1770-4 to channel enum
 * Fix some minor build issues

v1.0.3 released:

 * Fix build with recent speexdsp
 * Correct license file name
 * CMake option to disable static library
 * minimal-example.c: do not hard code program name in usage

Features
--------

* Portable ANSI C code
* Implements M, S and I modes
* Implements loudness range measurement (EBU - TECH 3342)
* True peak scanning
* Supports all samplerates by recalculation of the filter coefficients

Installation
------------

In the root folder, type:

    mkdir build
    cd build
    cmake ..
    make

If you want the git version, run simply:

    git clone git://github.com/jiixyj/libebur128.git

Usage
-----

Library usage should be pretty straightforward. All exported symbols are
documented in the ebur128.h header file. For a usage example, see
minimal-example.c in the tests folder.

Mix preview
-----------

With `EBUR128_MODE_MIX_PREVIEW` (which includes mode I) the library keeps,
for every complete 400 ms gating block (produced every 100 ms), the
covariance matrix of the K-weighted physical input channels instead of only
the weighted channel energies. The raw PCM is not stored. This allows
`ebur128_loudness_mix()` to report the integrated loudness of any linear
downmix of the inputs without replaying the audio:

* The mix matrix is row-major: `matrix[r * channels + c]` maps physical
  input channel `c` into mix output `r` (`1 <= rows <= 32`). Coefficients
  may be negative but must be finite.
* Each output channel energy is weighted by `weights[r]`; allowed weights
  are exactly 0.0, 1.0, 1.41 and 2.0.
* The channel map does not apply: columns always refer to physical input
  channels, so inputs mapped to `EBUR128_UNUSED` are still mixable.
* Gating happens on the mixed block energies: first the -70 LUFS absolute
  gate, then the -10 LU relative gate, exactly like
  `ebur128_loudness_global()`. Blocks below the absolute gate in the
  original domain are retained, so mixes that amplify can still pass.
* The number of retained blocks is `history / 100` (rounded down) of the
  effective history set with `ebur128_set_max_history()` (clamped to at
  least 400 ms); silent blocks count towards the history, and shortening
  the history evicts old blocks immediately.
* Querying does not advance filtering, time or gating statistics, so
  multiple matrices can be evaluated alternately.
* `EBUR128_MODE_MIX_PREVIEW` cannot be combined with
  `EBUR128_MODE_HISTOGRAM`; `ebur128_init()` returns NULL in that case.

See mix_preview_example.c in the test folder for a complete example.

On some operating systems, static libraries should be compiled as position
independent code. You can enable that by turning on `WITH_STATIC_PIC`.

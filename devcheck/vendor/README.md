# Offline decoder dependency

Unmodified `minimp3.h` and `LICENSE` (renamed `MINIMP3_LICENSE`) from
https://github.com/lieff/minimp3 at ea99364f61c14656440e8d77e9c233ccf3124633.
License: CC0-1.0. The embedded decoder is only a test dependency, not an encoder
implementation. No minimp3 source changes or SIMD dispatch extension are requested.

SHA256:
- minimp3.h: 57e437c5c1f0e8b243885d3929c8973b5e6c778451e0100ab4251d19915cb3ad
- MINIMP3_LICENSE: 6a1ee543e5282cd9061881edf462e6fdab181f328da71fc2c9a6950a80e94d01

The public WAV in `js/test/lib/encode.wav` belongs to the pinned Shine checkout;
its existing CLI path is exercised by `devcheck/legacy_cli.sh`.

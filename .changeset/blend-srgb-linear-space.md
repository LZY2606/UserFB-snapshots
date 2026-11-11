---
"chroma-js": minor
---

`chroma.blend()` and the eight `chroma.blend.<mode>()` shortcuts now accept an `options` argument with `space: 'srgb-linear'`. Colors are decoded with the sRGB transfer function, the blend mode is evaluated in linear-light sRGB with floating-point precision, and the result is encoded once; `rgb(false)` keeps the unrounded channels while `rgb()` and `hex()` round as before. Linear-light blending only supports opaque colors and throws a `RangeError` for any input whose alpha is not exactly `1`; an unknown `space` throws an `Error`. Omitting `options` or passing `{ space: 'srgb' }` keeps the existing encoded-sRGB behavior, including alpha compositing of translucent colors.

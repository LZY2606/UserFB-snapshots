# pcm-convert [![stable](https://img.shields.io/badge/stability-stable-green.svg)](http://github.com/badges/stability-badges)

Convert PCM audio data between formats — dtype, layout, endianness, container, and sample rate (polyphase FIR anti-aliased resampling via [`@audio/resample-polyphase`](https://github.com/audiojs/resample)). ESM.

[![npm install pcm-convert](https://nodei.co/npm/pcm-convert.png?mini=true)](https://npmjs.org/package/pcm-convert/)

```js
import convert, { parse, detect, stringify, sampleRate } from 'pcm-convert'

// dtype conversion
convert(new Float32Array([1, -1, 0.5]), 'int16')

// sample-rate conversion (rates on both sides → resampled, anti-aliased)
convert(samples, 'float32 44100', 'float32 48000')
convert(samples, 'int16 stereo interleaved 48000', 'float32 16000')  // the 48k → 16k voice-agent path

// interleaved → planar, dtype change
convert(new Uint8Array([127, 200, 127, 200]), 'uint8 stereo interleaved', 'float32 planar')

// endianness swap
convert(new Float32Array([1, .5, -.5, -1]), 'le', 'be')

// planar channel arrays → interleaved (replaces pcm-encode)
convert([new Float32Array([1, 0, -1]), new Float32Array([0, 0.5, -0.5])], 'int16 interleaved')

// AudioBuffer input (auto-detected)
convert(audioBuffer, 'int16 interleaved')

// any format → AudioBuffer (browser-native; Node.js auto-discovers audio-buffer package if installed)
convert(new Int16Array([...]), { dtype: 'int16', channels: 2, interleaved: true, sampleRate: 44100 }, 'audiobuffer')

// no AudioBuffer global and no synchronous require (a browser worker, Node before 20.19 / 22.12): the
// audio-buffer package loads in the background; await it before the first conversion to 'audiobuffer'
import { ready } from 'pcm-convert'
await ready

// write into existing buffer
convert(new Uint8Array([0, 255]), new Uint16Array(2))

// object format
convert(data, { dtype: 'float32', channels: 2, interleaved: false }, { dtype: 'int16', interleaved: true })

// format utilities
parse('float32 stereo planar 44100')  // → { dtype: 'float32', channels: 2, interleaved: false, sampleRate: 44100 }
stringify({ dtype: 'float32', channels: 2, interleaved: false })  // → 'float32 stereo planar'
detect(new Int16Array(4))  // → { dtype: 'int16' }
detect([new Float32Array(4), new Float32Array(4)])  // → { dtype: 'float32', channels: 2, interleaved: false }
```

## API

### `convert(src, from?, to?, dst?)`

Converts `src` from format `from` to format `to`. If `from` is omitted, it is detected from `src`. If `dst` is provided, result is written into it.

**Source types**: `Float32Array`, `Float64Array`, `Int8/16/32Array`, `Uint8/16/32Array`, `Array` (float -1..1), `Float32Array[]` (planar channels), `AudioBuffer` (Web Audio API), `ArrayBuffer`, `Buffer` (Node.js).

### Format

String (`'float32 stereo planar 44100'`) or object (`{ dtype, channels, interleaved, endianness, sampleRate, container }`). Tokens in any order; commas, semicolons, underscores accepted as separators.

| Field | Tokens / values |
|---|---|
| `dtype` | `float32`, `float64`, `float`, `int8`, `int16`, `int32`, `int`, `uint8`, `uint16`, `uint32`, `uint` |
| `channels` | `mono`, `stereo`, `quad`, `2.1`, `5.1`, `N-channel` — or any number in object format |
| `interleaved` | `interleaved`, `planar` — or boolean in object format |
| `endianness` | `le`, `be` |
| `sampleRate` | `8000` `11025` `16000` `22050` `44100` `48000` `88200` `96000` `176400` `192000` `352800` `384000` — or any number in object format |
| `container` | `array`, `arraybuffer`, `buffer` (Node.js), `audiobuffer` |

In string format, bare numbers are matched against the sample rate whitelist. Use `N-channel` for arbitrary channel counts.

When both `from.sampleRate` and `to.sampleRate` are present and differ, samples are resampled — rational polyphase FIR (Kaiser-windowed, ~85 dB stopband), output length `round(n · to/from)`. Equal or absent rates pass through untouched. `AudioBuffer` sources carry their own rate, so `convert(audioBuffer, 'audiobuffer 48000')` resamples and stamps the target rate.

Object aliases: `type` → `dtype`, `numberOfChannels` → `channels`, `rate` → `sampleRate`.

### Endianness and the two kinds of input

The source argument is interpreted according to its container, not to `from.endianness` alone:

- **Raw byte containers — `Buffer` (Node.js) and `ArrayBuffer`** — hold bytes, not samples. They are decoded first
  with `from.dtype` and `from.endianness` (`'le'` / `'be'`). If `from.endianness` is omitted the platform's native
  byte order is assumed. A source whose length is not a whole number of samples (or whole channel frames when
  `channels` is given) throws before anything is written.
  ```js
  // bytes 40 00 c0 00, big-endian int16 → +16384, -16384 → 0.5, -0.5
  convert(Buffer.from([0x40, 0x00, 0xc0, 0x00]), { dtype: 'int16', endianness: 'be' }, 'float64')
  // → Float64Array [ 0.5, -0.5 ]
  ```
  A `Buffer` view's own `byteOffset`/`byteLength` is honoured — only its visible region is read.

- **Numeric containers — plain `Array`, every `TypedArray`, planar `TypedArray[]`, and `AudioBuffer`** — already hold
  sample values. They are converted numerically; `from.endianness` never reinterprets their underlying memory, so a
  big-endian `Int16Array` token changes nothing about how its elements are read.

`int8`/`uint8` are single-byte dtypes: endianness has no effect on them in either direction.

### Output byte order and destinations

`to.endianness` fixes the **physical byte order** of every multi-byte output and applies even when the source
declares no endianness. A returned `TypedArray` still reads back in native order as numbers, so verify big-endian
output by inspecting the bytes (e.g. `Buffer.readInt16BE`), not by reading the view's elements:

```js
let out = convert(new Int16Array([1, -2]), 'int16', { dtype: 'int16', endianness: 'be' })
Buffer.from(out.buffer).toString('hex')   // '0001fffe' — big-endian on the wire
```

A plain `Array` result (JS numbers) and an `AudioBuffer` (native-order float32 channels) have no physical byte
order, so `endianness` is not represented there.

When a `dst` is supplied it is always the object returned, and only its valid region is written: a `subarray`
destination leaves every byte outside the view untouched, and an `ArrayBuffer` destination is returned as that same
`ArrayBuffer`. If the source and destination share underlying storage, the result matches what an independent copy
of the source would produce. An undersized destination, an incomplete raw source, or an incomplete channel frame
throws before the destination is modified.

### `parse(fmt)` → descriptor  •  `detect(data)` → descriptor  •  `stringify(descriptor, omit?)` → string

### `sampleRate` — standard sample rates array

## Absorbs

- [audio-format](https://npmjs.org/package/audio-format) → `parse`, `detect`, `stringify`
- [sample-rate](https://npmjs.org/package/sample-rate) → `sampleRate`
- pcm-encode → `convert([...channels], format)`

## License

MIT

<p align=center><a href="https://github.com/krishnized/license/">ॐ</a></p>

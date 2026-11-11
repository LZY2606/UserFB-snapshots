# flac

[![Build Status](https://travis-ci.org/mewkiz/flac.svg?branch=master)](https://travis-ci.org/mewkiz/flac)
[![Coverage Status](https://img.shields.io/coveralls/mewkiz/flac.svg)](https://coveralls.io/r/mewkiz/flac?branch=master)
[![GoDoc](https://pkg.go.dev/badge/github.com/mewkiz/flac)](https://pkg.go.dev/github.com/mewkiz/flac)

This package provides access to [FLAC][1] (Free Lossless Audio Codec) streams.

[1]: http://flac.sourceforge.net/format.html

## Documentation

Documentation provided by GoDoc.

- [flac]: provides access to FLAC (Free Lossless Audio Codec) streams.
    - [frame][flac/frame]: implements access to FLAC audio frames.
    - [meta][flac/meta]: implements access to FLAC metadata blocks.

[flac]: http://pkg.go.dev/github.com/mewkiz/flac
[flac/frame]: http://pkg.go.dev/github.com/mewkiz/flac/frame
[flac/meta]: http://pkg.go.dev/github.com/mewkiz/flac/meta

## Encoding with automatic compression

`Encoder` writes frames verbatim by default, preserving the original byte-level
behavior. An encoder can opt into per-frame automatic compression with
`EnableAutoCompression(true)`:

```go
enc, err := flac.NewEncoder(w, info)
if err != nil {
    log.Fatal(err)
}
enc.EnableAutoCompression(true)
```

Automatic compression is disabled by default and only engages with frames
whose subframes are all verbatim with no wasted bits; frames that explicitly
select another prediction method keep using the original write path. The
supported input range is one or two channels of 8, 16 or 24-bit signed PCM,
with the frame block size matching each subframe's `NSamples` and `Samples`
length. Mono frames analyze their own samples. Stereo frames compare the
independent (L,R), left/side, side/right and mid/side representations, and each
candidate subframe is stored as constant, verbatim, or fixed prediction of
order 0 through 4 with Rice1 residuals and partition orders 0 through 8 (Rice
parameter k in 0 through 14). Rice2, escape codes, LPC prediction and
wasted-bit search are not used.

Candidates are ranked by the total effective bit count of the two subframes,
including subframe headers, warm-up samples, residual method and partition
fields and every partition parameter, so constant blocks and high-entropy
short blocks participate in the selection instead of being forced through
fixed prediction. Side channels use a container one bit wider than the
declared audio bit depth; the frame header still declares the original bit
depth, and the StreamInfo sample/channel counts and MD5 checksum describe the
original interleaved PCM. The caller's PCM, channel assignment and prediction
metadata are never modified. See `ExampleEncoder_EnableAutoCompression` for a
runnable example.

## Changes

* Version 1.0.8 (2023-04-09)
    - Fix race condition when reading meta data (see [#56](https://github.com/mewkiz/flac/pull/56)). Thanks to [Zach Orosz](https://github.com/zachorosz).
    - Fix encoding of 8-bps WAV audio samples (see [#52](https://github.com/mewkiz/flac/pull/52)). Thanks to [Martijn van Beurden](https://github.com/ktmf01).
    - Fix StreamInfo block type error message (see [#49](https://github.com/mewkiz/flac/pull/49)).

* Version 1.0.7 (2021-01-28)
    - Add seek API (see [#44](https://github.com/mewkiz/flac/pull/44) and [#46](https://github.com/mewkiz/flac/pull/46)). Thanks to [Craig Swank](https://github.com/cswank).

* Version 1.0.6 (2019-12-20)
    - Add experimental Encoder API to encode audio samples and metadata blocks (see [#32](https://github.com/mewkiz/flac/pull/32)).
    - Use go.mod.
    - Skip ID3v2 data prepended to flac files when parsing (see [36cc17e](https://github.com/mewkiz/flac/commit/36cc17efed51a9bae283d6a3a7a10997492945e7)).
        - Remove dependency on encodebytes. Thanks to [Mikey Dickerson](https://github.com/mdickers47).
    - Add 16kHz test case. Thanks to [Chewxy](https://github.com/chewxy).
    - Fix lint issues (see [#25](https://github.com/mewkiz/flac/issues/25)).

* Version 1.0.5 (2016-05-06)
    - Simplify import paths. Drop use of gopkg.in, and rely on vendoring instead (see [azul3d/engine#1](https://github.com/azul3d/engine/issues/1)).
    - Add FLAC decoding benchmark (see [d675e0a](https://github.com/mewkiz/flac/blob/d675e0aaccf2e43055f56b9b3feeddfdeed402e2/frame/frame_test.go#L60))

* Version 1.0.4 (2016-02-11)
    - Add API examples to documentation (see [#11](https://github.com/mewkiz/flac/issues/11)).
    - Extend test cases (see [aadf80a](https://github.com/mewkiz/flac/commit/aadf80aa28c463a94b8d5c49757e5a0948613ce2)).

* Version 1.0.3 (2016-02-02)
    - Implement decoding of FLAC files with wasted bits-per-sample (see [#12](https://github.com/mewkiz/flac/issues/12)).
    - Stress test the library using [go-fuzz](https://github.com/dvyukov/go-fuzz) (see [#10](https://github.com/mewkiz/flac/pull/10)). Thanks to [Patrick Mézard](https://github.com/pmezard).

* Version 1.0.2 (2015-06-05)
    - Fix decoding of blocking strategy (see [#9](https://github.com/mewkiz/flac/pull/9)). Thanks to [Sergey Didyk](https://github.com/sdidyk).

* Version 1.0.1 (2015-02-25)
    - Fix two subframe decoding bugs (see [#7](https://github.com/mewkiz/flac/pull/7)). Thanks to [Jonathan MacMillan](https://github.com/perotinus).
    - Add frame decoding test cases.

* Version 1.0.0 (2014-09-30)
    - Initial release.
    - Implement decoding of FLAC files.

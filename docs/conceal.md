# AAC access-unit concealment

`NeAACDecConceal()` and `NeAACDecConceal2()` synthesize one missing 1024-sample
AAC-LC access unit without receiving compressed input:

```c
void *NeAACDecConceal(NeAACDecHandle decoder, NeAACDecFrameInfo *info);

void *NeAACDecConceal2(NeAACDecHandle decoder,
                       NeAACDecFrameInfo *info,
                       void **sample_buffer,
                       unsigned long sample_buffer_size);
```

The return value, `info`, and buffer ownership are the same as
`NeAACDecDecode()` and `NeAACDecDecode2()`. A successful call sets
`bytesconsumed = 0`, `error = 0`, and
`samples = 1024 * info->channels`.

## Supported scope

Concealment is intentionally limited to:

- the normal floating-point `libfaad` build;
- raw AAC-LC initialized with `NeAACDecInit2()`;
- 44.1 kHz or 48 kHz;
- one SCE (mono) or one CPE (stereo);
- `FAAD_FMT_16BIT` or `FAAD_FMT_FLOAT` output.

PCE, SBR/PS, downmixing, configuration changes, ADTS/ADIF initialization, and
other object types are outside the scope. Such handles return `NULL` with a
nonzero error instead of attempting concealment.

At least one complete and successful AU must have been decoded. The first good
AU keeps FAAD's normal priming suppression (`samples == 0`), but it still
establishes concealment history. A suppressed first AU does not suppress a
later concealed AU.

## Buffer handling

`NeAACDecConceal()` uses the decoder-owned PCM buffer. The returned pointer has
the same lifetime as a pointer returned by `NeAACDecDecode()`: it remains valid
until the next decode/conceal call that reallocates or overwrites it, or until
`NeAACDecClose()`.

`NeAACDecConceal2()` writes into the caller's buffer through the supplied
non-null `void **`. Its capacity is measured in bytes. For the supported
formats a complete frame needs:

- mono 16-bit: `1024 * 1 * 2`;
- stereo 16-bit: `1024 * 2 * 2`;
- mono float: `1024 * 1 * sizeof(float)`;
- stereo float: `1024 * 2 * sizeof(float)`.

A null/empty buffer or a capacity smaller than one complete interleaved frame
returns `NULL`, `error = 27`, and `samples = 0`. That rejection does not modify
the caller's buffer or decoder state, so retrying later with enough capacity is
equivalent to one successful call.

## State and invalidation

The decoder keeps a fixed-size copy of the most recent good AU's per-channel
spectral coefficients after PNS, mid/side, intensity stereo, TNS, and DRC, but
before `ifilter_bank()`. It also keeps that AU's window sequence and shape.

For consecutive missing frame `k` (starting at `1`), the original good spectrum
is multiplied by `0.75^k`; the stored good spectrum is never itself attenuated.
For `k >= 32` the spectral vector is zero. The real filterbank overlap,
previous window shape, synthesis window sequence, and frame counter are
advanced by every successful concealment call. Concealment does not draw PNS
random numbers or run TNS/DRC again.

History is invalidated by:

- a failed normal decode;
- `NeAACDecPostSeekReset()`;
- reinitialization with `NeAACDecInit()` or `NeAACDecInit2()`.

The next complete good AU installs fresh history and clears the consecutive
missing-frame counter.

## Example

After building the library with `bash task_support/build.sh`:

```sh
cc -std=c99 -O2 -Iinclude examples/conceal_example.c \
   .task-build/library/libfaad.a -lm \
   -o .task-build/conceal_example
.task-build/conceal_example task_support/normal.aac
```

The example parses ADTS headers, passes the raw AU payloads to
`NeAACDecDecode2()`, and replaces one known-lost AU with
`NeAACDecConceal2()`.

## Tests

```sh
bash task_support/run.sh target
```

`tests/conceal.c` registers `spectrum`, `continuity`, and `buffers` groups.
Its reference copies a good AU's pre-filterbank spectrum and current overlap,
then drives an independent `ifilter_bank()` instance rather than refreshing
expectations from the decoder under test after each missing frame.

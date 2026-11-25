# AAC access-unit concealment

FAAD2 provides two packet-loss concealment entry points:

```c
void *NeAACDecConceal(NeAACDecHandle hDecoder,
                      NeAACDecFrameInfo *hInfo);

void *NeAACDecConceal2(NeAACDecHandle hDecoder,
                       NeAACDecFrameInfo *hInfo,
                       void **sample_buffer,
                       unsigned long sample_buffer_size);
```

Their return types and output-buffer semantics match `NeAACDecDecode` and
`NeAACDecDecode2`, but neither call accepts compressed input. A successful
call synthesizes one missing 1024-sample AAC access unit:

- `hInfo->bytesconsumed == 0`
- `hInfo->error == 0`
- `hInfo->samples == 1024 * channels`
- sample rate and channel positions come from the active decoder configuration

`NeAACDecConceal` uses the decoder-owned output buffer. `NeAACDecConceal2`
uses the caller's buffer; `sample_buffer` must be a non-null `void **` whose
pointee is non-null, and `sample_buffer_size` is its capacity in bytes. The
complete float stereo frame needs `1024 * 2 * sizeof(float)` bytes; the
16-bit stereo frame needs `1024 * 2 * sizeof(int16_t)` bytes. An empty buffer
or an undersized buffer returns `NULL` with `error == 27` and
`samples == 0` without modifying decoder state.

## When concealment is available

The current implementation supports:

- the normal floating-point library build;
- raw AAC-LC initialized with `NeAACDecInit2`;
- 44.1 kHz or 48 kHz;
- one mono SCE or one stereo CPE;
- `FAAD_FMT_16BIT` or `FAAD_FMT_FLOAT` output.

PCE, SBR/PS, downmixing, mid-stream configuration changes, ADTS/ADIF
concealment, and fixed-point builds are outside the supported scope. Normal
ADTS/ADIF decoding and the existing decode entry points keep their original
behavior.

At least one complete, error-free audio AU must have been decoded before
calling concealment. The first good AU still follows FAAD2's original priming
suppression (`NeAACDecFrameInfo.samples == 0`), but it establishes history.
Concealment itself is never priming-suppressed.

History is invalidated by:

- a failed normal decode;
- `NeAACDecPostSeekReset`;
- reinitialization with `NeAACDecInit2` or `NeAACDecInit`.

The next complete good AU rebuilds it. Calls made without usable history
return `NULL`, a non-zero error, and `samples == 0` without advancing audio
state. The new error codes and messages are code 35,
"Concealment requires a successfully decoded AAC frame", and code 36,
"Concealment is not supported for this configuration"; code 27 remains the
buffer-too-small error.

## Concealment behavior

The decoder atomically saves the most recent complete good AU's per-channel
1024 spectral coefficients after PNS, mid/side decoding, intensity stereo,
TNS, and DRC, but immediately before the real `ifilter_bank()` call. It also
saves the window sequence and shape. Quantized coefficients and output PCM are
not used.

For the `k`-th consecutive missing AU, synthesis uses the immutable good
spectrum multiplied by `0.75^k`; the stored spectrum is never repeatedly
attenuated. For `k >= 32`, a zero spectrum is used and the counter does not
wrap. Missing AUs do not run PNS, TNS, or DRC again and do not consume PNS
random numbers.

The synthesis window sequence advances from the last actually synthesized
sequence:

| Last sequence | Next concealment sequence |
| --- | --- |
| `ONLY_LONG` | `ONLY_LONG` |
| `LONG_START` | `EIGHT_SHORT` |
| `EIGHT_SHORT` | `EIGHT_SHORT` |
| `LONG_STOP` | `ONLY_LONG` |

Spectrum coefficients are reused only when the saved and synthesized
geometries are both long or both short. In particular, a long `LONG_START`
history moving to `EIGHT_SHORT` uses a zero spectrum rather than
reinterpreting the 1024 long coefficients. The normal short-block filterbank
still runs and the existing overlap is retained, producing the correct tail
response.

Every successful concealment advances the real overlap, previous window shape,
and frame counter. The following good compressed AU resumes from that state;
after it succeeds, the consecutive-missing count returns to zero.

## Example

A runnable example is provided in `examples/conceal_example.c`. After
`bash task_support/build.sh`:

```sh
cc -std=c99 -O2 -Iinclude examples/conceal_example.c \
   .task-build/library/libfaad.a -lm \
   -o .task-build/conceal_example

.task-build/conceal_example task_support/normal.aac out.pcm 3
```

The test suite in `tests/conceal.c` contains independent filterbank
references and can be run with:

```sh
bash task_support/run.sh target
```

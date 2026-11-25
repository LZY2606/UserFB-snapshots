# Float sideband bit-level compatibility tests

These tests pin the on-wire behavior of the WavPack 32-bit float "sideband"
(the WVX bitstream plus the `ID_FLOAT_INFO` metadata). They complement, and do
not replace, the existing `cli/wvtest.c` float coverage.

## How to run

```sh
bash task_support/run.sh baseline   # pre-existing 16-pattern float round trip
bash task_support/run.sh target     # this regression plus two semantic mutants
```

`target` first runs the normal regression (it must print `TARGET checks=...`
and exit 0), then rebuilds the test twice with one compilable semantic change
applied to `src/unpack_floats.c`:

* `negative_zero` — the negative-zero sign bit is still consumed from the
  sideband but discarded (`set_sign(outval, getbit(...) & 0)`);
* `nan` — the 23-bit NaN payload read from the sideband is replaced by zero
  (`set_mantissa(outval, 0)`).

Each mutant must print a line beginning with `FAIL ` and exit with status 42.
A build failure, a crash, or a run with zero assertions does not count as
catching the change.

## Files

| File | Role |
| --- | --- |
| `tests/float_wire_model.h/.c` | Independent reference model. Takes only bit patterns and raw byte arrays; never includes `WavpackStream`, never performs float arithmetic, never calls the production `getbits`/`putbits` macros or float conversion helpers. |
| `tests/float_wire_packets.c/.h` | Public container material: in-memory block output/input through the public API, little-endian header parsing, metadata chunk traversal, and the block rebuilder used for the handcrafted new-format WVX material. |
| `tests/float_wire_test.c` | `void task_cases(void)` plus the generated audio material; all comparisons go through `TASK_CHECK` and there is no local `main`. |

## What the independent reference proves

WavPack stores 32-bit float audio as a 24/25-bit integer "main" value in the
regular WV bitstream plus, when needed, a lossless sideband in WVX. Numeric
float comparison is not sufficient here because `-0.0`, NaN payloads and
subnormals are distinguished only by their exact 32 bits.

The model in `float_wire_model.[ch]` is built solely from explicit IEEE-754
sign/exponent/mantissa fields (`FWM_SIGN`, `FWM_EXP`, `FWM_MANT`) and its own
LSB-first bit array. From the raw input patterns it independently derives,
for every block:

* the shared block exponent (`max_exp`, the rounded exponent of the block's
  maximum finite magnitude, which changes how every other sample is aligned);
* the signed main integer written to the WV stream and `float_shift`;
* the block `float_flags`, covering `SHIFT_ONES`, `SHIFT_SAME`,
  `SHIFT_SENT`, `ZEROS_SENT`, `NEG_ZEROS` and `EXCEPTIONS`, including the
  no-WVX integer-only path;
* the exact sideband bit range consumed by each sample. Adjacent values share
  one contiguous bit array; per-sample offsets are asserted to walk the stream
  with no per-sample realignment;
* the running `crc_x` (`crc*27 + mantissa*9 + exponent*3 + sign`).

The same model drives both directions:

1. **Write direction** — the model emits the sideband it predicts
   (`fwm_send`); the test encodes the same patterns through
   `WavpackOpenFileOutput`/`WavpackPackSamples`, walks the resulting chunks
   with the public container traversal, and compares the physical
   `ID_FLOAT_INFO` bytes and every sideband bit (excluding byte-alignment
   padding) against the model. The consumed length must match exactly. A
   changed encoder and decoder that still read each other's output would fail
   here because the prediction is external to both.
2. **Read direction** — the test assembles its own main integers and sideband
   and calls the public decode path; `fwm_restore` independently reconstructs
   every 32-bit pattern and the final `crc_x`, which are compared sample by
   sample.

## What the existing MD5 round trip proves

`wvtest` (and the MD5 checks in this test) compute an MD5 over the decoded
sample words and compare it with the MD5 stored in the file. That is a strong
end-to-end integrity check: any changed output bit, lost frame or channel
reordering breaks the stored hash, and it exercises the whole public API
including multi-block and seek behavior.

It cannot, by itself, say *where* the wire format changed. If the encoder and
the decoder were both modified to agree on a new sideband layout, the decoded
words — and therefore the MD5 — could still match. The bit-level reference
closes that gap by pinning the shared exponent, main integers, flags,
per-sample sideband slices and CRC independently of the implementation, and
by reporting the first divergent layer (FLOAT_INFO byte, sideband bit with
the owning sample, or reconstructed pattern).

## Material and path coverage

The block maximum finite value is chosen deliberately so it shifts the
representation of every other sample in the block. The groups contain normal
positive/negative audio, integer-precision floats (no WVX), all-zero blocks,
mixed positive/negative zero, the smallest and largest subnormals, finite
values small enough that the main integer becomes zero, +/-infinity, and NaNs
with different signs and payloads.

| Group | Scheme exercised |
| --- | --- |
| `shift_sent` | `FLOAT_SHIFT_SENT` with mixed shifted bits, false zeros, negative zeros and exceptions; also the `max_exp >= 25` exponent-byte path |
| `shift_ones` | `FLOAT_SHIFT_ONES` only, plus zero markers and negative-zero sign bits |
| `shift_same` | `FLOAT_SHIFT_SAME` (shifted parts all equal, ones and zeros), plus false zeros |
| `integer` | no sideband at all; trailing-zero `float_shift` reduction and `float_values_nowvx` |
| `big_scale` | largest finite float drives `max_exp` to 255, forcing exponent bytes and false zeros |

### New WVX `min_shifted_zeros` / `max_shifted_ones`

The production float encoder still writes the legacy `ID_WVX_BITSTREAM`, so
the new `ID_WVX_NEW_BITSTREAM` float layout is exercised with handcrafted
material rather than a new encoder path. A real public-encoder block is taken,
its WVX chunk is replaced (every other chunk and the integer stream are kept,
so the audio CRC stays valid), and the block checksum is recomputed.

* `max_shifted_ones` — low one-bits are omitted on the wire and restored by
  the reader;
* `min_shifted_zeros` — low zero-bits are omitted;
* the 10-bit min/max preamble and the sideband are packed into one contiguous
  bitstream with no byte realignment, and the test asserts both that omitted
  bits are recovered and that the bit cursor lands exactly on the next sample
  (`bs.bitpos == valid_bits`).

Four zero bytes of physical padding follow each handcrafted sideband so the
production 16-bit `Bitstream` getbit/getbits macros always have a full word in
reserve. Padding is never counted as valid bits, and truncated-stream error
handling is unchanged — the padding is only a read reserve, not a change to
how corrupt input is tolerated.

## Public API encode/decode

Stereo (2-channel) and 5.1 (6-channel) material is generated entirely in C,
with a different bit-pattern plan per channel, spanning several compression
blocks and read back in small batches. Each returned `uint32_t` word, the
returned frame counts, `WavpackGetNumErrors`, `WavpackLossyBlocks`, the sample
index and the stored MD5 are checked. The chunk traversal prints the actual
`FLOAT_INFO` flags and WVX kind of *each* audio block rather than merely
probing for a byte somewhere in the file. The blocks confirm the encoder
still emits the legacy WVX (`wvx=legacy`) for float data.

The default non-normalizing read behavior is used (no `OPEN_NORMALIZE`), and
neither hybrid lossy nor DSD paths are exercised.

## Observed first differences under the mutants

The mutant changes corrupt values before the block's `crc_x` is finalized, so
the production CRC check mutes the whole block; the public decode then
returns zeroed words and records an error.

* `negative_zero` — direct semantic break: a negative zero (`0x80000000`)
  loses its sign and would reconstruct as positive zero (`0x00000000`). The
  first reported failure is the muted `shift_sent` block at sample 0
  (`0x447a0000 -> 0x00000000`), printed as
  `FAIL group 'shift_sent' decoded sample 0 ...`.
* `nan` — direct semantic break: a NaN such as `0x7fc12345` loses its payload
  and would reconstruct as positive infinity (`0x7f800000`). The first
  reported failure is likewise the muted `shift_sent` block at sample 0.

Both runs print `FAIL ` and exit 42, which `task_support/run.sh` requires.

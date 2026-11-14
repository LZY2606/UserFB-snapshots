# Embedded G.722 contract: reference provenance and coverage

`tests/embedded_qmf_contract.c` is a C regression suite for the parts of G.722
that the Python hash tests cannot see: the state shared by the three embedded
bit rates, the two QMF filters, and the bit-packing delays. It is compiled by
`bash task_support/run.sh target` together with `task_support/driver.c` and the
existing `checks/baseline.sh` six-configuration baseline.

All references are built from the ITU-T G.722 specification and computed in
plain C (plus the system math library for test-signal generation only). The
suite never downloads speech and never compares a lossy round trip against the
original PCM.

## What the old tests could not show

The existing Python tests (`tests/test_encode_decode.py`) take one PCM file and
pin the MD5 of the encoded bit stream and of the decoded PCM for six
configurations. That is a strong fingerprint, but it answers a single question:
"did every byte of this one input change?" It cannot:

- show that the 56 and 48 kbit/s streams are a strict truncation of the
  64 kbit/s stream. A hashed file would change wholesale even if only one rate
  disagreed, and the hash gives no position or direction.
- distinguish a bit-stream error from an audio error. A hash mismatch says the
  output differs, not whether the quantizer index, the predictor state, the
  band split or the synthesis filter moved.
- expose phase and ordering. Two filter branches with swapped coefficient
  direction produce a different waveform, but the hash only reports "different".
- show that the synthesis tail after silence is delivered and shaped correctly.
- show the packed-path call cadence: which codewords arrive on this call, which
  stay in the cache, and what the next call delivers.
- show anything about the *state relation* between rates: that the same four-bit
  low-band index drives an identical predictor and scale state at 48, 56 and
  64 kbit/s even though the reconstructed audio differs.

The new suite keeps bit-stream correctness ("is the codeword right") and audio
correctness ("is the PCM right") in separate groups, so a failure names which
one broke. It prints the direction, the bit rate, the sample or codeword
position and the first differing state field.

## Where each reference comes from

### QMF coefficients (`QMF_C`)

The 12-tap quadrature mirror filter from ITU-T G.722 (§3, "Quadrature mirror
filter"). The same normative values appear as `qmf_coeffs[]` in
`g722_encode.c` and `g722_decode.c`. The test carries its own copy so that a
change to the library table cannot silently become the reference. The test
does **not** read the library's `x[]` history; it recomputes the convolution.

### Transmit band split (`analysis_reference`)

Written directly from the natural-time sample sequence. For pair `n` the
odd-phase taps are `pcm[2n + 1 - 2j]` and the even-phase taps are
`pcm[2n - 22 + 2j]`, for `j = 0..11`; samples before the stream start are zero.
The two accumulators are combined as

```
low  = limit((sum_j C[j]*odd_j + sum_j C[j]*even_j) >> 14)
high = limit((sum_j C[j]*odd_j - sum_j C[j]*even_j) >> 14)
```

using 64-bit accumulation, explicit arithmetic right shift (floor division, as
the implementation's `>>`), and final saturation to int16. This is deliberately
**not** a copy of the library's two-sample buffer-shuffle loop. Reversing one
branch's coefficient direction in the library therefore changes the result
instead of being mirrored by the reference (see `analysis_reference_reversed`,
which the suite uses to prove the phase checks are sensitive).

### Transmit ADPCM (`transmit_ref`)

Two separate `g722_encoder_new(64000, 0)` instances are switched to
`itu_test_mode`. ITU test mode bypasses the band split and treats each input
sample as one sub-band sample, so a 64 kbit/s code carries the low-band
quantizer index in its low six bits and the high-band index in its top two
bits. The test feeds the low-band reference samples (`low << 1`) to one
instance and the high-band samples (`high << 1`) to the other; `<< 1` inverts
the `>> 1` that test mode applies, recovering the exact filter-domain value.
The expected 64 kbit/s codeword is then

```
low_code & 0x3F | ((high_code >> 6) << 6)
```

The two instances run continuously across the whole segment and never share
history. `case_analysis_band_isolation` confirms they are distinct instances
with distinct band state and non-identical index streams.

ITU test mode supplies **only** the ADPCM half of the model. The band split
above is the test's own convolution; the model never asks the library what the
filtered sub-bands should be.

### Receive ADPCM and band join (`synthesis_reference`)

A separate `g722_decoder_new(rate, 0)` in `itu_test_mode` produces the true
reconstructed `rlow` and `rhigh` (recovered as `out >> 1`). The test then
applies its own causal synthesis QMF. Writing pair `n` into the history as
`x[2n] = rlow[n] + rhigh[n]` and `x[2n+1] = rlow[n] - rhigh[n]` gives

```
out[2n]     = limit((sum_j C[j]*x[2n - j])           >> 11)
out[2n + 1] = limit((sum_j C[j]*x[2n + 1 - 11 + j])  >> 11)
```

Again 64-bit accumulation, floor shift and final limiting. The mirrored
coefficient order of the two phases is exactly what separates them. ITU mode
provides only the reconstructed sub-bands; the PCM expectation comes from the
test's own filter.

### Rate tables (`WM2`, `WM4`, `WM5`, `WM6`)

The normative inverse-quantizer tables from ITU-T G.722 for the high band and
for the 48 / 56 / 64 kbit/s low bands. They match the library's `qm2`, `qm4`,
`qm5`, `qm6` and serve two purposes: the known-answer reconstruction case (a
codeword decoded from a zero state must produce `(32*table) >> 15` in the low
band and `(8*WM2[ihigh]) >> 15` in the high band), and a human-readable record
of which table belongs to which rate.

## What each group owns

### `embedded`: the shared codeword stream

- `code_hierarchy` — over one continuous segment, every 64 kbit/s codeword
  shifted right by one bit equals the 56 kbit/s code and shifted right by two
  bits equals the 48 kbit/s code. One encoder instance per rate spans the
  segment; no per-block re-creation. Also checks the shared high-band index
  survives each truncation.
- `decoder_state_shared` — all three decoders get their own truncation of the
  same signal. The four-bit low-band index, and every meaningful field of both
  band predictors/scales, must agree at every position; the reconstructed low
  band must still differ, so the state equality is not vacuous.
- `refinement_state` — hand-made codewords `0x14` (refinement 0) and `0x17`
  (refinement 3) share low index 5 and high index 0. The reconstructed low
  band changes, but the scale, predictor, history and partial-reconstruction
  fields stay identical: the refinement bit is not part of the prediction
  history.
- `known_reconstruction` — every code value for each rate, decoded from a zero
  state, must equal the rate's own table with the rate's own index width.
  A 56 kbit/s decoder wrongly using the 64 kbit/s six-bit table fails here.
- `state_field_isolation` — two identical instances agree field by field even
  though the structure contains padding; a real difference must be reported.
  This is the explicit, field-wise alternative to a whole-struct `memcmp`.
- `eight_k_bypass` — 8 kHz mode: the low six bits match an ITU-mode encoder
  fed the same samples, the high two bits are constant, the QMF history stays
  zero, and the high-band state is untouched on both the encoder and the
  decoder. The 8 kHz decoder output equals the ITU-mode low-band output.

### `analysis`: transmit band split plus per-band ADPCM

The library encoder is fed the same signal in even-sample chunks (sample pairs
are never split) and its codewords are compared against the independent QMF
plus two continuously running ITU-mode encoders.

- `two_tone` — a low tone and an asymmetric high tone.
- `even_impulses` / `odd_impulses` — alternating-sign pulses placed on even
  and on odd sample positions.
- `signed_sequence` — a deterministic sequence with alternating sign.
- `silence_tail` — signal followed by zero samples. The filtered sub-bands
  still ring for the length of the QMF window, then go quiet, and every
  codeword must match the reference through and past the splice.
- `scaling` — a lone impulse at an odd position gives `low == high`, the same
  impulse one sample later gives `low == -high`, and the first non-zero pair is
  pinned exactly against the coefficient that lands under the impulse, which
  catches a wrong shift or scale factor.
- `band_isolation` — a witness that the two reference encoders keep separate
  band histories and produce non-identical index streams.
- `phase_order` — the two branches are separable: an odd-position pulse gives
  `low == high`, an even-position pulse gives `low == -high` (within the floor
  rounding), and the onset pair moves by one between the two cases. A reversed
  coefficient direction changes both relations.

### `synthesis`: receive QMF on independently decoded sub-bands

The library decoder and a fresh ITU-mode decoder are fed the same delivered
codewords; the test's own synthesis QMF turns the ITU-mode sub-bands into the
expected PCM, and every sample is compared.

- `segment` — all three rates, start to finish.
- `chunked` — the same codeword stream decoded in irregular blocks must match a
  single whole-array call exactly, proving the synthesis QMF state is carried
  between calls.
- `silence_tail` — a real signal encoded, then zero samples appended. The tail
  must match sample by sample and its energy must fall; the encoder still emits
  low-level codes in silence, so the tail is expected to settle, not vanish.
- `startup` — the first output samples from a zero state, catching any
  off-by-one in history shifting.
- `phase_order` — with the low band pinned to zero at 48 kbit/s only the high
  band excites the filter; the two output phases must differ, and the direct
  impulse response shows the even output walking the coefficients forward and
  the odd output backward.

Send and receive are verified separately: the transmit reference builds
codewords and the receive reference builds PCM, and neither is checked by
decoding the other and comparing to the original signal.

### `packing`: the packed 48 / 56 kbit/s paths

- `encode_lsb_first` — the library's packed bytes are compared against an
  independently LSB-first-packed copy of the non-packed codewords. Chunk
  lengths are irregular, so calls end mid-byte and residual bits carry across
  calls.
- `decode_prefix` — a known packed stream is fed in 1, 3, 11, ... byte
  chunks. Each call's delivered codeword count and valid residual bits are
  predicted by an independent bit count
  (`(8*bytes - 1)/bits`, residual `= 8*bytes - bits*delivered`). The delivered
  prefix must equal both the same codewords fed to a non-packed decoder and an
  independently unpacked prefix, and the cached residual bits must equal the
  next bits of the packed stream. This reproduces, rather than "fixes", the
  existing rule that the final cached codeword waits for the next non-empty
  call. A zero-length call must not drain the cache.
- `full_words` — a stream of complete codewords that happens to fill whole
  bytes must not yield padding samples; the last codeword is still cached and
  arrives on the next non-empty call as the last delivered-prefix sample.
- `interleaved` — two packed decoder instances fed alternating one-byte calls
  must each match their own non-packed reference and must not converge to
  each other.

Only the valid residual bits (`in_buffer & ((1u << in_bits) - 1)`) are
compared; the bits above `in_bits` are unspecified and never inspected.

## Running

```sh
bash task_support/run.sh target
bash tests/check_embedded_mutants.sh
```

`task_support/run.sh target` builds the new file, requires the driver to report
all four groups (`embedded`, `analysis`, `synthesis`, `packing`) and
`TARGET_COMPLETE`, then still runs the original six-configuration baseline.

`tests/check_embedded_mutants.sh` copies the tree into a scratch directory for
each degradation. Each copy must first build and pass unmutated; then the
mutation is applied and the copy must build and fail on a numeric content
assertion. Build errors, crashes, missing files and still-passing runs are all
reported as "not detected". The three degradations are:

- transmit QMF: one branch's coefficient direction reversed;
- 56 kbit/s decode: the six-bit reconstruction table reused;
- packed encode: carried residual bits cleared at the start of every call.

# LC3 spectral entropy content regression

This directory adds a self-contained, non-Python content regression for the
LC3 packet entropy layer (the forward arithmetic coder and the backward
ordinary bit plane) used by the spectral decoder.

It is built and run by the fixed entry point:

```
bash task_support/target.sh
```

which builds only with the system C/C++ compiler and the math library, runs
three groups (`planes`, `spectrum`, `frames`), then two injected mutants and
the existing `baseline.sh`. A content failure always prints a line starting
with `LC3_ENTROPY_MISMATCH` and exits with status `2`. Infrastructure/setup
problems use a different prefix (`LC3_ENTROPY_INFRA_ERROR`) and a non-2
status, so they cannot be mistaken for a detected entropy bug.

## Files

| File | Role |
| --- | --- |
| `entropy_reference.h` / `.cpp` | Independent C++17 model of the entropy layer. |
| `target_bridge.c` / `.h` | Plain-C bridge that `#include`s the real `src/spec.c` and exposes its file-static stages and read-only trace hooks. |
| `target.cpp` | The `void task_run(const char* group)` entry and all assertions. It defines no `main`. |
| `checks.h`, `main.cpp`, `build.sh`, `target.sh`, `mutate.awk`, `baseline.sh` | Pre-existing fixed harness, unchanged. |

## Boundaries of the independent model

`entropy_reference.*` is a from-scratch description of the format, not a copy
of the implementation:

- It keeps its own byte array, a backward LSB-first ordinary-bit cursor
  (`BackWriter`/`BackReader`), and a 24-bit forward interval coder
  (`AcEncoder`/`AcDecoder`).
- Arithmetic symbols are decoded by an explicit linear interval search over
  the cumulative-frequency table. The production binary decision tree in
  `bits.h` is not used.
- It never calls the production bit/spec readers, writers, budget or
  termination functions (`lc3_get_*`, `lc3_put_*`, `lc3_setup_bits`,
  `lc3_flush_bits`, `lc3_check_bits`, ...), and it does not reuse the
  production 32-bit accumulator cache or its renormalization helpers.
- The only imported material is format data: the probability distributions
  and spectral context constants of `tables.c`, reached through the C
  bridge accessors. The arithmetic model layout is mirrored by an
  ABI-identical POD type, so no production header (which relies on C
  designated initializers) is included by the C++17 model.
- The gain is evaluated independently as `double pow(10, g/28)`.

The model is deliberately restricted to ordinary (non high-resolution) LC3:
7.5/10 ms frames at 8/16/24/32/48 kHz. The high-resolution 48/96 kHz modes
are out of scope.

The C bridge, by contrast, compiles the real `src/spec.c` unchanged and drives
its actual static stages (`get_quantized`, `get_residual`, `get_lsb`,
`estimate_noise`, `fill_noise`) over raw packet bytes. Expectations always
come from the C++ model; the functions under test never compute an expected
value.

## What the groups actually exercise

### planes

A fixed symbol run on spectrum model 40 interleaved with ordinary bits is
encoded with the independent writer and decoded with the production reader,
then encoded with the production writer and decoded with the independent
reader. Both directions compare the actual symbols, individual bit values and
the logical remaining bit count; the two writers also emit byte-identical
packets.

Branches proven to execute (asserted at the end of the group):

- delayed carry propagation (`carry_events >= 1`),
- a run of at least three consecutive pending `0xff` bytes later resolved by
  the carry (`pending_ff_peak >= 3`, with the `0xff` run also verified in the
  emitted bytes),
- arithmetic termination sharing the last byte with the backward plane,
  including a deterministically chosen tight packet where both planes meet in
  the very same byte (`gap == 0`),
- ordinary bits crossing the 32-bit accumulator through a 25-bit + 1-bit +
  13-bit (39-bit) sequence, on a non-byte-aligned boundary.

### spectrum

Each case starts from an explicit signed integer coefficient plan. The plan
is first described independently (`ref_describe_plan`): escape-layer count,
sign positions and the evolving `(rate, half, state)` context, before any
packet exists. The packet is then parsed by both the independent reader and
the real static stages through the bridge. Integer quantized values are
required to be exactly equal; residual/LSB-refined values and gain/noise
spectra use the requested tolerance `2e-5 * (1 + |reference|)`.

Covered format branches:

- front/back half-spectrum model switch (11 and 4 pairs decoded with the
  second-half lookup),
- high-rate gate on both sides (`nbytes` below and above
  `20*(1+sr_ind)`),
- at least two escape layers (plans reach 2, 3 and 4 escape symbols on a
  single coefficient pair),
- residual correction with LSB disabled,
- LSB enabled with a zero high part recovered as a signed unit coefficient
  (a `-1` at index 0 and a `+1` at index 41),
- explicit deferred-bit exhaustion: cuts placed inside the quantization
  region stop the reader either in the middle of a pair or before a sign
  bit, recorded in `PairTrace.stop_at_sign`, without ever zero-filling
  unconsumed symbols.

### frames

Full legal packets are constructed in C++ from explicit side-info plans;
non-target fields use the production SNS/TNS writers only as constructors,
never as an entropy expectation source.

- One frame enables a non-zero TNS filter (order 4). Its arithmetic symbol
  prefix and the spectrum share one interval-coder state; the independent
  reader walks from the very first packet byte through the side info and the
  TNS symbols before reaching the spectrum (it never starts from a production
  cursor or snapshot).
- Under `LC3_ENTROPY_TRACE`, a read-only bridge hook snapshots the spectrum
  immediately after `spec_decode` and before TNS synthesis. The normal build
  has empty stubs and adds no state, callback or allocation. The snapshot is
  compared coefficient-by-coefficient with the independent gained spectrum.
- Checks cover continuous nonzero PCM, output sample count, the untouched
  stride-2 guard region, and two interleaved decoder instances.
- A constructed corrupt packet claims all coefficient pairs in a 40-byte
  frame. The bridge confirms it fails through explicit bit-budget exhaustion
  (`bits_left_after_quant == 0`, bitstream error), not an arbitrary bit flip.
  Two decoders with identical history then compare the corrupt frame against
  an explicit `NULL` PLC call for this frame and the following good frame;
  their PCM is identical.

MDCT, SNS, TNS filtering and PLC synthesis are intentionally not
re-implemented, and PCM round-tripping is never treated as lossless.

## First-difference diagnostics

Every per-coefficient failure reports duration, sample rate, packet size and
the coefficient index through a common `loc()` formatter, for example:

```
LC3_ENTROPY_MISMATCH spectrum: quant mismatch dt=3 sr=4 nbytes=80 idx=10
LC3_ENTROPY_MISMATCH lsb: production negative unit mismatch
```

The two mutants in `target.sh` demonstrate the diagnostic path:

- `context` removes the `(state << 4)` history term and is first caught at
  `dt=3 sr=4 nbytes=80 idx=10`,
- `lsb-sign` forces recovered unit coefficients positive and is caught at the
  negative-unit LSB check.

## Combinations not covered upstream

The Python `test/` suite treats `bitstream.py` as a reference model used by
the module tests; it does not include:

- a two-direction C-implementation vs independent-model byte/symbol/bit
  interchange with byte-identical output,
- explicit assertions on delayed carries and runs of pending `0xff` bytes,
- a deterministic shared-final-byte (`gap == 0`) termination case,
- ordinary-bit sequences that cross the 32-bit accumulator boundary,
- explicit mid-pair / before-sign truncation that forbids zero-filling,
- a corrupted frame proven to enter PLC through bit-budget exhaustion and
  matched against an explicit `NULL` PLC call over consecutive frames.

These are the gaps the new regression fills, while leaving the production
algorithm, public ABI, existing tests and the fixed harness untouched.

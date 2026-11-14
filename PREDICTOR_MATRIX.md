# Predictor / Matrix / Packet Regression Tests

`tests/predictor_matrix.cpp` adds numerical regression coverage for the
ALAC decoder's dynamic predictor, stereo matrixing and full compressed
packet decoding. Production code and public interfaces are unchanged; the
tests plug an independent reference model into the real decode paths.

## Reference model (independent of production predictor/matrix code)

The reference works in natural sampling coordinates: the prediction
history at sample `j` is simply `out[j-1], out[j-2], ...`. It implements
its own signed rounding, channel bit-width truncation and coefficient
adaptation, and never calls `unpc_block`/`pc_block`/`unmix*` to compute
expectations.

Signed rounding used everywhere in the reference:

    floor_shift(w, s) = floor(w / 2^s)          (exact for negative w)
    sign(v)           = (v > 0) - (v < 0)

Channel truncation to `chanbits`:

    wrap(x) = sign_extend_{chanbits}(x mod 2^chanbits)

### Predictor reference (orders 1..30, plus the 0 and 31 branches)

    out[0] = pc[0]
    order 0:   out[j] = pc[j]                       (verbatim copy)
    order 31:  out[j] = wrap(pc[j] + out[j-1])      (first difference)
    1..30:
      j <= order:  out[j] = wrap(pc[j] + out[j-1])
      j >  order:  top  = out[j-order-1]
                   pred = floor_shift(sum_k a[k]*(out[j-1-k] - top)
                                      + 2^(denshift-1), denshift)
                   out[j] = wrap(pc[j] + top + pred)

Coefficient adaptation is driven by the residual sign `sg = sign(pc[j])`
with `del0 = pc[j]`, scanning taps from the most recent history sample
(`k = order-1`) to the earliest (`k = 0`):

    sg > 0:  dd = top - out[j-1-k]
             a[k] -= sign(dd)
             del0 -= (order-k) * floor_shift(sign(dd)*dd, denshift)
             stop when del0 <= 0
    sg < 0:  a[k] += sign(dd)
             del0 -= (order-k) * floor_shift(-sign(dd)*dd, denshift)
             stop when del0 >= 0
    sg = 0:  no adaptation

Test material alternates positive/negative/zero residuals so that both
early stops (del0 crosses zero before the earliest tap) and full passes
(updating the earliest coefficient) are provably reached — the reference
counts both events and the cases assert the counts are nonzero.

### Matrix reference

    mixRes != 0:  L = u + v - floor_shift(mixRes * v, mixBits)
                  R = L - v
    mixRes == 0:  L = u, R = v

`mixRes` is signed; negative `mixRes*v` products are where floor rounding
differs from truncation toward zero, and the vectors are built to contain
them (asserted via a count of such samples). Shift-off output stages are
modeled as `(x << 8*bytesShifted) | shiftByte` with the decoder's
little-endian 24-bit byte layout.

## What each group covers

### predictor (direct `unpc_block` calls)

* `order4_unrolled` — 4-tap unrolled loop, 16-bit channel width; samples
  and final coefficients compared point by point.
* `order8_unrolled` — 8-tap unrolled loop, 17-bit channel width.
* `general_order` — order 6, exercising the generic prediction loop.
* `special_branches` — order 0 (verbatim copy, no truncation), order 31
  first-difference branch run in place (`pc == out`, as the decoder does),
  and in-place output through the unrolled 8-tap loop.

### matrix (direct `unmix16/24/32` calls)

* `unmix16_rounding` — positive and negative `mixRes`, separated stereo
  (`mixRes == 0`), and a stride larger than the channel count.
* `unmix24_shift_off` — one-byte shift-off with asymmetric low bytes,
  matrixed and separated paths, plus the no-shift path.
* `unmix32_shift_off` — two-byte shift-off with matrixing, plain
  interleave, and one-byte shift interleave.

### packets (full `ALACDecoder::Decode` on real compressed packets)

* `handbuilt_cpe_sce_24bit` — a 24-bit, 3-channel packet assembled with an
  independent MSB-first bit writer (only the residual entropy coding uses
  the production `dyn_comp`): one CPE (partial-frame length, one byte
  shifted off, `mixBits=2`/`mixRes=3`, 8-tap U channel, `modeV=1`
  two-stage prediction over the order-31 branch) followed by one SCE
  (4-tap, shift-off) and `ID_END`. Expected PCM comes solely from the
  residual plan through the reference predictor and matrix. Checks cover
  channel interleaving byte-for-byte, the decoded frame count, and the
  bit position after `ID_END` byte alignment.
* `production_encoder_compat` — compatibility control: a packet produced
  by the production `ALACEncoder` (partial frame, matrixed stereo) is
  parsed with the independent bit reader to confirm it is a real
  compressed (non-escape) CPE packet, then decoded and compared against
  the source PCM.

## Running

    bash task_support/run.sh target      # builds codec + tests, runs all groups
    bash task_support/variants.sh        # both source variants must be caught

`variants.sh` applies two temporary mutations in scratch copies: a flipped
coefficient-update direction in the predictor and a truncation-instead-of-
floor division in the matrix. Each must fail a content assertion with a
`CONTENT_MISMATCH` first-difference report (exit status 4) after a
successful compile/link.

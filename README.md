![eclint](https://github.com/aikiriao/SRLA/actions/workflows/eclint.yml/badge.svg?branch=main)
![C/C++ CI](https://github.com/aikiriao/SRLA/actions/workflows/c-cpp.yml/badge.svg?branch=main)
![Repo size](https://img.shields.io/github/repo-size/aikiriao/SRLA)
![License](https://img.shields.io/github/license/aikiriao/SRLA)

# SRLA

aka Soleil Rising Lossless Audio codec

# How to build

## Requirement

* [CMake](https://cmake.org) >= 3.15

## Build SRLA Codec

```bash
git clone https://github.com/aikiriao/SRLA.git
cd SRLA/tools/srla_codec
cmake -B build
cmake --build build
```

# Usage

## SRLA Codec

### Encode

```bash
./srla -e INPUT.wav OUTPUT.srl
```

#### Mode `-m`

You can change compression mode by `-m` option.
Following example encoding in maximum compression (but slow) option.

```bash
./srla -e -m 4 INPUT.wav OUTPUT.srl
```
#### Max block size `-B`

The'- B' option can change the maximum block size.
The following example encodes with a maximum block size of 4096 samples.

```bash
./srla -e -B 4096 INPUT.wav OUTPUT.srl
```

#### Number of divisions in block `-V`

You can change the number of divisions using the `-V` option. The number of divisions specifies the search depth of optimal block division.
The following example encodes with a number of divisions in the block to be $2^{2} = 4$.

```bash
./srla -e -V 2 INPUT.wav OUTPUT.srl
```

#### Lookahead sample factor `-L`

You can change the multiplying factor for lookahead samples in variable block division.
The following example is encoding with a lookahead factor of 8.

```bash
./srla -e -L 8 INPUT.wav OUTPUT.srl
```

#### Long-term prediction order `-P`

The `-P` option specifies long-term (pitch) prediction order, which improves the compression ratio, especially for vocal/pure tonal signals.
Following example encoding with long-term prediction order 3.

```bash
./srla -e -P 3 INPUT.wav OUTPUT.srl
```

### Decode

```bash
./srla -d INPUT.srl OUTPUT.wav
```
## LTP candidate comparison (C API)

The encoder can, per encoded signal representation and per channel, compare
several long-term (pitch) prediction configurations and pick the one with the
smallest actual bitstream cost:

* LTP disabled,
* first-order LTP at each candidate period,
* third-order LTP at each candidate period.

Each candidate is fitted, quantized and coded independently: the LPC analysis
and the entropy code length are recomputed on that candidate's own quantized
LTP residual, so a cheaper combination is never chosen based on correlation
peaks or prediction error energy alone. Ties prefer LTP disabled, then the
lower order, then the smaller period. Periods for which the history is too
short are skipped. When the feature is disabled, the encoded bytes are
unchanged from the legacy behaviour.

```c
#include "srla_encoder.h"

/* ... create the encoder and call SRLAEncoder_SetEncodeParameter() ... */

/* Candidate periods (8..262, at most 8 entries). Order does not matter and
 * duplicates are removed. Passing count = 0 disables the feature. */
uint32_t periods[] = { 40, 64, 128 };
if (SRLAEncoder_SetLTPCandidates(encoder, periods, 3) != SRLA_APIRESULT_OK) {
    /* invalid configuration: the previous setting is kept */
}

/* Encode as usual. ComputeBlockSize and EncodeBlock use the same cost model,
 * including the LR/MS/LS/SR channel-process decision, so their sizes match. */
uint32_t estimate, written;
SRLAEncoder_ComputeBlockSize(encoder, input, num_samples, &estimate);
SRLAEncoder_EncodeBlock(encoder, input, num_samples, data, data_size, &written);
/* estimate == written */
```

The candidate setting is independent of `SRLAEncoder_SetEncodeParameter()`: it
is not cleared when the encode parameters are re-applied, and new encoder
instances start with the feature disabled. Decoding needs no change — the
selected order, period and quantized coefficients are written into the normal
compressed block, and the existing decoder reconstructs the PCM sample-exactly
for 16/24-bit audio with 1 to 8 channels.

For fixed-period fitting at the LPC layer, `LPCCalculator_CalculateLTPCoefficientsAtPeriod()`
solves the same windowed, ridge-regularized normal equations as the automatic
pitch search, but at a caller-specified lag (which need not be a local peak).
It returns a failure code (without writing NaN coefficients) for insufficient
history, zero energy, singular systems or out-of-range arguments; the automatic
`LPCCalculator_CalculateLTPCoefficients()` interface keeps its previous
behaviour.

## Performance

We use [RWC music dataset](https://staff.aist.go.jp/m.goto/RWC-MDB/) for comparison.

### Decoding speed vs compression ratio

![Decoding speed vs compression rate](./evaluation/decodespeed_vs_compressionrate_4096_total.png)

### Encoding speed vs compression ratio

![Encoding speed vs compression rate](./evaluation/encodespeed_vs_compressionrate_4096_total.png)

## License

MIT

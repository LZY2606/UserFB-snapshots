FFTConvolver
============

FFTConvolver is a C++ library for highly efficient convolution of
audio data (e.g. for usage in real-time convolution reverbs etc.).

- Partitioned convolution algorithm (using uniform block sizes)
- Optional support for non-uniform block sizes (TwoStageFFTConvolver)
- Multi-channel convolution matrix with frequency-domain FFT sharing
  (MatrixConvolver)
- No external dependencies (FFT already included)
- Optional optimization for SSE (enabled by defining FFTCONVOLVER_USE_SSE)


MatrixConvolver
===============

`fftconvolver::MatrixConvolver` convolves a sparse matrix of connections
between several planar input channels and several planar output channels:
each connection is one causal convolution of an input by an impulse
response, and an output channel is the sum of all connections ending at it.
It uses the same short-head / long-tail two-stage partition strategy as
`TwoStageFFTConvolver`, but the FFT work is shared across the whole matrix.

- Each input channel performs only **one forward FFT per partition**, no
  matter how many connections depart from it.
- All connections ending at the same output channel are accumulated in the
  frequency domain and share **one inverse FFT per partition**.
- Like the single-channel convolvers, it has zero extra block latency and
  performs no heap allocation or deallocation while `process()` is running.

Channel layout
--------------

Audio data is passed in **planar** layout: `process()` receives one sample
array per channel, and every channel processes the same number of frames in
a call. The frame count of a call is arbitrary; splitting a stream into
chunks of different lengths does not change the sample positions of the
result.

An output array is allowed to alias the array of a different input channel
(cross-channel in-place operation, e.g. output 0 overwrites input 1 while
output 1 still needs the original input 1), because all inputs are
snapshotted internally before any output is written. Arrays of different
output channels must not overlap each other. A call with a frame count of 0
is allowed (the pointers may be null) and does not advance any history.

Initialization
--------------

`init()` receives the number of input and output channels, the head and tail
block sizes, and a table of `MatrixConnection` entries, each holding an
input index, an output index and an impulse response array:

- Channel indices outside the declared ranges, zero channel counts,
  non-power-of-2 block size relationships (the head block size must not
  exceed the tail block size; both are rounded up to the next power of 2),
  null response data for a non-empty response and non-finite coefficients
  make `init()` return `false` and leave the convolver reset.
- An impulse response of length 0 contributes nothing. Outputs without any
  connection are written with zeros.
- If the same input/output pair appears more than once, the convolutions
  are added together - later entries never replace earlier ones.
- All response data is copied and transformed during `init()`, so the
  connection table and the response arrays can be released or modified
  afterwards without affecting the sound.
- `reset()` discards the matrix and the complete processing history; a
  following `init()` starts with zero history, so no old tail survives.

Example
-------

```cpp
#include "MatrixConvolver.h"

// 4 capture inputs, 2 headphone outputs
const size_t numInputs = 4;
const size_t numOutputs = 2;

fftconvolver::MatrixConnection connections[3];
connections[0] = {0, 0, ir00.data(), ir00.size()}; // input 0 -> output 0
connections[1] = {2, 0, ir20.data(), ir20.size()}; // input 2 -> output 0
connections[2] = {3, 1, ir31.data(), ir31.size()}; // input 3 -> output 1

fftconvolver::MatrixConvolver convolver;
bool ok = convolver.init(numInputs, numOutputs,
                         /*headBlockSize*/ 64,
                         /*tailBlockSize*/ 512,
                         connections, 3);
assert(ok);

// Planar channel arrays; all channels use the same frame count
const fftconvolver::Sample* inputs[4]  = {in0, in1, in2, in3};
fftconvolver::Sample*       outputs[2] = {out0, out1};
convolver.process(inputs, outputs, frameCount);

convolver.reset(); // discards the matrix and all history
```

The FFT call counters returned by `fftStats()` are separated by partition
scale and direction (head/tail, forward/inverse); they scale with the
numbers of active input and output channels, never with the number of
connections.

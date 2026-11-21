FFTConvolver
============

FFTConvolver is a C++ library for highly efficient convolution of
audio data (e.g. for usage in real-time convolution reverbs etc.).

- Partitioned convolution algorithm (using uniform block sizes)
- Optional support for non-uniform block sizes (TwoStageFFTConvolver)
- AdaptiveConvolver: causal block frequency domain NLMS which learns the
  crosstalk from up to 6 reference channels (block size 8..1024, up to 65536
  taps per channel)
- No external dependencies (FFT already included)
- Optional optimization for SSE (enabled by defining FFTCONVOLVER_USE_SSE)

The fixed and the adaptive convolvers share the partitioned spectrum ring in
PartitionedFFTCore.h/.cpp.

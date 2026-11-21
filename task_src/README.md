# AdaptiveConvolver example

This directory is intentionally free of .cpp files for the default build. The
runnable demonstration of fftconvolver::AdaptiveConvolver lives in
test/adaptive.cpp and is executed by `bash task_support/run.sh target`:

- TestLearningConvergence() trains two uncorrelated references through a known
  sparse 129 tap response and asserts at least 18 dB of residual energy
  reduction over the last 256 blocks, then freezes the filter and confirms the
  following output with an independent time domain convolution of the exported
  taps.
- TestLearningExampleRendersVoice() renders a mix track which contains an
  independent voice on top of the reference crosstalk and prints the energy
  before and after the removal (EXAMPLE_RENDER ...).

To build a standalone example, add any .cpp file here and it is compiled into
the production library by task_support/build.sh.

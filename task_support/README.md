# Starting environment

Use `bash task_support/run.sh baseline` to build the unchanged production library, decode a tiny stereo IMA WAVE, compare its known samples and perform a real seek. It also reports what the current library does with LIST/wavl; that observation is not a permanent failure assertion.

`bash task_support/run.sh target` requires the future tests, src/wavl.h, src/wavl.c and examples/wavl_ima.c. It compiles the production library with the required header included, explicitly links the new production object, runs the required example, runs all registered test groups, and keeps the normal IMA path. Missing/empty files, no tests, skipped groups, an early exit or a failing example cannot count as success.

The seek/segment implementation is not supplied. Build tools are Bash, CMake, and C/C++ compilers. Optional codecs, MPEG, shared-library symbol generation and Python discovery are disabled. Existing upstream Python maintenance scripts are retained unchanged as provenance; this build never runs them. Fixtures, the baseline, and all requested new implementation/tests/examples use C. No device, external media, package cache or network is needed.

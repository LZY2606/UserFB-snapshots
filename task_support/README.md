The target tests/timed_events.c and all timed playback APIs are intentionally absent.
Register timeline, voices, stream via check.h; no main. The driver requires executed assertions and shell checks every group plus final completion outside the process.
checks/baseline.c and tone.mod are reused byte-for-byte from the earlier libxmp scaffold at this same upstream commit. Four original tests are linked unchanged through checks/old-tests.c.
Dependencies: C99 compiler, CMake, Bash, libm. No network, device or external package.

This directory supplies build/baseline scaffolding and the common assertion driver.
It does not implement tests/ms_timeline.c or the requested regression suite.

Requirements: C99 compiler, CMake, Bash, make/build tool, patch, tar, standard libm.
No online package or model downloads occur. DRED/OSCE/SIMD are disabled.

Run the starting production checks with:
    bash task_support/run.sh baseline
After adding the requested registered tests:
    bash task_support/run.sh target
    bash task_support/mutants.sh

TASK_CHECK records a content assertion and returns 1 from the current int case on
failure. Export a const struct task_case array and const size_t task_case_count.
The driver supplies main. The three groups are framing, mapping, recovery.

mutants.sh requires the unchanged suite to pass on normal source, then rebuilds
two independent temporary copies. A mutant is detected only by driver status 4
and a TASK_CHECK failure from tests/ms_timeline.c. Compile failures and signals
are rejected. Per-run logs are stored in task_support/results.

legacy_projection.c is the historical baseline scaffold, originally
derived from the same public Opus interfaces; it is recompiled at this seed's
fixed commit. baseline.c adds a direct 3-stream/1-coupled six-packet production
round trip. Neither contains the future target matrix or its expected PCM.
Upstream COPYING and source notices remain in the restored tree.

Run bash task_support/run.sh baseline for the original decoder observation.
Add tests/latm_decode.c, include task_support/check.h and export task_cases[] and task_case_count. Each case returns int (zero succeeds) and uses TASK_CHECK. Register nonempty transport, continuity and buffers groups. No main. Additional tests/latm_*.c are linked.
Run bash task_support/run.sh target to configure/build the current production faad library, link the new regression sources, execute every registered case and the baseline. libfaad/*.c is discovered by upstream CMake each configure.
No package installation, ffmpeg, Python or network is used by these commands.

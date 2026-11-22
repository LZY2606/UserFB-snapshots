This support directory adds build/replay material only. It contains no -F client,
no implementation of the requested shared runner, and no target regression cases.

Run `sh support/run.sh baseline` for existing minimization and byte analysis.
The input consumer is linked to the real llvm_mode/afl-llvm-rt.o.c, and records
simple deterministic coverage in that runtime's bitmap. No LLVM plugin, Python,
package download, or x86 assembler is required by this path.

file-shm.c supplies only the IPC_PRIVATE/shmat/IPC_RMID subset these programs use.
The host sandbox denies shmat. The shim names one private 0600 file per tool PID
in a fresh directory and maps it MAP_SHARED in the tool and target. It preserves
cross-process visibility, fork inheritance and unlink-after-mapping semantics;
it is not a general SysV IPC emulation. Other commands and normal Makefile builds
continue to use the native platform APIs. The production code is not edited.

After implementation, `sh support/run.sh target` explicitly compiles
`afl-tool-runner.c` into both tools, runs real -F smoke executions, then compiles
`tests/forkserver_target.c` with the current upstream runtime and invokes the
five functions in `tests/forkserver_contract.c` through contract-main.c.
Each function has signature `int fn(const char *tmin, const char *analyze,
const char *target)`; zero means the actual case passed, nonzero fails. There is
no skip convention. The third argument is the newly compiled test target path.
All subprocesses and temporary files created by tests must be cleaned up.
The run script rejects missing/empty source, propagates compile/test failures,
and requires five completed cases. It does not implement the new tests for you.

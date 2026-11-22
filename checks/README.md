# Local deterministic prelude checks

Run `bash task_support/run.sh baseline` for the unchanged six-stage production
mutation loops on a finite synthetic input. `AFL_TASK_PROBE` enables only two
observation seams: a candidate executor and the handoff after the bitflip stages.
No target process or vulnerability search is started. The normal afl-fuzz build
does not enable either seam. The feedback has one active eight-byte region.
The baseline checks all six stage counts, the actual automatic token, sparse
effector blocks, and queue flags. It stops before arithmetic and random havoc.

Add `checks/resume_contract.c` for the requested implementation and tests.
It may include afl-fuzz.c with main renamed, as the baseline does, or use a new
public session header. Adjust build.sh to link new implementation files as
needed; keep the old production baseline. The target runner compiles and runs
the added C file and requires both registered groups and real checks.

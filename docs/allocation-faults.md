# Allocation fault handling in theft

This document describes how theft behaves when `malloc`/`calloc`/`realloc`
fail at runtime, how the bounded fault-injection tests in
`test/alloc_failure_contract.c` exercise those paths, what the ownership
ledger can detect, and what it deliberately cannot prove.

## 1. Fault scope

The fault tests exercise **only the real, non-`fork` `theft_run` path**
inside this repository's library. They do not replace the process-wide
libc allocator and do not inject faults into the operating system or any
third-party program.

`task_support/fault-build.sh` compiles each `src/*.c` translation unit
with the preprocessor overrides

```
-Dmalloc=qa_malloc -Dcalloc=qa_calloc -Drealloc=qa_realloc -Dfree=qa_free
```

so every allocation *made by the library* is routed to the recorder in
`test/alloc_fault.c`. That translation unit, the test driver and the
property callbacks are compiled with the normal allocator. The ordinary
baseline library (`task_support/run.sh baseline`, the Makefile build) is
compiled **without** the overrides, so production behavior is unchanged.

The recorder keeps all of its metadata in fixed-capacity static storage
and never allocates memory for itself. An injected fault therefore cannot
recursively corrupt the bookkeeping.

Two independent ledgers are maintained:

* the **library ledger**, for blocks obtained through the redirected
  `qa_malloc/qa_calloc/qa_realloc/qa_free`;
* the **user ledger**, for property argument/candidate objects that the
  test allocates explicitly with `qa_user_*`. User-object leaks are thus
  never conflated with the recorder's own overhead.

For every real allocation attempt the recorder stores a monotonic
sequence number, the operation, the requested size, success or failure,
the block identity, and its live/retired state. Address reuse never
inherits an old identity: only a live record may own an address.

## 2. Which allocation failures may degrade

Most allocation failures are hard errors and must surface through the
public result codes. Exactly one subsystem is allowed to degrade: the
**Bloom filter** used for duplicate-call detection.

| Allocation | On failure |
| --- | --- |
| `struct theft`, PRNG state, print-trial-result env, autoshrink env, bit pool, request index | hard error |
| Top-level Bloom table (`theft_bloom_init`) | degrade: run without dedup (`t->bloom == NULL`) |
| Lazily grown Bloom filter block (`theft_bloom_mark`) | degrade: that mark is skipped, the run continues |
| Bit-pool / request-index `realloc` during autoshrink | hard error; the old block and its identity stay valid |

Rationale: losing duplicate detection only reduces an optimization,
whereas losing the runner, the PRNG, an argument or a shrinking pool
would leave theft without state it needs to continue correctly. A Bloom
failure is therefore *not* unconditionally turned into
`THEFT_RUN_ERROR_MEMORY`; every other shortage is.

Result-code separation, per the existing public API:

* initialization shortage -> `THEFT_RUN_ERROR_MEMORY`;
* argument generation / shrinking callback shortage ->
  `THEFT_RUN_ERROR` (the run aborts and cleans up);
* ordinary completion stays `THEFT_RUN_PASS` / `THEFT_RUN_FAIL` /
  `THEFT_RUN_SKIP`, and is only accepted for a genuinely degradable
  Bloom fault;
* a hook-requested stop, a hook `*_ERROR`, or a property `FAIL` are
  control-flow outcomes and are exercised separately, never confused
  with an allocation failure.

## 3. Ownership and cleanup rules enforced

After every run the tests reconcile both ledgers and require, apart from
results the test explicitly retains:

* no library block and no user argument object still live;
* no double free and no free/realloc of an unknown address — such a call
  is recorded as a violation and is never passed to the system allocator
  (which could abort);
* no lost block from a failed `realloc` — on failure the old bytes and
  identity remain valid, on success the old record is closed and a new
  identity is linked;
* `calloc` multiplication that wraps is rejected rather than handed on;
* `free(NULL)` is accepted;
* no half-built argument tuple is ever passed to the property: if
  multi-argument generation fails partway, every earlier object is
  released exactly once;
* during shrinking, after a candidate is rejected or a shrink callback
  fails, the current argument and the autoshrink bit pool still match,
  and the object finally freed is the current one — a handed-off old
  object is never freed a second time.

Test objects carry a serial number and a read-only FNV-1a checksum, so
the property and the free callback can confirm they received intact,
unmodified instances.

When a live block or violation is found, the harness first writes a
replayable plan and a violation report (under
`.task-build/alloc-fault-plans/`) and only then fails the check; it never
cleans up on the library's behalf first, which would mask the leak as
zero.

## 4. Bounded fuzzing methodology

For each fixed `(seed, config, property)`:

1. A **fault-free baseline** is run to capture the real allocation
   trajectory (attempt count, events, per-stage reachability, object
   alloc/free counts).
2. From a clean recorder, the run is replayed once per positive-length
   allocation attempt *k*, failing only attempt *k*. A replay must
   actually reach *k* and observe the fault; an early exit does not count
   as coverage.
3. For the shrinking configurations, limited **two-fault combinations**
   are run across the candidate-build, bit-pool-growth and
   argument-generation stages. The second position is selected from the
   trajectory observed *after the first fault fired*, not copied from the
   fault-free numbering.
4. After the faults are withdrawn, the same configuration is re-run in
   the same process; the result, property call count and object free
   count must match the clean baseline, proving no residual state
   pollutes later fuzzing.

Hard per-plan bounds: at most 4096 allocation attempts/events, 32 trials
and 5 seconds per run. If a bound is hit the recorder flags
`budget_exceeded` and the corresponding check fails (reported as
incomplete) rather than silently passing; configurations are sized so the
reachable later stages (pool/index growth, shrinking) are still covered
instead of being avoided by lowering the budget.

## 5. Reproducing a failure

Build and run the fault-injected target, then the unchanged baselines:

```sh
bash task_support/run.sh target     # fault library + three test groups
bash task_support/run.sh baseline   # original shrink and fork checks
```

The target registers three groups via `task_support/check.h`:

* `init` — the minimal progress-env `calloc` failure repro, with and
  without a hash callback, and a custom `trial_post` control;
* `trials` — partial multi-argument generation, normal `SKIP`, a user
  allocation error, and a stop-requesting callback;
* `shrinking` — hand-written shrinking and real autoshrinking over
  integer/byte-like objects with different seeds, exercising bit-pool
  and request-index growth.

Each run writes a text plan keyed by seed, config name and the sequence
of failed attempts, followed by the observed result, the hit operation,
any surviving identities, and the last stage reached. Plans contain data
(seed + attempt numbers), not pointers. A short standalone replayer is
in `test/alloc_fault_replay_example.c`:

```sh
cc -std=c99 -I. -Iinc -Isrc -I.task-build \
  test/alloc_fault_replay_example.c test/alloc_fault.c \
  .task-build/fault/libtheft.a -lm -o replay_example
./replay_example .task-build/alloc-fault-plans/init_default_post_hash.txt
```

## 6. What these tests do NOT prove

The ledger establishes **ownership and lifetime** correctness under
injected allocation failures. It does not provide general memory-safety
guarantees. In particular it does not, by itself, prove the absence of:

* out-of-bounds reads or writes that do not alter a tracked object's
  checksum;
* use-after-free reads of memory that has not yet been reused;
* uninitialized-memory use, data races, or undefined behavior;
* faults inside libc itself, the OS, or any third-party code.

For those classes, run the library under AddressSanitizer/UBSan or a
debug allocator in addition to these tests. The fault harness is a
complement to, not a replacement for, such tooling.

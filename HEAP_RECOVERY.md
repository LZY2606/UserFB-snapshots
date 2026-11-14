# Trusted-ledger heap metadata recovery

This note describes the metadata rebuild facility added on top of the
classic MEMWATCH allocator. It restores *diagnostic metadata only*:
management headers and chain links. User payloads, the under/overflow
guard templates and the 0xFC no-mans-land (NML) fill are never
repaired, overwritten or cleared by a rebuild, so real anomalies stay
visible.

## Components

- `memwatch.c` keeps a side **ledger** (`mwLedgerEntry`) allocated with
  the plain system allocator. The ledger and its index are never passed
  through `mwMalloc()`, so they cannot appear in the observed heap.
- `mw_recovery.{h,c}` is a generic, pointer-free planning engine. It
  speaks integer tokens (`mwToken`) and reaches every observed field
  through a `mwPlanAdapter`. The real-heap adapter and the plain-array
  mirror adapter run the *same* plan-build, second-stage verification
  and write-set application code; there is no mirror-only checker.
- Public entry points: `mwHeapPrepare()`, `mwHeapCommit()` and
  `mwHeapPlanFree()` (declared in `memwatch.h`). Mirror entry points:
  `mwMirrorValidate/Prepare/Commit/Touch` (declared in
  `mw_recovery.h`).

## Trust boundary

What is trusted:

- The ledger. Every delivered allocation records the raw system
  address/length, the exact user start and requested size, the
  allocation count, source file/line and the LIVE/NML state.
- Ownership is decided by an exact match on the user start address.
  For an unknown address, nothing in front of the pointer is read.
- Guard zones and NML areas during checks/teardown are located from the
  ledger's trusted request size, never from a header's `size` field.

What is untrusted:

- Every `mwData` header field (`prev`, `next`, `file`, `count`,
  `check`, `size`, `line`, `flag`) and the `mwHead`/`mwTail` ends.
  They are snapshotted as observations and diffed against the ledger;
  they never derive canonical facts. A file pointer is copied as a
  token and never dereferenced or probed.

The engine assumes the calling thread has quiesced the heap; recovery
under concurrent writes, or after the ledger memory itself has been
corrupted, is outside the threat model.

## Plan lifetime

1. `mwHeapPrepare()` validates the ledger facts (duplicate identities,
   overlapping extents, size wraparound), snapshots all observed
   headers and the chain ends, computes canonical headers and a chain
   ordered by descending allocation count, and records per-field
   discrepancy masks plus chain-level flags (bad head/tail, duplicate
   links, cycles, unreachable or foreign links). It never writes the
   heap and never runs the old heuristic relink.
2. The returned plan owns its snapshots, expected values, diffs, the
   ledger version and the initialization generation. Later changes to
   the heap, the log, or the caller's arrays cannot mutate a published
   plan.
3. `mwHeapCommit()` first re-checks the generation and ledger version,
   then re-reads every observed field and both chain ends, comparing
   them field by field with the snapshot. Only if *all* members still
   match does it hand the precomputed write set to the adapter once.
   Commit allocates no memory. On success every registered block is
   linked exactly once into the canonical chain; user and guard bytes
   are untouched.
4. Any successful alloc/free, NML conversion/eviction, realloc or other
   committed rebuild bumps the ledger version and generation, which
   makes older plans stale. Pure queries (`mwTest`, `mwTestBuffer`,
   previews) and requests that fail without changing the heap (for
   example a limit rejection) do not age a plan.
5. After `mwTerm()`/`mwAbort()` followed by re-initialization the epoch
   changes; an old plan can only be previewed (the `mwPlan*` accessors)
   or released with `mwHeapPlanFree()`. Committing it returns
   `MW_HEAP_STALE` and touches no old address.

## Failure behavior

- Status codes: `MW_HEAP_OK`, `MW_HEAP_STALE`, `MW_HEAP_INVALID`,
  `MW_HEAP_NOMEM`, `MW_HEAP_NOTREADY` (mirror equivalents `MW_RC_*`).
- If any ledger fact is illegal, or an observation cannot be made,
  prepare publishes nothing (`*out_plan == NULL`); no half plan
  escapes.
- If anything moved between prepare and commit, commit returns
  `MW_HEAP_STALE` and writes zero headers. The mirror asserts the whole
  target array is byte-identical to its pre-commit snapshot in that
  case.
- Allocation record/index preparation happens before the system block
  is published; if ledger storage fails, no half-registered object is
  delivered. Ledger storage failure is reported directly and never
  invokes NML pressure reclamation.
- Once header inconsistency is detected, `mwMalloc`, `mwRealloc`,
  `mwFree` and the check entry points refuse to operate (the degraded
  state) rather than copying or emitting damaged size/file/chain data.
  Checks stay read-only and never silently repair. Only an explicit
  successful `mwHeapCommit()` clears the degraded state.
- `mwAbort()` and the topmost `mwTerm()` release every LIVE and NML raw
  allocation and the ledger itself by walking the ledger, not the
  chain: no block can be lost, truncated or double-freed, even while
  degraded. Nested middle-layer `mwTerm()` calls only decrement the
  init depth and leave the ledger intact. NML entries stay in the
  ledger until the underlying block is really returned (normal free,
  pressure eviction or teardown); evictions that did occur are always
  reflected in the ledger.

## Plain-array mirror

`mwMirror` pairs trusted descriptors (`mwFact`) with a second array of
observed headers (`mwObsHead`). All addresses are integer tokens and
source identities are controlled integers; tokens are never converted
to pointers. Illegal descriptors (duplicate identities, overlapping
extents, wrapping sizes) are reported by `mwMirrorValidate()`. A
`blocked` token simulates an unreadable header, and `mwMirrorTouch()`
simulates a structural heap change. The mirror atomically applies the
same write set the real adapter would, and demonstrates that a failed
second-stage revalidation leaves the entire array unchanged.

## Test and build

- `bash task_support/run.sh baseline` builds and runs the unchanged
  healthy-allocation reference.
- `bash task_support/run.sh target` additionally builds and runs
  `test/heap_recovery_contract.c` (groups `ledger`, `repair`,
  `lifecycle`) alongside the baseline.
- Corrupted fields, broken links and stale observations exist only in
  mirror arrays. Real allocations in the tests use healthy production
  calls exclusively; no test writes a real management header, guard
  zone or freed region. Deterministic failure/reclaim seams
  (`mwTestSetPrepFailure`, `mwTestForceReclaim`, `mwTestMarkDegraded`)
  exist only under `MW_HEAP_TESTING` and do not replace production
  paths.

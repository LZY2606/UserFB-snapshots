# DUMA access diagnostics

`duma_access.c` adds offline, integer-addressable allocation identity and access
layout diagnostics.  The public types and signatures are declared in `duma.h`.

## Core ideas

A current address-to-block lookup cannot safely describe an access because an
old pointer may outlive its allocation and its address may be reused.  Each real
DUMA user allocation therefore receives a process-unique identity.  An acquired
`duma_access_ref` owns a reference to that identity and records:

- unique allocation identity;
- original request interval;
- allocation source (`malloc`, `calloc`, `memalign`, `valloc`, `strdup`, or
  `realloc`);
- observed lifecycle: `LIVE`, `FREED`, or `REALLOCATED`;
- monotonic transition sequence and realloc successor identity.

A `duma_access_event` is an immutable snapshot.  `duma_access_explain()` returns
its serializable POD report: access range/type, reference facts, independent
spatial out-of-bounds fragments, and physical segments at observation time.

The diagnostic never dereferences an analyzed address and never installs a
signal handler.

## Usage

```c
duma_access_ref *ref = NULL;
duma_access_event *event = NULL;

if (duma_access_acquire((uintptr_t)p, &ref) == DUMA_ACCESS_OK &&
    duma_access_capture(ref, (uintptr_t)p + delta, length,
                        DUMA_ACCESS_READ, &event) == DUMA_ACCESS_OK) {
    const duma_access_report *r = duma_access_explain(event);
    /* inspect r->flags, r->segments, r->oob, r->codes */
    duma_access_release_event(event);
}
duma_access_release_ref(ref);
```

`ref` may be `NULL` for `duma_access_capture()`; the report is then explicitly
`unassociated` and contains only physical facts.  It does not infer
use-after-free merely because a physical segment is a freed/retained region.

Call `duma_access_release()` for either object kind, or the typed release
functions.  Releasing `NULL` is a no-op.

## Interpretation rules

- `DUMA_ACCESS_F_VALID` means associated with a `LIVE` reference, non-zero
  length, and every byte lies in that reference's original request interval.
- Readable alignment slack is physically readable but still
  `out_of_bounds` relative to the requested size.
- Zero length produces `no_access`; reference lifecycle can still be shown, but
  no memory-access violation is reported.
- `stale_reference` is independent of `out_of_bounds`; both can occur together.
- An old reference whose address now contains another live request gets both
  `stale_reference` and `reused_by_other`.
- Unknown gaps are reported as `unknown`; the interpreter never invents an
  owner from proximity.
- Slot-table, free-reserve, allocator-internal and diagnostic-storage ranges
  have owner zero and are not user allocation objects.

## Integer-array interpreter

`duma_access_model_run()` validates caller-owned integer arrays of identities,
lifecycle facts, physical layout segments and the access record, then runs the
same segmentation and interpretation code as a real capture.  It rejects:

- duplicate/conflicting identities;
- invalid lifecycle or segment kinds;
- overlapping physical segments;
- missing segment/reference owners;
- address-plus-length wrap;
- impossible realloc successor state.

This entry point makes address reuse and the two-phase protected-reserve
reclamation sequence deterministic without touching guard or freed pages.

## Real allocator integration

`duma.c` collects trusted Slot facts while holding DUMA's existing allocator
lock and calls the shared interpreter.  All supported C allocation entry points
funnel through the same allocation path.  Successful realloc ends the old
identity as `REALLOCATED` and creates a new identity, even when the returned
address is unchanged.  A failed realloc leaves the old identity `LIVE`.  The
actual zero-size path is accounted from DUMA's real return/slot behavior rather
than assuming every `NULL` is an allocation failure.

Free protection, partial reserve reclamation, complete mapping deletion, Slot
expansion and address reuse change only future captures.  Already created
events are copied snapshots and do not reference the current `allocList`.

Diagnostic metadata uses direct OS anonymous mappings (`mmap`/`VirtualAlloc`),
not `malloc()`/`free()`, so it cannot recursively enter the observed allocator.
Identity and event storage is reclaimed when the last reference/event holder is
released; DUMA's Slot holds its own identity reference only for the current
generation.  If raw diagnostic storage fails, diagnostics latch unavailable:
new acquire/capture/model calls fail, existing events remain explainable, and
ordinary DUMA allocations continue.

## Limits

- Physical segment kinds are derived from DUMA Slot state and the known
  protection/reclamation operations, not from a separate page-table probe.
- The diagnostic does not prevent an invalid access; DUMA guard pages still
  provide the immediate fault mechanism when the program actually performs the
  access.
- Sequence numbers are monotonic within one process and are not reset by
  `duma_init()`; there is no new allocator reset/reinitialization protocol.
- The raw storage implementation targets pthreads plus `mmap` on POSIX and
  `VirtualAlloc` on Windows; the supplied task build is exercised on macOS.
- Reports contain pointers to event-owned arrays; serialize those arrays before
  releasing the event.

# JitAllocator Span Identity and Raw-Pointer Compatibility

This note describes the allocation-identity rules implemented by `JitAllocator` and how they affect
`JitAllocator::Span` and the raw-pointer based APIs.

## Allocation identity

Every successful `JitAllocator::alloc()` creates exactly one allocation identity. Every `Span` that refers to that
allocation — the one returned by `alloc()` and any later populated by `query()` — carries the same private identity
(owning allocator, a monotonic identity number, and a generation). A `Span` is a lightweight, copyable handle:

- Copying a `Span`, or writing through one copy, never invalidates the other copies.
- `query()` never returns a suffix segment. Any byte address of a live allocation (including interior addresses) is
  normalized to the whole allocation: the returned `Span` starts at the allocation's start address, carries the
  matching read-write address (different from read-execute only with dual mapping), and reports the current
  granularity-rounded capacity.
- Holes, released areas, block initial padding, and addresses past the end of an allocation fail `query()` and leave
  the output as a default-constructed empty `Span`. For two adjacent allocations the boundary address belongs to the
  second allocation. The lookup is logarithmic in the number of blocks and never scans all live allocations.

A `Span` becomes stale, and is then rejected by every Span-based API, when its allocation has been:

- released (through either release overload),
- invalidated by `reset()` — both soft and hard reset,
- shrunk to a different granularity-rounded capacity.

A shrink whose rounded capacity does not change keeps the identity, so all existing copies stay valid. After a
capacity-changing shrink, use the returned `Span` or call `query()` again to obtain valid copies.

If a freed address is later handed out by a new allocation, stale `Span` copies keep failing. They can never be
reinterpreted as handles of the new allocation, which would otherwise be a use-after-free write into executable
memory that now belongs to someone else. A `Span` created by one `JitAllocator` is likewise rejected by any other
allocator. `Span::is_valid()` reports whether a span still references a live allocation of its owning allocator.

## Where the checks happen

Validation runs before any byte is copied, before any `WriteFunc` is called, before page permissions are changed,
and before bit-vectors or statistics are touched. On failure nothing is copied and no state changes. This applies
to both `write()` overloads, `shrink()`, `release(Span&)`, and the `WriteScope` entry points; even a zero-length
write requires a valid span.

## Write callbacks

A write callback may reenter the same allocator to `alloc()`, `query()`, `write()`, or `shrink()` other
allocations; the allocator lock is reentrant for the calling thread, so this cannot self-deadlock. A callback must
not release or reset the allocation it is currently writing (including a zero-sized `shrink()`); doing so is
unsupported. Concurrent release of an allocation while another thread is inside its write callback is not
supported either — synchronization between executing/updating code and freeing it remains the caller's
responsibility.

## Raw-pointer interface compatibility

- `release(void*)` accepts only the exact start address of a currently live allocation. An interior address, even
  when it is granularity aligned, is rejected and changes no counters or bit-vectors.
- A raw pointer carries no identity. After an address is freed and reallocated, a stale raw pointer and a fresh one
  are indistinguishable, so `release(void*)` still relies on the caller to pass a pointer that belongs to a current
  allocation. The staleness guarantees above are provided only through the Span-based APIs, including
  `release(Span&)`.
- The private metadata changes the in-memory layout of `JitAllocator::Span`; there is no binary-ABI compatibility
  requirement for this change, and the public function signatures are otherwise unchanged. Span remains copyable
  and its public accessors (`rx()`, `rw()`, `size()`, `flags()`, `shrink()`) keep their signatures.

// Target test for JitAllocator allocation identity, Span staleness, and interior query normalization.
//
// This file is compiled with the project's default (single/MAP_JIT) mapping. The dual-mapping address
// correspondence is still exercised when a Span happens to have distinct rx/rw pointers, but that branch is not
// required to run on this host.
#include <asmjit/core.h>
#include "harness.h"

#include <cstdint>
#include <cstring>
#include <cstdio>

using namespace asmjit;

namespace {

using Byte = unsigned char;

// Observable callback behavior. The harness' check_assert() aborts on the first failed check, so these counters
// only describe successful runs.
unsigned g_callback_invocations;
unsigned g_reentrant_allocs;
unsigned g_rejected_writes;

constexpr Byte kPatternA[8] = {11, 22, 33, 44, 55, 66, 77, 88};
constexpr Byte kPatternB[8] = {99, 88, 77, 66, 55, 44, 33, 22};
constexpr Byte kPoison[4]   = {0xDE, 0xAD, 0xBE, 0xEF};

// Normal write callback: writes its own span, reenters the allocator to manage another allocation, and shrinks the
// span it is writing (the JitRuntime pattern).
Error reentrant_callback(JitAllocator::Span& span, void* user_data) noexcept {
  JitAllocator* allocator = static_cast<JitAllocator*>(user_data);
  g_callback_invocations++;

  std::memset(span.rw(), 0x5A, 8);

  JitAllocator::Span other;
  if (allocator->alloc(Out(other), span.size() / 2u) == Error::kOk) {
    g_reentrant_allocs++;

    Byte bytes[2] = {3, 4};
    if (allocator->write(other, 0, bytes, sizeof(bytes)) != Error::kOk) {
      return Error::kInvalidState;
    }

    JitAllocator::Span queried;
    if (allocator->query(Out(queried), other.rx()) != Error::kOk || queried.rx() != other.rx()) {
      return Error::kInvalidState;
    }
  }

  // Shrink the allocation being written to one granularity.
  span.shrink(allocator->granularity());
  return Error::kOk;
}

Error counting_callback(JitAllocator::Span&, void*) noexcept {
  g_callback_invocations++;
  return Error::kOk;
}

Error failing_callback(JitAllocator::Span&, void*) noexcept {
  g_callback_invocations++;
  return Error::kInvalidState;
}

// Truncates inside a callback without crossing a granularity boundary, so the allocation area and identity stay.
Error in_place_shrink_callback(JitAllocator::Span& span, void*) noexcept {
  g_callback_invocations++;
  std::memset(span.rw(), 0x33, 4);
  span.shrink(span.size() - 1u); // Still rounds up to the same granularity-rounded capacity.
  return Error::kOk;
}

// Interior addresses normalize to the whole allocation; holes, padding, and out-of-range addresses fail.
void group_interior() {
  JitAllocator allocator;
  const size_t granularity = allocator.granularity();

  JitAllocator::Span first;
  JitAllocator::Span second;
  CHECK_CHECK(allocator.alloc(Out(first), granularity * 3u) == Error::kOk);
  CHECK_CHECK(allocator.alloc(Out(second), granularity * 2u) == Error::kOk);
  CHECK_CHECK(allocator.statistics().allocation_count() == 2u);

  // An interior byte address resolves to the start of the whole allocation, with its full capacity.
  JitAllocator::Span queried;
  void* interior = static_cast<Byte*>(first.rx()) + granularity;
  CHECK_CHECK(allocator.query(Out(queried), interior) == Error::kOk);
  CHECK_CHECK(queried.rx() == first.rx());
  CHECK_CHECK(queried.rw() == first.rw());
  CHECK_CHECK(queried.size() == first.size());

  // The last byte of the allocation still resolves to that same allocation.
  void* last_byte = static_cast<Byte*>(first.rx()) + first.size() - 1u;
  CHECK_CHECK(allocator.query(Out(queried), last_byte) == Error::kOk);
  CHECK_CHECK(queried.rx() == first.rx());

  // Two adjacent allocations: the boundary address is the start of the second allocation.
  void* boundary = static_cast<Byte*>(first.rx()) + first.size();
  if (boundary == second.rx()) {
    CHECK_CHECK(allocator.query(Out(queried), boundary) == Error::kOk);
    CHECK_CHECK(queried.rx() == second.rx());
    CHECK_CHECK(queried.size() == second.size());
  }

  // Failures leave an empty Span and touch nothing.
  CHECK_CHECK(allocator.query(Out(queried), static_cast<Byte*>(first.rx()) - granularity) != Error::kOk);
  CHECK_CHECK(queried.rx() == nullptr);
  CHECK_CHECK(!queried.is_valid());

  CHECK_CHECK(allocator.query(Out(queried), static_cast<Byte*>(second.rx()) + second.size()) != Error::kOk);
  CHECK_CHECK(queried.rx() == nullptr);

  // Copies share one identity; writing through a copy must not invalidate the other copies.
  JitAllocator::Span copy = first;
  CHECK_CHECK(allocator.write(copy, 0, kPatternA, sizeof(kPatternA)) == Error::kOk);
  CHECK_CHECK(std::memcmp(first.rx(), kPatternA, sizeof(kPatternA)) == 0);
  CHECK_CHECK(allocator.write(first, 0, kPatternB, sizeof(kPatternB)) == Error::kOk);
  CHECK_CHECK(std::memcmp(copy.rx(), kPatternB, sizeof(kPatternB)) == 0);

  // A raw-pointer release only accepts the exact allocation start; an aligned interior address cannot release a
  // tail segment or alter the counters.
  CHECK_CHECK(allocator.release(interior) != Error::kOk);
  CHECK_CHECK(allocator.statistics().allocation_count() == 2u);
  CHECK_CHECK(copy.is_valid());

  // WriteScope goes through the same identity checks, and works for a valid span.
  JitAllocator::WriteScope scope(allocator);
  CHECK_CHECK(scope.write(first, 0, kPatternA, 4u) == Error::kOk);
  CHECK_CHECK(std::memcmp(first.rx(), kPatternA, 4u) == 0);

  // The exact start address is accepted by the raw-pointer interface.
  CHECK_CHECK(allocator.release(second.rx()) == Error::kOk);
  CHECK_CHECK(allocator.statistics().allocation_count() == 1u);
}

// Releasing and resetting invalidate copies; address reuse must not resurrect a stale Span.
void group_stale() {
  JitAllocator allocator;
  const size_t granularity = allocator.granularity();
  const size_t capacity = granularity * 3u;

  JitAllocator::Span span;
  CHECK_CHECK(allocator.alloc(Out(span), capacity) == Error::kOk);
  CHECK_CHECK(allocator.write(span, 0, kPatternA, sizeof(kPatternA)) == Error::kOk);
  const size_t used_when_live = allocator.statistics().used_size();

  JitAllocator::Span stale = span;
  CHECK_CHECK(stale.is_valid());

  // Span-based release invalidates every copy and clears the released handle.
  CHECK_CHECK(allocator.release(span) == Error::kOk);
  CHECK_CHECK(span.rx() == nullptr);
  CHECK_CHECK(!stale.is_valid());
  CHECK_CHECK(allocator.statistics().allocation_count() == 0u);
  // Only block padding (at most one granularity) can remain in the used counters; the whole capacity is freed.
  const size_t used_after_release = allocator.statistics().used_size();
  CHECK_CHECK(used_after_release <= granularity);
  CHECK_CHECK(used_when_live - used_after_release == capacity);

  // Every Span-based entry point rejects the stale copy before touching memory or statistics - including a
  // zero-length write and the WriteScope entry.
  g_rejected_writes = 0;
  CHECK_CHECK(allocator.write(stale, 0, kPoison, sizeof(kPoison)) != Error::kOk);
  CHECK_CHECK(allocator.write(stale, 0, kPoison, 0u) != Error::kOk);
  CHECK_CHECK(allocator.shrink(stale, granularity) != Error::kOk);
  CHECK_CHECK(allocator.release(stale) != Error::kOk);
  {
    JitAllocator::WriteScope scope(allocator);
    CHECK_CHECK(scope.write(stale, 0, kPoison, sizeof(kPoison)) != Error::kOk);
  }
  g_rejected_writes += 5u;

  JitAllocator::Span queried;
  CHECK_CHECK(allocator.query(Out(queried), stale.rx()) != Error::kOk);
  CHECK_CHECK(queried.rx() == nullptr);

  // The same address is handed out again, but as a brand new allocation identity.
  JitAllocator::Span reused;
  CHECK_CHECK(allocator.alloc(Out(reused), capacity) == Error::kOk);
  CHECK_CHECK(reused.rx() == stale.rx());
  CHECK_CHECK(allocator.write(reused, 0, kPatternB, sizeof(kPatternB)) == Error::kOk);

  // The stale copy still cannot write; the new allocation's bytes are untouched by the rejected attempts.
  CHECK_CHECK(allocator.write(stale, 0, kPoison, sizeof(kPoison)) != Error::kOk);
  CHECK_CHECK(std::memcmp(reused.rx(), kPatternB, sizeof(kPatternB)) == 0);
  CHECK_CHECK(allocator.statistics().allocation_count() == 1u);
  CHECK_CHECK(allocator.statistics().used_size() - used_after_release == capacity);
  g_rejected_writes++;

  // A fresh query yields a working handle for the new identity.
  CHECK_CHECK(allocator.query(Out(queried), reused.rx()) == Error::kOk);
  CHECK_CHECK(queried.rx() == reused.rx());
  CHECK_CHECK(queried.size() == capacity);
  CHECK_CHECK(allocator.write(queried, 4u, kPatternB + 4u, 4u) == Error::kOk);
  CHECK_CHECK(std::memcmp(reused.rx(), kPatternB, sizeof(kPatternB)) == 0);

  // Soft reset invalidates outstanding Spans but keeps the allocator usable.
  JitAllocator::Span soft;
  CHECK_CHECK(allocator.alloc(Out(soft), granularity) == Error::kOk);
  JitAllocator::Span soft_stale = soft;
  allocator.reset(ResetPolicy::kSoft);
  CHECK_CHECK(!soft_stale.is_valid());
  CHECK_CHECK(allocator.write(soft_stale, 0, kPoison, 1u) != Error::kOk);
  CHECK_CHECK(allocator.query(Out(queried), soft_stale.rx()) != Error::kOk);

  JitAllocator::Span after_soft;
  CHECK_CHECK(allocator.alloc(Out(after_soft), granularity) == Error::kOk);
  CHECK_CHECK(after_soft.is_valid());

  // Hard reset invalidates outstanding Spans as well.
  JitAllocator::Span hard_stale = after_soft;
  allocator.reset(ResetPolicy::kHard);
  CHECK_CHECK(!hard_stale.is_valid());
  CHECK_CHECK(allocator.write(hard_stale, 0, kPoison, 1u) != Error::kOk);

  std::printf("  stale: rejected_write_attempts=%u\n", g_rejected_writes);
}

// Shrinking rotates the identity when the rounded capacity actually changes; otherwise copies stay valid.
void group_shrink() {
  JitAllocator allocator;
  const size_t granularity = allocator.granularity();

  JitAllocator::Span span;
  CHECK_CHECK(allocator.alloc(Out(span), granularity * 6u) == Error::kOk);
  CHECK_CHECK(allocator.write(span, 0, kPatternA, 4u) == Error::kOk);
  const size_t used_before_shrink = allocator.statistics().used_size();

  JitAllocator::Span before = span;

  // Real capacity change: the passed Span gets the new identity, all older copies become stale.
  CHECK_CHECK(allocator.shrink(span, granularity) == Error::kOk);
  CHECK_CHECK(span.size() == granularity);
  CHECK_CHECK(span.is_valid());
  CHECK_CHECK(!before.is_valid());

  // The old copy cannot write the reclaimed tail, nor the still-live head.
  CHECK_CHECK(allocator.write(before, granularity * 2u, kPoison, sizeof(kPoison)) != Error::kOk);
  CHECK_CHECK(allocator.write(before, 0u, kPoison, sizeof(kPoison)) != Error::kOk);
  CHECK_CHECK(allocator.release(before) != Error::kOk);

  // A new query describes the shrunken capacity and produces a working copy.
  JitAllocator::Span queried;
  CHECK_CHECK(allocator.query(Out(queried), static_cast<Byte*>(span.rx()) + 8u) == Error::kOk);
  CHECK_CHECK(queried.rx() == span.rx());
  CHECK_CHECK(queried.size() == granularity);
  CHECK_CHECK(allocator.write(queried, 0u, kPatternB, 4u) == Error::kOk);
  CHECK_CHECK(std::memcmp(span.rx(), kPatternB, 4u) == 0);

  // A shrink that does not change the granularity-rounded size keeps the identity and all copies.
  JitAllocator::Span noop_copy = queried;
  CHECK_CHECK(allocator.shrink(queried, 1u) == Error::kOk);
  CHECK_CHECK(queried.size() == granularity);
  CHECK_CHECK(noop_copy.is_valid());
  CHECK_CHECK(allocator.write(noop_copy, 0u, kPatternA, 4u) == Error::kOk);

  CHECK_CHECK(allocator.statistics().allocation_count() == 1u);
  CHECK_CHECK(used_before_shrink - allocator.statistics().used_size() == granularity * 5u);

  // Shrinking to zero releases through the Span identity; every copy goes stale.
  JitAllocator::Span zeroable;
  JitAllocator::Span zero_stale;
  CHECK_CHECK(allocator.alloc(Out(zeroable), granularity * 2u) == Error::kOk);
  zero_stale = zeroable;
  CHECK_CHECK(allocator.shrink(zeroable, 0u) == Error::kOk);
  CHECK_CHECK(zeroable.rx() == nullptr);
  CHECK_CHECK(!zero_stale.is_valid());
  CHECK_CHECK(allocator.write(zero_stale, 0u, kPoison, 1u) != Error::kOk);
  CHECK_CHECK(allocator.statistics().allocation_count() == 1u);
}

// Spans are bound to the allocator that created them.
void group_ownership() {
  JitAllocator first;
  JitAllocator second;
  const size_t granularity = first.granularity();

  JitAllocator::Span left;
  JitAllocator::Span right;
  CHECK_CHECK(first.alloc(Out(left), granularity) == Error::kOk);
  CHECK_CHECK(second.alloc(Out(right), granularity) == Error::kOk);

  // A foreign allocator rejects every Span-based operation without changing its own state.
  CHECK_CHECK(second.write(left, 0u, kPoison, sizeof(kPoison)) != Error::kOk);
  CHECK_CHECK(first.write(right, 0u, kPoison, sizeof(kPoison)) != Error::kOk);
  CHECK_CHECK(second.release(left) != Error::kOk);
  CHECK_CHECK(first.release(right) != Error::kOk);
  CHECK_CHECK(second.shrink(left, 1u) != Error::kOk);

  JitAllocator::Span queried;
  CHECK_CHECK(second.query(Out(queried), left.rx()) != Error::kOk);
  CHECK_CHECK(queried.rx() == nullptr);

  // Default-constructed Spans have no identity at all.
  JitAllocator::Span empty;
  CHECK_CHECK(!empty.is_valid());
  CHECK_CHECK(first.write(empty, 0u, kPoison, 1u) != Error::kOk);

  // Both allocations remain alive and usable through their own allocators.
  CHECK_CHECK(left.is_valid());
  CHECK_CHECK(right.is_valid());
  CHECK_CHECK(first.write(left, 0u, kPatternA, 4u) == Error::kOk);
  CHECK_CHECK(second.write(right, 0u, kPatternB, 4u) == Error::kOk);
  CHECK_CHECK(std::memcmp(left.rx(), kPatternA, 4u) == 0);
  CHECK_CHECK(std::memcmp(right.rx(), kPatternB, 4u) == 0);
  CHECK_CHECK(first.statistics().allocation_count() == 1u);
  CHECK_CHECK(second.statistics().allocation_count() == 1u);

  // Dual-mapping correspondence, when present, is preserved through query and shrink.
  if (left.rw() != left.rx()) {
    CHECK_CHECK(first.query(Out(queried), left.rx()) == Error::kOk);
    CHECK_CHECK(queried.rw() == left.rw());
    CHECK_CHECK(first.shrink(queried, 1u) == Error::kOk);
    CHECK_CHECK(queried.rw() == left.rw());
  }
}

// Write callbacks can reenter the allocator; rejected or failing callbacks never touch the target allocation.
void group_callbacks() {
  g_callback_invocations = 0;
  g_reentrant_allocs = 0;
  g_rejected_writes = 0;

  JitAllocator allocator;
  const size_t granularity = allocator.granularity();

  JitAllocator::Span main_span;
  CHECK_CHECK(allocator.alloc(Out(main_span), granularity * 2u) == Error::kOk);

  // A normal callback writes its own bytes, allocates/writes/queries another object reentrantly, and shrinks the
  // allocation being written. The reentrant allocator calls must not deadlock on the allocator lock.
  CHECK_CHECK(allocator.write(main_span, &reentrant_callback, &allocator) == Error::kOk);
  CHECK_CHECK(g_callback_invocations == 1u);
  CHECK_CHECK(g_reentrant_allocs == 1u);
  CHECK_CHECK(main_span.size() == granularity);
  CHECK_CHECK(main_span.is_valid());

  Byte expected[8];
  std::memset(expected, 0x5A, sizeof(expected));
  CHECK_CHECK(std::memcmp(main_span.rx(), expected, sizeof(expected)) == 0);
  CHECK_CHECK(allocator.statistics().allocation_count() == 2u);

  // A stale Span must never reach the callback.
  JitAllocator::Span temporary;
  CHECK_CHECK(allocator.alloc(Out(temporary), granularity) == Error::kOk);
  JitAllocator::Span stale = temporary;
  CHECK_CHECK(allocator.release(temporary) == Error::kOk);

  unsigned invocations_before = g_callback_invocations;
  CHECK_CHECK(allocator.write(stale, &counting_callback, nullptr) != Error::kOk);
  {
    JitAllocator::WriteScope scope(allocator);
    CHECK_CHECK(scope.write(stale, &counting_callback, nullptr) != Error::kOk);
  }
  CHECK_CHECK(g_callback_invocations == invocations_before);
  g_rejected_writes += 2u;

  // A callback that returns an error propagates it and leaves the Span valid and usable.
  JitAllocator::Span error_target;
  CHECK_CHECK(allocator.alloc(Out(error_target), granularity) == Error::kOk);
  CHECK_CHECK(allocator.write(error_target, &failing_callback, nullptr) == Error::kInvalidState);
  CHECK_CHECK(g_callback_invocations == invocations_before + 1u);
  CHECK_CHECK(error_target.is_valid());
  CHECK_CHECK(allocator.write(error_target, 0u, kPatternA, 4u) == Error::kOk);
  CHECK_CHECK(std::memcmp(error_target.rx(), kPatternA, 4u) == 0);

  // A callback that truncates within the same granularity-rounded area keeps the identity: the span is re-published
  // with its rounded capacity and previously made copies remain usable.
  JitAllocator::Span stable_span;
  CHECK_CHECK(allocator.alloc(Out(stable_span), granularity * 2u) == Error::kOk);
  JitAllocator::Span stable_copy = stable_span;
  CHECK_CHECK(allocator.write(stable_span, &in_place_shrink_callback, nullptr) == Error::kOk);
  CHECK_CHECK(stable_span.size() == granularity * 2u);
  CHECK_CHECK(stable_span.is_valid());
  CHECK_CHECK(stable_copy.is_valid());
  Byte in_place_expected[4];
  std::memset(in_place_expected, 0x33, sizeof(in_place_expected));
  CHECK_CHECK(std::memcmp(stable_copy.rx(), in_place_expected, 4u) == 0);
  CHECK_CHECK(allocator.write(stable_copy, 0u, kPatternB, 4u) == Error::kOk);

  std::printf("  callbacks: invocations=%u reentrant_allocs=%u rejected=%u\n",
              g_callback_invocations, g_reentrant_allocs, g_rejected_writes);
}

} // namespace

extern "C" void check_case(const char* group) {
  if (std::strcmp(group, "interior") == 0) {
    group_interior();
  }
  else if (std::strcmp(group, "stale") == 0) {
    group_stale();
  }
  else if (std::strcmp(group, "shrink") == 0) {
    group_shrink();
  }
  else if (std::strcmp(group, "ownership") == 0) {
    group_ownership();
  }
  else if (std::strcmp(group, "callbacks") == 0) {
    group_callbacks();
  }
}

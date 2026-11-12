// This file is part of AsmJit project <https://asmjit.com>
//
// See <asmjit/core.h> or LICENSE.md for license and copyright information
// SPDX-License-Identifier: Zlib

#include <asmjit/core/api_build_p.h>
#ifndef ASMJIT_NO_JIT

#include <asmjit/axl/arena.h>
#include <asmjit/axl/arena_list.h>
#include <asmjit/axl/arena_pool.h>
#include <asmjit/axl/arena_tree.h>
#include <asmjit/axl/bit_set_ops.h>
#include <asmjit/axl/commons.h>
#include <asmjit/axl/mem_alloc.h>
#include <asmjit/axl/support_p.h>
#include <asmjit/core/arch_traits.h>
#include <asmjit/core/globals.h>
#include <asmjit/core/jit_allocator.h>
#include <asmjit/core/os_utils_p.h>
#include <asmjit/core/pauth_utils_p.h>
#include <asmjit/core/virt_mem.h>
#include <asmjit/core/virt_mem_p.h>

#if defined(ASMJIT_TEST)
#include <asmjit-testing/commons/random.h>
#endif // ASMJIT_TEST

ASMJIT_BEGIN_NAMESPACE

// JitAllocator - Constants
// ========================

//! Number of pools to use when `JitAllocatorOptions::kUseMultiplePools` is set.
//!
//! Each pool increases granularity twice to make memory management more
//! efficient. Ideal number of pools appears to be 3 to 4 as it distributes
//! small and large functions properly.
static constexpr uint32_t kJitAllocatorMultiPoolCount = 3;

//! Minimum granularity (and the default granularity for pool #0).
static constexpr uint32_t kJitAllocatorBaseGranularity = 64;

//! Maximum block size (32MB).
static constexpr uint32_t kJitAllocatorMaxBlockSize = 1024 * 1024 * 64;

// JitAllocator - Fill Pattern
// ===========================

static inline uint32_t JitAllocator_default_fill_pattern() noexcept {
#if ASMJIT_TARGET_ARCH_X86
  // X86 and X86_64 - 4x 'int3' instruction.
  return 0xCCCCCCCCu;
#else
  // Unknown...
  return 0u;
#endif
}

// JitAllocator - Pool
// ===================

class JitAllocatorBlock;

class JitAllocatorPool {
public:
  ASMJIT_NONCOPYABLE(JitAllocatorPool)

  //! \name Members
  //! \{

  //! Double linked list of blocks.
  axl::ArenaList<JitAllocatorBlock> blocks;
  //! Where to start looking first.
  JitAllocatorBlock* cursor = nullptr;

  //! Count of blocks.
  uint32_t block_count = 0;
  //! Allocation granularity.
  uint16_t granularity = 0;
  //! Log2(granularity).
  uint8_t granularity_log2 = 0;
  //! Count of empty blocks (either 0 or 1 as we won't keep more blocks empty).
  uint8_t empty_block_count = 0;

  //! Number of bits reserved across all blocks.
  size_t total_area_size[2] {};
  //! Number of bits used across all blocks.
  size_t total_area_used[2] {};
  //! Overhead of all blocks (in bytes).
  size_t total_overhead_bytes = 0;

  //! \}

  ASMJIT_INLINE JitAllocatorPool(uint32_t granularity) noexcept
    : blocks(),
      granularity(uint16_t(granularity)),
      granularity_log2(uint8_t(axl::ctz(granularity))) {}

  ASMJIT_INLINE void reset() noexcept {
    blocks.reset();
    cursor = nullptr;
    block_count = 0u;
    total_area_size[0] = 0u;
    total_area_size[1] = 0u;
    total_area_used[0] = 0u;
    total_area_used[1] = 0u;
    total_overhead_bytes = 0u;
  }

  ASMJIT_INLINE_NODEBUG size_t byte_size_from_area_size(uint32_t area_size) const noexcept { return size_t(area_size) * granularity; }
  ASMJIT_INLINE_NODEBUG uint32_t area_size_from_byte_size(size_t size) const noexcept { return uint32_t((size + granularity - 1) >> granularity_log2); }

  ASMJIT_INLINE_NODEBUG size_t bit_word_count_from_area_size(uint32_t area_size) const noexcept {
    static constexpr uint32_t kBitWordSizeInBits = axl::bit_size_of<axl::BitWord>;
    return axl::align_up<size_t>(area_size, kBitWordSizeInBits) / kBitWordSizeInBits;
  }
};

// JitAllocator - Block
// ====================

class JitAllocatorBlock : public axl::ArenaTreeNodeT<JitAllocatorBlock>,
                          public axl::ArenaListNode<JitAllocatorBlock> {
public:
  ASMJIT_NONCOPYABLE(JitAllocatorBlock)

  enum Flags : uint32_t {
    //! Block has initial padding, see \ref JitAllocatorOptions::kDisableInitialPadding.
    kFlagInitialPadding = 0x00000001u,
    //! Block is empty.
    kFlagEmpty = 0x00000002u,
    //! Block is dirty (dirty largest_unused_area, search_start, search_end, ...).
    kFlagDirty = 0x00000004u,
    //! Block is in incremental mode, which means that there is no memory used after search_start.
    kFlagIncremental = 0x00000008u,
    //! Block represents memory that is using large pages.
    kFlagLargePages = 0x00000010u,
    //! Block represents memory that is dual-mapped.
    kFlagDualMapped = 0x00000020u
  };

  static_assert(kFlagInitialPadding == 1, "JitAllocatorBlock::kFlagInitialPadding must be equal to 1");

  static inline uint32_t initial_area_start_by_flags(uint32_t flags) noexcept {
    return flags & kFlagInitialPadding;
  }

  //! Link to the pool that owns this block.
  JitAllocatorPool* _pool {};
  //! Virtual memory mapping - either single mapping (both pointers equal) or
  //! dual mapping, where one pointer is Read+Execute and the second Read+Write.
  VirtMem::DualMapping _mapping {};
  //! Virtual memory size (block size) [bytes].
  size_t _block_size = 0;

  //! Block flags.
  uint32_t _flags = 0;
  //! Size of the whole block area (bit-vector size).
  uint32_t _area_size = 0;
  //! Used area (number of bits in bit-vector used).
  uint32_t _area_used = 0;
  //! The largest unused continuous area in the bit-vector (or `area_size` to initiate rescan).
  uint32_t _largest_unused_area = 0;
  //! Start of a search range (for unused bits).
  uint32_t _search_start = 0;
  //! End of a search range (for unused bits).
  uint32_t _search_end = 0;

  //! Used bit-vector (0 = unused, 1 = used).
  axl::BitWord* _used_bit_vector {};
  //! Stop bit-vector (0 = don't care, 1 = stop).
  axl::BitWord* _stop_bit_vector {};

  ASMJIT_INLINE JitAllocatorBlock(
    JitAllocatorPool* pool,
    VirtMem::DualMapping mapping,
    size_t block_size,
    uint32_t block_flags,
    axl::BitWord* used_bit_vector,
    axl::BitWord* stop_bit_vector,
    uint32_t area_size
  ) noexcept
    : ArenaTreeNodeT(),
      _pool(pool),
      _mapping(mapping),
      _block_size(block_size),
      _flags(block_flags),
      _area_size(area_size),
      _used_bit_vector(used_bit_vector),
      _stop_bit_vector(stop_bit_vector) {
    clear_block();
  }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG JitAllocatorPool* pool() const noexcept { return _pool; }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG uint8_t* rx_ptr() const noexcept { return static_cast<uint8_t*>(_mapping.rx); }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG uint8_t* rw_ptr() const noexcept { return static_cast<uint8_t*>(_mapping.rw); }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG bool has_flag(uint32_t f) const noexcept { return (_flags & f) != 0; }

  ASMJIT_INLINE_NODEBUG void add_flags(uint32_t f) noexcept { _flags |= f; }
  ASMJIT_INLINE_NODEBUG void clear_flags(uint32_t f) noexcept { _flags &= ~f; }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG bool is_empty() const noexcept { return has_flag(kFlagEmpty); }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG bool is_dirty() const noexcept { return has_flag(kFlagDirty); }

  ASMJIT_INLINE_NODEBUG void make_dirty() noexcept { add_flags(kFlagDirty); }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG bool is_incremental() const noexcept { return has_flag(kFlagIncremental); }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG bool has_large_pages() const noexcept { return has_flag(kFlagLargePages); }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG bool has_initial_padding() const noexcept { return has_flag(kFlagInitialPadding); }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG uint32_t initial_area_start() const noexcept { return initial_area_start_by_flags(_flags); }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG size_t block_size() const noexcept { return _block_size; }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG uint32_t area_size() const noexcept { return _area_size; }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG uint32_t area_used() const noexcept { return _area_used; }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG uint32_t area_available() const noexcept { return _area_size - _area_used; }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG uint32_t largest_unused_area() const noexcept { return _largest_unused_area; }

  ASMJIT_INLINE void clear_block() noexcept {
    bool bit = has_initial_padding();
    size_t bit_word_count = _pool->bit_word_count_from_area_size(_area_size);

    memset(_used_bit_vector, 0, bit_word_count * sizeof(axl::BitWord));
    memset(_stop_bit_vector, 0, bit_word_count * sizeof(axl::BitWord));

    axl::bit_vector_set_bit(_used_bit_vector, 0, bit);
    axl::bit_vector_set_bit(_stop_bit_vector, 0, bit);

    uint32_t start = initial_area_start_by_flags(_flags);
    _area_used = start;
    _largest_unused_area = _area_size - start;
    _search_start = start;
    _search_end = _area_size;

    add_flags(JitAllocatorBlock::kFlagEmpty | JitAllocatorBlock::kFlagIncremental);
    clear_flags(JitAllocatorBlock::kFlagDirty);
  }

  ASMJIT_INLINE void mark_allocated_area(uint32_t allocated_area_start, uint32_t allocated_area_end) noexcept {
    uint32_t allocated_area_size = allocated_area_end - allocated_area_start;

    // Mark the newly allocated space as occupied and also the sentinel.
    axl::bit_vector_fill(_used_bit_vector, allocated_area_start, allocated_area_size);
    axl::bit_vector_set_bit(_stop_bit_vector, allocated_area_end - 1, true);

    // Update search region and statistics.
    _pool->total_area_used[size_t(has_large_pages())] += allocated_area_size;
    _area_used += allocated_area_size;

    if (area_available() == 0) {
      _search_start = _area_size;
      _search_end = 0;
      _largest_unused_area = 0;

      clear_flags(kFlagDirty | kFlagEmpty);
    }
    else {
      if (_search_start == allocated_area_start) {
        _search_start = allocated_area_end;
      }

      if (_search_end == allocated_area_end) {
        _search_end = allocated_area_start;
      }

      add_flags(kFlagDirty);
      clear_flags(kFlagEmpty);
    }
  }

  ASMJIT_INLINE void mark_released_area(uint32_t released_area_start, uint32_t released_area_end) noexcept {
    uint32_t released_area_size = released_area_end - released_area_start;

    // Update the search region and statistics.
    _pool->total_area_used[size_t(has_large_pages())] -= released_area_size;
    _area_used -= released_area_size;

    // Unmark occupied bits and also the sentinel.
    axl::bit_vector_clear(_used_bit_vector, released_area_start, released_area_size);
    axl::bit_vector_set_bit(_stop_bit_vector, released_area_end - 1, false);

    if (axl::bool_and(is_incremental(), _search_start == released_area_end)) {
      // Incremental mode: If the area released is at the end of the fully used area, we would like to
      // keep the incremental mode of the block. In order to do that, we are going to use a different
      // approach - decrement `_search_start` and increment `_largest_unused_area`, which are essential
      // for incremental mode blocks. Everything after `_search_start` is unused, so the search region
      // must end at `_area_size` (a full block sets `_search_end` to zero).
      ASMJIT_ASSERT(_search_start >= released_area_size);
      _search_start -= released_area_size;
      _search_end = _area_size;
      _largest_unused_area += released_area_size;

      if (area_used() == initial_area_start()) {
        clear_flags(kFlagDirty);
        add_flags(kFlagEmpty);
      }
    }
    else {
      _search_start = axl::min(_search_start, released_area_start);
      _search_end = axl::max(_search_end, released_area_end);
      clear_flags(kFlagDirty | kFlagIncremental);

      if (area_used() == initial_area_start()) {
        _search_start = initial_area_start();
        _search_end = _area_size;
        _largest_unused_area = _area_size - initial_area_start();
        add_flags(kFlagEmpty);
      }
      else {
        add_flags(kFlagDirty);
      }
    }
  }

  ASMJIT_INLINE void mark_shrunk_area(uint32_t shrunk_area_start, uint32_t shrunk_area_end) noexcept {
    uint32_t shrunk_area_size = shrunk_area_end - shrunk_area_start;

    // Shrunk area cannot start at zero as it would mean that we have shrunk the first
    // block to zero bytes, which is not allowed as such block must be released instead.
    ASMJIT_ASSERT(shrunk_area_start != 0);
    ASMJIT_ASSERT(shrunk_area_size != 0);

    // Update the search region and statistics.
    _pool->total_area_used[size_t(has_large_pages())] -= shrunk_area_size;
    _area_used -= shrunk_area_size;

    if (axl::bool_and(is_incremental(), _search_start == shrunk_area_end)) {
      _search_start -= shrunk_area_size;
      _search_end = _area_size;
      _largest_unused_area += shrunk_area_size;
    }
    else {
      _search_start = axl::min(_search_start, shrunk_area_start);
      _search_end = axl::max(_search_end, shrunk_area_end);

      clear_flags(kFlagIncremental);
      add_flags(kFlagDirty);
    }

    // Unmark the released space and move the sentinel.
    axl::bit_vector_clear(_used_bit_vector, shrunk_area_start, shrunk_area_size);
    axl::bit_vector_set_bit(_stop_bit_vector, shrunk_area_end - 1, false);
    axl::bit_vector_set_bit(_stop_bit_vector, shrunk_area_start - 1, true);
  }
};

static ASMJIT_INLINE_NODEBUG bool operator<(const JitAllocatorBlock& a, const JitAllocatorBlock& b) noexcept { return a.rx_ptr() < b.rx_ptr(); }
static ASMJIT_INLINE_NODEBUG bool operator<(const JitAllocatorBlock& a, const uint8_t* b) noexcept { return a.rx_ptr() + a.block_size() <= b; }
static ASMJIT_INLINE_NODEBUG bool operator<(const uint8_t* a, const JitAllocatorBlock& b) noexcept { return a < b.rx_ptr(); }

// Returns an opaque, per-process-unique identifier of the calling thread. The value is only compared for equality,
// it is never interpreted nor converted to an OS handle.
static uint64_t JitAllocator_current_thread_id() noexcept {
#if defined(_WIN32)
  return uint64_t(GetCurrentThreadId());
#elif !defined(__EMSCRIPTEN__)
  return uint64_t((uintptr_t)pthread_self());
#else
  return 0;
#endif
}

// JitAllocator - Allocation Registry
// ==================================

// Identity record of a single live allocation. One record exists per live allocation only - records are removed on
// release, reset, and re-identifying shrink, so the registry never accumulates history. Span handles copy the
// record's identity and generation, which makes every stale copy detectable even when the same address is reused.
class JitAllocationRecord {
public:
  //! Next record in the hash bucket chain.
  JitAllocationRecord* _hash_next = nullptr;
  //! Block that contains the allocation.
  JitAllocatorBlock* _block = nullptr;
  //! Start address of the allocation (read-execute view).
  void* _rx = nullptr;
  //! Allocation identity, unique within the owning allocator.
  uint64_t _identity = 0;
  //! Allocation generation, bumped when the allocation changes identity (shrunk capacity).
  uint64_t _generation = 0;
  //! Precalculated hash-code of `_rx`.
  uint32_t _hash_code = 0;
  //! First granularity unit of the allocation within `_block`.
  uint32_t _area_start = 0;
  //! Current allocation size in granularity units.
  uint32_t _area_size = 0;
};

static constexpr uint32_t JitAllocatorRegistryMinCapacity = 4;

static ASMJIT_INLINE_NODEBUG uint32_t JitAllocator_hash_rx(const void* rx) noexcept {
  // Allocations are always aligned to at least 64 bytes, so discard the always-zero low bits.
  uint64_t v = uint64_t(reinterpret_cast<uintptr_t>(rx)) >> 6;
  v ^= v >> 33;
  v *= 0xff51afd7ed558ccdULL;
  v ^= v >> 33;
  return uint32_t(v);
}

// Finds the area index at which the allocation containing `area_index` starts. `area_index` must reference a used
// area unit.
//
// The start cannot be derived from stop bits alone, because a shrink may leave a free hole (with no stop bit at its
// end) immediately before another allocation. It is therefore the maximum of:
//
//   - the first unit of the contiguous used run containing `area_index`, and
//   - one unit after the nearest preceding stop bit (which delimits allocations, including the initial padding).
static uint32_t JitAllocator_allocation_start(const JitAllocatorBlock* block, uint32_t area_index) noexcept {
  constexpr uint32_t kBitWordSizeInBits = axl::bit_size_of<axl::BitWord>;
  const axl::BitWord* used_vector = block->_used_bit_vector;
  const axl::BitWord* stop_vector = block->_stop_bit_vector;

  // First unit of the contiguous set run that contains `area_index`.
  uint32_t used_run_start = 0;
  {
    uint32_t i = area_index;
    for (;;) {
      uint32_t word_index = i / kBitWordSizeInBits;
      uint32_t word_base = word_index * kBitWordSizeInBits;
      axl::BitWord range_mask = (i - word_base + 1u >= kBitWordSizeInBits)
        ? ~axl::BitWord(0)
        : (axl::BitWord(1) << (i - word_base + 1u)) - axl::BitWord(1);
      axl::BitWord word = used_vector[word_index] & range_mask;

      axl::BitWord zero_bits = (~word) & range_mask;
      if (zero_bits) {
        uint32_t highest_zero = uint32_t(sizeof(axl::BitWord) == 8
          ? 63u - axl::clz_64(uint64_t(zero_bits))
          : 31u - axl::clz_32(uint32_t(zero_bits)));
        used_run_start = word_base + highest_zero + 1u;
        break;
      }

      if (word_base == 0) {
        used_run_start = 0;
        break;
      }
      i = word_base - 1u;
    }
  }

  // One unit after the nearest stop bit preceding `area_index`; zero when there is none.
  uint32_t after_previous_stop = 0;
  if (area_index != 0) {
    uint32_t i = area_index;
    for (;;) {
      uint32_t word_index = (i - 1u) / kBitWordSizeInBits;
      uint32_t word_base = word_index * kBitWordSizeInBits;
      axl::BitWord word = stop_vector[word_index];

      uint32_t bits_in_word = i - word_base;
      if (bits_in_word < kBitWordSizeInBits) {
        word &= (axl::BitWord(1) << bits_in_word) - axl::BitWord(1);
      }
      if (word) {
        uint32_t highest_bit = uint32_t(sizeof(axl::BitWord) == 8
          ? 63u - axl::clz_64(uint64_t(word))
          : 31u - axl::clz_32(uint32_t(word)));
        after_previous_stop = word_base + highest_bit + 1u;
        break;
      }

      if (word_base == 0) {
        break;
      }
      i = word_base;
    }
  }

  return axl::max(used_run_start, after_previous_stop);
}

// JitAllocator - PrivateImpl
// ==========================

class JitAllocatorPrivateImpl : public JitAllocator::Impl {
public:
  //! \name Members
  //! \{

  //! Lock for thread safety.
  mutable Lock lock;
  //! Number of times the lock is held (reentrancy support for write callbacks).
  uint32_t lock_depth = 0;
  //! Thread that currently holds the lock, or zero when it is free.
  uint64_t lock_owner_thread = 0;
  //! System page size (also a minimum block size).
  uint32_t page_size;
  //! Number of active allocations.
  size_t allocation_count;
  //! Address ranges for executable memory, in the order they are tried.
  VirtMem::AddressRange address_ranges[2];
  //! Number of address ranges.
  uint32_t address_range_count;
  //! True if an address range was requested, even if none could be computed.
  bool address_range_requested;

  //! Blocks from all pools in RBTree.
  axl::ArenaTree<JitAllocatorBlock> tree;
  //! Allocator pools.
  JitAllocatorPool* pools;
  //! Number of allocator pools.
  size_t pool_count;

  //! Hash table buckets of live allocation records (points to `_alloc_embedded` while empty/small).
  JitAllocationRecord** alloc_table;
  //! Embedded single-bucket storage, so an allocator needs no heap allocation for the registry itself.
  JitAllocationRecord* alloc_embedded[1];
  //! Number of hash table buckets.
  uint32_t alloc_capacity;
  //! Number of records in the hash table.
  uint32_t alloc_record_count;
  //! Monotonic identity counter, never reused within the allocator's lifetime.
  uint64_t next_identity;
  //! Monotonic generation counter, bumped on every invalidating event.
  uint64_t generation;
  //! Nesting depth of write protections (write callbacks may reenter the allocator).
  uint32_t write_scope_depth;
  //! Thread that owns the active write protection, or zero when there is none.
  uint64_t write_scope_owner_thread;

  //! \}

  ASMJIT_INLINE JitAllocatorPrivateImpl(JitAllocatorPool* pools, size_t pool_count) noexcept
    : JitAllocator::Impl {},
      page_size(0),
      allocation_count(0),
      address_ranges{},
      address_range_count(0),
      address_range_requested(false),
      pools(pools),
      pool_count(pool_count),
      alloc_table(alloc_embedded),
      alloc_embedded{nullptr},
      alloc_capacity(1),
      alloc_record_count(0),
      next_identity(1),
      generation(0),
      write_scope_depth(0),
      write_scope_owner_thread(0) {}
  ASMJIT_INLINE ~JitAllocatorPrivateImpl() noexcept {}

  //! Enters a write protection region. Returns true when the caller is the outermost one and must actually switch
  //! the pages to writable; a nested call on the same thread returns false and must not touch page permissions.
  ASMJIT_INLINE bool begin_write_protection() noexcept {
    uint64_t thread_id = JitAllocator_current_thread_id();
    if (write_scope_depth == 0 || write_scope_owner_thread != thread_id) {
      write_scope_owner_thread = thread_id;
      write_scope_depth = 1;
      return true;
    }
    write_scope_depth++;
    return false;
  }

  //! Leaves a write protection region; only the outermost caller restores execute permissions.
  ASMJIT_INLINE void end_write_protection() noexcept {
    write_scope_depth--;
    if (write_scope_depth == 0) {
      write_scope_owner_thread = 0;
    }
  }

  //! \name Allocation registry
  //! \{

  ASMJIT_INLINE void registry_init() noexcept {
    alloc_table = alloc_embedded;
    alloc_embedded[0] = nullptr;
    alloc_capacity = 1;
    alloc_record_count = 0;
  }

  ASMJIT_INLINE JitAllocationRecord* registry_find(void* rx) const noexcept {
    uint32_t hash_code = JitAllocator_hash_rx(rx);
    JitAllocationRecord* record = alloc_table[hash_code & (alloc_capacity - 1u)];

    while (record && record->_rx != rx) {
      record = record->_hash_next;
    }
    return record;
  }

  ASMJIT_INLINE void registry_insert(JitAllocationRecord* record) noexcept {
    if (alloc_record_count >= alloc_capacity) {
      registry_grow();
    }

    uint32_t index = record->_hash_code & (alloc_capacity - 1u);
    record->_hash_next = alloc_table[index];
    alloc_table[index] = record;
    alloc_record_count++;
  }

  ASMJIT_INLINE void registry_remove(JitAllocationRecord* record) noexcept {
    uint32_t index = record->_hash_code & (alloc_capacity - 1u);
    JitAllocationRecord** p = &alloc_table[index];

    while (*p != record) {
      p = &(*p)->_hash_next;
    }
    *p = record->_hash_next;
    record->_hash_next = nullptr;
    alloc_record_count--;
  }

  void registry_grow() noexcept {
    uint32_t new_capacity = axl::max(alloc_capacity * 2u, JitAllocatorRegistryMinCapacity);
    size_t byte_size = size_t(new_capacity) * sizeof(JitAllocationRecord*);
    JitAllocationRecord** new_table = static_cast<JitAllocationRecord**>(axl::mem_alloc(byte_size));

    if (ASMJIT_UNLIKELY(!new_table)) {
      return; // Keep using the old table; chaining remains correct.
    }

    memset(new_table, 0, byte_size);
    uint32_t new_mask = new_capacity - 1u;

    for (uint32_t i = 0; i < alloc_capacity; i++) {
      JitAllocationRecord* record = alloc_table[i];
      while (record) {
        JitAllocationRecord* next = record->_hash_next;
        uint32_t index = record->_hash_code & new_mask;
        record->_hash_next = new_table[index];
        new_table[index] = record;
        record = next;
      }
    }

    if (alloc_table != alloc_embedded) {
      axl::mem_free(alloc_table);
    }
    alloc_table = new_table;
    alloc_capacity = new_capacity;
  }

  //! Frees every record and restores the registry to its initial state.
  void registry_reset() noexcept {
    for (uint32_t i = 0; i < alloc_capacity; i++) {
      JitAllocationRecord* record = alloc_table[i];
      while (record) {
        JitAllocationRecord* next = record->_hash_next;
        axl::mem_free(record);
        record = next;
      }
    }

    if (alloc_table != alloc_embedded) {
      axl::mem_free(alloc_table);
    }
    registry_init();
  }

  //! \}
};

// Recursive counterpart of `LockGuard` for `JitAllocatorPrivateImpl::lock`.
//
// Write callbacks are allowed to call back into the same allocator (alloc/query/write/shrink of other
// allocations), so the allocator lock must be reacquirable by the thread that already holds it. Cross-thread
// contention is still handled by the underlying non-recursive lock.
class JitAllocatorLock {
public:
  ASMJIT_NONCOPYABLE(JitAllocatorLock)

  JitAllocatorPrivateImpl* _impl;
  bool _acquired;

  ASMJIT_INLINE explicit JitAllocatorLock(JitAllocatorPrivateImpl* impl) noexcept
    : _impl(impl),
      _acquired(false) {
    uint64_t thread_id = JitAllocator_current_thread_id();

    if (impl->lock_depth == 0 || impl->lock_owner_thread != thread_id) {
      impl->lock.lock();
      impl->lock_owner_thread = thread_id;
      _acquired = true;
    }
    impl->lock_depth++;
  }

  ASMJIT_INLINE ~JitAllocatorLock() noexcept {
    _impl->lock_depth--;
    if (_acquired) {
      _impl->lock_owner_thread = 0;
      _impl->lock.unlock();
    }
  }
};

// Write protection with per-thread nesting.
//
// A write callback can reenter the allocator to write, shrink, release, or allocate another allocation. On MAP_JIT
// platforms page permissions are process/thread global, so only the outermost region may toggle them; nested regions
// just bump a counter and let the outermost region restore permissions and flush the instruction cache once at the
// end.
class JitAllocatorWriteScopeGuard {
public:
  ASMJIT_NONCOPYABLE(JitAllocatorWriteScopeGuard)

  JitAllocatorPrivateImpl* _impl;
  void* _rx;
  size_t _size;
  VirtMem::CachePolicy _policy;
  bool _outer;

  ASMJIT_INLINE JitAllocatorWriteScopeGuard(
    JitAllocatorPrivateImpl* impl,
    void* rx,
    size_t size,
    VirtMem::CachePolicy policy) noexcept
    : _impl(impl),
      _rx(rx),
      _size(size),
      _policy(policy),
      _outer(impl->begin_write_protection()) {

    if (_outer) {
      VirtMem::protect_jit_memory(VirtMem::ProtectJitAccess::kReadWrite);
    }
  }

  ASMJIT_INLINE ~JitAllocatorWriteScopeGuard() noexcept {
    if (_outer) {
      VirtMem::protect_jit_memory(VirtMem::ProtectJitAccess::kReadExecute);
      if (_policy != VirtMem::CachePolicy::kNeverFlush) {
        VirtMem::flush_instruction_cache(_rx, _size);
      }
    }
    _impl->end_write_protection();
  }
};

static const JitAllocator::Impl JitAllocatorImpl_none {};
static const JitAllocator::CreateParams JitAllocatorParams_none {};

// JitAllocator - Utilities
// ========================

// Computes the ranges below and above the module that contains `addr` (or `addr` itself if it's not in a module), from
// where a 32-bit displacement reaches the whole module. Returns the number of ranges, ordered as they should be tried.
static uint32_t JitAllocator_near_address_ranges(uintptr_t addr, VirtMem::AddressRange* ranges) noexcept {
#if ASMJIT_TARGET_ARCH_X86 == 64
  // 2GiB minus 64kB.
  constexpr uintptr_t kReach = 0x7FFF0000u;
  // A lower bound that keeps the range above `vm.mmap_min_addr`.
  constexpr uintptr_t kMinAddress = 0x100000u;

  VirtMem::AddressRange module {};
  if (VirtMem::module_address_range(addr, Out(module)) != Error::kOk) {
    module = VirtMem::AddressRange{addr, addr + 1u};
  }

  if (module.end - module.start > kReach) {
    return 0;
  }

  VirtMem::AddressRange below { module.end > kMinAddress + kReach ? module.end - kReach : kMinAddress, module.start };
  VirtMem::AddressRange above { module.end, module.start + kReach };

#if defined(__APPLE__)
  VirtMem::AddressRange candidates[2] = { above, below };
#else
  VirtMem::AddressRange candidates[2] = { below, above };
#endif

  uint32_t count = 0;
  for (const VirtMem::AddressRange& range : candidates) {
    if (range.start < range.end) {
      ranges[count++] = range;
    }
  }
  return count;
#else
  axl::maybe_unused(addr, ranges);
  return 0;
#endif
}

static inline JitAllocatorPrivateImpl* JitAllocator_new_impl(const JitAllocator::CreateParams* params) noexcept {
  VirtMem::Info vm_info = VirtMem::info();

  if (!params) {
    params = &JitAllocatorParams_none;
  }

  JitAllocatorOptions options = params->options;
  uint32_t block_size = params->block_size;
  uint32_t granularity = params->granularity;
  uint32_t fill_pattern = params->fill_pattern;

  // Setup pool count to [1..3].
  size_t pool_count = 1;
  if (axl::test(options, JitAllocatorOptions::kUseMultiplePools)) {
    pool_count = kJitAllocatorMultiPoolCount;
  }

  // Setup block size [64kB..256MB].
  if (block_size < 64 * 1024 || block_size > 256 * 1024 * 1024 || !axl::is_power_of_2(block_size)) {
    block_size = vm_info.page_granularity;
  }

  // Setup granularity [64..256].
  if (granularity < 64 || granularity > 256 || !axl::is_power_of_2(granularity)) {
    granularity = kJitAllocatorBaseGranularity;
  }

  // Setup fill-pattern.
  if (uint32_t(options & JitAllocatorOptions::kCustomFillPattern) == 0) {
    fill_pattern = JitAllocator_default_fill_pattern();
  }

  size_t size = sizeof(JitAllocatorPrivateImpl) + sizeof(JitAllocatorPool) * pool_count;
  void* p = axl::mem_alloc(size);

  if (ASMJIT_UNLIKELY(!p)) {
    return nullptr;
  }

  VirtMem::HardenedRuntimeInfo hardened_rt_info = VirtMem::hardened_runtime_info();
  if (axl::test(hardened_rt_info.flags, VirtMem::HardenedRuntimeFlags::kEnabled)) {
    // If we are running within a hardened environment (mapping RWX is not allowed) then we have to use dual mapping
    // or other runtime capabilities like Apple specific MAP_JIT. There is no point in not enabling these as otherwise
    // the allocation would fail and JitAllocator would not be able to allocate memory.
    if (!axl::test(hardened_rt_info.flags, VirtMem::HardenedRuntimeFlags::kMapJit)) {
      options |= JitAllocatorOptions::kUseDualMapping;
    }
  }

  JitAllocatorPool* pools = reinterpret_cast<JitAllocatorPool*>((uint8_t*)p + sizeof(JitAllocatorPrivateImpl));
  JitAllocatorPrivateImpl* impl = new(axl::PlacementNew{p}) JitAllocatorPrivateImpl(pools, pool_count);

  impl->options = options;
  impl->block_size = block_size;
  impl->granularity = granularity;
  impl->fill_pattern = fill_pattern;
  impl->page_size = vm_info.page_size;

  if (params->address_range.end) {
    impl->address_ranges[0] = params->address_range;
    impl->address_range_count = 1;
    impl->address_range_requested = true;
  }
  else if (params->near_address) {
    impl->address_range_count = JitAllocator_near_address_ranges(params->near_address, impl->address_ranges);
    impl->address_range_requested = true;
  }

  for (size_t pool_id = 0; pool_id < pool_count; pool_id++) {
    new(axl::PlacementNew{&pools[pool_id]}) JitAllocatorPool(granularity << pool_id);
  }

  return impl;
}

static ASMJIT_INLINE void JitAllocator_destroy_impl(JitAllocatorPrivateImpl* impl) noexcept {
  impl->~JitAllocatorPrivateImpl();
  axl::mem_free(impl);
}

static ASMJIT_INLINE size_t JitAllocator_size_to_pool_id(const JitAllocatorPrivateImpl* impl, size_t size) noexcept {
  size_t pool_id = impl->pool_count - 1;
  size_t granularity = size_t(impl->granularity) << pool_id;

  while (pool_id) {
    if (axl::align_up(size, granularity) == size) {
      break;
    }
    pool_id--;
    granularity >>= 1;
  }

  return pool_id;
}

static ASMJIT_INLINE size_t JitAllocator_bit_vector_size_to_byte_size(uint32_t area_size) noexcept {
  static constexpr uint32_t kBitWordSizeInBits = axl::bit_size_of<axl::BitWord>;
  return ((area_size + kBitWordSizeInBits - 1u) / kBitWordSizeInBits) * sizeof(axl::BitWord);
}

static ASMJIT_INLINE size_t JitAllocator_calculate_ideal_block_size(JitAllocatorPrivateImpl* impl, JitAllocatorPool* pool, size_t allocation_size) noexcept {
  JitAllocatorBlock* last = pool->blocks.last();
  size_t block_size = last ? last->block_size() : size_t(impl->block_size);

  // We have to increase the allocation_size if we know that the block must provide padding.
  if (!axl::test(impl->options, JitAllocatorOptions::kDisableInitialPadding)) {
    size_t granularity = pool->granularity;
    if (SIZE_MAX - allocation_size < granularity) {
      return 0; // Overflown
    }
    allocation_size += granularity;
  }

  if (block_size < kJitAllocatorMaxBlockSize) {
    block_size *= 2u;
  }

  if (allocation_size > block_size) {
    block_size = axl::align_up(allocation_size, impl->block_size);
    if (ASMJIT_UNLIKELY(block_size < allocation_size)) {
      return 0; // Overflown.
    }
  }

  if (axl::test(impl->options, JitAllocatorOptions::kUseDualMapping)) {
    size_t dm_limit = VirtMem::dual_mapping_size_limit();
    if (block_size > dm_limit) {
      block_size = dm_limit;
      if (allocation_size > block_size) {
        return 0;
      }
    }

  }

  return block_size;
}

ASMJIT_NOINLINE
ASMJIT_FAVOR_SPEED static void JitAllocator_fill_pattern(void* mem, uint32_t pattern, size_t byte_size) noexcept {
  // NOTE: This is always used to fill a pattern in allocated / freed memory. The allocation has always
  // a granularity that is greater than the pattern, however, when shrink() is used, we may end up having
  // an unaligned start, so deal with it here and then copy aligned pattern in the loop.
  if ((uintptr_t(mem) & 0x1u) && byte_size >= 1u) {
    static_cast<uint8_t*>(mem)[0] = uint8_t(pattern & 0xFF);
    mem = static_cast<uint8_t*>(mem) + 1;
    byte_size--;
  }

  if ((uintptr_t(mem) & 0x2u) && byte_size >= 2u) {
    static_cast<uint16_t*>(mem)[0] = uint16_t(pattern & 0xFFFF);
    mem = static_cast<uint16_t*>(mem) + 1;
    byte_size -= 2;
  }

  // Something would be seriously broken if we end up with aligned `mem`, but unaligned `byte_size`.
  ASMJIT_ASSERT((byte_size & 0x3u) == 0u);

  uint32_t* mem32 = static_cast<uint32_t*>(mem);
  size_t n = byte_size / 4u;

  for (size_t i = 0; i < n; i++) {
    mem32[i] = pattern;
  }
}

// Allocates virtual memory of a block, within the address range first if the allocator has one.
static Error JitAllocator_alloc_virt_mem(const JitAllocatorPrivateImpl* impl, VirtMem::DualMapping& vm, size_t size, VirtMem::MemoryFlags mem_flags) noexcept {
  bool dual_mapping = axl::test(impl->options, JitAllocatorOptions::kUseDualMapping);
  bool strict = axl::test(impl->options, JitAllocatorOptions::kStrictAddressRange);

  // No range can be computed from `near_address` on architectures other than X86_64.
  Error err = Error::kFeatureNotEnabled;

  for (uint32_t i = 0; i < impl->address_range_count; i++) {
    const VirtMem::AddressRange& range = impl->address_ranges[i];

    err = dual_mapping ? VirtMem::alloc_dual_mapping(Out(vm), size, mem_flags, range)
                       : VirtMem::alloc(&vm.rx, size, mem_flags, range);
    if (err == Error::kOk) {
      return err;
    }
  }

  if (strict && impl->address_range_requested) {
    return make_error(err);
  }

  return dual_mapping ? VirtMem::alloc_dual_mapping(Out(vm), size, mem_flags)
                      : VirtMem::alloc(&vm.rx, size, mem_flags);
}

// Allocate a new `JitAllocatorBlock` for the given `block_size`.
//
// NOTE: The block doesn't have `kFlagEmpty` flag set, because the new block
// is only allocated when it's actually needed, so it would be cleared anyway.
static Error JitAllocator_new_block(JitAllocatorPrivateImpl* impl, JitAllocatorBlock** dst, JitAllocatorPool* pool, size_t block_size) noexcept {
  using axl::BitWord;
  static constexpr uint32_t kBitWordSizeInBits = axl::bit_size_of<axl::BitWord>;

  uint32_t block_flags = 0;
  if (!axl::test(impl->options, JitAllocatorOptions::kDisableInitialPadding)) {
    block_flags |= JitAllocatorBlock::kFlagInitialPadding;
  }

  VirtMem::DualMapping virt_mem {};
  VirtMem::MemoryFlags mem_flags = VirtMem::MemoryFlags::kAccessRWX;

  if (axl::test(impl->options, JitAllocatorOptions::kUseDualMapping)) {
    ASMJIT_PROPAGATE(JitAllocator_alloc_virt_mem(impl, virt_mem, block_size, mem_flags));
    block_flags |= JitAllocatorBlock::kFlagDualMapped;
  }
  else {
    bool allocate_regular_pages = true;
    if (axl::test(impl->options, JitAllocatorOptions::kUseLargePages)) {
      size_t large_page_size = VirtMem::large_page_size();
      bool try_large_page = block_size >= large_page_size || axl::test(impl->options, JitAllocatorOptions::kAlignBlockSizeToLargePage);

      // Only proceed if we can actually allocate large pages.
      if (large_page_size && try_large_page) {
        size_t large_block_size = axl::align_up(block_size, large_page_size);
        Error err = JitAllocator_alloc_virt_mem(impl, virt_mem, large_block_size, mem_flags | VirtMem::MemoryFlags::kMMapLargePages);

        if (err == Error::kOk) {
          // Large page(s) allocated.
          allocate_regular_pages = false;
          block_size = large_block_size;
          block_flags |= JitAllocatorBlock::kFlagLargePages;
        }
        else {
          // Fallback to regular pages if failed to allocate large page(s).
          mem_flags |= VirtMem::MemoryFlags::kMAdviseLargePages;
        }
      }
    }

    // Called either if large pages were not requested or large page(s) allocation failed.
    if (allocate_regular_pages) {
      ASMJIT_PROPAGATE(JitAllocator_alloc_virt_mem(impl, virt_mem, block_size, mem_flags));
    }

    virt_mem.rw = virt_mem.rx;
  }

  uint32_t area_size = uint32_t(block_size >> pool->granularity_log2);
  uint32_t bit_word_count = (area_size + kBitWordSizeInBits - 1u) / kBitWordSizeInBits;
  uint8_t* block_ptr = axl::mem_alloc<uint8_t>(sizeof(JitAllocatorBlock) + size_t(bit_word_count) * 2u * sizeof(BitWord));

  // Out of memory...
  if (ASMJIT_UNLIKELY(block_ptr == nullptr)) {
    if (axl::test(impl->options, JitAllocatorOptions::kUseDualMapping)) {
      (void)VirtMem::release_dual_mapping(virt_mem, block_size);
    }
    else {
      (void)VirtMem::release(virt_mem.rx, block_size);
    }
    return make_error(Error::kOutOfMemory);
  }

  // Fill the allocated virtual memory if secure mode is enabled. The nesting-aware guard keeps a write callback
  // (which may trigger a new block through reentrant alloc()) from flipping the JIT pages back to RX prematurely.
  if (axl::test(impl->options, JitAllocatorOptions::kFillUnusedMemory)) {
    JitAllocatorWriteScopeGuard scope(impl, virt_mem.rw, block_size, VirtMem::CachePolicy::kNeverFlush);
    JitAllocator_fill_pattern(virt_mem.rw, impl->fill_pattern, block_size);
  }

  BitWord* bit_words = reinterpret_cast<BitWord*>(block_ptr + sizeof(JitAllocatorBlock));
  *dst = new(axl::PlacementNew{block_ptr}) JitAllocatorBlock(pool, virt_mem, block_size, block_flags, bit_words, bit_words + bit_word_count, area_size);
  return Error::kOk;
}

static void JitAllocatorImpl_delete_block(JitAllocatorPrivateImpl* impl, JitAllocatorBlock* block) noexcept {
  axl::maybe_unused(impl);

  if (block->has_flag(JitAllocatorBlock::kFlagDualMapped)) {
    (void)VirtMem::release_dual_mapping(block->_mapping, block->block_size());
  }
  else {
    (void)VirtMem::release(block->rx_ptr(), block->block_size());
  }

  axl::mem_free(block);
}

static void JitAllocatorImpl_insert_block(JitAllocatorPrivateImpl* impl, JitAllocatorBlock* block) noexcept {
  JitAllocatorPool* pool = block->pool();

  if (!pool->cursor) {
    pool->cursor = block;
  }

  // Add to RBTree and List.
  impl->tree.insert(block);
  pool->blocks.append(block);

  // Update statistics.
  size_t stat_index = size_t(block->has_large_pages());
  pool->block_count++;
  pool->total_area_size[stat_index] += block->area_size();
  pool->total_area_used[stat_index] += block->area_used();
  pool->total_overhead_bytes += sizeof(JitAllocatorBlock) + JitAllocator_bit_vector_size_to_byte_size(block->area_size()) * 2u;
}

static void JitAllocatorImpl_remove_block(JitAllocatorPrivateImpl* impl, JitAllocatorBlock* block) noexcept {
  JitAllocatorPool* pool = block->pool();

  // Remove from RBTree and List.
  if (pool->cursor == block) {
    pool->cursor = block->has_prev() ? block->prev() : block->next();
  }

  impl->tree.remove(block);
  pool->blocks.unlink(block);

  // Update statistics.
  size_t stat_index = size_t(block->has_large_pages());
  pool->block_count--;
  pool->total_area_size[stat_index] -= block->area_size();
  pool->total_area_used[stat_index] -= block->area_used();
  pool->total_overhead_bytes -= sizeof(JitAllocatorBlock) + JitAllocator_bit_vector_size_to_byte_size(block->area_size()) * 2u;
}

static void JitAllocatorImpl_wipe_out_block(JitAllocatorPrivateImpl* impl, JitAllocatorBlock* block) noexcept {
  if (block->has_flag(JitAllocatorBlock::kFlagEmpty)) {
    return;
  }

  JitAllocatorPool* pool = block->pool();
  if (axl::test(impl->options, JitAllocatorOptions::kFillUnusedMemory)) {
    VirtMem::protect_jit_memory(VirtMem::ProtectJitAccess::kReadWrite);

    uint32_t granularity = pool->granularity;
    uint8_t* rw_ptr = block->rw_ptr();
    axl::BitVectorRangeIterator<axl::BitWord, 0> it(block->_used_bit_vector, pool->bit_word_count_from_area_size(block->area_size()));

    size_t range_start;
    size_t range_end;

    while (it.next_range(Out(range_start), Out(range_end))) {
      uint8_t* span_ptr = rw_ptr + range_start * granularity;
      size_t span_size = (range_end - range_start) * granularity;

      JitAllocator_fill_pattern(span_ptr, impl->fill_pattern, span_size);
      VirtMem::flush_instruction_cache(span_ptr, span_size);
    }
    VirtMem::protect_jit_memory(VirtMem::ProtectJitAccess::kReadExecute);
  }

  block->clear_block();
}

// JitAllocator - Reentrant Lock & Span Helpers
// ============================================

// Fills `out` with a Span that references the allocation described by `record`. Must be called with the allocator
// lock held (the data read from the record/block must not change concurrently).
static void JitAllocator_populate_span(
  JitAllocator* self,
  JitAllocationRecord* record,
  JitAllocator::Span& out) noexcept {

  JitAllocatorBlock* block = record->_block;
  JitAllocatorPool* pool = block->pool();
  size_t byte_offset = pool->byte_size_from_area_size(record->_area_start);

  out._rx = record->_rx;
  out._rw = block->rw_ptr() + byte_offset;
  out._size = pool->byte_size_from_area_size(record->_area_size);
  out._block = static_cast<void*>(block);
  out._flags = JitAllocator::Span::Flags::kNone;
  out._reserved = 0;
  out._allocator = self;
  out._identity = record->_identity;
  out._generation = record->_generation;
}

// Validates a Span against the live allocations of `self`. Returns the matching record, or null if the span is
// default-constructed, belongs to another allocator, or is stale (its allocation was released, reset, or got a new
// identity through shrinking). Must be called with the allocator lock held.
static JitAllocationRecord* JitAllocator_validate_span(
  const JitAllocator* self,
  const JitAllocatorPrivateImpl* impl,
  const JitAllocator::Span& span) noexcept {

  if (!span._rx || span._allocator != self) {
    return nullptr;
  }

  JitAllocationRecord* record = impl->registry_find(span._rx);
  if (!record || record->_identity != span._identity || record->_generation != span._generation) {
    return nullptr;
  }
  return record;
}

// Releases the allocation described by `record` and frees the record itself. Shared by the raw-pointer and the
// Span-based release overloads. Must be called with the allocator lock held.
static void JitAllocatorImpl_release_record(JitAllocatorPrivateImpl* impl, JitAllocationRecord* record) noexcept {
  JitAllocatorBlock* block = record->_block;
  JitAllocatorPool* pool = block->pool();

  uint32_t area_index = record->_area_start;
  uint32_t area_size = record->_area_size;
  uint32_t area_end = area_index + area_size;

  impl->allocation_count--;
  block->mark_released_area(area_index, area_end);

  impl->registry_remove(record);
  impl->generation++;
  axl::mem_free(record);

  // Fill the released memory if the secure mode is enabled.
  if (axl::test(impl->options, JitAllocatorOptions::kFillUnusedMemory)) {
    uint8_t* span_ptr = block->rw_ptr() + area_index * pool->granularity;
    size_t span_size = area_size * pool->granularity;

    JitAllocatorWriteScopeGuard scope(impl, span_ptr, span_size, VirtMem::CachePolicy::kDefault);
    JitAllocator_fill_pattern(span_ptr, impl->fill_pattern, span_size);
  }

  // Release the whole block if it became empty.
  if (block->is_empty()) {
    if (pool->empty_block_count || axl::test(impl->options, JitAllocatorOptions::kImmediateRelease)) {
      JitAllocatorImpl_remove_block(impl, block);
      JitAllocatorImpl_delete_block(impl, block);
    }
    else {
      pool->empty_block_count++;
    }
  }
}

// JitAllocator - Construction & Destruction
// =========================================

JitAllocator::JitAllocator(const CreateParams* params) noexcept {
  _impl = JitAllocator_new_impl(params);
  if (ASMJIT_UNLIKELY(!_impl)) {
    _impl = const_cast<JitAllocator::Impl*>(&JitAllocatorImpl_none);
  }
}

JitAllocator::~JitAllocator() noexcept {
  if (_impl == &JitAllocatorImpl_none) {
    return;
  }

  JitAllocatorPrivateImpl* impl = static_cast<JitAllocatorPrivateImpl*>(_impl);
  reset(ResetPolicy::kHard);
  impl->registry_reset();
  JitAllocator_destroy_impl(impl);
}

// JitAllocator - Reset
// ====================

void JitAllocator::reset(ResetPolicy reset_policy) noexcept {
  if (_impl == &JitAllocatorImpl_none) {
    return;
  }

  JitAllocatorPrivateImpl* impl = static_cast<JitAllocatorPrivateImpl*>(_impl);
  JitAllocatorLock guard(impl);

  // Every allocation loses its identity, which invalidates all outstanding Spans.
  impl->generation++;
  impl->allocation_count = 0;
  impl->registry_reset();

  impl->tree.reset();
  size_t pool_count = impl->pool_count;

  for (size_t pool_id = 0; pool_id < pool_count; pool_id++) {
    JitAllocatorPool& pool = impl->pools[pool_id];
    JitAllocatorBlock* block = pool.blocks.first();

    pool.reset();

    if (block) {
      JitAllocatorBlock* block_to_keep = nullptr;
      if (reset_policy != ResetPolicy::kHard && uint32_t(impl->options & JitAllocatorOptions::kImmediateRelease) == 0) {
        block_to_keep = block;
        block = block->next();
      }

      while (block) {
        JitAllocatorBlock* next = block->next();
        JitAllocatorImpl_delete_block(impl, block);
        block = next;
      }

      if (block_to_keep) {
        block_to_keep->_list_nodes[0] = nullptr;
        block_to_keep->_list_nodes[1] = nullptr;
        // The block is reinserted into a freshly reset RB-tree, so its tree node state must be cleared as well;
        // otherwise a node that happened to be red or have children would fail the tree's insertion invariants.
        block_to_keep->_tree_nodes[0] = 0;
        block_to_keep->_tree_nodes[1] = 0;
        JitAllocatorImpl_wipe_out_block(impl, block_to_keep);
        JitAllocatorImpl_insert_block(impl, block_to_keep);
        pool.empty_block_count = 1;
      }
    }
  }
}

// JitAllocator - Statistics
// =========================

JitAllocator::Statistics JitAllocator::statistics() const noexcept {
  Statistics statistics;
  statistics.reset();

  if (ASMJIT_LIKELY(_impl != &JitAllocatorImpl_none)) {
    JitAllocatorPrivateImpl* impl = static_cast<JitAllocatorPrivateImpl*>(_impl);
    JitAllocatorLock guard(impl);

    size_t pool_count = impl->pool_count;
    for (size_t pool_id = 0; pool_id < pool_count; pool_id++) {
      const JitAllocatorPool& pool = impl->pools[pool_id];
      statistics._block_count   += size_t(pool.block_count);
      statistics._reserved_size += size_t(pool.total_area_size[0] + pool.total_area_size[1]) * pool.granularity;
      statistics._used_size     += size_t(pool.total_area_used[0] + pool.total_area_used[1]) * pool.granularity;
      statistics._overhead_size += size_t(pool.total_overhead_bytes);
    }

    statistics._allocation_count = impl->allocation_count;
  }

  return statistics;
}

// JitAllocator - Alloc & Release
// ==============================

Error JitAllocator::alloc(Out<Span> out, size_t size) noexcept {
  constexpr uint32_t no_index = axl::max_value<uint32_t>();
  constexpr size_t max_request_size = axl::max_value<uint32_t>() / 2u;

  JitAllocatorPrivateImpl* impl = static_cast<JitAllocatorPrivateImpl*>(_impl);
  bool not_initialized = _impl == &JitAllocatorImpl_none;

  // Align to the minimum granularity by default.
  size = axl::align_up<size_t>(size, impl->granularity);
  out = Span{};

  if (ASMJIT_UNLIKELY(axl::bool_or(not_initialized, size - 1u >= max_request_size))) {
    return make_error(not_initialized ? Error::kNotInitialized  :
                      size == 0u      ? Error::kInvalidArgument : Error::kTooLarge);
  }

  JitAllocatorLock guard(impl);

  JitAllocationRecord* record = axl::mem_alloc<JitAllocationRecord>(sizeof(JitAllocationRecord));
  if (ASMJIT_UNLIKELY(!record)) {
    return make_error(Error::kOutOfMemory);
  }

  JitAllocatorPool* pool = &impl->pools[JitAllocator_size_to_pool_id(impl, size)];

  uint32_t area_index = no_index;
  uint32_t area_size = uint32_t(pool->area_size_from_byte_size(size));

  // Try to find the requested memory area in existing blocks.
  JitAllocatorBlock* block = pool->cursor;

  if (block) {
    JitAllocatorBlock* initial = block;

    do {
      uint32_t largest_unused_area = block->largest_unused_area();

      if (axl::bool_and(block->is_incremental(), largest_unused_area >= area_size)) {
        // Fast path: If the block is in incremental mode, which means that it's guaranteed it's full before
        // `search_start` and completely empty after it, we can just quickly increment `search_start` and be
        // done with the allocation. This is a little bit faster than constructing a BitVectorRangeIterator
        // and searching for zero bit clusters. When a block is in incremental mode its `largest_unused_area`
        // is basically the free space after `search_start`, so that's the only thing to check.
        area_index = block->_search_start;
        block->_largest_unused_area -= area_size;
        break;
      }
      else if (block->area_available() >= area_size) {
        // Regular path: Search for a cluster of bits that would mark an empty area we want to allocate.
        if (axl::bool_or(block->is_dirty(), largest_unused_area >= area_size)) {
          axl::BitVectorRangeIterator<axl::BitWord, 0> it(block->_used_bit_vector, pool->bit_word_count_from_area_size(block->area_size()), block->_search_start, block->_search_end);

          size_t range_start = 0;
          size_t range_end = block->area_size();

          size_t search_start = SIZE_MAX;
          size_t largest_area = 0;

          while (it.next_range(Out(range_start), Out(range_end), area_size)) {
            size_t range_size = range_end - range_start;
            if (range_size >= area_size) {
              area_index = uint32_t(range_start);
              break;
            }

            search_start = axl::min(search_start, range_start);
            largest_area = axl::max(largest_area, range_size);
          }

          if (area_index != no_index) {
            break;
          }

          if (search_start != SIZE_MAX) {
            // Because we have iterated over the entire block, we can now mark the
            // largest unused area that can be used to cache the next traversal.
            size_t search_end = range_end;

            block->_search_start = uint32_t(search_start);
            block->_search_end = uint32_t(search_end);
            block->_largest_unused_area = uint32_t(largest_area);
            block->clear_flags(JitAllocatorBlock::kFlagDirty);
          }
        }
      }

      // The block cursor doesn't have to start with the first block and we want to
      // iterate all before concluding that there is no free space in any block.
      block = block->has_next() ? block->next() : pool->blocks.first();
    } while (block != initial);
  }

  // Allocate a new block if there is no region of a required size.
  if (area_index == no_index) {
    size_t block_size = JitAllocator_calculate_ideal_block_size(impl, pool, size);
    if (ASMJIT_UNLIKELY(!block_size)) {
      axl::mem_free(record);
      return make_error(Error::kOutOfMemory);
    }

    Error block_err = JitAllocator_new_block(impl, &block, pool, block_size);
    if (ASMJIT_UNLIKELY(block_err != Error::kOk)) {
      axl::mem_free(record);
      return block_err;
    }
    area_index = block->initial_area_start();

    JitAllocatorImpl_insert_block(impl, block);
    block->_search_start += area_size;
    block->_largest_unused_area -= area_size;
  }
  else if (block->has_flag(JitAllocatorBlock::kFlagEmpty)) {
    pool->empty_block_count--;
    block->clear_flags(JitAllocatorBlock::kFlagEmpty);
  }

  // Update statistics.
  impl->allocation_count++;
  block->mark_allocated_area(area_index, area_index + area_size);

  // Register the new allocation identity before publishing the span.
  size_t offset = pool->byte_size_from_area_size(area_index);
  ASMJIT_ASSERT(offset <= block->block_size() - size);

  void* rx = block->rx_ptr() + offset;
  record->_hash_next = nullptr;
  record->_block = block;
  record->_rx = rx;
  record->_identity = impl->next_identity++;
  record->_generation = impl->generation;
  record->_hash_code = JitAllocator_hash_rx(rx);
  record->_area_start = area_index;
  record->_area_size = area_size;
  impl->registry_insert(record);

  JitAllocator_populate_span(this, record, out.value());
  return Error::kOk;
}

Error JitAllocator::release(void* rx) noexcept {
  bool not_initialized = _impl == &JitAllocatorImpl_none;

  if (ASMJIT_UNLIKELY(axl::bool_or(not_initialized, !rx))) {
    return make_error(not_initialized ? Error::kNotInitialized : Error::kInvalidArgument);
  }

  JitAllocatorPrivateImpl* impl = static_cast<JitAllocatorPrivateImpl*>(_impl);
  JitAllocatorLock guard(impl);

  // Only the exact start address of a live allocation has a registry record. Interior addresses, holes, padding,
  // and addresses past the last allocation are therefore rejected without touching the bit-vectors or counters.
  JitAllocationRecord* record = impl->registry_find(rx);
  if (ASMJIT_UNLIKELY(!record)) {
    return make_error(Error::kInvalidArgument);
  }

  JitAllocatorImpl_release_record(impl, record);
  return Error::kOk;
}

Error JitAllocator::release(Span& span) noexcept {
  if (_impl == &JitAllocatorImpl_none) {
    return make_error(Error::kNotInitialized);
  }

  JitAllocatorPrivateImpl* impl = static_cast<JitAllocatorPrivateImpl*>(_impl);
  JitAllocatorLock guard(impl);

  JitAllocationRecord* record = JitAllocator_validate_span(this, impl, span);
  if (ASMJIT_UNLIKELY(!record)) {
    return make_error(Error::kInvalidState);
  }

  JitAllocatorImpl_release_record(impl, record);
  span = Span{};
  return Error::kOk;
}

// Applies a validated shrink while the allocator lock is held. `span` must reference `record`'s current identity.
static Error JitAllocatorImpl_shrink(
  JitAllocator* self,
  JitAllocatorPrivateImpl* impl,
  JitAllocationRecord* record,
  JitAllocator::Span& span,
  size_t new_size) noexcept {

  JitAllocatorBlock* block = record->_block;
  JitAllocatorPool* pool = block->pool();

  uint32_t area_start = record->_area_start;
  uint32_t area_prev_size = record->_area_size;
  uint32_t span_prev_size = area_prev_size * pool->granularity;
  uint32_t area_shrunk_size = pool->area_size_from_byte_size(new_size);

  if (ASMJIT_UNLIKELY(area_shrunk_size > area_prev_size)) {
    return make_error(Error::kInvalidArgument);
  }

  uint32_t area_diff = area_prev_size - area_shrunk_size;
  if (area_diff) {
    block->mark_shrunk_area(area_start + area_shrunk_size, area_start + area_prev_size);

    // Fill released memory if the secure mode is enabled. When called from within a write callback this nests into
    // the active write protection instead of toggling page permissions back and forth.
    if (new_size < span_prev_size && axl::test(impl->options, JitAllocatorOptions::kFillUnusedMemory)) {
      uint8_t* span_ptr = block->rw_ptr() + (area_start + area_shrunk_size) * pool->granularity;
      size_t span_size = area_diff * pool->granularity;

      JitAllocatorWriteScopeGuard scope(impl, span_ptr, span_size, VirtMem::CachePolicy::kNeverFlush);
      JitAllocator_fill_pattern(span_ptr, impl->fill_pattern, span_size);
    }

    // The capacity changed: rotate the identity so every Span copy made before the shrink becomes stale.
    impl->generation++;
    record->_generation = impl->generation;
    record->_area_size = area_shrunk_size;
  }

  // Always republish the authoritative (granularity-rounded) state. This also rounds a size that was truncated
  // inside a write callback without changing the allocation's area, in which case the identity is preserved.
  JitAllocator_populate_span(self, record, span);
  return Error::kOk;
}

Error JitAllocator::shrink(Span& span, size_t new_size) noexcept {
  if (_impl == &JitAllocatorImpl_none) {
    return make_error(Error::kNotInitialized);
  }

  if (ASMJIT_UNLIKELY(!span.rx())) {
    return make_error(Error::kInvalidArgument);
  }

  // Shrinking to zero is a release, handled by the Span-based release so that staleness is enforced.
  if (ASMJIT_UNLIKELY(new_size == 0)) {
    return release(span);
  }

  JitAllocatorPrivateImpl* impl = static_cast<JitAllocatorPrivateImpl*>(_impl);
  JitAllocatorLock guard(impl);

  JitAllocationRecord* record = JitAllocator_validate_span(this, impl, span);
  if (ASMJIT_UNLIKELY(!record)) {
    return make_error(Error::kInvalidState);
  }

  return JitAllocatorImpl_shrink(this, impl, record, span, new_size);
}

Error JitAllocator::query(Out<Span> out, void* rx) const noexcept {
  *out = Span{};

  if (ASMJIT_UNLIKELY(_impl == &JitAllocatorImpl_none)) {
    return make_error(Error::kNotInitialized);
  }

  JitAllocatorPrivateImpl* impl = static_cast<JitAllocatorPrivateImpl*>(_impl);
  JitAllocatorLock guard(impl);

  // Block lookup is logarithmic in the number of managed blocks, never a scan of live allocations.
  JitAllocatorBlock* block = impl->tree.get(static_cast<uint8_t*>(rx));
  if (ASMJIT_UNLIKELY(!block)) {
    return make_error(Error::kInvalidArgument);
  }

  // Offset relative to the start of the block.
  JitAllocatorPool* pool = block->pool();
  size_t offset = (size_t)((uint8_t*)rx - block->rx_ptr());
  uint32_t area_index = uint32_t(offset >> pool->granularity_log2);

  // Holes, released areas, and block initial padding are not marked as allocated.
  if (ASMJIT_UNLIKELY(!axl::bit_vector_get_bit(block->_used_bit_vector, area_index))) {
    return make_error(Error::kInvalidArgument);
  }

  // Normalize an interior address to the start of the whole allocation (one unit after the previous stop bit).
  uint32_t area_start = JitAllocator_allocation_start(block, area_index);
  uint32_t area_end = uint32_t(axl::bit_vector_index_of(block->_stop_bit_vector, area_start, true)) + 1;

  void* allocation_rx = block->rx_ptr() + pool->byte_size_from_area_size(area_start);
  JitAllocationRecord* record = impl->registry_find(allocation_rx);
  if (ASMJIT_UNLIKELY(!record)) {
    return make_error(Error::kInvalidArgument);
  }

  ASMJIT_ASSERT(record->_area_start == area_start);
  ASMJIT_ASSERT(record->_area_size == area_end - area_start);
  JitAllocator_populate_span(const_cast<JitAllocator*>(this), record, out.value());
  return Error::kOk;
}

bool JitAllocator::Span::is_valid() const noexcept {
  if (!_allocator || _allocator->_impl == &JitAllocatorImpl_none) {
    return false;
  }

  JitAllocatorPrivateImpl* impl = static_cast<JitAllocatorPrivateImpl*>(_allocator->_impl);
  JitAllocatorLock guard(impl);
  return JitAllocator_validate_span(_allocator, impl, *this) != nullptr;
}

// JitAllocator - Write
// ====================

static ASMJIT_INLINE VirtMem::CachePolicy JitAllocator_default_policy_for_span(const JitAllocator::Span& span) noexcept {
  if (axl::test(span.flags(), JitAllocator::Span::Flags::kInstructionCacheClean)) {
    return VirtMem::CachePolicy::kNeverFlush;
  }
  else {
    return VirtMem::CachePolicy::kFlushAfterWrite;
  }
}

Error JitAllocator::write(Span& span, size_t offset, const void* src, size_t size, VirtMem::CachePolicy policy) noexcept {
  if (_impl == &JitAllocatorImpl_none) {
    return make_error(Error::kNotInitialized);
  }

  JitAllocatorPrivateImpl* impl = static_cast<JitAllocatorPrivateImpl*>(_impl);
  JitAllocatorLock guard(impl);

  // Validate before changing any page protection, bit-vector, or statistic. A zero-length write still requires a
  // live, owned span - a stale copy must never be reported as a successful write.
  JitAllocationRecord* record = JitAllocator_validate_span(this, impl, span);
  if (ASMJIT_UNLIKELY(!record)) {
    return make_error(Error::kInvalidState);
  }

  if (ASMJIT_UNLIKELY(offset > span.size() || span.size() - offset < size)) {
    return make_error(Error::kInvalidArgument);
  }

  if (policy == VirtMem::CachePolicy::kDefault) {
    policy = JitAllocator_default_policy_for_span(span);
  }

  if (size == 0) {
    return Error::kOk;
  }

  JitAllocatorWriteScopeGuard write_scope(impl, span.rx(), span.size(), policy);
  memcpy(static_cast<uint8_t*>(span.rw()) + offset, src, size);
  return Error::kOk;
}

Error JitAllocator::write(Span& span, WriteFunc write_fn, void* user_data, VirtMem::CachePolicy policy) noexcept {
  if (_impl == &JitAllocatorImpl_none) {
    return make_error(Error::kNotInitialized);
  }

  JitAllocatorPrivateImpl* impl = static_cast<JitAllocatorPrivateImpl*>(_impl);
  JitAllocatorLock guard(impl);

  // Reject stale or foreign spans (and empty spans) before entering the writable region or invoking the callback.
  JitAllocationRecord* record = JitAllocator_validate_span(this, impl, span);
  if (ASMJIT_UNLIKELY(!record)) {
    return make_error(Error::kInvalidState);
  }

  if (ASMJIT_UNLIKELY(span.size() == 0)) {
    return make_error(Error::kInvalidArgument);
  }

  if (policy == VirtMem::CachePolicy::kDefault) {
    policy = JitAllocator_default_policy_for_span(span);
  }

  size_t size_before = span.size();
  JitAllocatorWriteScopeGuard write_scope(impl, span.rx(), span.size(), policy);

  Error callback_err = write_fn(span, user_data);
  if (ASMJIT_UNLIKELY(callback_err != Error::kOk)) {
    return callback_err;
  }

  // If the callback truncated the span (via Span::shrink()), commit the new capacity while the writable region is
  // still active. The re-validation only guards against a callback that broke the documented no-release/no-reset
  // contract.
  if (span.size() != size_before) {
    size_t new_size = span.size();
    record = JitAllocator_validate_span(this, impl, span);
    if (ASMJIT_UNLIKELY(!record)) {
      return make_error(Error::kInvalidState);
    }
    return JitAllocatorImpl_shrink(this, impl, record, span, new_size);
  }

  return Error::kOk;
}

// JitAllocator - Write Scope
// ==========================

Error JitAllocator::begin_write_scope(WriteScopeData& scope, VirtMem::CachePolicy policy) noexcept {
  scope.policy = policy;
  scope.flags = 0u;
  scope.data[0] = 0u;
  scope.data[1] = 0u;
  return Error::kOk;
}

Error JitAllocator::end_write_scope(WriteScopeData& scope) noexcept {
  axl::maybe_unused(scope);
  return Error::kOk;
}

Error JitAllocator::flush_write_scope(WriteScopeData& scope) noexcept {
  axl::maybe_unused(scope);
  return Error::kOk;
}

Error JitAllocator::scoped_write(WriteScopeData& scope, Span& span, size_t offset, const void* src, size_t size) noexcept {
  return write(span, offset, src, size, scope.policy);
}

Error JitAllocator::scoped_write(WriteScopeData& scope, Span& span, WriteFunc write_fn, void* user_data) noexcept {
  return write(span, write_fn, user_data, scope.policy);
}

ASMJIT_END_NAMESPACE

#endif // !ASMJIT_NO_JIT

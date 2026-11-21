/*
 * Copyright (C) 2015, Simon Fuhrmann
 * TU Darmstadt - Graphics, Capture and Massively Parallel Computing
 * All rights reserved.
 *
 * This software may be modified and distributed under the terms
 * of the BSD 3-Clause license. See the LICENSE.txt file for details.
 *
 * Allocation accounting for the out-of-core extraction.
 *
 * The tracking counters are process-global and are only enabled while an
 * extraction call is running (see fssr/stream_surface.cc). Containers that
 * use TrackedAllocator are accounted for exactly, including the capacity
 * over-allocation of std::vector and the node allocations of std::map.
 * When tracking is disabled the allocator forwards directly to the global
 * operator new/delete without touching the counters.
 */

#ifndef FSSR_MEMORY_HEADER
#define FSSR_MEMORY_HEADER

#include <cstddef>
#include <new>

#include "fssr/defines.h"

FSSR_NAMESPACE_BEGIN
namespace detail
{
    extern std::size_t tracked_live_bytes;
    extern std::size_t tracked_peak_bytes;
    extern bool tracked_enabled;

    inline void
    track_allocate (std::size_t bytes)
    {
        if (!tracked_enabled)
            return;
        tracked_live_bytes += bytes;
        if (tracked_live_bytes > tracked_peak_bytes)
            tracked_peak_bytes = tracked_live_bytes;
    }

    inline void
    track_deallocate (std::size_t bytes)
    {
        if (!tracked_enabled)
            return;
        tracked_live_bytes -= bytes;
    }

    inline void
    tracking_reset (void)
    {
        tracked_live_bytes = 0;
        tracked_peak_bytes = 0;
        tracked_enabled = true;
    }

    inline void
    tracking_disable (void)
    {
        tracked_enabled = false;
    }
}

/** Allocator that reports its live allocation to the tracker. */
template <typename T>
struct TrackedAllocator
{
    typedef T value_type;

    template <typename U>
    struct rebind { typedef TrackedAllocator<U> other; };

    TrackedAllocator (void) noexcept { }
    template <typename U>
    TrackedAllocator (TrackedAllocator<U> const&) noexcept { }

    T* allocate (std::size_t n)
    {
        std::size_t const bytes = n * sizeof(T);
        T* ptr = static_cast<T*>(::operator new(bytes));
        detail::track_allocate(bytes);
        return ptr;
    }

    void deallocate (T* ptr, std::size_t n)
    {
        detail::track_deallocate(n * sizeof(T));
        ::operator delete(ptr);
    }

    template <typename U>
    bool operator== (TrackedAllocator<U> const&) const { return true; }
    template <typename U>
    bool operator!= (TrackedAllocator<U> const&) const { return false; }
};

FSSR_NAMESPACE_END

#endif /* FSSR_MEMORY_HEADER */

#pragma once
#ifndef ESP32
#error "PSRAMAllocatorESP32.h is ESP32-specific (uses esp_heap_caps.h)."
#endif
#include <esp_heap_caps.h>
#include <cstddef>
#include <cstdlib>

/*
 * A minimal C++ Allocator (usable as std::vector's second template
 * argument) that places its storage in PSRAM via ESP-IDF's
 * heap_caps_malloc(), instead of the regular heap. Intended for
 * TinyMPGDecoder<PSRAMAllocatorESP32<uint8_t>> on boards that have PSRAM
 * (e.g. ESP32-S3 modules with octal/quad PSRAM) - a plain ESP32 without
 * PSRAM should keep using the default TinyMPGDecoder<> (regular heap), see
 * mpg_config.h for the memory budget that implies. Ported (renamed to the
 * tinympg namespace, and made no-throw - see below) from TinyH264's own
 * PSRAMAllocatorESP32.h - this allocator is generic, not H.264-specific.
 *
 * Usage:
 *   #include <PSRAMAllocatorESP32.h>
 *   using namespace tinympg;
 *   TinyMPGDecoder<PSRAMAllocatorESP32<uint8_t>> decoder;
 *
 * @note When PSRAM allocation fails (exhausted or absent), this falls back
 * to the regular heap rather than failing outright. deallocate() using
 * heap_caps_free() for both cases is safe on ESP-IDF: malloc() is itself
 * backed by the same heap_caps/multi_heap registry, so heap_caps_free()
 * correctly frees pointers regardless of which of the two allocated them.
 *
 * @note allocate() returns nullptr rather than throwing std::bad_alloc on
 * failure (even if both the PSRAM and regular-heap fallback attempts
 * fail) - the same no-throw contract StdAllocator.h's StdAllocator
 * follows and for the same reason (see that file's comment): it lets
 * common/mpg_frame.h's Buffer<Allocator> detect and report an allocation
 * failure as DecodeStatus::kAllocationError instead of crashing, on every
 * toolchain regardless of whether C++ exceptions are enabled.
 */

namespace tinympg {

/**
 * C++ Allocator (std::allocator-compatible) that services allocate()
 * requests from PSRAM via ESP-IDF's heap_caps_malloc(), falling back to
 * the regular internal heap if PSRAM is absent or exhausted. Drop this in
 * as std::vector's second template argument (or as
 * TinyMPGDecoder<PSRAMAllocatorESP32<uint8_t>>'s allocator) to move the
 * decoder's frame buffer out of scarce internal DRAM on boards that have
 * PSRAM. Stateless (all instances compare equal), as required for a
 * standard-library Allocator.
 */
template <typename T>
class PSRAMAllocatorESP32 {
 public:
  using value_type = T;

  /// Default-constructs a stateless allocator instance.
  PSRAMAllocatorESP32() noexcept = default;

  /**
   * Rebinding constructor required by the Allocator concept (lets
   * std::vector<T, PSRAMAllocatorESP32<T>> construct the allocator for an
   * internal node/element type U from one for T).
   */
  template <typename U>
  PSRAMAllocatorESP32(const PSRAMAllocatorESP32<U>&) noexcept {}

  /**
   * Allocates storage for `n` objects of type T. Tries PSRAM first
   * (MALLOC_CAP_SPIRAM); if that fails (no PSRAM on this board, or it's
   * exhausted), falls back to the regular heap via malloc() so the
   * decoder still runs, just without the PSRAM benefit. Returns nullptr -
   * never throws - if both attempts fail, or if `n` would overflow the
   * byte-size multiplication.
   */
  T* allocate(std::size_t n) noexcept {
    if (n > (std::size_t(-1) / sizeof(T))) return nullptr;
    void* p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = malloc(n * sizeof(T));  // fallback to regular heap if PSRAM is exhausted
    return static_cast<T*>(p);
  }

  /**
   * Frees storage previously returned by allocate(). Always uses
   * heap_caps_free() regardless of which path allocate() actually took
   * (PSRAM or regular malloc()) - safe because ESP-IDF's malloc() is
   * itself backed by the same heap_caps/multi_heap registry, so
   * heap_caps_free() correctly identifies and frees either kind of
   * pointer.
   */
  void deallocate(T* p, std::size_t) noexcept { heap_caps_free(p); }

  /**
   * Allocators of this type are stateless, so any two instances (even for
   * different T) are interchangeable.
   */
  template <typename U>
  bool operator==(const PSRAMAllocatorESP32<U>&) const noexcept {
    return true;
  }
  /// See operator==; always false since operator== is always true.
  template <typename U>
  bool operator!=(const PSRAMAllocatorESP32<U>&) const noexcept {
    return false;
  }
};

template <typename T>
using PSRAMAllocator = PSRAMAllocatorESP32<T>;

}  // namespace tinympg

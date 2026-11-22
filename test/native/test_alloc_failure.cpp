// Correctness gate for the allocation-failure handling described in
// src/StdAllocator.h's file comment: an out-of-memory condition must
// surface as TinyMPGDecoder::Status::kAllocationError, not crash the
// process - see common/mpg_frame.h's Buffer<Allocator> and
// decoder/mpg_decoder.h's DecodeStatus::kAllocationError for the
// mechanics. This is the one test in the suite that plugs in a custom
// (non-default, non-PSRAM) Allocator, specifically to make the very first
// picture-buffer allocation fail on demand.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "TinyMPGDecoder.h"

using namespace tinympg;

static std::vector<uint8_t> readFile(const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) {
    fprintf(stderr, "cannot open %s (run from test/native/, see docs/testing.md)\n", path);
    exit(1);
  }
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  std::vector<uint8_t> data((size_t)size);
  size_t n = fread(data.data(), 1, (size_t)size, f);
  (void)n;
  fclose(f);
  return data;
}

/**
 * A StdAllocator-shaped allocator (see StdAllocator.h) whose allocate()
 * fails on demand - controlled via static counters rather than instance
 * state, since std::vector/Buffer may rebind or copy an Allocator
 * instance at will and this project's Allocator concept requires
 * statelessness (see StdAllocator's own comment). Follows the same
 * "return nullptr, never throw" no-crash contract as the real
 * allocators - that contract is exactly what's under test here.
 */
template <typename T>
struct FailingAllocator {
  using value_type = T;

  static int callCount;
  static int failAtCall;  // 0 = never fail; N = fail the Nth allocate() call (1-indexed)

  FailingAllocator() noexcept = default;
  template <typename U>
  FailingAllocator(const FailingAllocator<U>&) noexcept {}

  T* allocate(std::size_t n) noexcept {
    callCount++;
    if (failAtCall != 0 && callCount == failAtCall) return nullptr;
    return static_cast<T*>(malloc(n * sizeof(T)));
  }
  void deallocate(T* p, std::size_t) noexcept { free(p); }

  template <typename U>
  bool operator==(const FailingAllocator<U>&) const noexcept {
    return true;
  }
  template <typename U>
  bool operator!=(const FailingAllocator<U>&) const noexcept {
    return false;
  }
};

template <typename T>
int FailingAllocator<T>::callCount = 0;
template <typename T>
int FailingAllocator<T>::failAtCall = 0;

int main() {
  std::vector<uint8_t> stream = readFile("assets/small.m1v");
  using FailingDecoder = TinyMPGDecoder<FailingAllocator<uint8_t>>;

  // 1. The very first picture-buffer allocation fails (out of memory from
  //    the start) - write() must report kAllocationError, not crash, and
  //    hasError() must reflect it.
  {
    FailingAllocator<uint8_t>::callCount = 0;
    FailingAllocator<uint8_t>::failAtCall = 1;

    FailingDecoder dec;
    auto status = dec.write(stream.data(), stream.size());
    assert(status == FailingDecoder::Status::kAllocationError);
    assert(dec.lastStatus() == FailingDecoder::Status::kAllocationError);
    assert(dec.hasError());
  }

  // 2. begin() (the eager-allocation path) reports the same failure the
  //    same way, without needing any bitstream data at all.
  {
    FailingAllocator<uint8_t>::callCount = 0;
    FailingAllocator<uint8_t>::failAtCall = 1;

    FailingDecoder dec;
    bool ok = dec.begin();
    assert(!ok);
    assert(dec.lastStatus() == FailingDecoder::Status::kAllocationError);
    assert(dec.hasError());
  }

  // 3. A later allocation (not the first) failing - e.g. the second or
  //    third of Frame's three planes, or a later Frame's first plane -
  //    is caught the same way, and doesn't leave anything half-allocated
  //    that a subsequent begin() can't recover from once allocation
  //    starts succeeding again (Frame::ensureAllocated() releases
  //    whatever partially succeeded on failure - see mpg_frame.h).
  {
    FailingAllocator<uint8_t>::callCount = 0;
    FailingAllocator<uint8_t>::failAtCall = 2;  // dataU, the 2nd of curFrame_'s 3 planes

    FailingDecoder dec;
    bool ok = dec.begin();
    assert(!ok);
    assert(dec.hasError());

    FailingAllocator<uint8_t>::failAtCall = 0;  // now let allocation succeed
    ok = dec.begin();
    assert(ok);
    assert(!dec.hasError());
  }

  // 4. A fresh decoder with a never-failing allocator still decodes
  //    normally through the exact same public API - the FailingAllocator
  //    plumbing itself doesn't change correct-path behavior.
  {
    FailingAllocator<uint8_t>::callCount = 0;
    FailingAllocator<uint8_t>::failAtCall = 0;  // never fail

    FailingDecoder dec;
    int frameCount = 0;
    dec.setCallback([](FailingDecoder&, void* userData) { (*(int*)userData)++; }, &frameCount);
    auto status = dec.write(stream.data(), stream.size());
    assert(!dec.hasError());
    assert(status == FailingDecoder::Status::kFrameReady ||
           status == FailingDecoder::Status::kNeedMoreData);
    assert(frameCount == 10);  // assets/small.m1v: 10 I-pictures, see test_decode_iframe.cpp
  }

  printf("test_alloc_failure: OK\n");
  return 0;
}

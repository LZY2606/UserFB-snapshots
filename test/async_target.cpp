// ==================================================================================
// Copyright (c) 2017 HiFi-LoFi
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is furnished
// to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
// FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
// COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
// IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
// WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
// ==================================================================================

// Tests for fftconvolver::AsyncFFTConvolver. Registered with the C test driver
// in task_support/driver.c through the task_cases/task_case_count symbols.

#include "task_support/check.h"

#include "AsyncFFTConvolver.h"

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

// ----------------------------------------------------------------------------------
// Global heap instrumentation: process() must never allocate or free.
// ----------------------------------------------------------------------------------

static std::atomic<int> g_heapBan{0};
static std::atomic<int> g_heapHit{0};

void* operator new(std::size_t size)
{
  if (g_heapBan.load(std::memory_order_relaxed))
  {
    g_heapHit.store(1, std::memory_order_relaxed);
  }
  void* p = std::malloc(size ? size : 1);
  if (!p) throw std::bad_alloc();
  return p;
}

void* operator new[](std::size_t size)
{
  if (g_heapBan.load(std::memory_order_relaxed))
  {
    g_heapHit.store(1, std::memory_order_relaxed);
  }
  void* p = std::malloc(size ? size : 1);
  if (!p) throw std::bad_alloc();
  return p;
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
  if (g_heapBan.load(std::memory_order_relaxed))
  {
    g_heapHit.store(1, std::memory_order_relaxed);
  }
  return std::malloc(size ? size : 1);
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
  if (g_heapBan.load(std::memory_order_relaxed))
  {
    g_heapHit.store(1, std::memory_order_relaxed);
  }
  return std::malloc(size ? size : 1);
}

void operator delete(void* p) noexcept
{
  if (g_heapBan.load(std::memory_order_relaxed))
  {
    g_heapHit.store(1, std::memory_order_relaxed);
  }
  std::free(p);
}

void operator delete[](void* p) noexcept
{
  if (g_heapBan.load(std::memory_order_relaxed))
  {
    g_heapHit.store(1, std::memory_order_relaxed);
  }
  std::free(p);
}

void operator delete(void* p, std::size_t) noexcept
{
  if (g_heapBan.load(std::memory_order_relaxed))
  {
    g_heapHit.store(1, std::memory_order_relaxed);
  }
  std::free(p);
}

void operator delete[](void* p, std::size_t) noexcept
{
  if (g_heapBan.load(std::memory_order_relaxed))
  {
    g_heapHit.store(1, std::memory_order_relaxed);
  }
  std::free(p);
}

// ----------------------------------------------------------------------------------
// Controllable executor with a real worker thread, a mailbox and condvar gates.
// No sleeping is used anywhere: every schedule is driven by explicit signals.
// ----------------------------------------------------------------------------------

class ControlledExecutor : public fftconvolver::TailExecutor
{
public:
  enum Policy
  {
    POLICY_QUEUE,   // Hand the job to the persistent worker thread
    POLICY_INLINE,  // Run the job synchronously inside submit()
    POLICY_REJECT   // Refuse every job
  };

  std::atomic<unsigned> duplicateSubmissions{0};
  std::atomic<unsigned> wouldBlockSubmits{0};
  std::atomic<unsigned> quiesceCalls{0};

  ControlledExecutor() :
    _stop(false),
    _full(false),
    _hold(false),
    _pending(0),
    _policy(POLICY_QUEUE),
    _accepts(0),
    _rejects(0),
    _inlineRuns(0),
    _workerRuns(0),
    _worker([this] { workerLoop(); })
  {
  }

  ~ControlledExecutor() override
  {
    {
      std::lock_guard<std::mutex> lock(_mutex);
      _stop = true;
      _condition.notify_all();
    }
    if (_worker.joinable())
    {
      _worker.join();
    }
  }

  bool submit(void (*run)(void*), void* arg) override
  {
    // Non-blocking by contract: never wait on the worker or the mutex.
    std::unique_lock<std::mutex> lock(_mutex, std::try_to_lock);
    if (!lock.owns_lock())
    {
      ++wouldBlockSubmits;
      ++_rejects;
      return false;
    }

    if (_policy == POLICY_REJECT)
    {
      ++_rejects;
      return false;
    }

    if (_policy == POLICY_INLINE)
    {
      ++_accepts;
      ++_inlineRuns;
      lock.unlock();
      run(arg); // Executed before submit() returns - explicitly allowed.
      return true;
    }

    if (_full)
    {
      // The convolver may only submit while its single slot is idle, so a
      // mailbox still holding a job means a duplicate/buggy submission.
      ++duplicateSubmissions;
      ++_rejects;
      return false;
    }

    _run = run;
    _arg = arg;
    _full = true;
    ++_pending;
    ++_accepts;
    _condition.notify_all();
    return true;
  }

  void quiesce() override
  {
    ++quiesceCalls;
    std::unique_lock<std::mutex> lock(_mutex);
    // Open any closed gate so a held worker can finish; control-thread callers
    // are allowed to block here.
    _hold = false;
    _condition.notify_all();
    _condition.wait(lock, [this] { return _pending == 0; });
  }

  void setPolicy(Policy policy)
  {
    std::lock_guard<std::mutex> lock(_mutex);
    _policy = policy;
  }

  // Closes the gate: the worker holds accepted jobs without executing them,
  // keeping the convolver slot busy across arbitrary numbers of blocks.
  void setHold(bool hold)
  {
    std::lock_guard<std::mutex> lock(_mutex);
    _hold = hold;
    _condition.notify_all();
  }

  // Blocks until the worker has picked up the queued job (but, while the gate
  // is closed, not executed it).
  void waitUntilTaken()
  {
    std::unique_lock<std::mutex> lock(_mutex);
    _condition.wait(lock, [this] { return !_full || _stop; });
  }

  // Opens the gate and blocks until every accepted job has fully returned.
  void releaseAndJoin()
  {
    {
      std::lock_guard<std::mutex> lock(_mutex);
      _hold = false;
      _condition.notify_all();
    }
    quiesce();
  }

  unsigned accepts() const { return _accepts.load(); }
  unsigned rejects() const { return _rejects.load(); }
  unsigned inlineRuns() const { return _inlineRuns.load(); }
  unsigned workerRuns() const { return _workerRuns.load(); }
  unsigned pending() const { return _pending.load(); }

private:
  void workerLoop()
  {
    for (;;)
    {
      void (*run)(void*) = 0;
      void* arg = 0;
      {
        std::unique_lock<std::mutex> lock(_mutex);
        _condition.wait(lock, [this] { return _stop || _full; });
        if (_stop && !_full)
        {
          return;
        }
        run = _run;
        arg = _arg;
        _full = false;
        _condition.notify_all();

        // Delayed-start gate: the slot stays BUSY while we wait here.
        _condition.wait(lock, [this] { return !_hold || _stop; });
        if (_stop)
        {
          --_pending;
          _condition.notify_all();
          return;
        }
        lock.unlock();
      }

      run(arg); // The actual tail computation crosses any audio deadline.

      {
        std::lock_guard<std::mutex> lock(_mutex);
        --_pending;
        ++_workerRuns;
        _condition.notify_all();
      }
    }
  }

  std::mutex _mutex;
  std::condition_variable _condition;
  bool _stop;
  bool _full;
  bool _hold;
  std::atomic<unsigned> _pending;
  std::atomic<Policy> _policy;
  std::atomic<unsigned> _accepts;
  std::atomic<unsigned> _rejects;
  std::atomic<unsigned> _inlineRuns;
  std::atomic<unsigned> _workerRuns;
  void (*_run)(void*);
  void* _arg;
  std::thread _worker;
};

// ----------------------------------------------------------------------------------
// Shared helpers
// ----------------------------------------------------------------------------------

namespace
{

struct Setup
{
  size_t headBlock;
  size_t tailBlock;
  size_t irLen;
};

void makeIr(std::vector<float>& ir, size_t irLen, unsigned seed)
{
  ir.resize(irLen);
  for (size_t i = 0; i < irLen; ++i)
  {
    const float t = static_cast<float>(i);
    const float decay = std::exp(-3.0f * t / static_cast<float>(irLen ? irLen : 1));
    ir[i] = 0.4f * decay *
            (std::sin(t * (0.05f + 0.00003f * seed)) +
             0.4f * std::sin(t * 0.011f + static_cast<float>(seed)));
  }
}

void makeInput(std::vector<float>& x, size_t len, unsigned seed)
{
  x.resize(len);
  for (size_t i = 0; i < len; ++i)
  {
    const float t = static_cast<float>(i);
    x[i] = 0.5f * std::sin(t * 0.017f + static_cast<float>(seed)) +
           0.25f * std::sin(t * 0.0043f);
    if ((i + seed) % 257 == 0)
    {
      x[i] += 0.8f;
    }
  }
}

// Independent double-precision time-domain reference convolution.
void referenceConvolution(const std::vector<float>& ir,
                          const std::vector<float>& x,
                          std::vector<double>& y)
{
  y.assign(x.size(), 0.0);
  for (size_t n = 0; n < x.size(); ++n)
  {
    double sum = 0.0;
    const size_t kMax = std::min(ir.size(), n + 1);
    for (size_t k = 0; k < kMax; ++k)
    {
      sum += static_cast<double>(ir[k]) * static_cast<double>(x[n - k]);
    }
    y[n] = sum;
  }
}

double toleranceFor(const std::vector<float>& ir)
{
  double sumAbs = 0.0;
  for (float v : ir)
  {
    sumAbs += std::fabs(static_cast<double>(v));
  }
  return 2e-5 * (1.0 + sumAbs);
}

// Feeds the convolver in arbitrary chunks; optionally fully in-place.
void feedChunks(fftconvolver::AsyncFFTConvolver& convolver,
                const std::vector<float>& input,
                std::vector<float>& output,
                bool inPlace,
                size_t extraZero,
                unsigned patternSeed)
{
  const size_t total = input.size() + extraZero;
  std::vector<float> source = input;
  source.resize(total, 0.0f);
  output.assign(total, 0.0f);

  static const size_t kPatterns[] = {1, 7, 3, 16, 64, 257};
  size_t pos = 0;
  unsigned step = patternSeed;
  if (inPlace)
  {
    // The output buffer is also the input buffer: preload it with the whole
    // dry signal. Each in-place process() call only overwrites its own chunk,
    // which has already been consumed by the time the call returns, so the
    // remaining dry samples stay intact.
    output = source;
  }
  while (pos < total)
  {
    const size_t chunk = std::min(kPatterns[(step++) % 6], total - pos);
    if (inPlace)
    {
      convolver.process(output.data() + pos, output.data() + pos, chunk);
    }
    else
    {
      convolver.process(source.data() + pos, output.data() + pos, chunk);
    }
    pos += chunk;
  }
}

bool checkAgainstReference(const std::vector<float>& ir,
                           const std::vector<float>& input,
                           const std::vector<float>& output,
                           size_t extraZero,
                           const char* tag)
{
  std::vector<float> paddedInput = input;
  paddedInput.resize(input.size() + extraZero, 0.0f);
  std::vector<double> ref;
  referenceConvolution(ir, paddedInput, ref);

  if (ref.size() != output.size())
  {
    std::fprintf(stderr, "FAIL %s: length mismatch\n", tag);
    return false;
  }

  const double tol = toleranceFor(ir);
  double maxErr = 0.0;
  for (size_t n = 0; n < ref.size(); ++n)
  {
    if (!std::isfinite(output[n]))
    {
      std::fprintf(stderr, "FAIL %s: non-finite output at %zu\n", tag, n);
      return false;
    }
    maxErr = std::max(maxErr, std::fabs(ref[n] - static_cast<double>(output[n])));
  }
  if (!(maxErr <= tol))
  {
    std::fprintf(stderr, "FAIL %s: maxErr=%g tol=%g\n", tag, maxErr, tol);
    return false;
  }
  return true;
}

} // namespace

// ----------------------------------------------------------------------------------
// PCM group: numerical correctness against the double-precision reference
// ----------------------------------------------------------------------------------

static int pcm_local_all_rejected()
{
  const Setup setups[] = {
    {16, 64, 300},    // Long IR, several tail partitions
    {8, 64, 63},      // Short IR, head only
    {32, 256, 512},   // IR boundary at exactly 2*tailBlock
    {64, 1024, 5000}, // Long tail, many partitions
    {256, 4096, 1}    // Single coefficient
  };

  for (const Setup& setup : setups)
  {
    std::vector<float> ir, input, output;
    makeIr(ir, setup.irLen, 11);
    makeInput(input, 6 * setup.tailBlock + 137, 5);

    ControlledExecutor executor;
    executor.setPolicy(ControlledExecutor::POLICY_REJECT);
    fftconvolver::AsyncFFTConvolver convolver;
    TASK_CHECK(convolver.init(setup.headBlock, setup.tailBlock,
                              ir.data(), ir.size(), &executor));

    // Zero-padded flush exercises the overlap/tail drain path.
    const size_t extraZero = setup.irLen + setup.tailBlock;
    feedChunks(convolver, input, output, false, extraZero, 0);
    TASK_CHECK(checkAgainstReference(ir, input, output, extraZero, "pcm_local_all_rejected"));
    TASK_CHECK(executor.rejects() > 0 || setup.irLen <= 2 * setup.tailBlock);
    convolver.reset();
  }
  return 0;
}

static int pcm_inline_worker()
{
  const Setup setup = {16, 128, 1500};
  std::vector<float> ir, input, output;
  makeIr(ir, setup.irLen, 2);
  makeInput(input, 12 * setup.tailBlock + 91, 7);

  ControlledExecutor executor;
  executor.setPolicy(ControlledExecutor::POLICY_INLINE);
  fftconvolver::AsyncFFTConvolver convolver;
  TASK_CHECK(convolver.init(setup.headBlock, setup.tailBlock,
                            ir.data(), ir.size(), &executor));
  feedChunks(convolver, input, output, false, setup.irLen, 3);
  TASK_CHECK(checkAgainstReference(ir, input, output, setup.irLen, "pcm_inline_worker"));
  TASK_CHECK(executor.inlineRuns() > 0);
  convolver.reset();
  return 0;
}

static int pcm_queued_worker_chunks()
{
  const Setup setup = {32, 256, 3000};
  std::vector<float> ir, input, output;
  makeIr(ir, setup.irLen, 4);
  makeInput(input, 16 * setup.tailBlock + 203, 9);

  ControlledExecutor executor;
  fftconvolver::AsyncFFTConvolver convolver;
  TASK_CHECK(convolver.init(setup.headBlock, setup.tailBlock,
                            ir.data(), ir.size(), &executor));
  // Chunk pattern deliberately includes 1, 7, head and tailBlock+3 sizes.
  feedChunks(convolver, input, output, false, setup.irLen, 1);
  TASK_CHECK(checkAgainstReference(ir, input, output, setup.irLen, "pcm_queued_worker_chunks"));
  TASK_CHECK(executor.workerRuns() > 0);
  TASK_CHECK(executor.duplicateSubmissions.load() == 0);
  convolver.reset();
  TASK_CHECK(executor.quiesceCalls.load() >= 1);
  return 0;
}

static int pcm_fully_in_place()
{
  const Setup setup = {16, 128, 1200};
  std::vector<float> ir, input, output;
  makeIr(ir, setup.irLen, 6);
  makeInput(input, 10 * setup.tailBlock + 55, 13);

  ControlledExecutor executor;
  fftconvolver::AsyncFFTConvolver convolver;
  TASK_CHECK(convolver.init(setup.headBlock, setup.tailBlock,
                            ir.data(), ir.size(), &executor));
  feedChunks(convolver, input, output, true, setup.irLen, 2);
  TASK_CHECK(checkAgainstReference(ir, input, output, setup.irLen, "pcm_fully_in_place"));
  convolver.reset();
  return 0;
}

static int pcm_schedule_independent()
{
  const Setup setup = {16, 64, 300};
  std::vector<float> ir, input;
  makeIr(ir, setup.irLen, 8);
  makeInput(input, 14 * setup.tailBlock + 33, 21);

  std::vector<float> outReject, outInline, outHeld;

  {
    ControlledExecutor executor;
    executor.setPolicy(ControlledExecutor::POLICY_REJECT);
    fftconvolver::AsyncFFTConvolver convolver;
    TASK_CHECK(convolver.init(setup.headBlock, setup.tailBlock,
                              ir.data(), ir.size(), &executor));
    feedChunks(convolver, input, outReject, false, 0, 0);
    convolver.reset();
  }
  {
    ControlledExecutor executor;
    executor.setPolicy(ControlledExecutor::POLICY_INLINE);
    fftconvolver::AsyncFFTConvolver convolver;
    TASK_CHECK(convolver.init(setup.headBlock, setup.tailBlock,
                              ir.data(), ir.size(), &executor));
    feedChunks(convolver, input, outInline, false, 0, 0);
    convolver.reset();
  }
  {
    ControlledExecutor executor;
    executor.setHold(true);
    fftconvolver::AsyncFFTConvolver convolver;
    TASK_CHECK(convolver.init(setup.headBlock, setup.tailBlock,
                              ir.data(), ir.size(), &executor));
    feedChunks(convolver, input, outHeld, false, 0, 0); // All local fallbacks
    executor.releaseAndJoin();
    convolver.reset();
  }

  TASK_CHECK(outReject.size() == outInline.size());
  TASK_CHECK(outReject.size() == outHeld.size());
  for (size_t i = 0; i < outReject.size(); ++i)
  {
    TASK_CHECK(outReject[i] == outInline[i]);
    TASK_CHECK(outReject[i] == outHeld[i]);
  }
  TASK_CHECK(checkAgainstReference(ir, input, outReject, 0, "pcm_schedule_independent"));
  return 0;
}

// ----------------------------------------------------------------------------------
// Races group: real worker threads, explicit gates and deadlines
// ----------------------------------------------------------------------------------

static int races_reject_then_accept()
{
  const Setup setup = {16, 128, 900};
  std::vector<float> ir, input, output;
  makeIr(ir, setup.irLen, 3);
  makeInput(input, 10 * setup.tailBlock + 47, 17);

  ControlledExecutor executor;
  fftconvolver::AsyncFFTConvolver convolver;
  TASK_CHECK(convolver.init(setup.headBlock, setup.tailBlock,
                            ir.data(), ir.size(), &executor));

  executor.setPolicy(ControlledExecutor::POLICY_REJECT);
  std::vector<float> first(input.size() / 2);
  std::memcpy(first.data(), input.data(), first.size() * sizeof(float));
  std::vector<float> firstOut;
  feedChunks(convolver, first, firstOut, false, 0, 4);
  TASK_CHECK(executor.rejects() > 0);

  executor.setPolicy(ControlledExecutor::POLICY_QUEUE);
  std::vector<float> rest(input.begin() + static_cast<long>(first.size()), input.end());
  std::vector<float> restOut;
  feedChunks(convolver, rest, restOut, false, setup.irLen, 2);

  output = firstOut;
  output.insert(output.end(), restOut.begin(), restOut.end());
  TASK_CHECK(checkAgainstReference(ir, input, output, setup.irLen, "races_reject_then_accept"));
  convolver.reset();
  return 0;
}

static int races_delayed_start_past_deadline()
{
  const Setup setup = {16, 64, 300}; // Ring has 4 spectra; two wraps need 8 blocks
  std::vector<float> ir, input, output;
  makeIr(ir, setup.irLen, 5);
  makeInput(input, 20 * setup.tailBlock + 29, 19);

  ControlledExecutor executor;
  fftconvolver::AsyncFFTConvolver convolver;
  TASK_CHECK(convolver.init(setup.headBlock, setup.tailBlock,
                            ir.data(), ir.size(), &executor));

  // Close the gate before processing: the first worker picks the job up and
  // then holds it, so the slot stays BUSY well past its output deadline and
  // through more than two frequency-domain ring wrap-arounds.
  executor.setHold(true);
  const size_t heldSamples = 12 * setup.tailBlock; // 12 blocks > 2 ring cycles
  std::vector<float> heldPart(input.begin(), input.begin() + heldSamples);
  std::vector<float> heldOut;
  feedChunks(convolver, heldPart, heldOut, false, 0, 1);
  executor.waitUntilTaken();
  TASK_CHECK(executor.pending() == 1);

  // Let the tardy worker finish; its result must be discarded, not overlap-added.
  executor.releaseAndJoin();
  TASK_CHECK(executor.workerRuns() == 1);

  std::vector<float> rest(input.begin() + static_cast<long>(heldSamples), input.end());
  std::vector<float> restOut;
  feedChunks(convolver, rest, restOut, false, setup.irLen, 0);

  output = heldOut;
  output.insert(output.end(), restOut.begin(), restOut.end());
  TASK_CHECK(checkAgainstReference(ir, input, output, setup.irLen, "races_delayed_start_past_deadline"));
  TASK_CHECK(executor.duplicateSubmissions.load() == 0);
  convolver.reset();
  return 0;
}

static int races_complete_after_fallback()
{
  const Setup setup = {32, 256, 2500};
  std::vector<float> ir, input, output;
  makeIr(ir, setup.irLen, 9);
  makeInput(input, 14 * setup.tailBlock + 101, 23);

  ControlledExecutor executor;
  fftconvolver::AsyncFFTConvolver convolver;
  TASK_CHECK(convolver.init(setup.headBlock, setup.tailBlock,
                            ir.data(), ir.size(), &executor));

  // The first job is held across its deadline (2 tail blocks), so the audio
  // thread computes it locally. Only afterwards is the worker released.
  executor.setHold(true);
  const size_t heldSamples = 5 * setup.tailBlock;
  std::vector<float> heldPart(input.begin(), input.begin() + heldSamples);
  std::vector<float> heldOut;
  feedChunks(convolver, heldPart, heldOut, false, 0, 2);
  executor.waitUntilTaken();
  executor.releaseAndJoin();
  TASK_CHECK(executor.workerRuns() == 1);

  std::vector<float> rest(input.begin() + static_cast<long>(heldSamples), input.end());
  std::vector<float> restOut;
  feedChunks(convolver, rest, restOut, false, setup.irLen, 3);

  output = heldOut;
  output.insert(output.end(), restOut.begin(), restOut.end());
  TASK_CHECK(checkAgainstReference(ir, input, output, setup.irLen, "races_complete_after_fallback"));
  convolver.reset();
  return 0;
}

static int races_consecutive_local_generations()
{
  const Setup setup = {8, 64, 400};
  std::vector<float> ir, input, output;
  makeIr(ir, setup.irLen, 10);
  makeInput(input, 24 * setup.tailBlock + 17, 27);

  ControlledExecutor executor;
  fftconvolver::AsyncFFTConvolver convolver;
  TASK_CHECK(convolver.init(setup.headBlock, setup.tailBlock,
                            ir.data(), ir.size(), &executor));

  // One worker stays stuck on an old generation; every following generation
  // must be completed locally without overwriting the spectra it is reading.
  executor.setHold(true);
  feedChunks(convolver, input, output, false, 0, 5);
  executor.waitUntilTaken();
  TASK_CHECK(executor.pending() == 1);
  TASK_CHECK(executor.accepts() >= 1);
  executor.releaseAndJoin();

  TASK_CHECK(checkAgainstReference(ir, input, output, 0, "races_consecutive_local_generations"));
  convolver.reset();
  return 0;
}

static int races_no_heap_no_wait_in_process()
{
  const Setup setup = {16, 128, 1400};
  std::vector<float> ir, input;
  makeIr(ir, setup.irLen, 12);
  makeInput(input, 12 * setup.tailBlock + 63, 29);

  // Preallocated destination: nothing in the measured window may allocate.
  std::vector<float> output(input.size(), 0.0f);
  static const size_t kPatterns[] = {1, 7, 3, 16, 64, 257};

  ControlledExecutor rejectExec;
  rejectExec.setPolicy(ControlledExecutor::POLICY_REJECT);
  fftconvolver::AsyncFFTConvolver convolver;
  TASK_CHECK(convolver.init(setup.headBlock, setup.tailBlock,
                            ir.data(), ir.size(), &rejectExec));

  g_heapHit.store(0, std::memory_order_relaxed);
  g_heapBan.fetch_add(1, std::memory_order_relaxed);
  const unsigned quiesceBefore = rejectExec.quiesceCalls.load();
  {
    size_t pos = 0;
    unsigned step = 0;
    while (pos < input.size())
    {
      const size_t chunk = std::min(kPatterns[(step++) % 6], input.size() - pos);
      convolver.process(input.data() + pos, output.data() + pos, chunk);
      pos += chunk;
    }
  }
  const unsigned quiesceAfter = rejectExec.quiesceCalls.load();
  g_heapBan.fetch_sub(1, std::memory_order_relaxed);

  TASK_CHECK(g_heapHit.load() == 0);
  TASK_CHECK(quiesceBefore == quiesceAfter); // process() must never quiesce/wait
  TASK_CHECK(rejectExec.wouldBlockSubmits.load() == 0);
  TASK_CHECK(rejectExec.duplicateSubmissions.load() == 0);
  TASK_CHECK(checkAgainstReference(ir, input, output, 0, "races_no_heap_no_wait_in_process"));
  convolver.reset();

  // Same guarantees with synchronous (inline) background execution.
  ControlledExecutor inlineExec;
  inlineExec.setPolicy(ControlledExecutor::POLICY_INLINE);
  TASK_CHECK(convolver.init(setup.headBlock, setup.tailBlock,
                            ir.data(), ir.size(), &inlineExec));
  std::fill(output.begin(), output.end(), 0.0f);
  g_heapHit.store(0, std::memory_order_relaxed);
  g_heapBan.fetch_add(1, std::memory_order_relaxed);
  {
    size_t pos = 0;
    unsigned step = 0;
    while (pos < input.size())
    {
      const size_t chunk = std::min(kPatterns[(step++) % 6], input.size() - pos);
      convolver.process(input.data() + pos, output.data() + pos, chunk);
      pos += chunk;
    }
  }
  g_heapBan.fetch_sub(1, std::memory_order_relaxed);
  TASK_CHECK(g_heapHit.load() == 0);
  TASK_CHECK(inlineExec.duplicateSubmissions.load() == 0);
  TASK_CHECK(checkAgainstReference(ir, input, output, 0, "races_no_heap_inline"));
  convolver.reset();
  return 0;
}

// ----------------------------------------------------------------------------------
// Lifetime group: reset/reconfigure/destruction while workers are active
// ----------------------------------------------------------------------------------

static int lifetime_reset_pending_then_reinit()
{
  const Setup setup1 = {16, 64, 300};
  const Setup setup2 = {64, 1024, 5000};
  std::vector<float> ir1, ir2, input, output;
  makeIr(ir1, setup1.irLen, 31);
  makeIr(ir2, setup2.irLen, 32);
  makeInput(input, 16 * setup2.tailBlock + 149, 33);

  ControlledExecutor executor;
  fftconvolver::AsyncFFTConvolver convolver;
  TASK_CHECK(convolver.init(setup1.headBlock, setup1.tailBlock,
                            ir1.data(), ir1.size(), &executor));

  executor.setHold(true);
  std::vector<float> first(input.begin(), input.begin() + 4 * setup1.tailBlock);
  std::vector<float> firstOut;
  feedChunks(convolver, first, firstOut, false, 0, 0);
  executor.waitUntilTaken();
  TASK_CHECK(executor.pending() == 1);

  // reset() must join the in-flight worker before any job memory is reused.
  convolver.reset();
  TASK_CHECK(executor.pending() == 0);
  TASK_CHECK(executor.quiesceCalls.load() >= 1);

  // Reconfigure with different block sizes and IR while the same executor
  // remains valid; history starts from zero again.
  TASK_CHECK(convolver.init(setup2.headBlock, setup2.tailBlock,
                            ir2.data(), ir2.size(), &executor));
  feedChunks(convolver, input, output, false, setup2.irLen, 2);
  TASK_CHECK(checkAgainstReference(ir2, input, output, setup2.irLen, "lifetime_reset_pending_then_reinit"));
  convolver.reset();
  return 0;
}

static int lifetime_destruction_with_pending_worker()
{
  const Setup setup = {16, 128, 1000};
  std::vector<float> ir, input, output;
  makeIr(ir, setup.irLen, 41);
  makeInput(input, 6 * setup.tailBlock, 42);

  ControlledExecutor executor;
  {
    fftconvolver::AsyncFFTConvolver convolver;
    TASK_CHECK(convolver.init(setup.headBlock, setup.tailBlock,
                              ir.data(), ir.size(), &executor));
    executor.setHold(true);
    feedChunks(convolver, input, output, false, 0, 1);
    executor.waitUntilTaken();
    TASK_CHECK(executor.pending() == 1);
    // Destruction here must quiesce the worker instead of freeing live memory.
  }
  TASK_CHECK(executor.pending() == 0);
  TASK_CHECK(executor.quiesceCalls.load() >= 1);
  return 0;
}

static int lifetime_invalid_params_keep_config()
{
  const Setup setup = {16, 128, 900};
  std::vector<float> ir, input, goodOutput;
  makeIr(ir, setup.irLen, 51);
  makeInput(input, 8 * setup.tailBlock + 39, 52);

  ControlledExecutor executor;
  fftconvolver::AsyncFFTConvolver convolver;
  TASK_CHECK(convolver.init(setup.headBlock, setup.tailBlock,
                            ir.data(), ir.size(), &executor));

  const size_t badB[] = {0, 4, 7, 12, 512};
  for (size_t b : badB)
  {
    TASK_CHECK(!convolver.init(b, setup.tailBlock, ir.data(), ir.size(), &executor));
  }
  const size_t badT[] = {0, 32, 48, 8192};
  for (size_t t : badT)
  {
    TASK_CHECK(!convolver.init(setup.headBlock, t, ir.data(), ir.size(), &executor));
  }
  TASK_CHECK(!convolver.init(64, 128, ir.data(), ir.size(), &executor)); // T < 4B
  TASK_CHECK(!convolver.init(setup.headBlock, setup.tailBlock, ir.data(), 65537, &executor));

  std::vector<float> badIr = ir;
  badIr[10] = std::numeric_limits<float>::infinity();
  TASK_CHECK(!convolver.init(setup.headBlock, setup.tailBlock,
                             badIr.data(), badIr.size(), &executor));
  badIr[10] = std::numeric_limits<float>::quiet_NaN();
  TASK_CHECK(!convolver.init(setup.headBlock, setup.tailBlock,
                             badIr.data(), badIr.size(), &executor));

  // The original configuration must still produce the correct sound.
  feedChunks(convolver, input, goodOutput, false, 0, 0);
  TASK_CHECK(checkAgainstReference(ir, input, goodOutput, 0, "lifetime_invalid_params_keep_config"));
  convolver.reset();
  return 0;
}

static int lifetime_empty_ir_reset_zero_and_null_calls()
{
  ControlledExecutor executor;
  fftconvolver::AsyncFFTConvolver convolver;

  // Empty IR is legal and produces silence.
  TASK_CHECK(convolver.init(16, 128, 0, 0, &executor));
  std::vector<float> input(500, 0.7f), output(500, 1.0f);
  convolver.process(input.data(), output.data(), input.size());
  for (float v : output)
  {
    TASK_CHECK(v == 0.0f);
  }

  // len == 0 with null pointers has no side effects.
  convolver.process(nullptr, nullptr, 0);
  convolver.process(nullptr, nullptr, 0);

  // After reset() every process() call emits zeros.
  convolver.reset();
  std::fill(output.begin(), output.end(), 1.0f);
  convolver.process(nullptr, output.data(), output.size());
  for (float v : output)
  {
    TASK_CHECK(v == 0.0f);
  }
  return 0;
}

static int lifetime_null_executor_and_executor_swap()
{
  const Setup setup = {32, 256, 2600};
  std::vector<float> ir, input, output;
  makeIr(ir, setup.irLen, 61);
  makeInput(input, 12 * setup.tailBlock + 83, 62);

  // A null executor means fully local processing.
  fftconvolver::AsyncFFTConvolver convolver;
  TASK_CHECK(convolver.init(setup.headBlock, setup.tailBlock,
                            ir.data(), ir.size(), nullptr));
  feedChunks(convolver, input, output, true, setup.irLen, 4);
  TASK_CHECK(checkAgainstReference(ir, input, output, setup.irLen, "lifetime_null_executor_and_executor_swap"));
  convolver.reset();

  // Switch from a threaded executor to a null one across reinitialization.
  ControlledExecutor executor;
  TASK_CHECK(convolver.init(setup.headBlock, setup.tailBlock,
                            ir.data(), ir.size(), &executor));
  std::vector<float> partial;
  std::vector<float> partialIn(input.begin(), input.begin() + 3 * setup.tailBlock);
  feedChunks(convolver, partialIn, partial, false, 0, 0);
  convolver.reset();
  TASK_CHECK(executor.quiesceCalls.load() >= 1);

  TASK_CHECK(convolver.init(setup.headBlock, setup.tailBlock,
                            ir.data(), ir.size(), nullptr));
  feedChunks(convolver, input, output, false, setup.irLen, 1);
  TASK_CHECK(checkAgainstReference(ir, input, output, setup.irLen, "lifetime_executor_swap_local"));
  convolver.reset();
  return 0;
}

// ----------------------------------------------------------------------------------
// Registration (the C driver is compiled with the C++ compiler, so the symbols
// use ordinary C++ linkage as declared in task_support/check.h)
// ----------------------------------------------------------------------------------

const struct task_case task_cases[] = {
  {"pcm", "local_all_rejected", pcm_local_all_rejected},
  {"pcm", "inline_worker", pcm_inline_worker},
  {"pcm", "queued_worker_chunks", pcm_queued_worker_chunks},
  {"pcm", "fully_in_place", pcm_fully_in_place},
  {"pcm", "schedule_independent", pcm_schedule_independent},

  {"races", "reject_then_accept", races_reject_then_accept},
  {"races", "delayed_start_past_deadline", races_delayed_start_past_deadline},
  {"races", "complete_after_fallback", races_complete_after_fallback},
  {"races", "consecutive_local_generations", races_consecutive_local_generations},
  {"races", "no_heap_no_wait_in_process", races_no_heap_no_wait_in_process},

  {"lifetime", "reset_pending_then_reinit", lifetime_reset_pending_then_reinit},
  {"lifetime", "destruction_with_pending_worker", lifetime_destruction_with_pending_worker},
  {"lifetime", "invalid_params_keep_config", lifetime_invalid_params_keep_config},
  {"lifetime", "empty_ir_reset_zero_and_null_calls", lifetime_empty_ir_reset_zero_and_null_calls},
  {"lifetime", "null_executor_and_executor_swap", lifetime_null_executor_and_executor_swap}
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);

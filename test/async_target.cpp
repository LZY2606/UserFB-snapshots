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

#include "task_support/check.h"

#include "AsyncFFTConvolver.h"

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <new>
#include <random>
#include <thread>
#include <vector>

// ----------------------------------------------------------------------------------
// Global heap accounting: process() must not allocate or free anything.
// ----------------------------------------------------------------------------------

static std::atomic<unsigned long> g_allocs(0);
static std::atomic<unsigned long> g_deallocs(0);

void* operator new(std::size_t size)
{
  g_allocs.fetch_add(1, std::memory_order_relaxed);
  void* p = std::malloc(size);
  if (!p) throw std::bad_alloc();
  return p;
}

void* operator new[](std::size_t size)
{
  g_allocs.fetch_add(1, std::memory_order_relaxed);
  void* p = std::malloc(size);
  if (!p) throw std::bad_alloc();
  return p;
}

void operator delete(void* p) noexcept
{
  if (p) g_deallocs.fetch_add(1, std::memory_order_relaxed);
  std::free(p);
}

void operator delete[](void* p) noexcept
{
  if (p) g_deallocs.fetch_add(1, std::memory_order_relaxed);
  std::free(p);
}

void operator delete(void* p, std::size_t) noexcept
{
  if (p) g_deallocs.fetch_add(1, std::memory_order_relaxed);
  std::free(p);
}

void operator delete[](void* p, std::size_t) noexcept
{
  if (p) g_deallocs.fetch_add(1, std::memory_order_relaxed);
  std::free(p);
}

namespace
{

// ----------------------------------------------------------------------------------
// Deterministic, condition-variable driven executor with a real worker thread.
// No sleeps are used anywhere: every hand-off is synchronized explicitly.
// ----------------------------------------------------------------------------------

class ScriptedExecutor : public fftconvolver::TailExecutor
{
public:
  enum Mode
  {
    REJECT, // Every submit is refused (computed locally)
    SYNC,   // Every accepted callback runs inside submit()
    DEFER   // Accepted callbacks run on the worker, blocked behind a gate
  };

  ScriptedExecutor()
  {
    // Start the worker only after every flag it reads has been initialized.
    _worker = std::thread(&ScriptedExecutor::workerLoop, this);
  }

  ~ScriptedExecutor() override
  {
    {
      std::lock_guard<std::mutex> lock(_mutex);
      _stopped = true;
    }
    _cond.notify_all();
    _worker.join();
  }

  void setMode(Mode mode)
  {
    std::lock_guard<std::mutex> lock(_mutex);
    _mode = mode;
  }

  bool submit(void (*run)(void*), void* arg) override
  {
    ++_submits;
    Mode mode;
    {
      std::lock_guard<std::mutex> lock(_mutex);
      mode = _mode;
    }

    if (mode == REJECT)
    {
      ++_rejected;
      return false;
    }

    ++_accepted;
    if (mode == SYNC)
    {
      run(arg); // Executed exactly once, before submit returns
      return true;
    }

    {
      std::lock_guard<std::mutex> lock(_mutex);
      _queue.push_back(Job(run, arg));
    }
    _cond.notify_one();
    return true;
  }

  void quiesce() override
  {
    ++_quiesces;
    std::unique_lock<std::mutex> lock(_mutex);
    _doneCond.wait(lock, [this] {
      return _queue.empty() && _active == 0;
    });
  }

  // Blocks until the worker has picked up a deferred job and reached the gate.
  void waitWorkerArrived()
  {
    std::unique_lock<std::mutex> lock(_mutex);
    _arriveCond.wait(lock, [this] { return _arrived > 0; });
    --_arrived;
  }

  void closeGate()
  {
    std::lock_guard<std::mutex> lock(_mutex);
    _gateOpen = false;
  }

  void openGate()
  {
    {
      std::lock_guard<std::mutex> lock(_mutex);
      _gateOpen = true;
    }
    _gateCond.notify_all();
  }

  unsigned long submits() const { return _submits.load(); }
  unsigned long accepted() const { return _accepted.load(); }
  unsigned long rejected() const { return _rejected.load(); }
  unsigned long quiesces() const { return _quiesces.load(); }

private:
  struct Job
  {
    void (*run)(void*);
    void* arg;
    Job() : run(nullptr), arg(nullptr) {}
    Job(void (*r)(void*), void* a) : run(r), arg(a) {}
  };

  void workerLoop()
  {
    for (;;)
    {
      Job job;
      {
        std::unique_lock<std::mutex> lock(_mutex);
        _cond.wait(lock, [this] {
          return _stopped || !_queue.empty();
        });
        if (_stopped && _queue.empty())
        {
          return;
        }
        job = _queue.front();
        _queue.pop_front();
        ++_active;

        if (!_gateOpen)
        {
          ++_arrived;
          _arriveCond.notify_one();
          _gateCond.wait(lock, [this] { return _gateOpen; });
        }
      }

      job.run(job.arg);

      {
        std::lock_guard<std::mutex> lock(_mutex);
        --_active;
        if (_queue.empty() && _active == 0)
        {
          _doneCond.notify_all();
        }
      }
    }
  }

  std::mutex _mutex;
  std::condition_variable _cond;
  std::condition_variable _doneCond;
  std::condition_variable _gateCond;
  std::condition_variable _arriveCond;
  std::deque<Job> _queue;
  Mode _mode = REJECT;
  std::thread _worker;
  bool _stopped = false;
  unsigned int _active = 0;
  std::atomic<unsigned long> _submits{0};
  std::atomic<unsigned long> _accepted{0};
  std::atomic<unsigned long> _rejected{0};
  std::atomic<unsigned long> _quiesces{0};
  bool _gateOpen = true;
  unsigned int _arrived = 0;
};


// ----------------------------------------------------------------------------------
// Reference: independent double precision time-domain summation.
// ----------------------------------------------------------------------------------

std::vector<float> makeIR(std::mt19937& rng, size_t len, float scale)
{
  std::uniform_real_distribution<float> dist(-1.f, 1.f);
  std::vector<float> ir(len);
  for (size_t i = 0; i < len; ++i)
  {
    ir[i] = dist(rng) * scale;
  }
  if (len > 0)
  {
    ir[0] = 0.6f; // Keep a strong direct component
  }
  return ir;
}

std::vector<float> makeInput(std::mt19937& rng, size_t len)
{
  std::uniform_real_distribution<float> dist(-0.9f, 0.9f);
  std::vector<float> x(len);
  for (size_t i = 0; i < len; ++i)
  {
    x[i] = dist(rng);
  }
  return x;
}

void referenceConvolution(const std::vector<float>& ir,
                          const std::vector<float>& x,
                          std::vector<double>& ref)
{
  ref.assign(x.size(), 0.0);
  for (size_t n = 0; n < x.size(); ++n)
  {
    double acc = 0.0;
    const size_t kMax = std::min(ir.size(), n + 1);
    for (size_t k = 0; k < kMax; ++k)
    {
      acc += static_cast<double>(ir[k]) * static_cast<double>(x[n - k]);
    }
    ref[n] = acc;
  }
}

double toleranceFor(const std::vector<float>& ir)
{
  double sumAbs = 0.0;
  for (float v : ir) sumAbs += std::fabs(static_cast<double>(v));
  return 2e-5 * (1.0 + sumAbs);
}

// Feeds the convolver in arbitrary chunks and returns the produced PCM.
std::vector<float> feedChunks(fftconvolver::AsyncFFTConvolver& conv,
                              const std::vector<float>& x,
                              const std::vector<size_t>& chunks,
                              bool inPlace)
{
  std::vector<float> buf = x;
  std::vector<float> out(x.size(), 0.f);
  size_t pos = 0;
  size_t ci = 0;
  while (pos < x.size())
  {
    const size_t n = std::min(chunks[ci++ % chunks.size()], x.size() - pos);
    if (inPlace)
    {
      conv.process(buf.data() + pos, buf.data() + pos, n);
    }
    else
    {
      conv.process(buf.data() + pos, out.data() + pos, n);
    }
    pos += n;
  }
  return inPlace ? buf : out;
}

bool checkPCM(const std::vector<float>& got,
              const std::vector<float>& ir,
              const std::vector<float>& x,
              const char* label)
{
  std::vector<double> ref;
  referenceConvolution(ir, x, ref);
  const double tol = toleranceFor(ir);
  double maxErr = 0.0;
  for (size_t i = 0; i < got.size(); ++i)
  {
    if (!std::isfinite(got[i]))
    {
      std::fprintf(stderr, "%s: non-finite sample at %zu\n", label, i);
      return false;
    }
    maxErr = std::max(maxErr, std::fabs(static_cast<double>(got[i]) - ref[i]));
  }
  if (maxErr > tol)
  {
    std::fprintf(stderr, "%s: maxErr=%.9g tol=%.9g\n", label, maxErr, tol);
    return false;
  }
  return true;
}

// ----------------------------------------------------------------------------------
// PCM group
// ----------------------------------------------------------------------------------

int pcm_reject_chunks()
{
  std::mt19937 rng(1001);
  const size_t B = 16, T = 64;
  std::vector<float> ir = makeIR(rng, 513, 0.2f);
  std::vector<float> x = makeInput(rng, 4096);
  const std::vector<size_t> chunks = {1, 7, 3, 16, 67, 128, 255};

  ScriptedExecutor ex;
  ex.setMode(ScriptedExecutor::REJECT);
  fftconvolver::AsyncFFTConvolver conv;
  TASK_CHECK(conv.init(B, T, ir.data(), ir.size(), &ex));
  std::vector<float> got = feedChunks(conv, x, chunks, false);
  TASK_CHECK(checkPCM(got, ir, x, "pcm_reject_chunks"));
  TASK_CHECK(ex.rejected() > 0);
  TASK_CHECK(ex.accepted() == 0);
  return 0;
}

int pcm_sync_chunks()
{
  std::mt19937 rng(1002);
  const size_t B = 32, T = 128;
  std::vector<float> ir = makeIR(rng, 1500, 0.15f);
  std::vector<float> x = makeInput(rng, 6000);
  const std::vector<size_t> chunks = {1, 7, B, T + 3, 19};

  ScriptedExecutor ex;
  ex.setMode(ScriptedExecutor::SYNC);
  fftconvolver::AsyncFFTConvolver conv;
  TASK_CHECK(conv.init(B, T, ir.data(), ir.size(), &ex));
  std::vector<float> got = feedChunks(conv, x, chunks, false);
  TASK_CHECK(checkPCM(got, ir, x, "pcm_sync_chunks"));
  TASK_CHECK(ex.accepted() > 0);
  return 0;
}

int pcm_inplace()
{
  std::mt19937 rng(1003);
  const size_t B = 8, T = 64;
  std::vector<float> ir = makeIR(rng, 700, 0.2f);
  std::vector<float> x = makeInput(rng, 3000);
  const std::vector<size_t> chunks = {1, 7, 5, 13};

  ScriptedExecutor ex;
  ex.setMode(ScriptedExecutor::REJECT);
  fftconvolver::AsyncFFTConvolver conv;
  TASK_CHECK(conv.init(B, T, ir.data(), ir.size(), &ex));
  std::vector<float> got = feedChunks(conv, x, chunks, true);
  TASK_CHECK(checkPCM(got, ir, x, "pcm_inplace"));
  return 0;
}

int pcm_short_and_empty_ir()
{
  std::mt19937 rng(1004);
  std::vector<float> x = makeInput(rng, 2000);
  const std::vector<size_t> chunks = {1, 7, 16};

  // IR shorter than one head block: no tail partitions at all
  std::vector<float> shortIr(3);
  shortIr[0] = 0.5f;
  shortIr[1] = -0.25f;
  shortIr[2] = 0.125f;
  {
    ScriptedExecutor ex;
    fftconvolver::AsyncFFTConvolver conv;
    TASK_CHECK(conv.init(16, 64, shortIr.data(), shortIr.size(), &ex));
    std::vector<float> got = feedChunks(conv, x, chunks, false);
    TASK_CHECK(checkPCM(got, shortIr, x, "pcm_short_ir"));
    TASK_CHECK(ex.submits() == 0);
  }

  // Empty IR: legal, output all zero, no jobs
  {
    ScriptedExecutor ex;
    fftconvolver::AsyncFFTConvolver conv;
    TASK_CHECK(conv.init(16, 64, nullptr, 0, &ex));
    std::vector<float> got = feedChunks(conv, x, chunks, false);
    for (float v : got) TASK_CHECK(v == 0.f);
    TASK_CHECK(ex.submits() == 0);
  }
  return 0;
}

int pcm_zero_len_and_tail_drain()
{
  std::mt19937 rng(1005);
  const size_t B = 16, T = 64;
  std::vector<float> ir = makeIR(rng, 400, 0.2f);
  std::vector<float> x = makeInput(rng, 1024);

  ScriptedExecutor ex;
  ex.setMode(ScriptedExecutor::REJECT);
  fftconvolver::AsyncFFTConvolver conv;
  TASK_CHECK(conv.init(B, T, ir.data(), ir.size(), &ex));

  // len == 0 with null pointers must have no side effects
  conv.process(nullptr, nullptr, 0);

  // Signal followed by silence: the long tail must drain out correctly
  std::vector<float> extended = x;
  extended.resize(2048, 0.f);
  const std::vector<size_t> chunks = {B, T + 3, 7, 1};
  std::vector<float> got = feedChunks(conv, extended, chunks, false);
  TASK_CHECK(checkPCM(got, ir, extended, "pcm_tail_drain"));

  // Energy must still be present well after the signal stopped
  double lateEnergy = 0.0;
  for (size_t i = x.size(); i < extended.size(); ++i)
  {
    lateEnergy += static_cast<double>(got[i]) * got[i];
  }
  TASK_CHECK(lateEnergy > 1e-9);
  return 0;
}

int pcm_ring_wraparound()
{
  std::mt19937 rng(1006);
  const size_t B = 8, T = 64;
  // P = 3 partitions => ring size 5; use enough blocks to wrap > 4 times
  std::vector<float> ir = makeIR(rng, 2 * T + 3 * T - 1, 0.2f);
  std::vector<float> x = makeInput(rng, 40 * T);
  const std::vector<size_t> chunks = {1, 7, B, T + 3};

  ScriptedExecutor ex;
  ex.setMode(ScriptedExecutor::REJECT);
  fftconvolver::AsyncFFTConvolver conv;
  TASK_CHECK(conv.init(B, T, ir.data(), ir.size(), &ex));
  std::vector<float> got = feedChunks(conv, x, chunks, false);
  TASK_CHECK(checkPCM(got, ir, x, "pcm_ring_wraparound"));
  return 0;
}

// ----------------------------------------------------------------------------------
// Races group
// ----------------------------------------------------------------------------------

int races_deferred_across_deadline()
{
  std::mt19937 rng(2001);
  const size_t B = 16, T = 64;
  std::vector<float> ir = makeIR(rng, 900, 0.2f);
  std::vector<float> x = makeInput(rng, 4096);
  const std::vector<size_t> chunks = {1, 7, B, T + 3};

  ScriptedExecutor ex;
  fftconvolver::AsyncFFTConvolver conv;
  TASK_CHECK(conv.init(B, T, ir.data(), ir.size(), &ex));

  // First job is accepted but the worker does not start until we allow it.
  ex.setMode(ScriptedExecutor::DEFER);
  ex.closeGate();

  std::vector<float> out(x.size(), 0.f);
  size_t pos = 0, ci = 0;
  // Feed exactly the first tail block worth of samples.
  while (pos < T)
  {
    const size_t n = std::min(chunks[ci++ % chunks.size()], T - pos);
    conv.process(x.data() + pos, out.data() + pos, n);
    pos += n;
  }
  ex.waitWorkerArrived(); // Worker holds the job (and its private spectra)

  // Cross the deadline at 2*T with the result still unpublished: the audio
  // thread must fall back to local computation, never stall or skip audio.
  while (pos < 3 * T)
  {
    const size_t n = std::min(chunks[ci++ % chunks.size()], 3 * T - pos);
    conv.process(x.data() + pos, out.data() + pos, n);
    pos += n;
  }

  // Now let the late worker finish: its published result must be discarded,
  // never advancing overlap a second time.
  ex.openGate();
  ex.quiesce();

  // Remaining audio with normal synchronous background processing
  ex.setMode(ScriptedExecutor::SYNC);
  while (pos < x.size())
  {
    const size_t n = std::min(chunks[ci++ % chunks.size()], x.size() - pos);
    conv.process(x.data() + pos, out.data() + pos, n);
    pos += n;
  }

  TASK_CHECK(checkPCM(out, ir, x, "races_deferred_across_deadline"));
  TASK_CHECK(ex.accepted() >= 1);
  return 0;
}

int races_long_hold_multi_wrap()
{
  std::mt19937 rng(2002);
  const size_t B = 8, T = 64;
  // Small ring (P=3 => 5 slots): the held worker survives many wraparounds.
  std::vector<float> ir = makeIR(rng, 2 * T + 3 * T - 1, 0.2f);
  const size_t totalBlocks = 14; // > 2 full ring wraps while the slot is held
  std::vector<float> x = makeInput(rng, totalBlocks * T);
  const std::vector<size_t> chunks = {1, 7, B};

  ScriptedExecutor ex;
  fftconvolver::AsyncFFTConvolver conv;
  TASK_CHECK(conv.init(B, T, ir.data(), ir.size(), &ex));

  ex.setMode(ScriptedExecutor::DEFER);
  ex.closeGate();

  std::vector<float> out(x.size(), 0.f);
  size_t pos = 0, ci = 0;
  while (pos < T)
  {
    const size_t n = std::min(chunks[ci++ % chunks.size()], T - pos);
    conv.process(x.data() + pos, out.data() + pos, n);
    pos += n;
  }
  ex.waitWorkerArrived(); // One worker keeps the old spectra indefinitely

  // All subsequent generations are computed locally while the slot is busy;
  // the frequency-domain ring wraps more than twice under the held worker.
  while (pos < x.size())
  {
    const size_t n = std::min(chunks[ci++ % chunks.size()], x.size() - pos);
    conv.process(x.data() + pos, out.data() + pos, n);
    pos += n;
  }

  // Only one submit ever happened: a busy slot is never reused or resubmitted.
  TASK_CHECK(ex.submits() == 1);
  TASK_CHECK(ex.accepted() == 1);

  // Release the very late worker, then reclaim it.
  ex.openGate();
  ex.quiesce();

  // Full PCM before and after the release must match the reference exactly
  // within the numeric tolerance.
  TASK_CHECK(checkPCM(out, ir, x, "races_long_hold_multi_wrap"));
  return 0;
}

int races_mixed_reject_sync()
{
  std::mt19937 rng(2003);
  const size_t B = 16, T = 128;
  std::vector<float> ir = makeIR(rng, 2000, 0.15f);
  std::vector<float> x = makeInput(rng, 8000);
  const std::vector<size_t> chunks = {1, 7, 33, T + 3};

  ScriptedExecutor ex;
  fftconvolver::AsyncFFTConvolver conv;
  TASK_CHECK(conv.init(B, T, ir.data(), ir.size(), &ex));

  std::vector<float> out(x.size(), 0.f);
  size_t pos = 0, ci = 0;
  bool toggle = false;
  while (pos < x.size())
  {
    ex.setMode((toggle = !toggle) ? ScriptedExecutor::REJECT
                                  : ScriptedExecutor::SYNC);
    const size_t n = std::min(chunks[ci++ % chunks.size()], x.size() - pos);
    conv.process(x.data() + pos, out.data() + pos, n);
    pos += n;
  }
  TASK_CHECK(ex.rejected() > 0);
  TASK_CHECK(ex.accepted() > 0);
  TASK_CHECK(checkPCM(out, ir, x, "races_mixed_reject_sync"));
  return 0;
}

int races_no_heap_no_wait_no_resubmit()
{
  std::mt19937 rng(2004);
  const size_t B = 16, T = 64;
  std::vector<float> ir = makeIR(rng, 800, 0.2f);
  std::vector<float> x = makeInput(rng, 4096);

  ScriptedExecutor ex;
  ex.setMode(ScriptedExecutor::REJECT);
  fftconvolver::AsyncFFTConvolver conv;
  TASK_CHECK(conv.init(B, T, ir.data(), ir.size(), &ex));

  std::vector<float> out(x.size(), 0.f);

  const unsigned long allocsBefore = g_allocs.load();
  const unsigned long freesBefore = g_deallocs.load();
  const unsigned long quiescesBefore = ex.quiesces();

  conv.process(x.data(), out.data(), x.size());

  TASK_CHECK(g_allocs.load() == allocsBefore);
  TASK_CHECK(g_deallocs.load() == freesBefore);
  TASK_CHECK(ex.quiesces() == quiescesBefore); // process never quiesces/waits

  // Every tail block attempted exactly one submit; nothing is submitted twice.
  const unsigned long tailBlocks =
    static_cast<unsigned long>(x.size() / T);
  TASK_CHECK(ex.submits() == tailBlocks);
  TASK_CHECK(ex.rejected() == tailBlocks);
  TASK_CHECK(checkPCM(out, ir, x, "races_no_heap_no_wait_no_resubmit"));
  return 0;
}

int races_reset_while_pending()
{
  std::mt19937 rng(2005);
  const size_t B = 16, T = 64;
  std::vector<float> ir = makeIR(rng, 700, 0.2f);
  std::vector<float> x = makeInput(rng, 2048);

  ScriptedExecutor ex;
  fftconvolver::AsyncFFTConvolver conv;
  TASK_CHECK(conv.init(B, T, ir.data(), ir.size(), &ex));

  ex.setMode(ScriptedExecutor::DEFER);
  ex.closeGate();
  std::vector<float> out(x.size(), 0.f);
  conv.process(x.data(), out.data(), T);
  ex.waitWorkerArrived(); // A worker is still holding the pending job

  // reset() while work is pending: must reclaim the worker safely.
  ex.openGate();
  conv.reset();
  TASK_CHECK(ex.quiesces() >= 1);

  // After reset, process emits silence.
  std::vector<float> silence(x.size(), 1.f);
  conv.process(x.data(), silence.data(), x.size());
  for (float v : silence) TASK_CHECK(v == 0.f);

  // Reinitialize with a different IR and verify the new configuration works.
  std::vector<float> ir2 = makeIR(rng, 300, 0.3f);
  ex.setMode(ScriptedExecutor::SYNC);
  TASK_CHECK(conv.init(B, T, ir2.data(), ir2.size(), &ex));
  std::vector<float> out2(x.size(), 0.f);
  const std::vector<size_t> chunks = {1, 7, B, T + 3};
  size_t pos = 0, ci = 0;
  while (pos < x.size())
  {
    const size_t n = std::min(chunks[ci++ % chunks.size()], x.size() - pos);
    conv.process(x.data() + pos, out2.data() + pos, n);
    pos += n;
  }
  TASK_CHECK(checkPCM(out2, ir2, x, "races_reset_while_pending"));
  return 0;
}

// ----------------------------------------------------------------------------------
// Lifetime group
// ----------------------------------------------------------------------------------

int life_invalid_params_keep_state()
{
  std::mt19937 rng(3001);
  std::vector<float> ir = makeIR(rng, 500, 0.2f);
  std::vector<float> x = makeInput(rng, 2000);

  ScriptedExecutor ex;
  fftconvolver::AsyncFFTConvolver conv;
  TASK_CHECK(conv.init(16, 64, ir.data(), ir.size(), &ex));

  // A pristine convolver with the same configuration, used as the reference
  // for state continuity after the failed reinitializations.
  ScriptedExecutor refEx;
  fftconvolver::AsyncFFTConvolver reference;
  TASK_CHECK(reference.init(16, 64, ir.data(), ir.size(), &refEx));

  const std::vector<size_t> chunks = {1, 7, 16, 67};
  const size_t half = x.size() / 2;

  std::vector<float> good(x.size(), 0.f);
  std::vector<float> refGood(x.size(), 0.f);
  size_t pos = 0, ci = 0;
  while (pos < half)
  {
    const size_t n = std::min(chunks[ci++ % chunks.size()], half - pos);
    conv.process(x.data() + pos, good.data() + pos, n);
    reference.process(x.data() + pos, refGood.data() + pos, n);
    pos += n;
  }

  const size_t badB[] = {4, 12, 257, 32};
  const size_t badT[] = {32, 63, 4097, 96};
  for (size_t i = 0; i < 4; ++i)
  {
    TASK_CHECK(!conv.init(badB[i], badT[i], ir.data(), ir.size(), &ex));
  }
  TASK_CHECK(!conv.init(16, 64, ir.data(), 65537, &ex));

  std::vector<float> infIr = ir;
  infIr[10] = std::numeric_limits<float>::infinity();
  TASK_CHECK(!conv.init(16, 64, infIr.data(), infIr.size(), &ex));

  // Continue both convolvers: their state and audio must stay identical.
  while (pos < x.size())
  {
    const size_t n = std::min(chunks[ci++ % chunks.size()], x.size() - pos);
    conv.process(x.data() + pos, good.data() + pos, n);
    reference.process(x.data() + pos, refGood.data() + pos, n);
    pos += n;
  }

  double maxDiff = 0.0;
  for (size_t i = 0; i < good.size(); ++i)
  {
    maxDiff = std::max(maxDiff,
                       std::fabs(static_cast<double>(good[i]) - refGood[i]));
  }
  TASK_CHECK(maxDiff <= 1e-7);

  // And the continued output still matches the time-domain reference.
  TASK_CHECK(checkPCM(good, ir, x, "life_invalid_params_keep_state"));
  return 0;
}

int life_ir_private_copy()
{
  std::mt19937 rng(3002);
  std::vector<float> ir = makeIR(rng, 600, 0.2f);
  std::vector<float> x = makeInput(rng, 2000);

  ScriptedExecutor ex;
  fftconvolver::AsyncFFTConvolver conv;
  TASK_CHECK(conv.init(16, 64, ir.data(), ir.size(), &ex));

  // Mutate the caller's IR after init: the convolver must keep its own copy.
  std::fill(ir.begin(), ir.end(), 99.f);

  std::vector<float> got(x.size(), 0.f);
  const std::vector<size_t> chunks = {1, 7, 16, 67};
  size_t pos = 0, ci = 0;
  while (pos < x.size())
  {
    const size_t n = std::min(chunks[ci++ % chunks.size()], x.size() - pos);
    conv.process(x.data() + pos, got.data() + pos, n);
    pos += n;
  }

  // Regenerate the exact IR deterministically for the reference.
  std::mt19937 rngRef(3002);
  std::vector<float> irRef = makeIR(rngRef, 600, 0.2f);
  TASK_CHECK(checkPCM(got, irRef, x, "life_ir_private_copy"));
  return 0;
}

int life_null_executor_and_destructor()
{
  std::mt19937 rng(3003);
  std::vector<float> ir = makeIR(rng, 900, 0.2f);
  std::vector<float> x = makeInput(rng, 3000);

  // Null executor: fully local processing, still numerically correct.
  {
    fftconvolver::AsyncFFTConvolver conv;
    TASK_CHECK(conv.init(16, 64, ir.data(), ir.size(), nullptr));
    std::vector<float> got(x.size(), 0.f);
    const std::vector<size_t> chunks = {1, 7, 16};
    size_t pos = 0, ci = 0;
    while (pos < x.size())
    {
      const size_t n = std::min(chunks[ci++ % chunks.size()], x.size() - pos);
      conv.process(x.data() + pos, got.data() + pos, n);
      pos += n;
    }
    TASK_CHECK(checkPCM(got, ir, x, "life_null_executor"));
  }

  // Destruction while a worker is still holding a job: the destructor's
  // quiesce path must reclaim it without a data race or hang.
  {
    ScriptedExecutor ex;
    ex.setMode(ScriptedExecutor::DEFER);
    ex.closeGate();
    {
      fftconvolver::AsyncFFTConvolver conv;
      TASK_CHECK(conv.init(16, 64, ir.data(), ir.size(), &ex));
      const size_t tailLen = 64;
      std::vector<float> tmp(tailLen, 0.5f);
      std::vector<float> out(tailLen, 0.f);
      conv.process(tmp.data(), out.data(), tailLen);
      ex.waitWorkerArrived();
      ex.openGate();
      // Destructor runs here and must quiesce the pending worker.
    }
    ex.quiesce();
    TASK_CHECK(ex.accepted() == 1);
  }
  return 0;
}

int life_reinit_reclaims_old_work()
{
  std::mt19937 rng(3004);
  std::vector<float> ir1 = makeIR(rng, 700, 0.2f);
  std::vector<float> ir2 = makeIR(rng, 1200, 0.15f);
  std::vector<float> x = makeInput(rng, 3000);

  ScriptedExecutor ex;
  fftconvolver::AsyncFFTConvolver conv;
  TASK_CHECK(conv.init(16, 64, ir1.data(), ir1.size(), &ex));

  // Keep one old worker occupied, then reinitialize: the old task is
  // reclaimed before the new configuration takes over.
  ex.setMode(ScriptedExecutor::DEFER);
  ex.closeGate();
  const size_t tailLen = 64;
  std::vector<float> out1(tailLen, 0.f);
  std::vector<float> tmp(tailLen, 0.4f);
  conv.process(tmp.data(), out1.data(), tailLen);
  ex.waitWorkerArrived();
  ex.openGate();

  TASK_CHECK(conv.init(32, 256, ir2.data(), ir2.size(), &ex));
  TASK_CHECK(ex.quiesces() >= 1);

  ex.setMode(ScriptedExecutor::SYNC);
  std::vector<float> got(x.size(), 0.f);
  const std::vector<size_t> chunks = {1, 7, 33, 259};
  size_t pos = 0, ci = 0;
  while (pos < x.size())
  {
    const size_t n = std::min(chunks[ci++ % chunks.size()], x.size() - pos);
    conv.process(x.data() + pos, got.data() + pos, n);
    pos += n;
  }
  TASK_CHECK(checkPCM(got, ir2, x, "life_reinit_reclaims_old_work"));
  return 0;
}

} // Anonymous namespace

// ----------------------------------------------------------------------------------
// Registration (no main in this translation unit)
// ----------------------------------------------------------------------------------

extern const struct task_case task_cases[] = {
  {"pcm", "reject_chunks", pcm_reject_chunks},
  {"pcm", "sync_chunks", pcm_sync_chunks},
  {"pcm", "inplace", pcm_inplace},
  {"pcm", "short_and_empty_ir", pcm_short_and_empty_ir},
  {"pcm", "zero_len_and_tail_drain", pcm_zero_len_and_tail_drain},
  {"pcm", "ring_wraparound", pcm_ring_wraparound},

  {"races", "deferred_across_deadline", races_deferred_across_deadline},
  {"races", "long_hold_multi_wrap", races_long_hold_multi_wrap},
  {"races", "mixed_reject_sync", races_mixed_reject_sync},
  {"races", "no_heap_no_wait_no_resubmit", races_no_heap_no_wait_no_resubmit},
  {"races", "reset_while_pending", races_reset_while_pending},

  {"lifetime", "invalid_params_keep_state", life_invalid_params_keep_state},
  {"lifetime", "ir_private_copy", life_ir_private_copy},
  {"lifetime", "null_executor_and_destructor", life_null_executor_and_destructor},
  {"lifetime", "reinit_reclaims_old_work", life_reinit_reclaims_old_work},
};

extern const size_t task_case_count =
  sizeof(task_cases) / sizeof(task_cases[0]);

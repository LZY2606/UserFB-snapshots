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

// Example: asynchronous tail convolution driven by a real std::thread executor.
//
// The executor owns a single worker thread. submit() never blocks and never
// allocates on the audio path: it only moves a small job descriptor into a
// mutex-protected queue and signals a condition variable. If the worker is
// still busy when a new tail job is offered, submit() rejects it and the
// convolver computes that job locally on the calling thread instead.
// quiesce() drains the queue and the in-flight callback; it is only called
// from the non-audio control thread (here: before shutdown).

#include "AsyncFFTConvolver.h"

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>


class ThreadTailExecutor : public fftconvolver::TailExecutor
{
public:
  ThreadTailExecutor()
  {
    // Start the worker only after all flags have their initial values.
    _worker = std::thread(&ThreadTailExecutor::workerLoop, this);
  }

  ~ThreadTailExecutor() override
  {
    {
      std::lock_guard<std::mutex> lock(_mutex);
      _stopping = true;
    }
    _cond.notify_all();
    _worker.join();
  }

  bool submit(void (*run)(void*), void* arg) override
  {
    std::lock_guard<std::mutex> lock(_mutex);
    // One background call at a time: refuse rather than block the audio
    // thread. A rejected job is computed locally by the convolver.
    if (_inFlight > 0 || !_queue.empty())
    {
      return false;
    }
    _queue.push_back(Job(run, arg));
    ++_inFlight;
    _cond.notify_one();
    return true;
  }

  void quiesce() override
  {
    std::unique_lock<std::mutex> lock(_mutex);
    _drained.wait(lock, [this] {
      return _queue.empty() && _inFlight == 0;
    });
  }

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
          return _stopping || !_queue.empty();
        });
        if (_stopping && _queue.empty())
        {
          return;
        }
        job = _queue.front();
        _queue.pop_front();
      }

      job.run(job.arg);

      {
        std::lock_guard<std::mutex> lock(_mutex);
        --_inFlight;
        if (_queue.empty() && _inFlight == 0)
        {
          _drained.notify_all();
        }
      }
    }
  }

  std::mutex _mutex;
  std::condition_variable _cond;
  std::condition_variable _drained;
  std::deque<Job> _queue;
  bool _stopping = false;
  unsigned int _inFlight = 0;
  std::thread _worker;
};


int main()
{
  const size_t headBlockSize = 16;
  const size_t tailBlockSize = 64;

  // A long, exponentially decaying impulse response (well beyond 2*T).
  const size_t irLen = 2048;
  std::vector<float> ir(irLen, 0.f);
  for (size_t i = 0; i < irLen; ++i)
  {
    const double t = static_cast<double>(i);
    ir[i] = static_cast<float>(
      std::exp(-t / 350.0) * (0.6 * std::sin(t * 0.07) + 0.25 * std::sin(t * 0.193)));
  }
  ir[0] = 0.8f;

  // A few seconds of a deterministic, broadband-ish input signal.
  const size_t numSamples = 16000;
  std::vector<float> input(numSamples, 0.f);
  for (size_t i = 0; i < numSamples; ++i)
  {
    const double t = static_cast<double>(i);
    input[i] = static_cast<float>(
      0.5 * std::sin(t * 0.031) + 0.25 * std::sin(t * 0.117) +
      0.12 * std::sin(t * 0.331));
  }

  ThreadTailExecutor executor;
  fftconvolver::AsyncFFTConvolver convolver;
  if (!convolver.init(headBlockSize, tailBlockSize,
                      ir.data(), ir.size(), &executor))
  {
    std::fprintf(stderr, "init failed\n");
    return 1;
  }

  // Feed arbitrary-sized chunks (as a real audio callback would) and collect
  // the converted output.
  std::vector<float> output(numSamples, 0.f);
  const size_t chunkSizes[] = {1, 7, headBlockSize, tailBlockSize + 3, 33};
  size_t pos = 0;
  size_t chunkIndex = 0;
  while (pos < numSamples)
  {
    const size_t len = std::min(chunkSizes[chunkIndex++ % 5], numSamples - pos);
    convolver.process(input.data() + pos, output.data() + pos, len);
    pos += len;
  }

  // Control-thread shutdown: wait for every accepted background callback.
  executor.quiesce();
  convolver.reset();

  // Write the 32-bit float PCM output.
  std::ofstream out(".task-build/async-tail.pcm", std::ios::binary);
  if (!out)
  {
    std::fprintf(stderr, "cannot open output file\n");
    return 2;
  }
  out.write(reinterpret_cast<const char*>(output.data()),
            static_cast<std::streamsize>(output.size() * sizeof(float)));
  if (!out)
  {
    std::fprintf(stderr, "write failed\n");
    return 3;
  }

  double energy = 0.0;
  for (float v : output) energy += static_cast<double>(v) * v;
  std::printf("ASYNC_TAIL wrote=%zu samples energy=%.6f\n",
              static_cast<size_t>(output.size()), energy);
  return 0;
}

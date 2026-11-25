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

// Example: long-tail convolution driven by a real background worker thread.
//
// The executor below uses one persistent worker and a preallocated single-slot
// queue. submit() only takes a non-blocking trylock and never allocates; when
// the worker is still busy it rejects, and the convolver computes that tail job
// locally instead. quiesce() is the only blocking call and is used from the
// non-audio control thread when the convolver is reset or destroyed.

#include "AsyncFFTConvolver.h"

#include <cmath>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>


namespace
{

class ThreadTailExecutor : public fftconvolver::TailExecutor
{
public:
  ThreadTailExecutor() :
    _mutex(),
    _workCv(),
    _stop(false),
    _hasTask(false),
    _run(0),
    _arg(0),
    _worker([this] { workerLoop(); })
  {
  }

  ~ThreadTailExecutor() override
  {
    {
      std::lock_guard<std::mutex> lock(_mutex);
      _stop = true;
      _workCv.notify_all();
    }
    if (_worker.joinable())
    {
      _worker.join();
    }
  }

  bool submit(void (*run)(void*), void* arg) override
  {
    // Non-blocking by contract: never wait for the worker or the lock.
    std::unique_lock<std::mutex> lock(_mutex, std::try_to_lock);
    if (!lock.owns_lock() || _hasTask)
    {
      return false; // The convolver will fall back to local computation.
    }
    _run = run;
    _arg = arg;
    _hasTask = true;
    _workCv.notify_one();
    return true;
  }

  void quiesce() override
  {
    // Blocking is allowed here; this example never reuses the executor after
    // quiesce(), so a fresh worker is spun up for potential later submissions.
    {
      std::lock_guard<std::mutex> lock(_mutex);
      _stop = true;
      _workCv.notify_all();
    }
    if (_worker.joinable())
    {
      _worker.join();
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _stop = false;
    _hasTask = false;
    _worker = std::thread([this] { workerLoop(); });
  }

private:
  void workerLoop()
  {
    for (;;)
    {
      void (*run)(void*) = 0;
      void* arg = 0;
      {
        std::unique_lock<std::mutex> lock(_mutex);
        _workCv.wait(lock, [this] { return _stop || _hasTask; });
        if (_stop && !_hasTask)
        {
          return;
        }
        run = _run;
        arg = _arg;
        _hasTask = false;
      }
      run(arg);
    }
  }

  std::mutex _mutex;
  std::condition_variable _workCv;
  bool _stop;
  bool _hasTask;
  void (*_run)(void*);
  void* _arg;
  std::thread _worker;
};

} // namespace


int main()
{
  static const size_t kHeadBlockSize = 32;
  static const size_t kTailBlockSize = 256; // = 8 * head block size
  static const size_t kIrLen = 4000;        // Long decaying reverb tail
  static const size_t kNumSamples = 8 * 1024;

  std::vector<fftconvolver::Sample> ir(kIrLen);
  for (size_t i = 0; i < kIrLen; ++i)
  {
    const float decay = std::exp(-3.0f * static_cast<float>(i) / static_cast<float>(kIrLen));
    const float wave = std::sin(static_cast<float>(i) * 0.07f) +
                       0.5f * std::sin(static_cast<float>(i) * 0.013f);
    ir[i] = 0.35f * decay * wave;
  }

  std::vector<fftconvolver::Sample> input(kNumSamples);
  for (size_t i = 0; i < kNumSamples; ++i)
  {
    const float t = static_cast<float>(i);
    input[i] = 0.6f * std::sin(t * 0.02f) * std::exp(-2.0f * t / static_cast<float>(kNumSamples));
    // A few sparse excitations so the long reverb tail is clearly audible.
    if (i % 1024 == 37)
    {
      input[i] += 0.9f;
    }
  }

  ThreadTailExecutor executor;
  fftconvolver::AsyncFFTConvolver convolver;
  if (!convolver.init(kHeadBlockSize, kTailBlockSize, ir.data(), ir.size(), &executor))
  {
    std::fprintf(stderr, "Initialization failed\n");
    return 1;
  }

  std::vector<fftconvolver::Sample> output(kNumSamples, 0.0f);

  // Arbitrary small processing chunks as they would arrive from an audio callback.
  static const size_t kChunkSizes[] = {1, 7, 17, 33, kHeadBlockSize, kTailBlockSize + 3};
  size_t position = 0;
  size_t chunkIndex = 0;
  while (position < kNumSamples)
  {
    const size_t chunk = std::min(kChunkSizes[chunkIndex++ % 6], kNumSamples - position);
    convolver.process(input.data() + position, output.data() + position, chunk);
    position += chunk;
  }

  std::FILE* file = std::fopen(".task-build/async-tail.pcm", "wb");
  if (!file)
  {
    std::fprintf(stderr, "Could not open output file\n");
    return 2;
  }
  const size_t written = std::fwrite(output.data(),
                                     sizeof(fftconvolver::Sample),
                                     output.size(),
                                     file);
  std::fclose(file);
  if (written != output.size())
  {
    std::fprintf(stderr, "Incomplete output write\n");
    return 3;
  }

  double peak = 0.0;
  for (float sample : output)
  {
    peak = std::max(peak, double(std::fabs(sample)));
  }
  std::printf("ASYNC_EXAMPLE samples=%zu peak=%.6f file=.task-build/async-tail.pcm\n",
              output.size(), peak);

  convolver.reset(); // Joins any still running background job via quiesce()
  return 0;
}

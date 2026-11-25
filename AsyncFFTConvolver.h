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

#ifndef _FFTCONVOLVER_ASYNCFFTCONVOLVER_H
#define _FFTCONVOLVER_ASYNCFFTCONVOLVER_H

#include "AudioFFT.h"
#include "FFTConvolver.h"
#include "TailJob.h"
#include "Utilities.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>


namespace fftconvolver
{

/**
* @class TailExecutor
* @brief Host provided execution facility for background tail jobs
*
* The executor is used from two kinds of threads:
*
* - The audio thread calls submit() from process(). The host guarantees that
*   submit() never blocks and never allocates.
* - Control threads (init/reset/destruction, never concurrent with process())
*   may call quiesce(), which is allowed to block until every accepted
*   callback has fully returned.
*
* submit() returns true if the callback was accepted: it is executed exactly
* once, possibly before submit() returns or arbitrarily late. It returns
* false if the callback was not accepted and will never be executed.
*/
class TailExecutor
{
public:
  virtual ~TailExecutor() {}

  /**
  * @brief Schedules one background callback
  * @param run The callback
  * @param arg The user data pointer handed to the callback
  * @return true: Accepted and executed exactly once - false: Rejected, never run
  */
  virtual bool submit(void (*run)(void*), void* arg) = 0;

  /**
  * @brief Blocks until all accepted callbacks have fully returned
  *
  * Only callable from the non-audio thread which owns init/reset/destruction.
  */
  virtual void quiesce() = 0;
};


/**
* @class AsyncFFTConvolver
* @brief Two-stage partitioned convolution with asynchronous tail processing
*
* The first 2*tailBlockSize coefficients of the impulse response are handled
* by a short-partition head convolver (block size headBlockSize). The
* remaining coefficients are processed in tailBlockSize long partitions with
* 2*tailBlockSize point FFTs.
*
* Every tailBlockSize collected input samples produce one ordered tail job.
* If the background slot is free, the job is offered to the TailExecutor; the
* slot stays occupied until the worker has published its result and no longer
* touches any job buffer. When a tail result is due (2*tailBlockSize samples
* after the input block which produced it) but no completed background result
* is available, the audio thread computes the very same job itself using an
* independent workspace. A background result arriving afterwards is simply
* discarded - overlap is never advanced twice.
*
* process() never allocates, frees, locks, waits or calls quiesce().
* Control methods are never called concurrently with process(), while worker
* threads may run concurrently with both.
*/
class AsyncFFTConvolver
{
public:
  AsyncFFTConvolver();
  ~AsyncFFTConvolver();

  /**
  * @brief Initializes/reinitializes the convolver
  *
  * Valid parameters: headBlockSize (B) and tailBlockSize (T) powers of 2,
  * 8 <= B <= 256, 64 <= T <= 4096, T >= 4*B. The impulse response holds at
  * most 65536 finite floats; an empty IR is legal. On invalid parameters
  * the method returns false and the previous configuration and audio state
  * remain untouched. A successful init first safely reclaims any background
  * work of the previous configuration and stores a private copy of the IR.
  *
  * @param headBlockSize Short partition size (B)
  * @param tailBlockSize Long partition size (T)
  * @param ir The impulse response
  * @param irLen Length of the impulse response
  * @param executor Background executor, or 0 for fully local processing
  * @return true: Success - false: Failed
  */
  bool init(size_t headBlockSize,
            size_t tailBlockSize,
            const Sample* ir,
            size_t irLen,
            TailExecutor* executor);

  /**
  * @brief Convolves the given input samples and immediately outputs the result
  *
  * No allocation, deallocation, locking or waiting is performed.
  * input == output is supported. len == 0 has no side effects.
  */
  void process(const Sample* input, Sample* output, size_t len);

  /**
  * @brief Clears configuration and history; process() outputs zeros afterwards
  */
  void reset();

private:
  // Reclaims all background work: waits until an accepted worker has fully
  // returned, then clears the slot. Control threads only.
  void reclaimBackground();

  // Frees all configuration resources (control threads only).
  void teardown();

  // Emits the ordered tail job for one completely collected input block.
  void emitTailJob(const Sample* block);

  // Makes every due tail result available, computing locally when the
  // background result is not published in time.
  void advanceTailQueue();

  // Computes one tail job with the independent local workspace and copies
  // its result into the given (free) output block.
  void computeLocal(uint64_t generation,
                    const Sample* inputBlock,
                    Sample* destBlock);

  static bool IsPowerOfTwo(size_t val);
  static bool IsFiniteIR(const Sample* ir, size_t irLen);

  // Configuration
  size_t _headBlockSize;
  size_t _tailBlockSize;
  size_t _fftSize;
  size_t _fftComplexSize;
  size_t _tailPartitionCount;
  TailExecutor* _executor;
  std::vector<Sample> _irCopy;

  // Short head: first 2*T coefficients, B-sized partitions
  std::unique_ptr<FFTConvolver> _headConvolver;

  // Tail IR spectra (read-only after init)
  std::vector<SplitComplex*> _tailIR;
  std::unique_ptr<audiofft::AudioFFT> _tailFFT;
  SampleBuffer _tailFFTBuffer;

  // Frequency-domain ring of input spectra (size P+2)
  std::vector<SplitComplex*> _inputRing;
  size_t _ringPos;
  uint64_t _inputBlockCount;

  // Time-domain ring of input blocks (job input + local fallback)
  std::vector<std::unique_ptr<SampleBuffer>> _inputBlockRing;

  // Single background slot + independent local fallback workspace
  std::unique_ptr<TailJob> _backgroundJob;
  bool _slotOccupied;
  std::unique_ptr<TailJob> _localJob;

  // Ordered output blocks
  std::vector<std::unique_ptr<SampleBuffer>> _outputBlocks;
  uint64_t _nextOutputGeneration;

  // Stream state
  uint64_t _streamPosition;
  SampleBuffer _tailInputBuffer;
  size_t _tailInputFill;
  SampleBuffer _dryCopy;
  std::vector<const SplitComplex*> _historyPtrs;

  // Prevent uncontrolled usage
  AsyncFFTConvolver(const AsyncFFTConvolver&);
  AsyncFFTConvolver& operator=(const AsyncFFTConvolver&);
};

} // End of namespace fftconvolver

#endif // Header guard

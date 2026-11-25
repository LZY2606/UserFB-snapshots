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

#ifndef _FFTCONVOLVER_TAILJOB_H
#define _FFTCONVOLVER_TAILJOB_H

#include "AudioFFT.h"
#include "Utilities.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>


namespace fftconvolver
{

/**
* @class TailExecutor
* @brief Host provided execution environment for tail convolution jobs
*
* The convolver never blocks on background work: submit() has to be
* non-blocking and allocation-free from the audio thread. A submitted call
* runs exactly once - either before submit() returns or at some later time -
* and a rejected call is never executed. quiesce() waits until every accepted
* call has fully returned and may only be called from the non-audio thread
* which also owns init()/reset()/destruction.
*/
class TailExecutor
{
public:
  virtual ~TailExecutor() {}

  /**
  * @brief Hands one job to the executor
  * @param run The function to execute
  * @param arg The opaque argument handed to the run function
  * @return true: The call was accepted and will be executed exactly once
  *         false: The call was rejected and will never be executed
  */
  virtual bool submit(void (*run)(void*), void* arg) = 0;

  /**
  * @brief Blocks until every accepted job has fully returned
  */
  virtual void quiesce() = 0;
};


/**
* @class TailPlan
* @brief Immutable frequency-domain description of the tail part of an IR
*
* A plan is built once during initialization and afterwards shared read-only
* between the audio thread and any background worker. All tail partitions use
* a 2*tailBlockSize point FFT with tailBlockSize long time-domain partitions.
*/
class TailPlan
{
public:
  TailPlan();
  ~TailPlan();

  /**
  * @brief Builds the partitioned IR spectra
  * @param tailBlockSize The tail partition size (power of 2)
  * @param ir The tail impulse response (coefficients following the head part)
  * @param irLen Length of the tail impulse response
  * @return true: Success - false: Failed
  */
  bool build(size_t tailBlockSize, const Sample* ir, size_t irLen);

  void clear();

  TailPlan(TailPlan&& other);
  TailPlan& operator=(TailPlan&& other);

  size_t blockSize() const { return _blockSize; }
  size_t fftSize() const { return _fftSize; }
  size_t complexSize() const { return _complexSize; }
  size_t segmentCount() const { return _segCount; }
  const SplitComplex& segmentIR(size_t index) const { return *_segmentsIR[index]; }

private:
  size_t _blockSize;
  size_t _fftSize;
  size_t _complexSize;
  size_t _segCount;
  std::vector<SplitComplex*> _segmentsIR;

  TailPlan(const TailPlan&);
  TailPlan& operator=(const TailPlan&);
};


/**
* @brief Computes one tail convolution job
*
* The job is self-contained: the input spectra are read-only snapshots, the
* IR plan is shared read-only and every working buffer (accumulator, FFT
* buffer, FFT instance, result) is private to the calling thread. The routine
* therefore can run on a worker thread without touching any convolver state.
*
* @param plan The tail IR plan
* @param inputSpectra segCount spectra; index 0 is the current input block,
*                     index i is the block i generations in the past
* @param accum Private complex accumulator (complex size of the plan)
* @param fftBuffer Private time-domain FFT buffer (FFT size of the plan)
* @param fft Private FFT instance
* @param result Private result buffer of 2*blockSize samples
*/
void ComputeTailJob(const TailPlan& plan,
                    const SplitComplex* const* inputSpectra,
                    SplitComplex& accum,
                    SampleBuffer& fftBuffer,
                    audiofft::AudioFFT& fft,
                    Sample* result);


/**
* @class TailJobSlot
* @brief One background tail job including its private workspace
*
* The slot is the only memory a worker is allowed to touch besides the shared
* read-only TailPlan. The audio thread fills the read-only input snapshot and
* publishes the BUSY state before submitting the job. The worker publishes the
* DONE state as its very last action; the slot must not be reused before DONE
* (or a rejection) has been observed by the audio thread.
*/
class TailJobSlot
{
public:
  enum State : unsigned
  {
    IDLE = 0, ///< No worker is (or will be) accessing the slot
    BUSY = 1, ///< A submitted worker may be accessing the slot
    DONE = 2  ///< The worker has published the result and touches nothing
  };

  TailJobSlot();
  ~TailJobSlot();

  /**
  * @brief Allocates all private workspaces (init time only)
  * @param blockSize The tail partition size
  * @param segCount Number of tail IR partitions / input spectra per job
  */
  void configure(size_t blockSize, size_t segCount);

  /**
  * @brief Releases all workspaces
  */
  void clear();

  /**
  * @brief Fills the read-only input snapshot for a new ordered job
  *
  * Only called on the audio thread while the slot is IDLE.
  *
  * @param position Absolute sample position where the input block starts
  * @param plan The tail IR plan used by the job
  * @param ringInputs segCount spectra, index 0 being the current block
  */
  void arm(uint64_t position, const TailPlan* plan, const SplitComplex* const* ringInputs);

  /**
  * @brief Worker entry point (handed to TailExecutor::submit)
  */
  static void run(void* arg);

  State loadState() const { return static_cast<State>(_state.load(std::memory_order_acquire)); }
  void storeState(State state, std::memory_order order) { _state.store(static_cast<unsigned>(state), order); }

  uint64_t position() const { return _position; }
  size_t resultLength() const { return 2 * _blockSize; }
  const Sample* resultData() const { return _result.data(); }

  /**
  * @brief Read-only input snapshot - also readable by the audio thread while
  *        a worker is running (concurrent reads are safe)
  */
  const SplitComplex* const* snapshotData() const { return _snapshotPtrs.data(); }

private:
  size_t _blockSize;
  size_t _segCount;
  size_t _complexSize;
  const TailPlan* _plan;
  uint64_t _position;
  std::atomic<unsigned> _state;

  // Read-only input of the worker (published before submission)
  std::vector<SplitComplex*> _snapshot;
  std::vector<const SplitComplex*> _snapshotPtrs;

  // Private workspace of the worker
  SplitComplex _accum;
  SampleBuffer _fftBuffer;
  audiofft::AudioFFT _fft;
  SampleBuffer _result;

  TailJobSlot(const TailJobSlot&);
  TailJobSlot& operator=(const TailJobSlot&);
};

} // End of namespace fftconvolver

#endif // Header guard

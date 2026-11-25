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

#include "FFTConvolver.h"
#include "TailJob.h"
#include "Utilities.h"

#include <atomic>
#include <cstdint>
#include <vector>


namespace fftconvolver
{

/**
* @class AsyncFFTConvolver
* @brief Two-stage partitioned convolver with a non-blocking tail pipeline
*
* The first 2*tailBlockSize coefficients of the impulse response are handled by
* a short-block head convolver (headBlockSize partitions), the remaining tail
* is convolved in tailBlockSize long partitions using 2*tailBlockSize point
* FFTs. Whenever tailBlockSize new input samples have been collected, one
* independently computable, ordered tail job is produced.
*
* A tail job is either handed to the host provided TailExecutor or, when no
* background slot is available, when submission is rejected or when the result
* is due before a worker has published it, computed on the calling thread with
* a private workspace. The audio thread therefore never waits on a worker,
* never performs heap operations and never adds latency: a missing background
* result simply falls back to local computation, and a result that arrives too
* late is discarded.
*
* At most one background call may be accessing a job slot at any time; the slot
* is only reused after the worker has published completion and no longer
* touches it. A worker only accesses its own read-only input snapshot, the
* shared read-only IR plan and its private workspace.
*/
class AsyncFFTConvolver
{
public:
  AsyncFFTConvolver();
  ~AsyncFFTConvolver();

  /**
  * @brief Initializes/reconfigures the convolver
  *
  * Block sizes have to be powers of 2 with 8<=headBlockSize<=256,
  * 64<=tailBlockSize<=4096 and tailBlockSize>=4*headBlockSize. The impulse
  * response may contain at most 65536 finite samples; an empty IR is valid and
  * produces silence. On failure the previous configuration and audio history
  * remain untouched. A successful replacement first reclaims all background
  * jobs of the previous configuration.
  *
  * @param headBlockSize The head partition size
  * @param tailBlockSize The tail partition size
  * @param ir The impulse response (a private copy is stored)
  * @param irLen Length of the impulse response
  * @param executor Host executor (0 forces fully local tail processing); the
  *                 executor must stay valid until reset() has returned
  * @return true: Success - false: Invalid parameters
  */
  bool init(size_t headBlockSize,
            size_t tailBlockSize,
            const Sample* ir,
            size_t irLen,
            TailExecutor* executor);

  /**
  * @brief Convolves the given input samples and immediately outputs the result
  *
  * Never allocates, frees, locks, waits or calls quiesce(). input and output
  * may alias. len may be 0 (null pointers allowed in that case) without side
  * effects.
  */
  void process(const Sample* input, Sample* output, size_t len);

  /**
  * @brief Clears configuration and history; subsequent process() emits zeros
  *
  * Waits for every accepted background job to return. May only be called from
  * the non-audio thread which also owns init() and destruction.
  */
  void reset();

private:
  enum JobState : unsigned
  {
    JOB_EMPTY = 0, ///< No job stored in the record
    JOB_LOCAL = 1, ///< Computed locally, result is available immediately
    JOB_BUSY  = 2  ///< Submitted; the worker may still be running
  };

  // One ordered tail job record in the scheduling ring. Records are only ever
  // touched by the audio thread; the background slot itself carries the atomic
  // state shared with the worker.
  struct Record
  {
    uint64_t position;       ///< Absolute input position of the job
    unsigned state;          ///< One of JobState
    Sample* localResult;     ///< Own 2*tailBlockSize result buffer
    bool slotJob;            ///< true: The job was handed to the background slot

    Record() : position(0), state(JOB_EMPTY), localResult(0), slotJob(false) {}
  };

  bool validate(size_t headBlockSize, size_t tailBlockSize, size_t irLen) const;
  void teardown();
  void resetHistory();
  void processTailBlock(const Sample* dryInput);

  size_t _headBlockSize;
  size_t _tailBlockSize;
  TailExecutor* _executor;
  std::vector<Sample> _ir;

  // Head: short-partition convolution of the first 2*tailBlockSize coefficients
  FFTConvolver _head;

  // Tail: long-partition convolution of everything behind the head
  TailPlan _plan;
  size_t _tailSegCount;
  size_t _fftComplexSize;

  audiofft::AudioFFT _inputFft;
  SampleBuffer _inputFftBuffer;
  std::vector<SplitComplex*> _ring;   ///< Rotating input spectra
  size_t _ringCurrent;                ///< Index of the most recent block
  std::vector<const SplitComplex*> _ringView; ///< Newest...oldest view for arming

  TailJobSlot _slot;                  ///< The single background slot
  SplitComplex _localAccum;
  SampleBuffer _localFftBuffer;
  audiofft::AudioFFT _localFft;

  SampleBuffer _tailInput;            ///< Accumulates tailBlockSize dry samples
  size_t _tailInputFill;
  uint64_t _nextJobPosition;          ///< Position of the next ordered job
  uint64_t _nextDuePosition;          ///< Position of the next job becoming due

  std::vector<Record> _records;       ///< Ring of ordered job records
  size_t _recordHead;                 ///< Next record index to consume
  size_t _recordTail;                 ///< Next record index to produce
  SampleBuffer _overlap;              ///< Second half of the previous result
  SampleBuffer _tailOutput;           ///< Ready tail contribution for process()
  size_t _tailOutputPos;              ///< Consumption position within _tailOutput

  // Prevent uncontrolled usage
  AsyncFFTConvolver(const AsyncFFTConvolver&);
  AsyncFFTConvolver& operator=(const AsyncFFTConvolver&);
};

} // End of namespace fftconvolver

#endif // Header guard

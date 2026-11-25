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
#include <memory>
#include <vector>


namespace fftconvolver
{

/**
* @class TailJob
* @brief One independently computable partition of the long tail convolution
*
* A tail job owns everything a background worker needs to compute a single
* 2*tailBlockSize long chunk of the tail convolution:
*
* - A private read-only copy of the tailBlockSize input samples which
*   triggered the job.
* - Private read-only copies of the older input spectra required by the
*   partitioned multiply-accumulate path (the frequency-domain ring history).
* - Private FFT / complex multiply / inverse-FFT workspaces.
*
* The shared impulse-response spectra are read-only for the worker and are
* never modified after initialization.
*
* A job is prepared on the audio thread (all destinations are preallocated,
* so no heap operations are involved), submitted to a TailExecutor and later
* published through an atomic flag. Until publishCompleted() is observed, the
* owning convolver must not reuse any buffer of the job.
*/
class TailJob
{
public:
  TailJob();
  ~TailJob();

  /**
  * @brief Allocates the private workspaces
  * @param tailBlockSize The tail partition size (T)
  * @param partitionCount Number of tail IR partitions (P) - 0 is allowed
  * @param irSegments Read-only array of P IR spectra (not owned, must outlive the job)
  * @return true: Success - false: Failed
  */
  bool init(size_t tailBlockSize,
            size_t partitionCount,
            const SplitComplex* const* irSegments);

  /**
  * @brief Releases all workspaces
  */
  void reset();

  /**
  * @brief Prepares the job for one input block
  *
  * All data is copied into private storage: the caller is free to reuse or
  * overwrite the source ring buffers afterwards.
  *
  * @param generation Absolute input block number (position = generation * tailBlockSize)
  * @param inputBlock tailBlockSize time-domain input samples
  * @param history Read-only spectra ring, history[i] holding the spectrum of
  *                input block (generation-1-i) - may be 0 for missing history
  * @param historyCount Number of entries in history
  */
  void prepare(uint64_t generation,
               const Sample* inputBlock,
               const SplitComplex* const* history,
               size_t historyCount);

  /**
  * @brief Executor entry point (C-style callback)
  */
  static void runEntry(void* arg);

  /**
  * @brief Performs the actual tail convolution using private workspaces only
  *
  * The completion flag is published as the very last action, so once it is
  * observed the worker no longer touches any buffer of the job.
  */
  void run();

  /**
  * @brief Arms the job before submission (clears the completion flag)
  */
  void arm();

  /**
  * @brief Whether the worker has published the result (acquire semantics)
  */
  bool isCompleted() const;

  /**
  * @brief The absolute input block number this job was prepared for
  */
  uint64_t generation() const;

  /**
  * @brief The 2*tailBlockSize convolution result (valid after completion)
  */
  const Sample* result() const;

  size_t tailBlockSize() const;

private:
  size_t _tailBlockSize;
  size_t _fftSize;
  size_t _fftComplexSize;
  size_t _partitionCount;

  // Private copy of the read-only IR spectrum pointer array: the actual
  // spectra are shared and immutable after the convolver's initialization,
  // but the pointer array itself is owned by the job so that a worker never
  // dereferences a caller-side container.
  std::vector<const SplitComplex*> _irSegments;

  uint64_t _generation;
  size_t _historyCount;

  SampleBuffer _input;
  SampleBuffer _fftBuffer;
  SampleBuffer _result;
  SplitComplex _spectrum;
  SplitComplex _conv;
  std::vector<std::unique_ptr<SplitComplex>> _history;

  audiofft::AudioFFT _fft;
  std::atomic<unsigned int> _completed;

  // Prevent uncontrolled usage
  TailJob(const TailJob&);
  TailJob& operator=(const TailJob&);
};

} // End of namespace fftconvolver

#endif // Header guard

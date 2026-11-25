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

#ifndef _FFTCONVOLVER_PARTITIONEDCONVOLVERCORE_H
#define _FFTCONVOLVER_PARTITIONEDCONVOLVERCORE_H

#include "AudioFFT.h"
#include "Utilities.h"

#include <cstddef>
#include <vector>


namespace fftconvolver
{

/**
* @class PartitionedConvolverCore
* @brief Shared low-level engine for uniform-partition frequency-domain convolution
*
* The class owns the 2B-point FFT, the ring buffer of input partition spectra,
* the filter partition spectra, the FFT work buffer and the partitioned
* complex multiply-accumulate machinery. It works on whole B-sized blocks;
* sample-level buffering and overlap handling is left to the wrapping classes
* (FFTConvolver, AdaptiveConvolver).
*
* Two input framings are supported:
*
* - pushInputBlock(): overlap-save framing, i.e. the 2B-point FFT of the
*   concatenation of the previous and the current B-frame block
*   [block m-1 | block m]. The history is zero before the stream starts.
*
* - Direct ring-slot writes via ringSlot() and fft(): zero-padded framing
*   [block | zeros], as used by the classic overlap-add FFTConvolver path.
*
* Filter partition p is always stored as the FFT of B filter taps padded with
* B zeros. All scratch storage is allocated in init(), so none of the
* block-level routines allocate heap memory.
*/
class PartitionedConvolverCore
{
public:
  PartitionedConvolverCore();

  /**
  * @brief Initializes the core
  * @param blockSize Block/partition size B (must be a power of 2)
  * @param partitionCount Number of filter/ring partitions P
  * @return true: Success - false: Failed
  */
  bool init(size_t blockSize, size_t partitionCount);

  /**
  * @brief Resets the core and releases all buffers
  */
  void reset();

  /**
  * @brief Stores one filter partition: FFT of [taps (up to B) | zeros]
  *
  * Does not touch the input ring, so it can also be used while streaming.
  *
  * @param partition Partition index [0, partitionCount)
  * @param taps The filter taps of this partition (at most B)
  * @param tapCount Number of valid taps (the rest is implicitly zero)
  */
  void setFilterPartition(size_t partition, const Sample* taps, size_t tapCount);

  /**
  * @brief Zeroes one filter partition spectrum
  */
  void clearFilterPartition(size_t partition);

  /**
  * @brief Zeroes all filter partition spectra
  */
  void clearAllFilterPartitions();

  /**
  * @brief Clears the streaming state (input ring, block history, ring position)
  *
  * Keeps all allocations and filter spectra; used by adaptive resets that must
  * not release or reallocate heap memory.
  */
  void clearStreaming();

  /**
  * @brief Pushes one B-frame input block with overlap-save framing
  *
  * The 2B-point FFT input is [previousBlock | block], with the previous block
  * taken from the internal history (zeros before the stream starts).
  *
  * @param block The B new input samples
  */
  void pushInputBlock(const Sample* block);

  /**
  * @brief Pushes a zero input block into the ring (used for flushing)
  */
  void pushZeroInputBlock();

  /**
  * @brief Computes one block of convolution: IFFT(sum_p H[p] * X[m-p])
  *
  * Uses the filter spectra and the input ring as they are at call time, i.e.
  * filter updates performed afterwards do not affect the result. The 2B result
  * samples are written to @p result.
  *
  * @param result Output buffer of at least 2B samples
  */
  void convolveBlock(Sample* result);

  /**
  * @brief Adds the complex product of a ring partition and a filter partition
  *        to an accumulator (partitioned complex multiply-accumulate)
  * @param result Accumulator spectrum
  * @param ringOffset Ring offset relative to the newest block (0 = newest)
  * @param filterPartition Filter partition index
  */
  void accumulateProduct(SplitComplex& result, size_t ringOffset, size_t filterPartition);

  /**
  * @brief Adds the complex product of an explicit ring slot and a filter
  *        partition to an accumulator
  */
  void accumulateSlotProduct(SplitComplex& result, size_t ringSlot, size_t filterPartition);

  /**
  * @brief The spectrum of the ring input partition at the given ring offset
  *
  * Offset 0 is the most recently pushed block.
  */
  const SplitComplex& ringSpectrum(size_t ringOffset) const;

  /**
  * @brief Direct access to a ring slot by absolute index
  */
  SplitComplex& ringSlot(size_t slot);

  /** @overload */
  const SplitComplex& ringSlot(size_t slot) const;

  /**
  * @brief The spectrum of one filter partition
  */
  SplitComplex& filterSpectrum(size_t partition);

  /** @overload */
  const SplitComplex& filterSpectrum(size_t partition) const;

  /**
  * @brief Reads back the first B time-domain taps of one filter partition
  */
  void getFilterPartitionTaps(size_t partition, Sample* taps);

  /**
  * @brief The shared 2B-sample FFT work buffer
  */
  Sample* fftBuffer();

  /**
  * @brief A preallocated spectrum-sized scratch accumulator
  */
  SplitComplex& scratch() { return *_scratch; }

  size_t blockSize() const { return _blockSize; }
  size_t fftSize() const { return _fftSize; }
  size_t partitionCount() const { return _partitionCount; }
  size_t fftComplexSize() const { return _fftComplexSize; }
  audiofft::AudioFFT& fft() { return _fft; }
  const audiofft::AudioFFT& fft() const { return _fft; }

private:
  size_t ringIndex(size_t offset) const;

  size_t _blockSize;
  size_t _fftSize;
  size_t _partitionCount;
  size_t _fftComplexSize;
  audiofft::AudioFFT _fft;
  SampleBuffer _fftBuffer;
  std::vector<SplitComplex*> _ring;
  std::vector<SplitComplex*> _filters;
  SplitComplex* _scratch;
  SampleBuffer _previousBlock;
  size_t _ringPos;

  // Prevent uncontrolled usage
  PartitionedConvolverCore(const PartitionedConvolverCore&);
  PartitionedConvolverCore& operator=(const PartitionedConvolverCore&);
};

} // End of namespace fftconvolver

#endif // Header guard

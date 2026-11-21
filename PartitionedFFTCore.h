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
// IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
// CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
// ==================================================================================

#ifndef _FFTCONVOLVER_PARTITIONEDFFTCORE_H
#define _FFTCONVOLVER_PARTITIONEDFFTCORE_H

#include "AudioFFT.h"
#include "Utilities.h"

#include <vector>


namespace fftconvolver
{

/**
* @class SpectrumRing
* @brief Ring buffer of partitioned spectra shared by the convolver implementations
*
* A ring with N slots holds the spectra of the N most recent blocks. The slot
* returned by writeNext() is the one which will be published by the next call
* to commit(); at(0) is the most recently committed block. Slots which have
* never been committed are zero, which makes the ring usable for streaming
* from a zero-initialized history without any special-casing.
*
* All memory is allocated by init() and reused afterwards. writeNext(),
* commit(), at() and restart() never allocate or free memory.
*/
class SpectrumRing
{
public:
  SpectrumRing();
  ~SpectrumRing();

  /**
  * @brief Allocates the ring (frees a previous allocation first)
  * @param blockSize Block size B (spectra have 2*B real samples)
  * @param partitions Number of slots N
  */
  void init(size_t blockSize, size_t partitions);

  /**
  * @brief Frees the ring
  */
  void clear();

  /**
  * @brief Zeroes all spectra and restarts the streaming position without
  *        allocating or freeing memory
  */
  void restart();

  size_t blockSize() const { return _blockSize; }
  size_t partitions() const { return _partitions; }

  /**
  * @brief Slot which will be published by the next commit(); may be
  *        overwritten several times before being committed
  */
  SplitComplex& writeNext();

  /**
  * @brief Publishes the slot returned by writeNext() and advances the ring
  */
  void commit();

  /**
  * @brief Spectrum of the block offset steps ago (0 = most recent committed)
  *
  * Offsets which reach beyond the committed history return the zero
  * initialized slot which has not been written yet.
  */
  const SplitComplex& at(size_t offset) const;

private:
  size_t _blockSize;
  size_t _partitions;
  size_t _head;
  std::vector<SplitComplex*> _slots;

  SpectrumRing(const SpectrumRing&) = delete;
  SpectrumRing& operator=(const SpectrumRing&) = delete;
};

} // End of namespace fftconvolver

#endif // Header guard

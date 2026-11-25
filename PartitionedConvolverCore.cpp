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

#include "PartitionedConvolverCore.h"

#include <cassert>
#include <cstring>


namespace fftconvolver
{

PartitionedConvolverCore::PartitionedConvolverCore() :
  _blockSize(0),
  _fftSize(0),
  _partitionCount(0),
  _fftComplexSize(0),
  _fft(),
  _fftBuffer(),
  _ring(),
  _filters(),
  _scratch(0),
  _previousBlock(),
  _ringPos(0)
{
}


void PartitionedConvolverCore::reset()
{
  for (size_t i=0; i<_ring.size(); ++i)
  {
    delete _ring[i];
  }
  for (size_t i=0; i<_filters.size(); ++i)
  {
    delete _filters[i];
  }
  delete _scratch;

  _blockSize = 0;
  _fftSize = 0;
  _partitionCount = 0;
  _fftComplexSize = 0;
  _fft.init(0);
  _fftBuffer.clear();
  _ring.clear();
  _filters.clear();
  _scratch = 0;
  _previousBlock.clear();
  _ringPos = 0;
}


bool PartitionedConvolverCore::init(size_t blockSize, size_t partitionCount)
{
  reset();

  if (blockSize == 0 || partitionCount == 0)
  {
    return false;
  }

  // Block size must be a power of 2
  if ((blockSize & (blockSize - 1)) != 0)
  {
    return false;
  }

  _blockSize = blockSize;
  _fftSize = 2 * _blockSize;
  _partitionCount = partitionCount;
  _fftComplexSize = audiofft::AudioFFT::ComplexSize(_fftSize);

  _fft.init(_fftSize);
  _fftBuffer.resize(_fftSize);

  for (size_t i=0; i<_partitionCount; ++i)
  {
    _ring.push_back(new SplitComplex(_fftComplexSize));
    _filters.push_back(new SplitComplex(_fftComplexSize));
  }
  _scratch = new SplitComplex(_fftComplexSize);

  _previousBlock.resize(_blockSize);
  _previousBlock.setZero();
  _ringPos = 0;

  return true;
}


void PartitionedConvolverCore::setFilterPartition(size_t partition, const Sample* taps, size_t tapCount)
{
  assert(partition < _partitionCount);
  assert(tapCount <= _blockSize);

  CopyAndPad(_fftBuffer, taps, tapCount);
  ::memset(_fftBuffer.data() + tapCount, 0, (_fftSize - tapCount) * sizeof(Sample));
  _fft.fft(_fftBuffer.data(), _filters[partition]->re(), _filters[partition]->im());
}


void PartitionedConvolverCore::clearFilterPartition(size_t partition)
{
  assert(partition < _partitionCount);
  _filters[partition]->setZero();
}


void PartitionedConvolverCore::clearAllFilterPartitions()
{
  for (size_t i=0; i<_partitionCount; ++i)
  {
    _filters[i]->setZero();
  }
}


void PartitionedConvolverCore::clearStreaming()
{
  for (size_t i=0; i<_partitionCount; ++i)
  {
    _ring[i]->setZero();
  }
  _previousBlock.setZero();
  _ringPos = 0;
}


size_t PartitionedConvolverCore::ringIndex(size_t offset) const
{
  assert(offset < _partitionCount);
  // After pushInputBlock(), _ringPos points one past the newest slot
  return (_ringPos + 2 * _partitionCount - 1 - offset) % _partitionCount;
}


void PartitionedConvolverCore::pushInputBlock(const Sample* block)
{
  // 2B-point FFT of [previous B-frame block | current B-frame block]
  ::memcpy(_fftBuffer.data(), _previousBlock.data(), _blockSize * sizeof(Sample));
  ::memcpy(_fftBuffer.data() + _blockSize, block, _blockSize * sizeof(Sample));
  _fft.fft(_fftBuffer.data(), _ring[_ringPos]->re(), _ring[_ringPos]->im());

  ::memcpy(_previousBlock.data(), block, _blockSize * sizeof(Sample));
  _ringPos = (_ringPos + 1) % _partitionCount;
}


void PartitionedConvolverCore::pushZeroInputBlock()
{
  ::memcpy(_fftBuffer.data(), _previousBlock.data(), _blockSize * sizeof(Sample));
  ::memset(_fftBuffer.data() + _blockSize, 0, _blockSize * sizeof(Sample));
  _fft.fft(_fftBuffer.data(), _ring[_ringPos]->re(), _ring[_ringPos]->im());

  _previousBlock.setZero();
  _ringPos = (_ringPos + 1) % _partitionCount;
}


void PartitionedConvolverCore::accumulateProduct(SplitComplex& result,
                                                 size_t ringOffset,
                                                 size_t filterPartition)
{
  const size_t idx = ringIndex(ringOffset);
  ComplexMultiplyAccumulate(result, *_ring[idx], *_filters[filterPartition]);
}


void PartitionedConvolverCore::accumulateSlotProduct(SplitComplex& result,
                                                     size_t ringSlot,
                                                     size_t filterPartition)
{
  ComplexMultiplyAccumulate(result, *_ring[ringSlot], *_filters[filterPartition]);
}


const SplitComplex& PartitionedConvolverCore::ringSpectrum(size_t ringOffset) const
{
  return *_ring[ringIndex(ringOffset)];
}


SplitComplex& PartitionedConvolverCore::ringSlot(size_t slot)
{
  assert(slot < _partitionCount);
  return *_ring[slot];
}


const SplitComplex& PartitionedConvolverCore::ringSlot(size_t slot) const
{
  assert(slot < _partitionCount);
  return *_ring[slot];
}


SplitComplex& PartitionedConvolverCore::filterSpectrum(size_t partition)
{
  assert(partition < _partitionCount);
  return *_filters[partition];
}


const SplitComplex& PartitionedConvolverCore::filterSpectrum(size_t partition) const
{
  assert(partition < _partitionCount);
  return *_filters[partition];
}


void PartitionedConvolverCore::convolveBlock(Sample* result)
{
  SplitComplex& accumulator = *_scratch;
  accumulator.setZero();
  for (size_t p=0; p<_partitionCount; ++p)
  {
    accumulateProduct(accumulator, p, p);
  }
  _fft.ifft(result, accumulator.re(), accumulator.im());
}


void PartitionedConvolverCore::getFilterPartitionTaps(size_t partition, Sample* taps)
{
  assert(partition < _partitionCount);
  _fft.ifft(_fftBuffer.data(), _filters[partition]->re(), _filters[partition]->im());
  ::memcpy(taps, _fftBuffer.data(), _blockSize * sizeof(Sample));
}


Sample* PartitionedConvolverCore::fftBuffer()
{
  return _fftBuffer.data();
}

} // End of namespace fftconvolver

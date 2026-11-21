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

#include "PartitionedFFTCore.h"


namespace fftconvolver
{

SpectrumRing::SpectrumRing() :
  _blockSize(0),
  _partitions(0),
  _head(0),
  _slots()
{
}


SpectrumRing::~SpectrumRing()
{
  clear();
}


void SpectrumRing::init(size_t blockSize, size_t partitions)
{
  clear();

  if (blockSize == 0 || partitions == 0)
  {
    return;
  }

  _blockSize = blockSize;
  _partitions = partitions;
  _head = 0;

  const size_t complexSize = audiofft::AudioFFT::ComplexSize(2 * blockSize);
  _slots.reserve(partitions);
  for (size_t i=0; i<partitions; ++i)
  {
    _slots.push_back(new SplitComplex(complexSize));
  }
}


void SpectrumRing::clear()
{
  for (size_t i=0; i<_slots.size(); ++i)
  {
    delete _slots[i];
  }
  _slots.clear();
  _blockSize = 0;
  _partitions = 0;
  _head = 0;
}


void SpectrumRing::restart()
{
  for (size_t i=0; i<_slots.size(); ++i)
  {
    _slots[i]->setZero();
  }
  _head = 0;
}


SplitComplex& SpectrumRing::writeNext()
{
  assert(!_slots.empty());
  return *_slots[_head];
}


void SpectrumRing::commit()
{
  assert(!_slots.empty());
  _head = (_head + 1) % _slots.size();
}


const SplitComplex& SpectrumRing::at(size_t offset) const
{
  assert(!_slots.empty());
  assert(offset < _slots.size());
  const size_t index = (_head + _slots.size() - 1 - offset) % _slots.size();
  return *_slots[index];
}

} // End of namespace fftconvolver

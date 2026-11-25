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

#include "TailJob.h"

#include <cstring>


namespace fftconvolver
{

TailJob::TailJob() :
  _tailBlockSize(0),
  _fftSize(0),
  _fftComplexSize(0),
  _partitionCount(0),
  _irSegments(),
  _generation(0),
  _historyCount(0),
  _input(),
  _fftBuffer(),
  _result(),
  _spectrum(),
  _conv(),
  _history(),
  _fft(),
  _completed(1u)
{
}


TailJob::~TailJob()
{
  reset();
}


bool TailJob::init(size_t tailBlockSize,
                   size_t partitionCount,
                   const SplitComplex* const* irSegments)
{
  reset();

  if (tailBlockSize == 0)
  {
    return false;
  }

  _tailBlockSize = tailBlockSize;
  _fftSize = 2 * tailBlockSize;
  _fftComplexSize = audiofft::AudioFFT::ComplexSize(_fftSize);
  _partitionCount = partitionCount;
  _irSegments.assign(irSegments, irSegments + partitionCount);
  _historyCount = (partitionCount > 0) ? (partitionCount - 1) : 0;
  _generation = 0;
  _completed.store(1u, std::memory_order_relaxed);

  _fft.init(_fftSize);
  _input.resize(_tailBlockSize);
  _fftBuffer.resize(_fftSize);
  _result.resize(_fftSize);
  _spectrum.resize(_fftComplexSize);
  _conv.resize(_fftComplexSize);

  _history.clear();
  _history.reserve(_historyCount);
  for (size_t i = 0; i < _historyCount; ++i)
  {
    _history.emplace_back(new SplitComplex(_fftComplexSize));
  }

  return true;
}


void TailJob::reset()
{
  _tailBlockSize = 0;
  _fftSize = 0;
  _fftComplexSize = 0;
  _partitionCount = 0;
  _irSegments.clear();
  _generation = 0;
  _historyCount = 0;
  _input.clear();
  _fftBuffer.clear();
  _result.clear();
  _spectrum.clear();
  _conv.clear();
  _history.clear();
  _fft.init(0);
  _completed.store(1u, std::memory_order_relaxed);
}


void TailJob::prepare(uint64_t generation,
                      const Sample* inputBlock,
                      const SplitComplex* const* history,
                      size_t historyCount)
{
  _generation = generation;

  // Private copy of the triggering input block
  ::memcpy(_input.data(), inputBlock, _tailBlockSize * sizeof(Sample));

  // Private copies of the read-only input spectra required by the
  // partitioned multiply-accumulate path. Missing history (zero input
  // before the start of the stream) is represented by zero spectra.
  for (size_t i = 0; i < _historyCount; ++i)
  {
    if (i < historyCount && history[i] != nullptr)
    {
      _history[i]->copyFrom(*history[i]);
    }
    else
    {
      _history[i]->setZero();
    }
  }
}


void TailJob::arm()
{
  _completed.store(0u, std::memory_order_relaxed);
}


void TailJob::runEntry(void* arg)
{
  static_cast<TailJob*>(arg)->run();
}


void TailJob::run()
{
  // Forward FFT of the private input block
  CopyAndPad(_fftBuffer, _input.data(), _tailBlockSize);
  _fft.fft(_fftBuffer.data(), _spectrum.re(), _spectrum.im());

  // Partitioned complex multiply-accumulate:
  //   conv = X(g) * H(0) + sum_{i=1..P-1} X(g-i) * H(i)
  _conv.setZero();
  if (_partitionCount > 0 && !_irSegments.empty() && _irSegments[0] != nullptr)
  {
    ComplexMultiplyAccumulate(_conv, _spectrum, *_irSegments[0]);
  }
  for (size_t i = 1; i < _partitionCount; ++i)
  {
    const SplitComplex* irSegment = _irSegments[i];
    if (irSegment != nullptr)
    {
      ComplexMultiplyAccumulate(_conv, *_history[i - 1], *irSegment);
    }
  }

  // Inverse FFT into the private result buffer
  _fft.ifft(_result.data(), _conv.re(), _conv.im());

  // Publish as the very last action: after this store the worker no longer
  // accesses any buffer of the job.
  _completed.store(1u, std::memory_order_release);
}


bool TailJob::isCompleted() const
{
  return _completed.load(std::memory_order_acquire) != 0u;
}


uint64_t TailJob::generation() const
{
  return _generation;
}


const Sample* TailJob::result() const
{
  return _result.data();
}


size_t TailJob::tailBlockSize() const
{
  return _tailBlockSize;
}

} // End of namespace fftconvolver

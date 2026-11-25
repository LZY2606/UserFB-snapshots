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

TailPlan::TailPlan() :
  _blockSize(0),
  _fftSize(0),
  _complexSize(0),
  _segCount(0),
  _segmentsIR()
{
}


TailPlan::TailPlan(TailPlan&& other) :
  _blockSize(other._blockSize),
  _fftSize(other._fftSize),
  _complexSize(other._complexSize),
  _segCount(other._segCount),
  _segmentsIR(std::move(other._segmentsIR))
{
  other._blockSize = 0;
  other._fftSize = 0;
  other._complexSize = 0;
  other._segCount = 0;
}


TailPlan& TailPlan::operator=(TailPlan&& other)
{
  if (this != &other)
  {
    clear();
    _blockSize = other._blockSize;
    _fftSize = other._fftSize;
    _complexSize = other._complexSize;
    _segCount = other._segCount;
    _segmentsIR = std::move(other._segmentsIR);
    other._blockSize = 0;
    other._fftSize = 0;
    other._complexSize = 0;
    other._segCount = 0;
  }
  return *this;
}


TailPlan::~TailPlan()
{
  clear();
}


void TailPlan::clear()
{
  for (size_t i = 0; i < _segmentsIR.size(); ++i)
  {
    delete _segmentsIR[i];
  }
  _segmentsIR.clear();
  _blockSize = 0;
  _fftSize = 0;
  _complexSize = 0;
  _segCount = 0;
}


bool TailPlan::build(size_t tailBlockSize, const Sample* ir, size_t irLen)
{
  clear();

  if (tailBlockSize == 0)
  {
    return false;
  }

  _blockSize = tailBlockSize;
  _fftSize = 2 * _blockSize;
  _complexSize = audiofft::AudioFFT::ComplexSize(_fftSize);
  _segCount = (irLen + _blockSize - 1) / _blockSize;

  audiofft::AudioFFT fft;
  fft.init(_fftSize);
  SampleBuffer fftBuffer(_fftSize);

  _segmentsIR.reserve(_segCount);
  for (size_t i = 0; i < _segCount; ++i)
  {
    SplitComplex* segment = new SplitComplex(_complexSize);
    const size_t remaining = irLen - (i * _blockSize);
    const size_t sizeCopy = (remaining >= _blockSize) ? _blockSize : remaining;
    CopyAndPad(fftBuffer, ir + (i * _blockSize), sizeCopy);
    fft.fft(fftBuffer.data(), segment->re(), segment->im());
    _segmentsIR.push_back(segment);
  }

  return true;
}


void ComputeTailJob(const TailPlan& plan,
                    const SplitComplex* const* inputSpectra,
                    SplitComplex& accum,
                    SampleBuffer& fftBuffer,
                    audiofft::AudioFFT& fft,
                    Sample* result)
{
  const size_t segCount = plan.segmentCount();

  // Frequency-domain multiply-accumulate over all tail partitions:
  // out_spectrum = sum_j FFT(input block j generations ago) * FFT(IR partition j)
  accum.setZero();
  for (size_t j = 0; j < segCount; ++j)
  {
    ComplexMultiplyAccumulate(accum, *inputSpectra[j], plan.segmentIR(j));
  }

  // Back to the time domain: the valid linear convolution result lives in a
  // 2*blockSize window which the caller schedules at the right position.
  fft.ifft(fftBuffer.data(), accum.re(), accum.im());
  ::memcpy(result, fftBuffer.data(), (2 * plan.blockSize()) * sizeof(Sample));
}


TailJobSlot::TailJobSlot() :
  _blockSize(0),
  _segCount(0),
  _complexSize(0),
  _plan(0),
  _position(0),
  _state(IDLE),
  _snapshot(),
  _snapshotPtrs(),
  _accum(),
  _fftBuffer(),
  _fft(),
  _result()
{
}


TailJobSlot::~TailJobSlot()
{
  clear();
}


void TailJobSlot::configure(size_t blockSize, size_t segCount)
{
  clear();

  _blockSize = blockSize;
  _segCount = segCount;
  _complexSize = audiofft::AudioFFT::ComplexSize(2 * blockSize);

  // Private snapshot of the read-only input spectra: the values are copied so
  // that the audio thread can keep rotating its frequency-domain ring while a
  // worker is still running.
  _snapshot.reserve(segCount);
  _snapshotPtrs.resize(segCount);
  for (size_t i = 0; i < segCount; ++i)
  {
    SplitComplex* spectrum = new SplitComplex(_complexSize);
    _snapshot.push_back(spectrum);
    _snapshotPtrs[i] = spectrum;
  }

  _accum.resize(_complexSize);
  _fftBuffer.resize(2 * blockSize);
  _fft.init(2 * blockSize);
  _result.resize(2 * blockSize);

  _state.store(IDLE, std::memory_order_release);
}


void TailJobSlot::clear()
{
  for (size_t i = 0; i < _snapshot.size(); ++i)
  {
    delete _snapshot[i];
  }
  _snapshot.clear();
  _snapshotPtrs.clear();
  _accum.clear();
  _fftBuffer.clear();
  _fft.init(0);
  _result.clear();
  _plan = 0;
  _position = 0;
  _blockSize = 0;
  _segCount = 0;
  _complexSize = 0;
  _state.store(IDLE, std::memory_order_release);
}


void TailJobSlot::arm(uint64_t position, const TailPlan* plan, const SplitComplex* const* ringInputs)
{
  _plan = plan;
  _position = position;
  for (size_t i = 0; i < _segCount; ++i)
  {
    _snapshot[i]->copyFrom(*ringInputs[i]);
  }
}


void TailJobSlot::run(void* arg)
{
  TailJobSlot* self = static_cast<TailJobSlot*>(arg);

  // Pairs with the BUSY release-store performed before submission: regardless
  // of how the executor hands the call to its worker, this guarantees that the
  // armed snapshot and plan pointer are visible.
  (void)self->_state.load(std::memory_order_acquire);

  ComputeTailJob(*(self->_plan),
                 self->_snapshotPtrs.data(),
                 self->_accum,
                 self->_fftBuffer,
                 self->_fft,
                 self->_result.data());

  // Publish completion as the very last action: after this store the worker
  // accesses no slot memory anymore.
  self->_state.store(DONE, std::memory_order_release);
}

} // End of namespace fftconvolver

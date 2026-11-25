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

#include "FFTConvolver.h"

#include <cassert>
#include <cmath>


namespace fftconvolver
{

FFTConvolver::FFTConvolver() :
  _blockSize(0),
  _segSize(0),
  _segCount(0),
  _fftComplexSize(0),
  _core(),
  _preMultiplied(),
  _conv(),
  _overlap(),
  _current(0),
  _inputBuffer(),
  _inputBufferFill(0)
{
}


FFTConvolver::~FFTConvolver()
{
  reset();
}


void FFTConvolver::reset()
{
  _core.reset();

  _blockSize = 0;
  _segSize = 0;
  _segCount = 0;
  _fftComplexSize = 0;
  _preMultiplied.clear();
  _conv.clear();
  _overlap.clear();
  _current = 0;
  _inputBuffer.clear();
  _inputBufferFill = 0;
}


bool FFTConvolver::init(size_t blockSize, const Sample* ir, size_t irLen)
{
  reset();

  if (blockSize == 0)
  {
    return false;
  }

  // Ignore zeros at the end of the impulse response because they only waste computation time
  while (irLen > 0 && ::fabs(ir[irLen-1]) < 0.000001f)
  {
    --irLen;
  }

  if (irLen == 0)
  {
    return true;
  }

  _blockSize = NextPowerOf2(blockSize);
  _segSize = 2 * _blockSize;
  _segCount = static_cast<size_t>(::ceil(static_cast<float>(irLen) / static_cast<float>(_blockSize)));
  _fftComplexSize = audiofft::AudioFFT::ComplexSize(_segSize);

  // Shared partitioned convolution engine (FFT, input ring, filter spectra)
  if (!_core.init(_blockSize, _segCount))
  {
    return false;
  }

  // Prepare IR partitions
  for (size_t i=0; i<_segCount; ++i)
  {
    const size_t remaining = irLen - (i * _blockSize);
    const size_t sizeCopy = (remaining >= _blockSize) ? _blockSize : remaining;
    _core.setFilterPartition(i, &ir[i*_blockSize], sizeCopy);
  }

  // Prepare convolution buffers
  _preMultiplied.resize(_fftComplexSize);
  _conv.resize(_fftComplexSize);
  _overlap.resize(_blockSize);

  // Prepare input buffer
  _inputBuffer.resize(_blockSize);
  _inputBufferFill = 0;

  // Reset current position
  _current = 0;

  return true;
}


void FFTConvolver::process(const Sample* input, Sample* output, size_t len)
{
  if (_segCount == 0)
  {
    ::memset(output, 0, len * sizeof(Sample));
    return;
  }

  size_t processed = 0;
  while (processed < len)
  {
    const bool inputBufferWasEmpty = (_inputBufferFill == 0);
    const size_t processing = std::min(len-processed, _blockSize-_inputBufferFill);
    const size_t inputBufferPos = _inputBufferFill;
    ::memcpy(_inputBuffer.data()+inputBufferPos, input+processed, processing * sizeof(Sample));

    // Forward FFT
    Sample* const fftBuffer = _core.fftBuffer();
    ::memcpy(fftBuffer, &_inputBuffer[0], _blockSize * sizeof(Sample));
    ::memset(fftBuffer + _blockSize, 0, (_core.fftSize() - _blockSize) * sizeof(Sample));
    _core.fft().fft(fftBuffer,
                    _core.ringSlot(_current).re(),
                    _core.ringSlot(_current).im());

    // Complex multiplication
    if (inputBufferWasEmpty)
    {
      _preMultiplied.setZero();
      for (size_t i=1; i<_segCount; ++i)
      {
        const size_t indexIr = i;
        const size_t indexAudio = (_current + i) % _segCount;
        _core.accumulateSlotProduct(_preMultiplied, indexAudio, indexIr);
      }
    }
    _conv.copyFrom(_preMultiplied);
    _core.accumulateSlotProduct(_conv, _current, 0);

    // Backward FFT
    _core.fft().ifft(_core.fftBuffer(), _conv.re(), _conv.im());

    // Add overlap
    Sum(output+processed, _core.fftBuffer()+inputBufferPos, _overlap.data()+inputBufferPos, processing);

    // Input buffer full => Next block
    _inputBufferFill += processing;
    if (_inputBufferFill == _blockSize)
    {
      // Input buffer is empty again now
      _inputBuffer.setZero();
      _inputBufferFill = 0;

      // Save the overlap
      ::memcpy(_overlap.data(), _core.fftBuffer()+_blockSize, _blockSize * sizeof(Sample));

      // Update current segment
      _current = (_current > 0) ? (_current - 1) : (_segCount - 1);
    }

    processed += processing;
  }
}

} // End of namespace fftconvolver

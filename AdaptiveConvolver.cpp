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

#include "AdaptiveConvolver.h"

#include <cassert>
#include <cmath>
#include <cstring>


namespace fftconvolver
{

AdaptiveConvolver::AdaptiveConvolver() :
  _blockSize(0),
  _filterLen(0),
  _channels(0),
  _partitions(0),
  _segSize(0),
  _fftComplexSize(0),
  _mu(0.0f),
  _beta(0.0f),
  _delta(0.0f),
  _learning(true),
  _configured(false),
  _fft(),
  _fftBuffer(),
  _referenceSpectra(),
  _filters(),
  _power(),
  _conv(),
  _error(),
  _gradient(),
  _previousReferences(),
  _blockReferences(),
  _blockDesired(),
  _stagedPredicted(),
  _stagedResidual(),
  _queuePredicted(),
  _queueResidual(),
  _inputFill(0),
  _outWrite(0),
  _outRead(0),
  _outCount(0),
  _exportBuffer(),
  _exportSpectrum()
{
}


AdaptiveConvolver::~AdaptiveConvolver()
{
  release();
}


bool AdaptiveConvolver::init(size_t B, size_t L, size_t channels, float mu, float beta, float delta)
{
  release();

  // Validate configuration
  if (B < 8 || B > 1024 || (B & (B - 1)) != 0)
  {
    return false;
  }
  if (L < 1 || L > 65536)
  {
    return false;
  }
  if (channels < 1 || channels > 6)
  {
    return false;
  }
  if (!(mu > 0.0f) || !(mu <= 0.5f))
  {
    return false;
  }
  if (!(beta > 0.0f) || !(beta <= 1.0f))
  {
    return false;
  }
  if (!std::isfinite(delta) || !(delta > 0.0f))
  {
    return false;
  }

  _blockSize = B;
  _filterLen = L;
  _channels = channels;
  _partitions = (L + B - 1) / B;
  _segSize = 2 * B;
  _fftComplexSize = audiofft::AudioFFT::ComplexSize(_segSize);
  _mu = mu;
  _beta = beta;
  _delta = delta;
  _learning = true;
  _configured = true;

  _fft.init(_segSize);
  _fftBuffer.resize(_segSize);

  // Reference spectrum rings: P partitions per channel, all start at zero
  _referenceSpectra.resize(_channels);
  for (size_t c=0; c<_channels; ++c)
  {
    _referenceSpectra[c].init(_partitions, _fftComplexSize);
  }

  // Adaptive filter spectra H[channel][partition], all start at zero
  _filters.reserve(_channels * _partitions);
  for (size_t i=0; i<_channels * _partitions; ++i)
  {
    _filters.push_back(new SplitComplex(_fftComplexSize));
  }

  _power.resize(_fftComplexSize);
  _conv.resize(_fftComplexSize);
  _error.resize(_fftComplexSize);
  _gradient.resize(_fftComplexSize);

  _previousReferences.resize(_channels * _blockSize);
  _blockReferences.resize(_channels * _blockSize);
  _blockDesired.resize(_blockSize);
  _stagedPredicted.resize(_blockSize);
  _stagedResidual.resize(_blockSize);
  _queuePredicted.resize(2 * _blockSize);
  _queueResidual.resize(2 * _blockSize);

  _exportBuffer.resize(_segSize);
  _exportSpectrum.resize(_fftComplexSize);

  _inputFill = 0;
  _outWrite = 0;
  _outRead = 0;
  _outCount = 0;

  return true;
}


void AdaptiveConvolver::processBlock()
{
  // 1) Forward spectra X[c,m] of the concatenated [previous block | current block]
  for (size_t c=0; c<_channels; ++c)
  {
    ::memcpy(_fftBuffer.data(),
             _previousReferences.data() + c * _blockSize,
             _blockSize * sizeof(Sample));
    ::memcpy(_fftBuffer.data() + _blockSize,
             _blockReferences.data() + c * _blockSize,
             _blockSize * sizeof(Sample));
    _fft.fft(_fftBuffer.data(),
             _referenceSpectra[c].current().re(),
             _referenceSpectra[c].current().im());
  }

  // 2) Prediction with the pre-update filters; keep the second half of the IFFT
  _conv.setZero();
  for (size_t c=0; c<_channels; ++c)
  {
    for (size_t p=0; p<_partitions; ++p)
    {
      ComplexMultiplyAccumulate(_conv,
                                *_filters[filterIndex(c, p)],
                                _referenceSpectra[c].at(p));
    }
  }
  _fft.ifft(_fftBuffer.data(), _conv.re(), _conv.im());
  for (size_t i=0; i<_blockSize; ++i)
  {
    _stagedPredicted[i] = _fftBuffer[_blockSize + i];
  }

  // 3) Error spectrum E = FFT([B zeros, e])
  for (size_t i=0; i<_blockSize; ++i)
  {
    _fftBuffer[i] = 0.0f;
    const Sample e = _blockDesired[i] - _stagedPredicted[i];
    _stagedResidual[i] = e;
    _fftBuffer[_blockSize + i] = e;
  }
  _fft.fft(_fftBuffer.data(), _error.re(), _error.im());

  // 4) Per-bin power S = (1-beta)*S + beta * sum(c,p) |X[c,m-p]|^2
  for (size_t k=0; k<_fftComplexSize; ++k)
  {
    Sample powerSum = 0.0f;
    for (size_t c=0; c<_channels; ++c)
    {
      for (size_t p=0; p<_partitions; ++p)
      {
        const SplitComplex& x = _referenceSpectra[c].at(p);
        powerSum += x.re()[k] * x.re()[k] + x.im()[k] * x.im()[k];
      }
    }
    _power[k] = (1.0f - _beta) * _power[k] + _beta * powerSum;
  }

  // 5) Filter updates through the time-domain causal projection
  if (_learning)
  {
    for (size_t c=0; c<_channels; ++c)
    {
      for (size_t p=0; p<_partitions; ++p)
      {
        const SplitComplex& x = _referenceSpectra[c].at(p);

        // Frequency-domain gradient conj(X) * E / (delta + S)
        _gradient.setZero();
        ComplexConjugateMultiplyAccumulate(_gradient, x, _error);
        for (size_t k=0; k<_fftComplexSize; ++k)
        {
          const Sample inv = 1.0f / (_delta + _power[k]);
          _gradient.re()[k] *= inv;
          _gradient.im()[k] *= inv;
        }

        // IFFT (1/(2B) normalization happens inside AudioFFT)
        _fft.ifft(_fftBuffer.data(), _gradient.re(), _gradient.im());

        // Causal projection: keep only the first min(B, L-p*B) taps
        const size_t keep = std::min(_blockSize, _filterLen - p * _blockSize);
        for (size_t i=keep; i<_segSize; ++i)
        {
          _fftBuffer[i] = 0.0f;
        }

        // H += mu * FFT(g)
        _fft.fft(_fftBuffer.data(), _gradient.re(), _gradient.im());
        SplitComplex& filter = *_filters[filterIndex(c, p)];
        for (size_t k=0; k<_fftComplexSize; ++k)
        {
          filter.re()[k] += _mu * _gradient.re()[k];
          filter.im()[k] += _mu * _gradient.im()[k];
        }
      }
    }
  }

  // 6) Rotate the audio history rings and shift the reference blocks
  for (size_t c=0; c<_channels; ++c)
  {
    _referenceSpectra[c].advance();
    ::memcpy(_previousReferences.data() + c * _blockSize,
             _blockReferences.data() + c * _blockSize,
             _blockSize * sizeof(Sample));
  }

  // 7) Queue the produced block (the queue never holds more than B frames)
  for (size_t i=0; i<_blockSize; ++i)
  {
    const size_t idx = (_outWrite + i) % (2 * _blockSize);
    _queuePredicted[idx] = _stagedPredicted[i];
    _queueResidual[idx] = _stagedResidual[i];
  }
  _outWrite = (_outWrite + _blockSize) % (2 * _blockSize);
  _outCount += _blockSize;
  // The strict B-frame delay bounds the queue depth to one block
  assert(_outCount <= _blockSize);
}


void AdaptiveConvolver::process(const Sample* const* references,
                                const Sample* desired,
                                Sample* predicted,
                                Sample* residual,
                                size_t len)
{
  if (!_configured || len == 0)
  {
    return;
  }

  // Every input frame produces exactly one output frame: frames arriving
  // before the first completed block are zeros (strict B-frame delay), all
  // later frames come from the fixed-depth output queue.
  size_t pos = 0;
  while (pos < len)
  {
    const size_t room = _blockSize - _inputFill;
    const size_t processing = std::min(room, len - pos);

    // desired may alias residual: capture the target frames before emitting
    for (size_t c=0; c<_channels; ++c)
    {
      ::memcpy(_blockReferences.data() + c * _blockSize + _inputFill,
               references[c] + pos,
               processing * sizeof(Sample));
    }
    ::memcpy(_blockDesired.data() + _inputFill,
             desired + pos,
             processing * sizeof(Sample));

    for (size_t i=0; i<processing; ++i)
    {
      if (_outCount > 0)
      {
        predicted[pos + i] = _queuePredicted[_outRead];
        residual[pos + i] = _queueResidual[_outRead];
        _outRead = (_outRead + 1) % (2 * _blockSize);
        --_outCount;
      }
      else
      {
        // First B output frames of the stream are silence
        predicted[pos + i] = 0.0f;
        residual[pos + i] = 0.0f;
      }
    }

    _inputFill += processing;
    pos += processing;

    if (_inputFill == _blockSize)
    {
      processBlock();
      _inputFill = 0;
    }
  }
}


bool AdaptiveConvolver::setLearning(bool enabled)
{
  if (!_configured)
  {
    return false;
  }
  if (_inputFill != 0)
  {
    return false;
  }
  _learning = enabled;
  return true;
}


void AdaptiveConvolver::getFilter(size_t channel, Sample* taps) const
{
  if (!_configured || channel >= _channels)
  {
    return;
  }

  for (size_t p=0; p<_partitions; ++p)
  {
    const SplitComplex& filter = *_filters[filterIndex(channel, p)];
    _exportSpectrum.copyFrom(filter);
    _fft.ifft(_exportBuffer.data(), _exportSpectrum.re(), _exportSpectrum.im());
    const size_t keep = std::min(_blockSize, _filterLen - p * _blockSize);
    ::memcpy(taps + p * _blockSize,
             _exportBuffer.data(),
             keep * sizeof(Sample));
  }
}


void AdaptiveConvolver::release()
{
  for (size_t i=0; i<_filters.size(); ++i)
  {
    delete _filters[i];
  }
  _filters.clear();

  for (size_t c=0; c<_referenceSpectra.size(); ++c)
  {
    _referenceSpectra[c].clear();
  }
  _referenceSpectra.clear();

  _blockSize = 0;
  _filterLen = 0;
  _channels = 0;
  _partitions = 0;
  _segSize = 0;
  _fftComplexSize = 0;
  _mu = 0.0f;
  _beta = 0.0f;
  _delta = 0.0f;
  _learning = true;
  _configured = false;

  _fft.init(0);
  _fftBuffer.clear();
  _power.clear();
  _conv.clear();
  _error.clear();
  _gradient.clear();
  _previousReferences.clear();
  _blockReferences.clear();
  _blockDesired.clear();
  _stagedPredicted.clear();
  _stagedResidual.clear();
  _queuePredicted.clear();
  _queueResidual.clear();
  _exportBuffer.clear();
  _exportSpectrum.clear();

  _inputFill = 0;
  _outWrite = 0;
  _outRead = 0;
  _outCount = 0;
}


void AdaptiveConvolver::reset()
{
  if (!_configured)
  {
    return;
  }

  for (size_t i=0; i<_filters.size(); ++i)
  {
    _filters[i]->setZero();
  }
  for (size_t c=0; c<_channels; ++c)
  {
    _referenceSpectra[c].setZero();
  }

  _power.setZero();
  _conv.setZero();
  _error.setZero();
  _gradient.setZero();
  _previousReferences.setZero();
  _blockReferences.setZero();
  _blockDesired.setZero();
  _stagedPredicted.setZero();
  _stagedResidual.setZero();
  _queuePredicted.setZero();
  _queueResidual.setZero();

  _learning = true;
  _inputFill = 0;
  _outWrite = 0;
  _outRead = 0;
  _outCount = 0;
}

} // End of namespace fftconvolver

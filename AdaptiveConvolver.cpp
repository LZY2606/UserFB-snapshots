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

#include "AdaptiveConvolver.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>


namespace fftconvolver
{

namespace
{

inline size_t TapsInPartition(size_t partition, size_t filterLength, size_t blockSize)
{
  const size_t start = partition * blockSize;
  assert(start < filterLength);
  return std::min(blockSize, filterLength - start);
}

} // End of anonymous namespace


AdaptiveConvolver::AdaptiveConvolver() :
  _configured(false),
  _blockSize(0),
  _filterLength(0),
  _channels(0),
  _partitions(0),
  _fftSize(0),
  _fftComplexSize(0),
  _mu(0.0f),
  _beta(0.0f),
  _delta(0.0f),
  _learning(true),
  _fft(),
  _referenceHistory(),
  _filters(),
  _referenceBlock(),
  _previousReference(),
  _desiredBlock(),
  _delayedDesired(),
  _predictionBlock(),
  _blockFill(0),
  _totalFrames(0),
  _fftTime(),
  _readTime(),
  _predictionSpectrum(),
  _desiredSpectrum(),
  _powerSpectrum(),
  _gradientSpectrum(),
  _bracketSpectrum()
{
}


AdaptiveConvolver::~AdaptiveConvolver()
{
  for (size_t i=0; i<_filters.size(); ++i)
  {
    delete _filters[i];
  }
  for (size_t i=0; i<_referenceHistory.size(); ++i)
  {
    delete _referenceHistory[i];
  }
}


bool AdaptiveConvolver::isPowerOf2(size_t value)
{
  return value != 0 && (value & (value - 1)) == 0;
}


bool AdaptiveConvolver::init(size_t B, size_t L, size_t channels, float mu, float beta, float delta)
{
  if (!isPowerOf2(B) || B < 8 || B > 1024)
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

  // Free a previous configuration (if any)
  for (size_t i=0; i<_filters.size(); ++i)
  {
    delete _filters[i];
  }
  _filters.clear();
  for (size_t i=0; i<_referenceHistory.size(); ++i)
  {
    delete _referenceHistory[i];
  }
  _referenceHistory.clear();
  _fft.init(0);
  _configured = false;

  _blockSize = B;
  _filterLength = L;
  _channels = channels;
  _partitions = (L + B - 1) / B;
  _fftSize = 2 * B;
  _fftComplexSize = audiofft::AudioFFT::ComplexSize(_fftSize);
  _mu = mu;
  _beta = beta;
  _delta = delta;
  _configured = true;

  _fft.init(_fftSize);

  _filters.reserve(_channels * _partitions);
  for (size_t i=0; i<_channels * _partitions; ++i)
  {
    _filters.push_back(new SplitComplex(_fftComplexSize));
  }

  _referenceHistory.reserve(_channels);
  for (size_t c=0; c<_channels; ++c)
  {
    SpectrumRing* ring = new SpectrumRing();
    ring->init(_blockSize, _partitions);
    _referenceHistory.push_back(ring);
  }

  _referenceBlock.resize(_blockSize * _channels);
  _previousReference.resize(_blockSize * _channels);
  _desiredBlock.resize(_blockSize);
  _delayedDesired.resize(_blockSize);
  _predictionBlock.resize(_blockSize);
  _fftTime.resize(_fftSize);
  _readTime.resize(_fftSize);

  _predictionSpectrum.resize(_fftComplexSize);
  _desiredSpectrum.resize(_fftComplexSize);
  _powerSpectrum.resize(_fftComplexSize);
  _gradientSpectrum.resize(_fftComplexSize);
  _bracketSpectrum.resize(_fftComplexSize);

  clearState();

  return true;
}


void AdaptiveConvolver::clearState()
{
  for (size_t i=0; i<_filters.size(); ++i)
  {
    _filters[i]->setZero();
  }
  for (size_t i=0; i<_referenceHistory.size(); ++i)
  {
    _referenceHistory[i]->restart();
  }
  _referenceBlock.setZero();
  _previousReference.setZero();
  _desiredBlock.setZero();
  _delayedDesired.setZero();
  _predictionBlock.setZero();
  _blockFill = 0;
  _totalFrames = 0;

  _predictionSpectrum.setZero();
  _desiredSpectrum.setZero();
  _powerSpectrum.setZero();
  _gradientSpectrum.setZero();
  _bracketSpectrum.setZero();

  _learning = true;
}


void AdaptiveConvolver::reset()
{
  if (!_configured)
  {
    return;
  }
  clearState();
}


void AdaptiveConvolver::process(const Sample* const* references, const Sample* desired,
                                Sample* predicted, Sample* residual, size_t len)
{
  if (!_configured || len == 0)
  {
    return;
  }
  if (!desired || !references)
  {
    // Invalid input for a non-empty chunk: leave the state untouched.
    return;
  }

  for (size_t i=0; i<len; ++i)
  {
    const size_t blockIndex = _totalFrames / _blockSize;
    const size_t blockOffset = _totalFrames % _blockSize;

    // Read the input before writing any output, so residual may alias
    // desired.
    const Sample desiredSample = desired[i];
    const Sample* referenceSamples[6];
    for (size_t c=0; c<_channels; ++c)
    {
      referenceSamples[c] = references[c] ? (references[c] + i) : 0;
    }

    if (blockIndex == 0)
    {
      // The external delay of exactly one block: the first B output frames
      // are silent.
      if (predicted)
      {
        predicted[i] = 0.0f;
      }
      if (residual)
      {
        residual[i] = 0.0f;
      }
    }
    else
    {
      // The prediction of the previous block is read out while the current
      // block is accumulated. delayedDesired[.] holds desired[t-B].
      const Sample prediction = _predictionBlock[blockOffset];
      if (predicted)
      {
        predicted[i] = prediction;
      }
      if (residual)
      {
        residual[i] = _delayedDesired[blockOffset] - prediction;
      }
    }

    // Accumulate the current block of references and desired signal
    for (size_t c=0; c<_channels; ++c)
    {
      _referenceBlock[c * _blockSize + blockOffset] = *referenceSamples[c];
    }
    _desiredBlock[blockOffset] = desiredSample;

    ++_totalFrames;
    ++_blockFill;

    if (_blockFill == _blockSize)
    {
      processBlock();
      _blockFill = 0;
    }
  }
}


void AdaptiveConvolver::processBlock()
{
  const size_t B = _blockSize;

  // Forward FFT of the concatenation of the previous and the current
  // reference block for every channel. The block is published before any
  // filter math, so at(p) later gives X[c,m-p].
  for (size_t c=0; c<_channels; ++c)
  {
    std::memcpy(_fftTime.data(), &_previousReference[c * B], B * sizeof(Sample));
    std::memcpy(_fftTime.data() + B, &_referenceBlock[c * B], B * sizeof(Sample));

    SpectrumRing& ring = *_referenceHistory[c];
    SplitComplex& slot = ring.writeNext();
    _fft.fft(_fftTime.data(), slot.re(), slot.im());
    ring.commit();
  }

  // Predict with the filters from before the update:
  // y = last B samples of IFFT(sum(c,p) H[c,p] * X[c,m-p])
  _predictionSpectrum.setZero();
  for (size_t c=0; c<_channels; ++c)
  {
    const SpectrumRing& ring = *_referenceHistory[c];
    for (size_t p=0; p<_partitions; ++p)
    {
      ComplexMultiplyAccumulate(_predictionSpectrum, *_filters[c * _partitions + p], ring.at(p));
    }
  }

  _fft.ifft(_fftTime.data(), _predictionSpectrum.re(), _predictionSpectrum.im());
  std::memcpy(_predictionBlock.data(), _fftTime.data() + B, B * sizeof(Sample));

  // E = FFT([B zeros, e]) with e = desired block - prediction
  for (size_t i=0; i<B; ++i)
  {
    _fftTime[B + i] = _desiredBlock[i] - _predictionBlock[i];
  }
  std::memset(_fftTime.data(), 0, B * sizeof(Sample));
  _fft.fft(_fftTime.data(), _desiredSpectrum.re(), _desiredSpectrum.im());

  // S = (1-beta)*S + beta*sum(c,p) |X[c,m-p]|^2
  {
    Sample* sRe = _powerSpectrum.re();
    Sample* sIm = _powerSpectrum.im();
    const Sample oneMinusBeta = 1.0f - _beta;
    for (size_t k=0; k<_fftComplexSize; ++k)
    {
      sRe[k] *= oneMinusBeta;
      sIm[k] *= oneMinusBeta;
    }
    for (size_t c=0; c<_channels; ++c)
    {
      const SpectrumRing& ring = *_referenceHistory[c];
      for (size_t p=0; p<_partitions; ++p)
      {
        const SplitComplex& x = ring.at(p);
        const Sample* xRe = x.re();
        const Sample* xIm = x.im();
        for (size_t k=0; k<_fftComplexSize; ++k)
        {
          sRe[k] += _beta * (xRe[k] * xRe[k] + xIm[k] * xIm[k]);
        }
      }
    }
  }

  // Bracket = E / (delta + S)
  {
    const Sample* eRe = _desiredSpectrum.re();
    const Sample* eIm = _desiredSpectrum.im();
    const Sample* sRe = _powerSpectrum.re();
    Sample* bRe = _bracketSpectrum.re();
    Sample* bIm = _bracketSpectrum.im();
    for (size_t k=0; k<_fftComplexSize; ++k)
    {
      const Sample denominator = _delta + sRe[k];
      bRe[k] = eRe[k] / denominator;
      bIm[k] = eIm[k] / denominator;
    }
  }

  if (_learning)
  {
    for (size_t c=0; c<_channels; ++c)
    {
      const SpectrumRing& ring = *_referenceHistory[c];
      for (size_t p=0; p<_partitions; ++p)
      {
        const SplitComplex& x = ring.at(p);
        const size_t complexSize = _fftComplexSize;

        // g = IFFT(conj(X[c,m-p]) * bracket)
        {
          const Sample* xRe = x.re();
          const Sample* xIm = x.im();
          const Sample* bRe = _bracketSpectrum.re();
          const Sample* bIm = _bracketSpectrum.im();
          Sample* gRe = _gradientSpectrum.re();
          Sample* gIm = _gradientSpectrum.im();
          for (size_t k=0; k<complexSize; ++k)
          {
            // conj(X) * Bracket
            gRe[k] = xRe[k] * bRe[k] + xIm[k] * bIm[k];
            gIm[k] = xRe[k] * bIm[k] - xIm[k] * bRe[k];
          }
        }

        _fft.ifft(_fftTime.data(), _gradientSpectrum.re(), _gradientSpectrum.im());

        // Causal projection: only the first min(B, L-p*B) time samples may
        // act as taps; everything else is discarded.
        const size_t keep = TapsInPartition(p, _filterLength, B);
        std::memset(_fftTime.data() + keep, 0, (_fftSize - keep) * sizeof(Sample));

        // H += mu*FFT(g), effective with the next block
        _fft.fft(_fftTime.data(), _gradientSpectrum.re(), _gradientSpectrum.im());

        SplitComplex& filter = *_filters[c * _partitions + p];
        Sample* hRe = filter.re();
        Sample* hIm = filter.im();
        const Sample* gRe = _gradientSpectrum.re();
        const Sample* gIm = _gradientSpectrum.im();
        for (size_t k=0; k<complexSize; ++k)
        {
          hRe[k] += _mu * gRe[k];
          hIm[k] += _mu * gIm[k];
        }
      }
    }
  }

  // Make the next block's history: the reference block becomes the previous
  // reference block and the desired block becomes the delayed desired block.
  for (size_t c=0; c<_channels; ++c)
  {
    std::memcpy(&_previousReference[c * B], &_referenceBlock[c * B], B * sizeof(Sample));
  }
  std::memcpy(_delayedDesired.data(), _desiredBlock.data(), B * sizeof(Sample));
}


bool AdaptiveConvolver::setLearning(bool enabled)
{
  if (!_configured)
  {
    return false;
  }
  if ((_totalFrames % _blockSize) != 0)
  {
    return false;
  }
  _learning = enabled;
  return true;
}


void AdaptiveConvolver::getFilter(size_t channel, Sample* taps) const
{
  if (!_configured || !taps || channel >= _channels)
  {
    return;
  }

  // Every partition spectrum is the 2B point FFT of a real signal whose last
  // B samples are zero. The normalized inverse FFT yields the taps of that
  // partition in its first B samples; the upper half is zero by
  // construction. Using the same FFT keeps this a pure read of the current
  // coefficient spectra.
  Sample* output = taps;
  for (size_t p=0; p<_partitions; ++p)
  {
    const size_t keep = TapsInPartition(p, _filterLength, _blockSize);
    const SplitComplex& filter = *_filters[channel * _partitions + p];
    _fft.ifft(_readTime.data(), filter.re(), filter.im());
    for (size_t n=0; n<keep; ++n)
    {
      *output++ = _readTime[n];
    }
  }
}

} // End of namespace fftconvolver

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

#include <algorithm>
#include <cmath>
#include <cstring>


namespace fftconvolver
{

AdaptiveConvolver::AdaptiveConvolver() :
  _blockSize(0),
  _fftSize(0),
  _filterLength(0),
  _channels(0),
  _partitionCount(0),
  _fftComplexSize(0),
  _mu(0.0f),
  _beta(0.0f),
  _delta(0.0f),
  _learning(true),
  _cores(),
  _referenceBlocks(),
  _desiredBlock(),
  _blockPrediction(),
  _blockResidual(),
  _power(),
  _errorSpectrum(),
  _gradientSpectrum(),
  _timeBuffer(),
  _queuedPrediction(),
  _queuedResidual(),
  _queueReadPos(0),
  _outputFill(0),
  _inputFill(0)
{
}


AdaptiveConvolver::~AdaptiveConvolver()
{
  reset();
  for (size_t c=0; c<_cores.size(); ++c)
  {
    delete _cores[c];
  }
  for (size_t c=0; c<_referenceBlocks.size(); ++c)
  {
    delete _referenceBlocks[c];
  }
}


bool AdaptiveConvolver::init(size_t B, size_t L, size_t channels, float mu, float beta, float delta)
{
  // Validate configuration
  const bool blockSizeValid = (B >= 8 && B <= 1024 && (B & (B - 1)) == 0);
  const bool filterLengthValid = (L >= 1 && L <= 65536);
  const bool channelsValid = (channels >= 1 && channels <= 6);
  const bool muValid = (mu > 0.0f && mu <= 0.5f && std::isfinite(mu));
  const bool betaValid = (beta > 0.0f && beta <= 1.0f && std::isfinite(beta));
  const bool deltaValid = (delta > 0.0f && std::isfinite(delta));

  if (!blockSizeValid || !filterLengthValid || !channelsValid ||
      !muValid || !betaValid || !deltaValid)
  {
    return false;
  }

  // Release any previous configuration (init is the only allocating entry point)
  reset();
  for (size_t c=0; c<_cores.size(); ++c)
  {
    delete _cores[c];
  }
  for (size_t c=0; c<_referenceBlocks.size(); ++c)
  {
    delete _referenceBlocks[c];
  }
  _cores.clear();
  _referenceBlocks.clear();

  _blockSize = B;
  _fftSize = 2 * B;
  _filterLength = L;
  _channels = channels;
  _partitionCount = (L + B - 1) / B;
  _fftComplexSize = audiofft::AudioFFT::ComplexSize(_fftSize);
  _mu = mu;
  _beta = beta;
  _delta = delta;
  _learning = true;

  // One shared-style partitioned core per reference channel
  for (size_t c=0; c<_channels; ++c)
  {
    PartitionedConvolverCore* core = new PartitionedConvolverCore();
    if (!core->init(_blockSize, _partitionCount))
    {
      delete core;
      for (size_t k=0; k<_cores.size(); ++k)
      {
        delete _cores[k];
      }
      for (size_t k=0; k<_referenceBlocks.size(); ++k)
      {
        delete _referenceBlocks[k];
      }
      _cores.clear();
      _referenceBlocks.clear();
      _blockSize = 0;
      return false;
    }
    core->clearAllFilterPartitions(); // H starts at zero
    _cores.push_back(core);

    SampleBuffer* refBlock = new SampleBuffer(_blockSize);
    refBlock->setZero();
    _referenceBlocks.push_back(refBlock);
  }

  _desiredBlock.resize(_blockSize);
  _blockPrediction.resize(_blockSize);
  _blockResidual.resize(_blockSize);
  _power.resize(_fftComplexSize);
  _errorSpectrum.resize(_fftComplexSize);
  _gradientSpectrum.resize(_fftComplexSize);
  _timeBuffer.resize(_fftSize);

  // Delayed-output ring: holds at most the B startup zero frames plus one block
  _queuedPrediction.resize(_fftSize);
  _queuedResidual.resize(_fftSize);
  _queuedPrediction.setZero();
  _queuedResidual.setZero();
  _queueReadPos = 0;
  _outputFill = _blockSize; // The queue starts holding the B zero output frames

  _inputFill = 0;

  return true;
}


void AdaptiveConvolver::clearStreamingState()
{
  for (size_t c=0; c<_channels; ++c)
  {
    _cores[c]->clearStreaming();
    _cores[c]->clearAllFilterPartitions();
    _referenceBlocks[c]->setZero();
  }
  _desiredBlock.setZero();
  _blockPrediction.setZero();
  _blockResidual.setZero();
  _power.setZero();
  _errorSpectrum.setZero();
  _gradientSpectrum.setZero();
  _timeBuffer.setZero();
  _queuedPrediction.setZero();
  _queuedResidual.setZero();
  _queueReadPos = 0;
  _outputFill = _blockSize; // Re-establish the B zero frames of startup delay
  _inputFill = 0;
}


void AdaptiveConvolver::reset()
{
  if (_blockSize == 0)
  {
    return;
  }
  clearStreamingState();
  _learning = true;
}


bool AdaptiveConvolver::setLearning(bool enabled)
{
  // Only valid at a block boundary; a half-accumulated block keeps the switch
  if (_blockSize == 0 || _inputFill != 0)
  {
    return false;
  }
  _learning = enabled;
  return true;
}


void AdaptiveConvolver::processCompleteBlock()
{
  // --- Reference input rings: X[c,m] = FFT([block m-1 | block m]) ---
  for (size_t c=0; c<_channels; ++c)
  {
    _cores[c]->pushInputBlock(_referenceBlocks[c]->data());
  }

  // --- Prediction with the pre-update filters: back B-frame of each IFFT ---
  Sample* const timeBuffer = _timeBuffer.data();
  for (size_t c=0; c<_channels; ++c)
  {
    _cores[c]->convolveBlock(timeBuffer);
    if (c == 0)
    {
      ::memcpy(_blockPrediction.data(), timeBuffer + _blockSize, _blockSize * sizeof(Sample));
    }
    else
    {
      for (size_t i=0; i<_blockSize; ++i)
      {
        _blockPrediction[i] += timeBuffer[_blockSize + i];
      }
    }
  }

  // --- Error block e = desired - prediction ---
  for (size_t i=0; i<_blockSize; ++i)
  {
    _blockResidual[i] = _desiredBlock[i] - _blockPrediction[i];
  }

  // E = FFT([B zeros | e])
  ::memset(timeBuffer, 0, _blockSize * sizeof(Sample));
  ::memcpy(timeBuffer + _blockSize, _blockResidual.data(), _blockSize * sizeof(Sample));
  _cores[0]->fft().fft(timeBuffer, _errorSpectrum.re(), _errorSpectrum.im());

  // --- Power estimate (updated first, before any gradient) ---
  for (size_t k=0; k<_fftComplexSize; ++k)
  {
    Sample powerSum = 0.0f;
    for (size_t c=0; c<_channels; ++c)
    {
      for (size_t p=0; p<_partitionCount; ++p)
      {
        const SplitComplex& x = _cores[c]->ringSpectrum(p);
        powerSum += x.re()[k] * x.re()[k] + x.im()[k] * x.im()[k];
      }
    }
    _power[k] = (1.0f - _beta) * _power[k] + _beta * powerSum;
  }

  // --- Filter updates through the time-domain causal projection ---
  if (_learning)
  {
    for (size_t c=0; c<_channels; ++c)
    {
      PartitionedConvolverCore& core = *_cores[c];
      for (size_t p=0; p<_partitionCount; ++p)
      {
        const SplitComplex& x = core.ringSpectrum(p);
        const Sample* xre = x.re();
        const Sample* xim = x.im();
        const Sample* ere = _errorSpectrum.re();
        const Sample* eim = _errorSpectrum.im();
        Sample* gre = _gradientSpectrum.re();
        Sample* gim = _gradientSpectrum.im();

        // g = conj(X) * E / (delta + S)
        for (size_t k=0; k<_fftComplexSize; ++k)
        {
          const Sample denom = _delta + _power[k];
          gre[k] = (xre[k] * ere[k] + xim[k] * eim[k]) / denom;
          gim[k] = (xre[k] * eim[k] - xim[k] * ere[k]) / denom;
        }

        // IFFT with the 1/(2B) normalization provided by AudioFFT
        core.fft().ifft(timeBuffer, gre, gim);

        // Causal projection: keep only the first min(B, L-p*B) taps
        const size_t keep = std::min(_blockSize, _filterLength - p * _blockSize);
        if (keep < _fftSize)
        {
          ::memset(timeBuffer + keep, 0, (_fftSize - keep) * sizeof(Sample));
        }

        // Re-transform the constrained gradient and add it to the filter
        core.fft().fft(timeBuffer, gre, gim);
        SplitComplex& filter = core.filterSpectrum(p);
        for (size_t k=0; k<_fftComplexSize; ++k)
        {
          filter.re()[k] += _mu * gre[k];
          filter.im()[k] += _mu * gim[k];
        }
      }
    }
  }
}


void AdaptiveConvolver::process(const Sample* const* references,
                                const Sample* desired,
                                Sample* predicted,
                                Sample* residual,
                                size_t len)
{
  if (len == 0 || _blockSize == 0)
  {
    return;
  }

  const size_t queueCapacity = _fftSize;
  size_t inPos = 0;
  size_t outPos = 0;

  while (inPos < len || outPos < len)
  {
    // Accumulate input until a complete B-frame block is available
    if (inPos < len && _inputFill < _blockSize)
    {
      const size_t feed = std::min(len - inPos, _blockSize - _inputFill);
      for (size_t c=0; c<_channels; ++c)
      {
        ::memcpy(_referenceBlocks[c]->data() + _inputFill,
                 references[c] + inPos,
                 feed * sizeof(Sample));
      }
      // Copy desired first: residual is allowed to alias desired
      ::memcpy(_desiredBlock.data() + _inputFill,
               desired + inPos,
               feed * sizeof(Sample));
      _inputFill += feed;
      inPos += feed;

      if (_inputFill == _blockSize)
      {
        // Computes prediction/residual and (if enabled) the filter update
        processCompleteBlock();
        _inputFill = 0;

        // Enqueue the B new output frames behind the delayed-output queue
        for (size_t i=0; i<_blockSize; ++i)
        {
          const size_t idx = (_queueReadPos + _outputFill + i) % queueCapacity;
          _queuedPrediction[idx] = _blockPrediction[i];
          _queuedResidual[idx] = _blockResidual[i];
        }
        _outputFill += _blockSize;
      }
    }

    // Emit whatever output is available (greedy draining keeps the ring bounded).
    // The emitted count must never pass the input cursor: residual is allowed
    // to alias desired, so positions that have not been consumed yet are off
    // limits for output writes.
    if (outPos < len && _outputFill > 0)
    {
      size_t emit = std::min(len - outPos, _outputFill);
      emit = std::min(emit, inPos - outPos);
      size_t emitted = 0;
      while (emitted < emit)
      {
        const size_t chunk = std::min(emit - emitted, queueCapacity - _queueReadPos);
        ::memcpy(predicted + outPos,
                 _queuedPrediction.data() + _queueReadPos,
                 chunk * sizeof(Sample));
        ::memcpy(residual + outPos,
                 _queuedResidual.data() + _queueReadPos,
                 chunk * sizeof(Sample));
        _queueReadPos = (_queueReadPos + chunk) % queueCapacity;
        outPos += chunk;
        emitted += chunk;
      }
      _outputFill -= emit;
    }

    // No more input to consume and the queue cannot serve the remaining output
    if (inPos >= len && (_outputFill == 0 || outPos >= len))
    {
      break;
    }
  }
}


void AdaptiveConvolver::getFilter(size_t channel, Sample* taps) const
{
  // The readback reuses the core's FFT work buffer; it does not change the filter
  PartitionedConvolverCore& core = const_cast<PartitionedConvolverCore&>(*_cores[channel]);
  Sample* const timeBuffer = const_cast<SampleBuffer&>(_timeBuffer).data();

  for (size_t p=0; p<_partitionCount; ++p)
  {
    core.getFilterPartitionTaps(p, timeBuffer);
    const size_t keep = std::min(_blockSize, _filterLength - p * _blockSize);
    ::memcpy(taps + p * _blockSize, timeBuffer, keep * sizeof(Sample));
  }
}

} // End of namespace fftconvolver

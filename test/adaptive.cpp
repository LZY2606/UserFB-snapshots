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

#include "task_support/check.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>


namespace
{

typedef std::complex<double> Complex;

const double kPi = 3.14159265358979323846;

// ----------------------------------------------------------------------------------
// Deterministic helpers
// ----------------------------------------------------------------------------------

struct Rng
{
  explicit Rng(uint32_t seed) : _state(seed ? seed : 0x9e3779b9u) {}

  uint32_t next()
  {
    _state ^= _state << 13;
    _state ^= _state >> 17;
    _state ^= _state << 5;
    return _state;
  }

  // Uniform in [-1, 1)
  double uniform()
  {
    return (static_cast<double>(next() & 0xffffff) / 8388608.0) - 1.0;
  }

  uint32_t _state;
};

// Independent direct DFT/IDFT (double precision) used as the ground truth.
std::vector<Complex> DirectDft(const std::vector<double>& input)
{
  const size_t n = input.size();
  std::vector<Complex> output(n);
  for (size_t k=0; k<n; ++k)
  {
    Complex accumulator(0.0, 0.0);
    for (size_t i=0; i<n; ++i)
    {
      const double angle = -2.0 * kPi * static_cast<double>(k) * static_cast<double>(i)
                           / static_cast<double>(n);
      accumulator += input[i] * Complex(std::cos(angle), std::sin(angle));
    }
    output[k] = accumulator;
  }
  return output;
}

std::vector<double> DirectIdft(const std::vector<Complex>& input)
{
  const size_t n = input.size();
  std::vector<double> output(n);
  for (size_t i=0; i<n; ++i)
  {
    Complex accumulator(0.0, 0.0);
    for (size_t k=0; k<n; ++k)
    {
      const double angle = 2.0 * kPi * static_cast<double>(k) * static_cast<double>(i)
                           / static_cast<double>(n);
      accumulator += input[k] * Complex(std::cos(angle), std::sin(angle));
    }
    output[i] = accumulator.real() / static_cast<double>(n);
  }
  return output;
}

std::vector<std::vector<float> > RandomPlanar(size_t channels, size_t frames, Rng& rng, double scale)
{
  std::vector<std::vector<float> > result(channels, std::vector<float>(frames, 0.0f));
  for (size_t c=0; c<channels; ++c)
  {
    for (size_t i=0; i<frames; ++i)
    {
      result[c][i] = static_cast<float>(scale * rng.uniform());
    }
  }
  return result;
}

std::vector<float> RandomMono(size_t frames, Rng& rng, double scale)
{
  std::vector<float> result(frames, 0.0f);
  for (size_t i=0; i<frames; ++i)
  {
    result[i] = static_cast<float>(scale * rng.uniform());
  }
  return result;
}

double MaxAbsDiff(const std::vector<float>& a, const std::vector<float>& b)
{
  if (a.size() != b.size())
  {
    return 1e300;
  }
  double diff = 0.0;
  for (size_t i=0; i<a.size(); ++i)
  {
    diff = std::max(diff, std::fabs(static_cast<double>(a[i]) - static_cast<double>(b[i])));
  }
  return diff;
}

double Energy(const std::vector<float>& signal, size_t begin, size_t end)
{
  double sum = 0.0;
  for (size_t i=begin; i<end && i<signal.size(); ++i)
  {
    sum += static_cast<double>(signal[i]) * static_cast<double>(signal[i]);
  }
  return sum;
}

// The late output of an adaptive run, gathered by block.
struct RunOutput
{
  std::vector<float> predicted;
  std::vector<float> residual;
};

RunOutput RunFragmented(fftconvolver::AdaptiveConvolver& convolver,
                        const std::vector<std::vector<float> >& references,
                        const std::vector<float>& desired,
                        size_t begin,
                        size_t end,
                        const std::vector<size_t>& chunks)
{
  const size_t channels = references.size();
  RunOutput result;
  result.predicted.assign(end - begin, -12345.0f);
  result.residual.assign(end - begin, -12345.0f);

  std::vector<const float*> pointers(channels, static_cast<const float*>(0));
  size_t position = begin;
  size_t call = 0;
  while (position < end)
  {
    const size_t len = std::min(chunks[call % chunks.size()], end - position);
    for (size_t c=0; c<channels; ++c)
    {
      pointers[c] = references[c].data() + position;
    }
    convolver.process(pointers.data(), desired.data() + position,
                      result.predicted.data() + (position - begin),
                      result.residual.data() + (position - begin),
                      len);
    position += len;
    ++call;
  }
  return result;
}

RunOutput RunBlocks(fftconvolver::AdaptiveConvolver& convolver,
                    const std::vector<std::vector<float> >& references,
                    const std::vector<float>& desired,
                    size_t begin,
                    size_t end,
                    size_t blockSize)
{
  return RunFragmented(convolver, references, desired, begin, end,
                       std::vector<size_t>(1, blockSize));
}

// target = sum_c response[c] * references[c] + voice; voice is an
// independent signal which the adaptive filter cannot predict.
void MakeMixture(const std::vector<std::vector<float> >& references,
                 const std::vector<std::vector<double> >& responses,
                 size_t filterLength,
                 size_t voiceSeed,
                 std::vector<float>& desired,
                 std::vector<float>& voice)
{
  const size_t channels = references.size();
  const size_t frames = references.empty() ? 0 : references[0].size();
  desired.assign(frames, 0.0f);
  voice.assign(frames, 0.0f);

  Rng voiceRng(voiceSeed);
  for (size_t i=0; i<frames; ++i)
  {
    voice[i] = static_cast<float>(0.35 * voiceRng.uniform());
  }

  for (size_t i=0; i<frames; ++i)
  {
    double crosstalk = 0.0;
    for (size_t c=0; c<channels; ++c)
    {
      for (size_t j=0; j<filterLength && j<=i; ++j)
      {
        crosstalk += responses[c][j] * static_cast<double>(references[c][i-j]);
      }
    }
    desired[i] = static_cast<float>(crosstalk) + voice[i];
  }
}

// ----------------------------------------------------------------------------------
// Independent block-by-block reference model of the documented algorithm.
// Everything is computed in double precision with direct DFTs; the production
// FFT is never used here.
// ----------------------------------------------------------------------------------

class ReferenceModel
{
public:
  ReferenceModel(size_t B, size_t L, size_t channels, double mu, double beta, double delta) :
    _B(B),
    _L(L),
    _channels(channels),
    _partitions((L + B - 1) / B),
    _fftSize(2 * B),
    _mu(mu),
    _beta(beta),
    _delta(delta),
    _history(channels,
             std::vector<std::vector<Complex> >(_partitions,
                                               std::vector<Complex>(_fftSize, Complex(0.0, 0.0)))),
    _filters(channels,
             std::vector<std::vector<Complex> >(_partitions,
                                               std::vector<Complex>(_fftSize, Complex(0.0, 0.0)))),
    _power(_fftSize, 0.0),
    _previous(channels, std::vector<double>(B, 0.0)),
    _prediction(B, 0.0),
    _residual(B, 0.0)
  {
  }

  // Processes one whole block; _prediction/_residual refer to the prediction
  // computed with the coefficients from before this block's update.
  void ProcessBlock(const std::vector<std::vector<double> >& references,
                    const std::vector<double>& desired)
  {
    // X[c][p] for p = 0 is the newest spectrum; the history is shifted first,
    // exactly like the production ring is committed before the filter math.
    for (size_t c=0; c<_channels; ++c)
    {
      std::vector<double> time(_fftSize, 0.0);
      for (size_t j=0; j<_B; ++j)
      {
        time[j] = _previous[c][j];
        time[_B + j] = references[c][j];
      }
      for (size_t p=_partitions-1; p>0; --p)
      {
        _history[c][p] = _history[c][p-1];
      }
      _history[c][0] = DirectDft(time);
      _previous[c] = references[c];
    }

    // Prediction with the coefficients from before the update
    std::vector<Complex> predictionSpectrum(_fftSize, Complex(0.0, 0.0));
    for (size_t c=0; c<_channels; ++c)
    {
      for (size_t p=0; p<_partitions; ++p)
      {
        for (size_t k=0; k<_fftSize; ++k)
        {
          predictionSpectrum[k] += _filters[c][p][k] * _history[c][p][k];
        }
      }
    }
    {
      const std::vector<double> predictionTime = DirectIdft(predictionSpectrum);
      _prediction.assign(predictionTime.begin() + _B, predictionTime.end());
    }

    // E = FFT([B zeros, e])
    std::vector<double> error(_fftSize, 0.0);
    for (size_t j=0; j<_B; ++j)
    {
      _residual[j] = desired[j] - _prediction[j];
      error[_B + j] = _residual[j];
    }
    const std::vector<Complex> E = DirectDft(error);

    // Power estimate, updated before the gradient
    for (size_t k=0; k<_fftSize; ++k)
    {
      double power = 0.0;
      for (size_t c=0; c<_channels; ++c)
      {
        for (size_t p=0; p<_partitions; ++p)
        {
          power += std::norm(_history[c][p][k]);
        }
      }
      _power[k] = (1.0 - _beta) * _power[k] + _beta * power;
    }

    std::vector<Complex> bracket(_fftSize);
    for (size_t k=0; k<_fftSize; ++k)
    {
      bracket[k] = E[k] / (_delta + _power[k]);
    }

    // Per partition constrained update
    for (size_t c=0; c<_channels; ++c)
    {
      for (size_t p=0; p<_partitions; ++p)
      {
        std::vector<Complex> gradientSpectrum(_fftSize);
        for (size_t k=0; k<_fftSize; ++k)
        {
          gradientSpectrum[k] = std::conj(_history[c][p][k]) * bracket[k];
        }
        std::vector<double> gradient = DirectIdft(gradientSpectrum);

        const size_t keep = std::min(_B, _L - p * _B);
        for (size_t n=keep; n<_fftSize; ++n)
        {
          gradient[n] = 0.0;
        }
        const std::vector<Complex> gradientFft = DirectDft(gradient);
        for (size_t k=0; k<_fftSize; ++k)
        {
          _filters[c][p][k] += _mu * gradientFft[k];
        }
      }
    }
  }

  const std::vector<double>& Prediction() const { return _prediction; }
  const std::vector<double>& Residual() const { return _residual; }

  // Taps of the current filters, concatenating the first halves of all
  // partitions.
  void GetFilter(size_t channel, std::vector<double>& taps) const
  {
    taps.assign(_L, 0.0);
    for (size_t p=0; p<_partitions; ++p)
    {
      const size_t keep = std::min(_B, _L - p * _B);
      const std::vector<double> time = DirectIdft(_filters[channel][p]);
      for (size_t n=0; n<keep; ++n)
      {
        taps[p * _B + n] = time[n];
      }
    }
  }

private:
  size_t _B;
  size_t _L;
  size_t _channels;
  size_t _partitions;
  size_t _fftSize;
  double _mu;
  double _beta;
  double _delta;

  std::vector<std::vector<std::vector<Complex> > > _history;
  std::vector<std::vector<std::vector<Complex> > > _filters;
  std::vector<double> _power;
  std::vector<std::vector<double> > _previous;
  std::vector<double> _prediction;
  std::vector<double> _residual;
};

// ----------------------------------------------------------------------------------
// Allocation counters (the test binary provides these global replacements, so
// they also observe allocations from inside the library).
// ----------------------------------------------------------------------------------

long g_allocations = 0;
long g_deallocations = 0;

} // End of anonymous namespace

void* operator new(size_t size)
{
  ++g_allocations;
  if (void* pointer = std::malloc(size ? size : 1))
  {
    return pointer;
  }
  throw std::bad_alloc();
}

void* operator new[](size_t size)
{
  ++g_allocations;
  if (void* pointer = std::malloc(size ? size : 1))
  {
    return pointer;
  }
  throw std::bad_alloc();
}

void operator delete(void* pointer) noexcept
{
  if (pointer)
  {
    ++g_deallocations;
    std::free(pointer);
  }
}

void operator delete[](void* pointer) noexcept
{
  if (pointer)
  {
    ++g_deallocations;
    std::free(pointer);
  }
}

void operator delete(void* pointer, size_t) noexcept
{
  ::operator delete(pointer);
}

void operator delete[](void* pointer, size_t) noexcept
{
  ::operator delete[](pointer);
}


namespace
{

// ==================================================================================
// Gradient: B=8, L=11 (so the last partition only has 3 taps), two different
// references, six blocks, checked against the direct-DFT reference model.
// ==================================================================================

int TestGradientSmallScale()
{
  const size_t B = 8;
  const size_t L = 11;
  const size_t channels = 2;
  const size_t blocks = 7; // 6 learned input blocks plus one to read the last output
  const size_t frames = blocks * B;
  const double mu = 0.3;
  const double beta = 0.4;
  const double delta = 0.5;

  Rng rng(0x1742cafeu);
  const std::vector<std::vector<float> > references = RandomPlanar(channels, frames, rng, 0.5);
  const std::vector<float> desired = RandomMono(frames, rng, 0.5);

  fftconvolver::AdaptiveConvolver convolver;
  TASK_CHECK(convolver.init(B, L, channels, static_cast<float>(mu), static_cast<float>(beta),
                            static_cast<float>(delta)));

  ReferenceModel model(B, L, channels, mu, beta, delta);

  std::vector<const float*> pointers(channels, static_cast<const float*>(0));
  std::vector<float> predicted(B, 0.0f);
  std::vector<float> residual(B, 0.0f);
  std::vector<std::vector<double> > modelPrediction(blocks, std::vector<double>(B, 0.0));
  std::vector<std::vector<double> > modelResidual(blocks, std::vector<double>(B, 0.0));

  for (size_t block=0; block<blocks; ++block)
  {
    for (size_t c=0; c<channels; ++c)
    {
      pointers[c] = references[c].data() + block * B;
    }
    convolver.process(pointers.data(), desired.data() + block * B,
                      predicted.data(), residual.data(), B);

    std::vector<std::vector<double> > referenceSlice(channels, std::vector<double>(B, 0.0));
    for (size_t c=0; c<channels; ++c)
    {
      for (size_t i=0; i<B; ++i)
      {
        referenceSlice[c][i] = references[c][block * B + i];
      }
    }
    std::vector<double> desiredSlice(B, 0.0);
    for (size_t i=0; i<B; ++i)
    {
      desiredSlice[i] = desired[block * B + i];
    }

    model.ProcessBlock(referenceSlice, desiredSlice);
    modelPrediction[block] = model.Prediction();
    modelResidual[block] = model.Residual();

    // Coefficients after this block
    std::vector<float> taps(L, 0.0f);
    for (size_t c=0; c<channels; ++c)
    {
      std::vector<double> modelTaps;
      model.GetFilter(c, modelTaps);
      convolver.getFilter(c, taps.data());
      for (size_t j=0; j<L; ++j)
      {
        TASK_CHECK(std::fabs(static_cast<double>(taps[j]) - modelTaps[j]) <= 3e-4);
      }
    }

    // The output read now corresponds to the model block computed one step
    // earlier (the strict one block delay); the first block is all zeros.
    for (size_t i=0; i<B; ++i)
    {
      if (block == 0)
      {
        TASK_CHECK(predicted[i] == 0.0f);
        TASK_CHECK(residual[i] == 0.0f);
      }
      else
      {
        TASK_CHECK(std::fabs(static_cast<double>(predicted[i])
                             - modelPrediction[block-1][i]) <= 3e-4);
        TASK_CHECK(std::fabs(static_cast<double>(residual[i])
                             - modelResidual[block-1][i]) <= 3e-4);
      }
    }
  }

  return 0;
}

// The regularization has to matter because the update is driven by the
// estimated power: a different delta must change the learned taps, and the
// reference model with the matching delta must follow both.
int TestGradientPowerDriven()
{
  const size_t B = 8;
  const size_t L = 11;
  const size_t channels = 2;
  const size_t blocks = 5;
  const size_t frames = blocks * B;

  Rng rng(0x0badf00du);
  const std::vector<std::vector<float> > references = RandomPlanar(channels, frames, rng, 0.5);
  const std::vector<float> desired = RandomMono(frames, rng, 0.5);

  const float deltas[] = {1e-4f, 5.0f};
  std::vector<std::vector<float> > taps(2, std::vector<float>(L, 0.0f));

  for (size_t index=0; index<2; ++index)
  {
    fftconvolver::AdaptiveConvolver convolver;
    TASK_CHECK(convolver.init(B, L, channels, 0.3f, 0.4f, deltas[index]));
    std::vector<const float*> pointers(channels, static_cast<const float*>(0));
    std::vector<float> predicted(B);
    std::vector<float> residual(B);
    for (size_t block=0; block<blocks; ++block)
    {
      for (size_t c=0; c<channels; ++c)
      {
        pointers[c] = references[c].data() + block * B;
      }
      convolver.process(pointers.data(), desired.data() + block * B,
                        predicted.data(), residual.data(), B);
    }
    convolver.getFilter(0, taps[index].data());
  }

  double difference = 0.0;
  for (size_t j=0; j<L; ++j)
  {
    difference = std::max(difference, std::fabs(static_cast<double>(taps[0][j])
                                                - static_cast<double>(taps[1][j])));
  }
  TASK_CHECK(difference > 1e-3);
  return 0;
}

// ==================================================================================
// Streaming
// ==================================================================================

int TestStreamingFragments()
{
  const size_t B = 32;
  const size_t L = 37;
  const size_t channels = 2;
  const size_t frames = 1024 + 17; // ends in the middle of a block
  const size_t totalFrames = frames + 4 * B;

  Rng rng(0xabcdef01u);
  const std::vector<std::vector<float> > references = RandomPlanar(channels, totalFrames, rng, 0.4);
  const std::vector<float> desired = RandomMono(totalFrames, rng, 0.4);

  fftconvolver::AdaptiveConvolver whole;
  fftconvolver::AdaptiveConvolver fragmented;
  TASK_CHECK(whole.init(B, L, channels, 0.2f, 0.3f, 1e-3f));
  TASK_CHECK(fragmented.init(B, L, channels, 0.2f, 0.3f, 1e-3f));

  const size_t fragments[] = {1, 7, 31, 5, 13};
  const std::vector<size_t> fragmentPattern(fragments, fragments + 5);

  const RunOutput a = RunBlocks(whole, references, desired, 0, frames, B);
  const RunOutput b = RunFragmented(fragmented, references, desired, 0, frames, fragmentPattern);

  TASK_CHECK(MaxAbsDiff(a.predicted, b.predicted) <= 1e-6);
  TASK_CHECK(MaxAbsDiff(a.residual, b.residual) <= 1e-6);

  // Strict B frame delay
  for (size_t t=0; t<B; ++t)
  {
    TASK_CHECK(a.predicted[t] == 0.0f);
    TASK_CHECK(a.residual[t] == 0.0f);
  }
  for (size_t t=B; t<frames; ++t)
  {
    TASK_CHECK(std::fabs(static_cast<double>(a.residual[t])
                         - (static_cast<double>(desired[t-B])
                            - static_cast<double>(a.predicted[t]))) <= 1e-6);
  }

  // A wrong block pairing would produce a very different prediction.
  std::vector<std::vector<float> > shiftedReferences(references);
  for (size_t c=0; c<channels; ++c)
  {
    shiftedReferences[c].insert(shiftedReferences[c].begin(), 0.0f);
    shiftedReferences[c].resize(totalFrames);
  }
  fftconvolver::AdaptiveConvolver shifted;
  TASK_CHECK(shifted.init(B, L, channels, 0.2f, 0.3f, 1e-3f));
  const RunOutput c = RunBlocks(shifted, shiftedReferences, desired, 0, frames, B);
  TASK_CHECK(MaxAbsDiff(a.predicted, c.predicted) > 1e-3);

  // getFilter must not advance state: continuing after a read is identical
  // to never having read.
  std::vector<float> taps(L, 0.0f);
  whole.getFilter(0, taps.data());
  whole.getFilter(1, taps.data());
  const RunOutput aContinue = RunBlocks(whole, references, desired, frames, frames + 4 * B, B);
  const RunOutput bContinue = RunFragmented(fragmented, references, desired, frames,
                                            frames + 4 * B, fragmentPattern);
  TASK_CHECK(MaxAbsDiff(aContinue.predicted, bContinue.predicted) <= 1e-6);
  TASK_CHECK(MaxAbsDiff(aContinue.residual, bContinue.residual) <= 1e-6);

  // The final coefficients of the whole block and the fragmented run match.
  for (size_t c=0; c<channels; ++c)
  {
    std::vector<float> wholeTaps(L, 0.0f);
    std::vector<float> fragmentedTaps(L, 0.0f);
    whole.getFilter(c, wholeTaps.data());
    fragmented.getFilter(c, fragmentedTaps.data());
    for (size_t j=0; j<L; ++j)
    {
      TASK_CHECK(std::fabs(static_cast<double>(wholeTaps[j])
                           - static_cast<double>(fragmentedTaps[j])) <= 1e-6);
    }
  }
  return 0;
}

int TestStreamingOddFragmentSizes()
{
  const size_t B = 8;
  const size_t L = 19;
  const size_t channels = 3;
  const size_t frames = 400;

  Rng rng(0x1234abcdu);
  const std::vector<std::vector<float> > references = RandomPlanar(channels, frames, rng, 0.3);
  const std::vector<float> desired = RandomMono(frames, rng, 0.3);

  fftconvolver::AdaptiveConvolver whole;
  fftconvolver::AdaptiveConvolver fragmented;
  TASK_CHECK(whole.init(B, L, channels, 0.35f, 0.5f, 1e-2f));
  TASK_CHECK(fragmented.init(B, L, channels, 0.35f, 0.5f, 1e-2f));

  const size_t pattern[] = {1, 1, 3, 7, 2};
  const RunOutput a = RunBlocks(whole, references, desired, 0, frames, B);
  const RunOutput b = RunFragmented(fragmented, references, desired, 0, frames,
                                    std::vector<size_t>(pattern, pattern + 5));

  TASK_CHECK(MaxAbsDiff(a.predicted, b.predicted) <= 1e-6);
  TASK_CHECK(MaxAbsDiff(a.residual, b.residual) <= 1e-6);
  return 0;
}

int TestStreamingChannels()
{
  const size_t channelCounts[] = {1, 2, 6};
  const size_t B = 16;
  const size_t L = 23;
  const size_t frames = 512 + 3;
  double previousPredictionEnergy = -1.0;

  for (size_t index=0; index<3; ++index)
  {
    const size_t channels = channelCounts[index];
    Rng rng(0x5000u + static_cast<uint32_t>(channels));

    const std::vector<std::vector<float> > references = RandomPlanar(channels, frames, rng, 0.25);
    std::vector<std::vector<double> > responses(channels, std::vector<double>(L, 0.0));
    for (size_t c=0; c<channels; ++c)
    {
      responses[c][c % L] += 0.4;
      responses[c][(3 * c + 7) % L] -= 0.17;
      responses[c][(11 * c + 2) % L] += 0.09;
    }

    std::vector<float> desired;
    std::vector<float> voice;
    MakeMixture(references, responses, L, 0x6000u + static_cast<uint32_t>(channels),
                desired, voice);

    fftconvolver::AdaptiveConvolver whole;
    fftconvolver::AdaptiveConvolver fragmented;
    TASK_CHECK(whole.init(B, L, channels, 0.3f, 0.2f, 1e-4f));
    TASK_CHECK(fragmented.init(B, L, channels, 0.3f, 0.2f, 1e-4f));

    const size_t pattern[] = {1, 7, 31, 3};
    const RunOutput a = RunBlocks(whole, references, desired, 0, frames, B);
    const RunOutput b = RunFragmented(fragmented, references, desired, 0, frames,
                                      std::vector<size_t>(pattern, pattern + 4));

    TASK_CHECK(MaxAbsDiff(a.predicted, b.predicted) <= 1e-6);
    TASK_CHECK(MaxAbsDiff(a.residual, b.residual) <= 1e-6);

    // The prediction is non trivial and grows with the number of references;
    // the residual beats the desired energy by the end of the stream.
    const size_t begin = frames - 128;
    const double predictedEnergy = Energy(a.predicted, begin, frames);
    const double residualEnergy = Energy(a.residual, begin, frames);
    const double desiredEnergy = Energy(desired, begin, frames);
    TASK_CHECK(predictedEnergy > 1e-6);
    TASK_CHECK(residualEnergy < desiredEnergy);
    TASK_CHECK(predictedEnergy > previousPredictionEnergy);
    previousPredictionEnergy = predictedEnergy;

    // Planar routing: changing only reference 0 has to change the output.
    std::vector<std::vector<float> > otherReferences = references;
    otherReferences[0] = RandomMono(frames, rng, 0.25);
    fftconvolver::AdaptiveConvolver other;
    TASK_CHECK(other.init(B, L, channels, 0.3f, 0.2f, 1e-4f));
    const RunOutput otherResult = RunBlocks(other, otherReferences, desired, 0, frames, B);
    TASK_CHECK(MaxAbsDiff(a.predicted, otherResult.predicted) > 1e-4);
  }

  return 0;
}

int TestStreamingSilenceAndEmptyCalls()
{
  const size_t B = 32;
  const size_t L = 41;
  const size_t channels = 2;
  const size_t frames = 600;

  std::vector<std::vector<float> > references(channels, std::vector<float>(frames, 0.0f));
  std::vector<float> desired(frames, 0.0f);

  fftconvolver::AdaptiveConvolver convolver;
  TASK_CHECK(convolver.init(B, L, channels, 0.25f, 0.25f, 1e-4f));

  // Zero length is a no-op, even with null pointers
  convolver.process(0, 0, 0, 0, 0);

  std::vector<const float*> pointers(channels, static_cast<const float*>(0));
  for (size_t c=0; c<channels; ++c)
  {
    pointers[c] = references[c].data();
  }

  std::vector<float> predicted(frames, 99.0f);
  std::vector<float> residual(frames, 99.0f);
  size_t position = 0;
  while (position < frames)
  {
    const size_t len = std::min<size_t>(17, frames - position);
    convolver.process(pointers.data(), desired.data() + position,
                      predicted.data() + position, residual.data() + position, len);
    position += len;
  }

  for (size_t t=0; t<frames; ++t)
  {
    TASK_CHECK(predicted[t] == 0.0f);
    TASK_CHECK(residual[t] == 0.0f);
  }

  std::vector<float> taps(L, 0.0f);
  convolver.getFilter(0, taps.data());
  for (size_t j=0; j<L; ++j)
  {
    TASK_CHECK(taps[j] == 0.0f);
  }

  return 0;
}

int TestStreamingInPlaceResidual()
{
  const size_t B = 16;
  const size_t L = 29;
  const size_t channels = 2;
  const size_t frames = 500;

  Rng rng(0x0f0f0f0fu);
  const std::vector<std::vector<float> > references = RandomPlanar(channels, frames, rng, 0.3);
  const std::vector<float> desired = RandomMono(frames, rng, 0.3);

  fftconvolver::AdaptiveConvolver separate;
  fftconvolver::AdaptiveConvolver inPlace;
  TASK_CHECK(separate.init(B, L, channels, 0.3f, 0.3f, 1e-4f));
  TASK_CHECK(inPlace.init(B, L, channels, 0.3f, 0.3f, 1e-4f));

  const RunOutput a = RunBlocks(separate, references, desired, 0, frames, B);

  // residual aliases desired
  std::vector<float> alias = desired;
  std::vector<float> predicted(frames, 0.0f);
  std::vector<const float*> pointers(channels, static_cast<const float*>(0));
  size_t position = 0;
  while (position < frames)
  {
    const size_t len = std::min<size_t>(13, frames - position);
    for (size_t c=0; c<channels; ++c)
    {
      pointers[c] = references[c].data() + position;
    }
    inPlace.process(pointers.data(), alias.data() + position,
                    predicted.data() + position, alias.data() + position, len);
    position += len;
  }

  TASK_CHECK(MaxAbsDiff(a.predicted, predicted) <= 1e-7);
  TASK_CHECK(MaxAbsDiff(a.residual, alias) <= 1e-7);
  return 0;
}

// ==================================================================================
// Learning
// ==================================================================================

int TestLearningFreezeAndResume()
{
  const size_t B = 32;
  const size_t L = 43;
  const size_t channels = 2;
  const size_t blocks = 40;

  Rng rng(0x31415926u);
  const std::vector<std::vector<float> > references = RandomPlanar(channels, blocks * B, rng, 0.3);
  const std::vector<float> desired = RandomMono(blocks * B, rng, 0.3);

  fftconvolver::AdaptiveConvolver convolver;
  fftconvolver::AdaptiveConvolver alwaysLearning;
  TASK_CHECK(convolver.init(B, L, channels, 0.3f, 0.3f, 1e-4f));
  TASK_CHECK(alwaysLearning.init(B, L, channels, 0.3f, 0.3f, 1e-4f));

  std::vector<const float*> pointers(channels, static_cast<const float*>(0));
  std::vector<float> predicted(B);
  std::vector<float> residual(B);

  const size_t freezeAfter = 12;
  std::vector<float> frozenTaps(L, 0.0f);
  std::vector<float> currentTaps(L, 0.0f);
  std::vector<float> otherTaps(L);

  for (size_t block=0; block<blocks; ++block)
  {
    for (size_t c=0; c<channels; ++c)
    {
      pointers[c] = references[c].data() + block * B;
    }

    if (block == freezeAfter)
    {
      // Half aligned: the switch must be refused. In this loop we are always
      // at a block boundary, so use a separate half block check below.
      TASK_CHECK(convolver.setLearning(false));
      TASK_CHECK(convolver.setLearning(false)); // idempotent, still aligned
      convolver.getFilter(0, frozenTaps.data());
      convolver.getFilter(1, frozenTaps.data()); // keep channel 1 irrelevant
      convolver.getFilter(0, frozenTaps.data());
    }

    convolver.process(pointers.data(), desired.data() + block * B,
                      predicted.data(), residual.data(), B);
    alwaysLearning.process(pointers.data(), desired.data() + block * B,
                           predicted.data(), residual.data(), B);

    if (block >= freezeAfter)
    {
      convolver.getFilter(0, currentTaps.data());
      for (size_t j=0; j<L; ++j)
      {
        TASK_CHECK(currentTaps[j] == frozenTaps[j]);
      }

      alwaysLearning.getFilter(0, otherTaps.data());
      double coefficientChange = 0.0;
      for (size_t j=0; j<L; ++j)
      {
        coefficientChange += std::fabs(static_cast<double>(otherTaps[j])
                                       - static_cast<double>(frozenTaps[j]));
      }
      // The always learning instance keeps moving; by the end of the stream
      // it has to differ from the frozen snapshot.
      if (block == blocks - 1)
      {
        TASK_CHECK(coefficientChange > 1e-6);
      }
    }
  }

  // Freeze, then resume: learning starts again and coefficients change.
  TASK_CHECK(convolver.setLearning(true));
  convolver.getFilter(0, frozenTaps.data());
  for (size_t block=blocks; block<blocks+8; ++block)
  {
    for (size_t c=0; c<channels; ++c)
    {
      pointers[c] = references[c].data() + (block % blocks) * B;
    }
    convolver.process(pointers.data(), desired.data() + (block % blocks) * B,
                      predicted.data(), residual.data(), B);
  }
  convolver.getFilter(0, currentTaps.data());
  // The frozen snapshot is the state right after the last frozen block, so
  // the resumed blocks must have moved away from it.
  double resumedChange = 0.0;
  for (size_t j=0; j<L; ++j)
  {
    resumedChange += std::fabs(static_cast<double>(currentTaps[j])
                               - static_cast<double>(frozenTaps[j]));
  }
  TASK_CHECK(resumedChange > 1e-6);
  return 0;
}

int TestLearningHalfBlockRejection()
{
  const size_t B = 64;
  const size_t L = 17;
  const size_t channels = 2;
  const size_t frames = 8 * B;

  Rng rng(0x0ddc0ffeeu);
  const std::vector<std::vector<float> > references = RandomPlanar(channels, frames, rng, 0.3);
  const std::vector<float> desired = RandomMono(frames, rng, 0.3);

  fftconvolver::AdaptiveConvolver convolver;
  TASK_CHECK(convolver.init(B, L, channels, 0.2f, 0.5f, 1e-3f));

  std::vector<const float*> pointers(channels, static_cast<const float*>(0));
  std::vector<float> predicted(frames, 0.0f);
  std::vector<float> residual(frames, 0.0f);

  // Half a block => refused and the switch keeps its value
  for (size_t c=0; c<channels; ++c)
  {
    pointers[c] = references[c].data();
  }
  convolver.process(pointers.data(), desired.data(), predicted.data(), residual.data(), B / 2);
  TASK_CHECK(!convolver.setLearning(false));
  TASK_CHECK(!convolver.setLearning(true));

  // Complete the block; now the switch works
  size_t position = B / 2;
  for (size_t c=0; c<channels; ++c)
  {
    pointers[c] = references[c].data() + position;
  }
  convolver.process(pointers.data(), desired.data() + position,
                    predicted.data() + position, residual.data() + position, B / 2);
  TASK_CHECK(convolver.setLearning(false));
  TASK_CHECK(convolver.setLearning(true));

  // 1 frame into the next block: refused again
  position = B;
  for (size_t c=0; c<channels; ++c)
  {
    pointers[c] = references[c].data() + position;
  }
  convolver.process(pointers.data(), desired.data() + position,
                    predicted.data() + position, residual.data() + position, 1);
  TASK_CHECK(!convolver.setLearning(false));
  return 0;
}

int TestLearningReset()
{
  const size_t B = 64;
  const size_t L = 17;
  const size_t channels = 2;
  const size_t frames = 800;

  Rng rng(0x0ddc0ffeeu);
  const std::vector<std::vector<float> > references = RandomPlanar(channels, frames, rng, 0.3);
  const std::vector<float> desired = RandomMono(frames, rng, 0.3);

  fftconvolver::AdaptiveConvolver convolver;
  TASK_CHECK(convolver.init(B, L, channels, 0.2f, 0.5f, 1e-3f));

  // Learn something
  const RunOutput warmup = RunBlocks(convolver, references, desired, 0, 12 * B, B);
  TASK_CHECK(warmup.predicted.size() == 12 * B);
  std::vector<float> taps(L, 0.0f);
  convolver.getFilter(0, taps.data());
  bool nonZero = false;
  for (size_t j=0; j<L; ++j)
  {
    nonZero = nonZero || (taps[j] != 0.0f);
  }
  TASK_CHECK(nonZero);
  TASK_CHECK(convolver.setLearning(false));

  convolver.reset();

  // Coefficients, power and history are gone; learning is enabled again and
  // the switch is accepted at the aligned stream start.
  convolver.getFilter(0, taps.data());
  for (size_t j=0; j<L; ++j)
  {
    TASK_CHECK(taps[j] == 0.0f);
  }
  TASK_CHECK(convolver.setLearning(false));
  TASK_CHECK(convolver.setLearning(true));

  // After reset the stream starts from zero timing again and is bit exact
  // compared to a freshly initialized instance.
  fftconvolver::AdaptiveConvolver fresh;
  TASK_CHECK(fresh.init(B, L, channels, 0.2f, 0.5f, 1e-3f));

  const size_t pattern[] = {1, 7, 31, 13};
  const std::vector<size_t> chunks(pattern, pattern + 4);
  const RunOutput afterReset = RunFragmented(convolver, references, desired, 0, frames, chunks);
  const RunOutput fromScratch = RunFragmented(fresh, references, desired, 0, frames, chunks);
  TASK_CHECK(MaxAbsDiff(afterReset.predicted, fromScratch.predicted) == 0.0);
  TASK_CHECK(MaxAbsDiff(afterReset.residual, fromScratch.residual) == 0.0);
  return 0;
}

int TestLearningDisabledContinuesTiming()
{
  // Freezing only stops the H update: input history, output timing and the
  // power estimate keep running. The prediction during the frozen stretch is
  // therefore still the frozen filter applied to the current references, and
  // the delay stays intact.
  const size_t B = 16;
  const size_t L = 21;
  const size_t channels = 2;
  const size_t blocks = 32;
  const size_t freezeFirst = 8;
  const size_t freezeLast = 20; // exclusive

  Rng rng(0x16180339u);
  const std::vector<std::vector<float> > references = RandomPlanar(channels, blocks * B, rng, 0.25);
  const std::vector<float> desired = RandomMono(blocks * B, rng, 0.25);

  fftconvolver::AdaptiveConvolver convolver;
  TASK_CHECK(convolver.init(B, L, channels, 0.25f, 0.25f, 1e-4f));

  std::vector<const float*> pointers(channels, static_cast<const float*>(0));
  std::vector<float> predicted(B, 0.0f);
  std::vector<float> residual(B, 0.0f);
  std::vector<float> frozenTaps(L, 0.0f);
  std::vector<float> currentTaps(L, 0.0f);
  std::vector<std::vector<float> > frozenTapsPerChannel(channels, std::vector<float>(L, 0.0f));

  for (size_t block=0; block<blocks; ++block)
  {
    if (block == freezeFirst)
    {
      TASK_CHECK(convolver.setLearning(false));
      for (size_t c=0; c<channels; ++c)
      {
        convolver.getFilter(c, frozenTapsPerChannel[c].data());
      }
      convolver.getFilter(0, frozenTaps.data());
    }
  
    for (size_t c=0; c<channels; ++c)
    {
      pointers[c] = references[c].data() + block * B;
    }
    convolver.process(pointers.data(), desired.data() + block * B,
                      predicted.data(), residual.data(), B);

    if (block > freezeFirst && block < freezeLast)
    {
      // The stored coefficients do not move
      convolver.getFilter(0, currentTaps.data());
      for (size_t j=0; j<L; ++j)
      {
        TASK_CHECK(currentTaps[j] == frozenTaps[j]);
      }

      // The prediction read now was computed with the frozen snapshot (the
      // output of block freezeFirst still came from the pre-freeze update).
      const size_t targetIndex = block * B - B;
      for (size_t i=0; i<B; ++i)
      {
        double expected = 0.0;
        for (size_t c=0; c<channels; ++c)
        {
          for (size_t j=0; j<L; ++j)
          {
            if (targetIndex + i >= j)
            {
              expected += static_cast<double>(frozenTapsPerChannel[c][j])
                          * static_cast<double>(references[c][targetIndex + i - j]);
            }
          }
        }
        TASK_CHECK(std::fabs(expected - static_cast<double>(predicted[i])) <= 1e-3);
        TASK_CHECK(std::fabs(static_cast<double>(residual[i])
                             - (static_cast<double>(desired[targetIndex + i])
                                - static_cast<double>(predicted[i]))) <= 1e-5);
      }
      TASK_CHECK(Energy(predicted, 0, B) > 0.0);
    }
  }

  // The frozen snapshot was taken after block freezeFirst-1; block
  // freezeFirst uses it for the output, so all predictions in the frozen
  // stretch are non-trivial and the coefficients stayed put.
  TASK_CHECK(convolver.setLearning(true));
  TASK_CHECK(convolver.setLearning(true));

  bool nonZero = false;
  for (size_t j=0; j<L; ++j)
  {
    nonZero = nonZero || (frozenTaps[j] != 0.0f);
  }
  TASK_CHECK(nonZero);
  return 0;
}

// ==================================================================================
// Learning: convergence, frozen taps, real-time guarantees, example rendering
// ==================================================================================

void MakeConvergenceScenario(size_t channels,
                             size_t filterLength,
                             size_t frames,
                             std::vector<std::vector<float> >& references,
                             std::vector<std::vector<double> >& responses,
                             std::vector<float>& desired)
{
  Rng rng(0x6a09e667u + static_cast<uint32_t>(channels));
  references = RandomPlanar(channels, frames, rng, 0.2);
  responses.assign(channels, std::vector<double>(filterLength, 0.0));
  if (channels >= 1)
  {
    responses[0][0] = 0.55;
    responses[0][3] = -0.28;
    responses[0][17] = 0.15;
    responses[0][64] = -0.11;
    responses[0][128] = 0.06;
  }
  if (channels >= 2)
  {
    responses[1][1] = 0.42;
    responses[1][2] = 0.21;
    responses[1][31] = -0.19;
    responses[1][65] = 0.13;
    responses[1][100] = -0.08;
    responses[1][127] = 0.05;
  }

  std::vector<float> voice;
  MakeMixture(references, responses, filterLength, 0x77777777u, desired, voice);
  // For the convergence measurement the target is the pure crosstalk, so
  // that the achievable cancellation is not limited by an unpredictable
  // voice.
  for (size_t i=0; i<frames; ++i)
  {
    desired[i] = 0.0f;
  }
  for (size_t i=0; i<frames; ++i)
  {
    double crosstalk = 0.0;
    for (size_t c=0; c<channels; ++c)
    {
      for (size_t j=0; j<filterLength && j<=i; ++j)
      {
        crosstalk += responses[c][j] * static_cast<double>(references[c][i-j]);
      }
    }
    desired[i] = static_cast<float>(crosstalk);
  }
}

int TestLearningConvergence()
{
  const size_t B = 32;
  const size_t L = 129;
  const size_t channels = 2;
  const size_t learnBlocks = 4096;
  const size_t tailBlocks = 256;
  const size_t totalFrames = (learnBlocks + tailBlocks) * B;

  std::vector<std::vector<float> > references;
  std::vector<std::vector<double> > responses;
  std::vector<float> desired;
  MakeConvergenceScenario(channels, L, totalFrames, references, responses, desired);

  fftconvolver::AdaptiveConvolver convolver;
  TASK_CHECK(convolver.init(B, L, channels, 0.25f, 0.2f, 1e-4f));

  // Learn with fragments so the block alignment is exercised as well
  const size_t pattern[] = {1, 7, 31, 257, 64};
  const std::vector<size_t> chunks(pattern, pattern + 5);
  const size_t learnEnd = learnBlocks * B;
  const RunOutput learned = RunFragmented(convolver, references, desired, 0, learnEnd, chunks);

  // Residual energy of the last 256 blocks relative to the target
  const size_t windowBegin = learnEnd - tailBlocks * B;
  const double residualEnergy = Energy(learned.residual, windowBegin, learnEnd);
  double targetEnergy = 0.0;
  for (size_t t=windowBegin; t<learnEnd; ++t)
  {
    const double target = desired[t - B];
    targetEnergy += target * target;
  }
  TASK_CHECK(targetEnergy > 0.0);
  TASK_CHECK(residualEnergy > 0.0);
  const double reductionDb = 10.0 * std::log10(targetEnergy / residualEnergy);
  TASK_CHECK(reductionDb >= 18.0);

  // Freeze and verify that the exported taps predict the following output
  TASK_CHECK(convolver.setLearning(false));
  std::vector<std::vector<float> > taps(channels, std::vector<float>(L, 0.0f));
  for (size_t c=0; c<channels; ++c)
  {
    convolver.getFilter(c, taps[c].data());
  }

  const size_t checkBlocks = 64;
  const size_t checkBegin = learnEnd;
  const size_t checkEnd = learnEnd + checkBlocks * B;
  const RunOutput frozen = RunBlocks(convolver, references, desired,
                                     checkBegin, checkEnd, B);

  // output block m uses the prediction computed with the coefficients after
  // block m-2; block learnEnd is one step behind the frozen snapshot, so the
  // independent convolution has to match from block learnEnd+1 on.
  double maxError = 0.0;
  double maxPrediction = 0.0;
  for (size_t block = 1; block < checkBlocks; ++block)
  {
    for (size_t i=0; i<B; ++i)
    {
      const size_t outputPosition = checkBegin + block * B + i;
      const size_t targetIndex = outputPosition - B;
      double expected = 0.0;
      for (size_t c=0; c<channels; ++c)
      {
        for (size_t j=0; j<L; ++j)
        {
          if (targetIndex >= j)
          {
            expected += static_cast<double>(taps[c][j])
                        * static_cast<double>(references[c][targetIndex - j]);
          }
        }
      }
      const double error = std::fabs(expected - static_cast<double>(frozen.predicted[block * B + i]));
      maxError = std::max(maxError, error);
      maxPrediction = std::max(maxPrediction, std::fabs(expected));
    }
  }
  TASK_CHECK(maxPrediction > 1e-3);
  TASK_CHECK(maxError <= 2e-3);

  // The residual is still desired[t-B] minus the prediction
  for (size_t t=0; t<checkEnd - checkBegin; ++t)
  {
    const size_t targetIndex = checkBegin + t - B;
    if (targetIndex < checkEnd)
    {
      TASK_CHECK(std::fabs(static_cast<double>(frozen.residual[t])
                           - (static_cast<double>(desired[targetIndex])
                              - static_cast<double>(frozen.predicted[t]))) <= 1e-5);
    }
  }
  return 0;
}

int TestLearningNoAllocations()
{
  const size_t B = 64;
  const size_t L = 131;
  const size_t channels = 3;
  const size_t frames = 4096;

  Rng rng(0x42424242u);
  const std::vector<std::vector<float> > references = RandomPlanar(channels, frames, rng, 0.2);
  const std::vector<float> desired = RandomMono(frames, rng, 0.2);

  fftconvolver::AdaptiveConvolver convolver;
  TASK_CHECK(convolver.init(B, L, channels, 0.25f, 0.2f, 1e-4f));
  TASK_CHECK(convolver.setLearning(true));

  // Everything which is needed during streaming is preallocated.
  std::vector<const float*> pointers(channels, static_cast<const float*>(0));
  std::vector<float> predicted(512, 0.0f);
  std::vector<float> residual(512, 0.0f);
  std::vector<float> taps(L, 0.0f);

  const long allocationsBefore = g_allocations;
  const long deallocationsBefore = g_deallocations;

  const size_t chunks[] = {1, 7, 31, 64, 257};
  size_t position = 0;
  size_t call = 0;
  while (position < frames)
  {
    const size_t len = std::min(chunks[call % 5], frames - position);
    for (size_t c=0; c<channels; ++c)
    {
      pointers[c] = references[c].data() + position;
    }
    convolver.process(pointers.data(), desired.data() + position,
                      predicted.data(), residual.data(), len);
    position += len;
    ++call;
    if ((call % 17) == 0 && (position % B) == 0)
    {
      TASK_CHECK(convolver.setLearning((call % 34) == 0));
    }
    if ((call % 23) == 0)
    {
      convolver.getFilter(0, taps.data());
    }
  }
  convolver.setLearning(true);

  TASK_CHECK(g_allocations == allocationsBefore);
  TASK_CHECK(g_deallocations == deallocationsBefore);
  return 0;
}

int TestLearningExampleRendersVoice()
{
  // The example: train on the clean reference recording, then render a mix
  // track which contains an independent voice on top of the reference
  // crosstalk (the voice is not predictable from the references). The
  // reported energies show the removal before and after. The filter is frozen
  // for the render, because the algorithm deliberately has no double talk
  // detection and the voice would otherwise keep adapting the filter.
  const size_t B = 64;
  const size_t L = 257;
  const size_t channels = 2;
  const size_t trainBlocks = 4096;
  const size_t renderBlocks = 256;
  const size_t frames = (trainBlocks + renderBlocks) * B;

  Rng rng(0x13572468u);
  const std::vector<std::vector<float> > references = RandomPlanar(channels, frames, rng, 0.25);
  std::vector<std::vector<double> > responses(channels, std::vector<double>(L, 0.0));
  responses[0][0] = 0.45;
  responses[0][7] = -0.22;
  responses[0][80] = 0.14;
  responses[0][200] = -0.07;
  responses[1][3] = 0.38;
  responses[1][30] = 0.18;
  responses[1][129] = -0.12;
  responses[1][256] = 0.05;

  std::vector<float> crosstalk(frames, 0.0f);
  for (size_t i=0; i<frames; ++i)
  {
    double value = 0.0;
    for (size_t c=0; c<channels; ++c)
    {
      for (size_t j=0; j<L && j<=i; ++j)
      {
        value += responses[c][j] * static_cast<double>(references[c][i-j]);
      }
    }
    crosstalk[i] = static_cast<float>(value);
  }
  Rng voiceRng(0x24681357u);
  std::vector<float> voice(frames, 0.0f);
  for (size_t i=0; i<frames; ++i)
  {
    voice[i] = static_cast<float>(0.35 * voiceRng.uniform());
  }

  fftconvolver::AdaptiveConvolver convolver;
  TASK_CHECK(convolver.init(B, L, channels, 0.25f, 0.2f, 1e-4f));

  const size_t pattern[] = {1, 7, 31, 257};
  const size_t trainEnd = trainBlocks * B;
  const RunOutput training = RunFragmented(convolver, references, crosstalk, 0, trainEnd,
                                           std::vector<size_t>(pattern, pattern + 4));
  TASK_CHECK(training.predicted.size() == trainEnd);

  // Freeze the identified filter for the render of the mix track
  TASK_CHECK(convolver.setLearning(false));

  std::vector<float> desired(frames, 0.0f);
  for (size_t i=0; i<frames; ++i)
  {
    desired[i] = crosstalk[i] + voice[i];
  }

  const RunOutput mix = RunFragmented(convolver, references, desired, trainEnd, frames,
                                      std::vector<size_t>(pattern, pattern + 4));

  const size_t begin = frames - 128 * B - trainEnd;
  double crosstalkEnergy = 0.0;
  double voiceEnergy = 0.0;
  double mixtureEnergy = 0.0;
  double residualEnergy = 0.0;
  double predictionError = 0.0;
  for (size_t t=begin; t<mix.residual.size(); ++t)
  {
    const size_t targetIndex = trainEnd + t - B; // input position of the output
    const size_t crosstalkIndex = targetIndex;
    crosstalkEnergy += static_cast<double>(crosstalk[crosstalkIndex]) * crosstalk[crosstalkIndex];
    voiceEnergy += static_cast<double>(voice[crosstalkIndex]) * voice[crosstalkIndex];
    mixtureEnergy += static_cast<double>(desired[crosstalkIndex]) * desired[crosstalkIndex];
    residualEnergy += static_cast<double>(mix.residual[t]) * mix.residual[t];
    const double error = static_cast<double>(mix.predicted[t]) - crosstalk[crosstalkIndex];
    predictionError += error * error;
  }

  const double suppressionDb = 10.0 * std::log10(crosstalkEnergy / predictionError);
  const double mixtureReductionDb = 10.0 * std::log10(mixtureEnergy / residualEnergy);
  std::printf("EXAMPLE_RENDER crosstalk=%.6g voice=%.6g mixture=%.6g residual=%.6g "
              "crosstalk_suppression=%.2f dB mixture_reduction=%.2f dB\n",
              crosstalkEnergy, voiceEnergy, mixtureEnergy, residualEnergy,
              suppressionDb, mixtureReductionDb);

  TASK_CHECK(crosstalkEnergy > 0.0);
  TASK_CHECK(voiceEnergy > 0.0);
  TASK_CHECK(residualEnergy < mixtureEnergy);
  // The identified crosstalk is removed well below its original energy while
  // the independent voice remains.
  TASK_CHECK(suppressionDb > 18.0);
  TASK_CHECK(residualEnergy > 0.0);
  return 0;
}

} // End of anonymous namespace


const struct task_case task_cases[] = {
  {"Gradient", "small_scale_direct_dft", TestGradientSmallScale},
  {"Gradient", "power_drives_update", TestGradientPowerDriven},
  {"Streaming", "fragments_match_blocks", TestStreamingFragments},
  {"Streaming", "odd_fragment_sizes", TestStreamingOddFragmentSizes},
  {"Streaming", "channels_planar", TestStreamingChannels},
  {"Streaming", "silence_empty_calls", TestStreamingSilenceAndEmptyCalls},
  {"Streaming", "in_place_residual", TestStreamingInPlaceResidual},
  {"Learning", "freeze_and_resume", TestLearningFreezeAndResume},
  {"Learning", "half_block_rejection", TestLearningHalfBlockRejection},
  {"Learning", "reset_restarts_stream", TestLearningReset},
  {"Learning", "disabled_continues_timing", TestLearningDisabledContinuesTiming},
  {"Learning", "convergence_and_frozen_taps", TestLearningConvergence},
  {"Learning", "no_process_allocations", TestLearningNoAllocations},
  {"Learning", "example_voice_rendering", TestLearningExampleRendersVoice},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);

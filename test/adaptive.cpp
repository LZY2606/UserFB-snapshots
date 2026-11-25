// ==================================================================================
// Adaptive convolver task tests
//
// Registered through task_support/check.h (no main() in this file).
// The Gradient cases validate the production implementation against an
// independent double-precision direct DFT model - the production FFT is never
// used as a reference.
// ==================================================================================

#include "AdaptiveConvolver.h"
#include "task_support/check.h"

#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <vector>


namespace
{

const double kPi = 3.14159265358979323846;

// ----------------------------------------------------------------------------
// Deterministic signal generation
// ----------------------------------------------------------------------------

uint32_t xorshift(uint32_t& state)
{
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}


float nextSample(uint32_t& state)
{
  return static_cast<float>(static_cast<int32_t>(xorshift(state) & 0xFFFFu) - 32768) / 100000.0f;
}


std::vector<std::vector<float> > makeReferences(size_t channels, size_t frames, uint32_t seed)
{
  std::vector<std::vector<float> > refs(channels, std::vector<float>(frames, 0.0f));
  for (size_t c=0; c<channels; ++c)
  {
    uint32_t state = seed + static_cast<uint32_t>(c) * 2654435761u;
    for (size_t i=0; i<frames; ++i)
    {
      refs[c][i] = nextSample(state);
    }
  }
  return refs;
}


std::vector<float> makeDesired(size_t frames, uint32_t seed)
{
  std::vector<float> desired(frames, 0.0f);
  uint32_t state = seed;
  for (size_t i=0; i<frames; ++i)
  {
    desired[i] = nextSample(state);
  }
  return desired;
}


// ----------------------------------------------------------------------------
// Independent double-precision DFT model (unscaled forward, 1/N inverse)
// ----------------------------------------------------------------------------

typedef std::complex<double> Cplx;


void directDft(const std::vector<double>& in, std::vector<Cplx>& out, bool inverse)
{
  const size_t n = in.size();
  out.assign(n, Cplx(0.0, 0.0));
  const double sign = inverse ? +1.0 : -1.0;
  for (size_t k=0; k<n; ++k)
  {
    Cplx sum(0.0, 0.0);
    for (size_t j=0; j<n; ++j)
    {
      const double angle = sign * 2.0 * kPi * static_cast<double>(k * j) / static_cast<double>(n);
      sum += in[j] * Cplx(std::cos(angle), std::sin(angle));
    }
    out[k] = inverse ? sum / static_cast<double>(n) : sum;
  }
}


void directDft(const std::vector<Cplx>& in, std::vector<Cplx>& out, bool inverse)
{
  const size_t n = in.size();
  out.assign(n, Cplx(0.0, 0.0));
  const double sign = inverse ? +1.0 : -1.0;
  for (size_t k=0; k<n; ++k)
  {
    Cplx sum(0.0, 0.0);
    for (size_t j=0; j<n; ++j)
    {
      const double angle = sign * 2.0 * kPi * static_cast<double>(k * j) / static_cast<double>(n);
      sum += in[j] * Cplx(std::cos(angle), std::sin(angle));
    }
    out[k] = inverse ? sum / static_cast<double>(n) : sum;
  }
}


/**
* @brief Independent reference model of the adaptive algorithm
*/
class ReferenceModel
{
public:
  ReferenceModel(size_t B, size_t L, size_t channels, double mu, double beta, double delta) :
    _B(B),
    _L(L),
    _channels(channels),
    _P((L + B - 1) / B),
    _N(2 * B),
    _mu(mu),
    _beta(beta),
    _delta(delta),
    _H(channels * _P, std::vector<Cplx>(_N, Cplx(0.0, 0.0))),
    _ring(channels, std::vector<std::vector<Cplx> >(_P, std::vector<Cplx>(_N, Cplx(0.0, 0.0)))),
    _S(_N, 0.0)
  {
  }

  // Processes one B-frame block, appending predicted/residual output frames
  void processBlock(const std::vector<std::vector<double> >& refs,
                    const std::vector<double>& desired,
                    size_t blockIndex,
                    std::vector<double>& predicted,
                    std::vector<double>& residual,
                    std::vector<std::vector<double> >& taps)
  {
    const size_t m = blockIndex;

    // X[c,m] = DFT of [previous B frames | current B frames], zeros before stream start
    std::vector<std::vector<Cplx> > xCurrent(_channels, std::vector<Cplx>(_N));
    for (size_t c=0; c<_channels; ++c)
    {
      std::vector<double> segment(_N, 0.0);
      for (size_t n=0; n<_N; ++n)
      {
        const long globalIndex = static_cast<long>((m - 1) * _B + n);
        if (globalIndex >= 0)
        {
          segment[n] = refs[c][static_cast<size_t>(globalIndex)];
        }
      }
      directDft(segment, xCurrent[c], false);
    }

    // Spectrum of block m-p (p == 0 is the just transformed current block)
    auto spectrumAt = [&](size_t c, size_t p) -> const std::vector<Cplx>&
    {
      static const std::vector<Cplx> zero(_N, Cplx(0.0, 0.0));
      if (p == 0)
      {
        return xCurrent[c];
      }
      if (m < p)
      {
        return zero;
      }
      return _ring[c][p - 1];
    };

    // Prediction using the pre-update filters
    std::vector<Cplx> ySpectrum(_N, Cplx(0.0, 0.0));
    for (size_t c=0; c<_channels; ++c)
    {
      for (size_t p=0; p<_P; ++p)
      {
        const std::vector<Cplx>& x = spectrumAt(c, p);
        for (size_t k=0; k<_N; ++k)
        {
          ySpectrum[k] += _H[c * _P + p][k] * x[k];
        }
      }
    }
    std::vector<Cplx> yComplex;
    directDft(ySpectrum, yComplex, true);

    std::vector<double> e(_B, 0.0);
    for (size_t i=0; i<_B; ++i)
    {
      const double pred = yComplex[_B + i].real();
      predicted.push_back(pred);
      e[i] = desired[m * _B + i] - pred;
      residual.push_back(e[i]);
    }

    // E = DFT([B zeros, e])
    std::vector<double> ePadded(_N, 0.0);
    for (size_t i=0; i<_B; ++i)
    {
      ePadded[_B + i] = e[i];
    }
    std::vector<Cplx> eSpectrum;
    directDft(ePadded, eSpectrum, false);

    // S = (1-beta)*S + beta * sum(c,p) |X[c,m-p]|^2
    for (size_t k=0; k<_N; ++k)
    {
      double powerSum = 0.0;
      for (size_t c=0; c<_channels; ++c)
      {
        for (size_t p=0; p<_P; ++p)
        {
          const Cplx& x = spectrumAt(c, p)[k];
          powerSum += x.real() * x.real() + x.imag() * x.imag();
        }
      }
      _S[k] = (1.0 - _beta) * _S[k] + _beta * powerSum;
    }

    // Updates through the time-domain causal projection
    for (size_t c=0; c<_channels; ++c)
    {
      for (size_t p=0; p<_P; ++p)
      {
        const std::vector<Cplx>& x = spectrumAt(c, p);
        std::vector<Cplx> gradient(_N);
        for (size_t k=0; k<_N; ++k)
        {
          gradient[k] = std::conj(x[k]) * eSpectrum[k] / (_delta + _S[k]);
        }
        std::vector<Cplx> gComplex;
        directDft(gradient, gComplex, true);
        std::vector<double> g(_N, 0.0);
        const size_t keep = std::min(_B, _L - p * _B);
        for (size_t i=0; i<keep; ++i)
        {
          g[i] = gComplex[i].real();
        }
        std::vector<Cplx> gSpectrum;
        directDft(g, gSpectrum, false);
        for (size_t k=0; k<_N; ++k)
        {
          _H[c * _P + p][k] += _mu * gSpectrum[k];
        }
      }
    }

    // Current spectra become the oldest visible history for the next block
    for (size_t c=0; c<_channels; ++c)
    {
      for (size_t p=_P - 1; p>0; --p)
      {
        _ring[c][p] = _ring[c][p - 1];
      }
      _ring[c][0] = xCurrent[c];
    }

    // Export current time-domain taps
    taps.assign(_channels, std::vector<double>(_L, 0.0));
    for (size_t c=0; c<_channels; ++c)
    {
      for (size_t p=0; p<_P; ++p)
      {
        std::vector<Cplx> hTime;
        directDft(_H[c * _P + p], hTime, true);
        const size_t keep = std::min(_B, _L - p * _B);
        for (size_t i=0; i<keep; ++i)
        {
          taps[c][p * _B + i] = hTime[i].real();
        }
      }
    }
  }

private:
  size_t _B;
  size_t _L;
  size_t _channels;
  size_t _P;
  size_t _N;
  double _mu;
  double _beta;
  double _delta;
  std::vector<std::vector<Cplx> > _H;
  std::vector<std::vector<std::vector<Cplx> > > _ring;
  std::vector<double> _S;
};


// ----------------------------------------------------------------------------
// Allocation accounting: process()/setLearning() must not touch the heap
// ----------------------------------------------------------------------------

uint64_t g_newCalls = 0;
uint64_t g_deleteCalls = 0;


template<typename T>
std::vector<const T*> planePointers(const std::vector<std::vector<T> >& planes)
{
  std::vector<const T*> pointers(planes.size(), nullptr);
  for (size_t c=0; c<planes.size(); ++c)
  {
    pointers[c] = planes[c].data();
  }
  return pointers;
}

} // anonymous namespace


void* operator new(std::size_t size)
{
  ++g_newCalls;
  void* ptr = std::malloc(size);
  if (!ptr) throw std::bad_alloc();
  return ptr;
}

void* operator new[](std::size_t size)
{
  ++g_newCalls;
  void* ptr = std::malloc(size);
  if (!ptr) throw std::bad_alloc();
  return ptr;
}

void operator delete(void* ptr) noexcept
{
  ++g_deleteCalls;
  std::free(ptr);
}

void operator delete[](void* ptr) noexcept
{
  ++g_deleteCalls;
  std::free(ptr);
}

void operator delete(void* ptr, std::size_t) noexcept
{
  ++g_deleteCalls;
  std::free(ptr);
}

void operator delete[](void* ptr, std::size_t) noexcept
{
  ++g_deleteCalls;
  std::free(ptr);
}


// ============================================================================
// Gradient group: block-by-block validation against the double DFT model
// ============================================================================

static int gradientAgainstDirectDft()
{
  const size_t B = 8;
  const size_t L = 11;
  const size_t channels = 2;
  const size_t P = 2;
  const size_t checkedBlocks = 6;
  const float mu = 0.1f;
  const float beta = 0.3f;
  const float delta = 1e-3f;
  const double tol = 3e-4;

  const size_t frames = (checkedBlocks + 1) * B;
  const std::vector<std::vector<float> > refsF = makeReferences(channels, frames, 0x1234abcd);
  const std::vector<float> desiredF = makeDesired(frames, 0x9e3779b9);

  std::vector<std::vector<double> > refs(channels, std::vector<double>(frames, 0.0));
  std::vector<double> desired(frames, 0.0);
  for (size_t c=0; c<channels; ++c)
    for (size_t i=0; i<frames; ++i)
      refs[c][i] = static_cast<double>(refsF[c][i]);
  for (size_t i=0; i<frames; ++i)
    desired[i] = static_cast<double>(desiredF[i]);

  fftconvolver::AdaptiveConvolver convolver;
  TASK_CHECK(convolver.init(B, L, channels, mu, beta, delta));

  ReferenceModel model(B, L, channels, mu, beta, delta);
  std::vector<double> modelPred;
  std::vector<double> modelRes;
  std::vector<std::vector<double> > modelTaps;

  std::vector<float> pred(B, 0.0f);
  std::vector<float> residual(B, 0.0f);
  std::vector<float> taps(L, 0.0f);

  for (size_t m=0; m<checkedBlocks; ++m)
  {
    // References and target both advance by one B-frame block per call
    std::vector<const float*> blockRefs(channels, nullptr);
    for (size_t c=0; c<channels; ++c)
    {
      blockRefs[c] = refsF[c].data() + m * B;
    }
    convolver.process(blockRefs.data(),
                      desiredF.data() + m * B,
                      pred.data(),
                      residual.data(),
                      B);

    model.processBlock(refs, desired, m, modelPred, modelRes, modelTaps);

    if (m > 0)
    {
      const size_t modelBlock = m - 1;
      for (size_t i=0; i<B; ++i)
      {
        const size_t idx = modelBlock * B + i;
        TASK_CHECK(std::fabs(static_cast<double>(pred[i]) - modelPred[idx]) <= tol);
        TASK_CHECK(std::fabs(static_cast<double>(residual[i]) - modelRes[idx]) <= tol);
      }
    }
    else
    {
      // Strict delay: the first B outputs are zeros
      for (size_t i=0; i<B; ++i)
      {
        TASK_CHECK(pred[i] == 0.0f);
        TASK_CHECK(residual[i] == 0.0f);
      }
    }

    // Taps after this block's update, including the short last partition
    for (size_t c=0; c<channels; ++c)
    {
      convolver.getFilter(c, taps.data());
      for (size_t l=0; l<L; ++l)
      {
        TASK_CHECK(std::fabs(static_cast<double>(taps[l]) - modelTaps[c][l]) <= tol);
      }
    }
  }

  // The last partition only keeps 3 of B taps: feed one more block and make
  // sure the projected taps still track the model for every partition.
  std::vector<const float*> tailRefs(channels, nullptr);
  for (size_t c=0; c<channels; ++c) tailRefs[c] = refsF[c].data() + checkedBlocks * B;
  convolver.process(tailRefs.data(), desiredF.data() + checkedBlocks * B,
                    pred.data(), residual.data(), B);
  model.processBlock(refs, desired, checkedBlocks, modelPred, modelRes, modelTaps);

  // This seventh call emits the sixth model block (index checkedBlocks-1),
  // so prediction and residual are checked for every one of the first six blocks
  const size_t emittedBlock = checkedBlocks - 1;
  for (size_t i=0; i<B; ++i)
  {
    const size_t idx = emittedBlock * B + i;
    TASK_CHECK(std::fabs(static_cast<double>(pred[i]) - modelPred[idx]) <= tol);
    TASK_CHECK(std::fabs(static_cast<double>(residual[i]) - modelRes[idx]) <= tol);
  }

  for (size_t c=0; c<channels; ++c)
  {
    convolver.getFilter(c, taps.data());
    for (size_t l=0; l<L; ++l)
    {
      TASK_CHECK(std::fabs(static_cast<double>(taps[l]) - modelTaps[c][l]) <= tol);
    }
  }
  TASK_CHECK(P == 2);

  return 0;
}


static int gradientInvalidConfigurations()
{
  fftconvolver::AdaptiveConvolver c;
  TASK_CHECK(!c.init(7, 11, 2, 0.1f, 0.3f, 1e-3f));     // B not power of 2
  TASK_CHECK(!c.init(4, 11, 2, 0.1f, 0.3f, 1e-3f));     // B too small
  TASK_CHECK(!c.init(2048, 11, 2, 0.1f, 0.3f, 1e-3f));  // B too large
  TASK_CHECK(!c.init(8, 0, 2, 0.1f, 0.3f, 1e-3f));      // L empty
  TASK_CHECK(!c.init(8, 65537, 2, 0.1f, 0.3f, 1e-3f));  // L too large
  TASK_CHECK(!c.init(8, 11, 0, 0.1f, 0.3f, 1e-3f));     // no channel
  TASK_CHECK(!c.init(8, 11, 7, 0.1f, 0.3f, 1e-3f));     // too many channels
  TASK_CHECK(!c.init(8, 11, 2, 0.0f, 0.3f, 1e-3f));     // mu zero
  TASK_CHECK(!c.init(8, 11, 2, -0.1f, 0.3f, 1e-3f));    // mu negative
  TASK_CHECK(!c.init(8, 11, 2, 0.51f, 0.3f, 1e-3f));    // mu too large
  TASK_CHECK(!c.init(8, 11, 2, 0.1f, 0.0f, 1e-3f));     // beta zero
  TASK_CHECK(!c.init(8, 11, 2, 0.1f, 1.1f, 1e-3f));     // beta too large
  TASK_CHECK(!c.init(8, 11, 2, 0.1f, 0.3f, 0.0f));      // delta zero
  TASK_CHECK(!c.init(8, 11, 2, 0.1f, 0.3f,
                     std::numeric_limits<float>::infinity())); // delta not finite
  TASK_CHECK(!c.init(8, 11, 2, 0.1f, 0.3f, -1e-3f));    // delta negative

  TASK_CHECK(c.init(8, 11, 2, 0.5f, 1.0f, 1e-6f));      // Boundary values valid
  return 0;
}


// ============================================================================
// Streaming group: fragmented calls must match block-aligned calls exactly
// ============================================================================

static void runStream(fftconvolver::AdaptiveConvolver& convolver,
                      const std::vector<std::vector<float> >& refs,
                      const std::vector<float>& desired,
                      const std::vector<size_t>& chunkSizes,
                      std::vector<float>& predicted,
                      std::vector<float>& residual)
{
  const size_t frames = desired.size();
  const size_t channels = refs.size();
  predicted.assign(frames, 0.0f);
  residual.assign(frames, 0.0f);
  std::vector<const float*> ptrs(channels, nullptr);

  size_t pos = 0;
  size_t chunk = 0;
  while (pos < frames)
  {
    size_t n = chunkSizes[chunk % chunkSizes.size()];
    ++chunk;
    n = std::min(n, frames - pos);
    for (size_t c=0; c<channels; ++c)
    {
      ptrs[c] = refs[c].data() + pos;
    }
    convolver.process(ptrs.data(), desired.data() + pos,
                      predicted.data() + pos, residual.data() + pos, n);
    pos += n;
  }
}


static int streamingWholeVsFragments()
{
  const size_t channelCounts[] = {1, 2, 6};
  for (size_t ci=0; ci<3; ++ci)
  {
    const size_t channels = channelCounts[ci];
    const size_t B = 16;
    const size_t L = 33;
    const size_t frames = 401; // Not a multiple of B: leaves a partial block

    const auto refs = makeReferences(channels, frames, 0xabcdef01u + 17u * static_cast<uint32_t>(channels));
    const auto desired = makeDesired(frames, 0x55aa55aau + 31u * static_cast<uint32_t>(channels));

    fftconvolver::AdaptiveConvolver whole;
    fftconvolver::AdaptiveConvolver fragmented;
    TASK_CHECK(whole.init(B, L, channels, 0.2f, 0.5f, 1e-3f));
    TASK_CHECK(fragmented.init(B, L, channels, 0.2f, 0.5f, 1e-3f));

    std::vector<float> predWhole, resWhole, predFrag, resFrag;
    runStream(whole, refs, desired, {256}, predWhole, resWhole);
    runStream(fragmented, refs, desired, {1, 7, 31}, predFrag, resFrag);

    TASK_CHECK(predWhole.size() == frames && predFrag.size() == frames);
    TASK_CHECK(std::memcmp(predWhole.data(), predFrag.data(), frames * sizeof(float)) == 0);
    TASK_CHECK(std::memcmp(resWhole.data(), resFrag.data(), frames * sizeof(float)) == 0);

    std::vector<float> tapsWhole(L, 0.0f), tapsFrag(L, 0.0f);
    for (size_t c=0; c<channels; ++c)
    {
      whole.getFilter(c, tapsWhole.data());
      fragmented.getFilter(c, tapsFrag.data());
      TASK_CHECK(std::memcmp(tapsWhole.data(), tapsFrag.data(), L * sizeof(float)) == 0);
    }
  }
  return 0;
}


static int streamingSilence()
{
  const size_t B = 8;
  const size_t L = 20;
  const size_t channels = 2;
  const size_t frames = 100;
  const std::vector<std::vector<float> > refs(channels, std::vector<float>(frames, 0.0f));
  const std::vector<float> desired(frames, 0.0f);

  fftconvolver::AdaptiveConvolver convolver;
  TASK_CHECK(convolver.init(B, L, channels, 0.25f, 0.4f, 1e-4f));

  std::vector<float> pred, residual;
  runStream(convolver, refs, desired, {1, 7, 31}, pred, residual);

  for (size_t i=0; i<frames; ++i)
  {
    TASK_CHECK(pred[i] == 0.0f);
    TASK_CHECK(residual[i] == 0.0f);
  }
  std::vector<float> taps(L, 1.0f);
  convolver.getFilter(0, taps.data());
  for (size_t i=0; i<L; ++i)
  {
    TASK_CHECK(taps[i] == 0.0f);
  }
  return 0;
}


static int streamingInPlaceResidual()
{
  const size_t B = 8;
  const size_t L = 19;
  const size_t channels = 2;
  const size_t frames = 137;
  const auto refs = makeReferences(channels, frames, 0x0f0f0f0f);
  const auto desired = makeDesired(frames, 0xf0f0f0f0);

  fftconvolver::AdaptiveConvolver separate;
  fftconvolver::AdaptiveConvolver inPlace;
  TASK_CHECK(separate.init(B, L, channels, 0.15f, 0.6f, 1e-3f));
  TASK_CHECK(inPlace.init(B, L, channels, 0.15f, 0.6f, 1e-3f));

  std::vector<float> predSep(frames, 0.0f), resSep(frames, 0.0f);
  std::vector<float> predIn(frames, 0.0f), workingDesired = desired;

  size_t pos = 0;
  size_t call = 0;
  const size_t sizes[] = {1, 7, 31};
  while (pos < frames)
  {
    size_t n = std::min(frames - pos, sizes[call++ % 3]);
    std::vector<const float*> chunkPtrs(channels, nullptr);
    for (size_t c=0; c<channels; ++c) chunkPtrs[c] = refs[c].data() + pos;
    separate.process(chunkPtrs.data(), desired.data() + pos, predSep.data() + pos, resSep.data() + pos, n);
    inPlace.process(chunkPtrs.data(), workingDesired.data() + pos, predIn.data() + pos, workingDesired.data() + pos, n);
    pos += n;
  }

  TASK_CHECK(std::memcmp(predSep.data(), predIn.data(), frames * sizeof(float)) == 0);
  TASK_CHECK(std::memcmp(resSep.data(), workingDesired.data(), frames * sizeof(float)) == 0);
  return 0;
}


static int streamingDelayIdentity()
{
  const size_t B = 8;
  const size_t L = 11;
  const size_t channels = 2;
  const size_t frames = 129;
  const auto refs = makeReferences(channels, frames, 0x22446688);
  const auto desired = makeDesired(frames, 0x88664422);

  fftconvolver::AdaptiveConvolver convolver;
  TASK_CHECK(convolver.init(B, L, channels, 0.1f, 0.3f, 1e-3f));
  std::vector<float> predicted, residual;
  runStream(convolver, refs, desired, {1, 7, 31}, predicted, residual);

  for (size_t t=0; t<frames; ++t)
  {
    if (t < B)
    {
      // Strict B-frame delay: nothing is emitted before the first block lands
      TASK_CHECK(predicted[t] == 0.0f);
      TASK_CHECK(residual[t] == 0.0f);
    }
    else
    {
      // Correct block pairing: the outputs at t belong to desired[t-B]
      const double sum = static_cast<double>(predicted[t]) + static_cast<double>(residual[t]);
      TASK_CHECK(std::fabs(sum - static_cast<double>(desired[t - B])) <= 1e-6);
    }
  }
  return 0;
}


static int streamingZeroLengthAndReset()
{
  const size_t B = 8;
  const size_t L = 12;
  const size_t channels = 1;
  const size_t frames = 64;
  const auto refs = makeReferences(channels, frames, 0x13579bdf);
  const auto desired = makeDesired(frames, 0x2468ace0);
  auto ptrs = planePointers(refs);

  fftconvolver::AdaptiveConvolver convolver;
  TASK_CHECK(convolver.init(B, L, channels, 0.2f, 0.4f, 1e-3f));

  // Empty call with null pointers must be a no-op
  convolver.process(nullptr, nullptr, nullptr, nullptr, 0);

  std::vector<float> predA(frames, 0.0f), resA(frames, 0.0f);
  convolver.process(ptrs.data(), desired.data(), predA.data(), resA.data(), frames);

  // Reset retains the configuration but restarts timing from zero
  convolver.reset();
  std::vector<float> zeroTaps(L, 1.0f);
  convolver.getFilter(0, zeroTaps.data());
  for (size_t i=0; i<L; ++i)
  {
    TASK_CHECK(zeroTaps[i] == 0.0f); // coefficients, history and queue cleared
  }

  std::vector<float> predB(frames, 0.0f), resB(frames, 0.0f);
  convolver.process(ptrs.data(), desired.data(), predB.data(), resB.data(), frames);
  TASK_CHECK(std::memcmp(predA.data(), predB.data(), frames * sizeof(float)) == 0);
  TASK_CHECK(std::memcmp(resA.data(), resB.data(), frames * sizeof(float)) == 0);
  return 0;
}


// ============================================================================
// Learning group: freeze/resume, half-block rejection and convergence
// ============================================================================

static int learningFreezeResume()
{
  const size_t B = 8;
  const size_t L = 23;
  const size_t channels = 2;
  const size_t frames = 17 * B;
  const auto refs = makeReferences(channels, frames, 0x10293847);
  const auto desired = makeDesired(frames, 0x56473829);

  fftconvolver::AdaptiveConvolver blockwise;
  fftconvolver::AdaptiveConvolver fragmented;
  TASK_CHECK(blockwise.init(B, L, channels, 0.2f, 0.5f, 1e-3f));
  TASK_CHECK(fragmented.init(B, L, channels, 0.2f, 0.5f, 1e-3f));

  // Learn blocks 0..4, freeze 5..8, learn 9..14, freeze 15..16
  auto scheduleLearning = [&](size_t nextBlock)
  {
    const bool learn = !(nextBlock >= 5 && nextBlock <= 8) && !(nextBlock >= 15);
    return learn;
  };

  std::vector<float> predBlock(frames, 0.0f), resBlock(frames, 0.0f);
  std::vector<float> predFrag(frames, 0.0f), resFrag(frames, 0.0f);
  std::vector<float> tapsBlock(L, 0.0f), tapsFrag(L, 0.0f);

  for (size_t m=0; m<17; ++m)
  {
    TASK_CHECK(blockwise.setLearning(scheduleLearning(m)));
    std::vector<const float*> blockPtrs(channels, nullptr);
    for (size_t c=0; c<channels; ++c) blockPtrs[c] = refs[c].data() + m * B;
    blockwise.process(blockPtrs.data(), desired.data() + m * B,
                      predBlock.data() + m * B, resBlock.data() + m * B, B);
  }

  // Fragmented run with the same schedule; toggles mid-block must be rejected
  size_t pos = 0;
  size_t call = 0;
  const size_t sizes[] = {1, 7, 31};
  size_t nextBlock = 0;
  while (pos < frames)
  {
    const size_t blockStart = nextBlock * B;
    if (pos == blockStart)
    {
      TASK_CHECK(fragmented.setLearning(scheduleLearning(nextBlock)));
      ++nextBlock;
    }
    else
    {
      // Any toggle while a partial block is accumulated is rejected
      TASK_CHECK(!fragmented.setLearning(!scheduleLearning(nextBlock - 1)));
    }
    size_t n = std::min(frames - pos, sizes[call++ % 3]);
    const size_t boundary = ((pos / B) + 1) * B;
    if (pos + n > boundary)
    {
      if (pos % B != 0)
      {
        // Landing inside a block: a learning toggle must be rejected
        TASK_CHECK(!fragmented.setLearning(!scheduleLearning(nextBlock - 1)));
      }
      n = boundary - pos;
    }
    std::vector<const float*> chunkPtrs(channels, nullptr);
    for (size_t c=0; c<channels; ++c) chunkPtrs[c] = refs[c].data() + pos;
    fragmented.process(chunkPtrs.data(), desired.data() + pos,
                       predFrag.data() + pos, resFrag.data() + pos, n);
    pos += n;
  }

  TASK_CHECK(std::memcmp(predBlock.data(), predFrag.data(), frames * sizeof(float)) == 0);
  TASK_CHECK(std::memcmp(resBlock.data(), resFrag.data(), frames * sizeof(float)) == 0);

  for (size_t c=0; c<channels; ++c)
  {
    blockwise.getFilter(c, tapsBlock.data());
    fragmented.getFilter(c, tapsFrag.data());
    TASK_CHECK(std::memcmp(tapsBlock.data(), tapsFrag.data(), L * sizeof(float)) == 0);
  }

  // While frozen, the exported taps must not move
  fftconvolver::AdaptiveConvolver frozen;
  TASK_CHECK(frozen.init(B, L, channels, 0.2f, 0.5f, 1e-3f));
  std::vector<float> pred(B, 0.0f), res(B, 0.0f), before(L, 0.0f), after(L, 0.0f);
  for (size_t m=0; m<5; ++m)
  {
    std::vector<const float*> blockPtrs(channels, nullptr);
    for (size_t c=0; c<channels; ++c) blockPtrs[c] = refs[c].data() + m * B;
    frozen.process(blockPtrs.data(), desired.data() + m * B, pred.data(), res.data(), B);
  }
  TASK_CHECK(frozen.setLearning(false));
  frozen.getFilter(0, before.data());
  for (size_t m=5; m<9; ++m)
  {
    std::vector<const float*> blockPtrs(channels, nullptr);
    for (size_t c=0; c<channels; ++c) blockPtrs[c] = refs[c].data() + m * B;
    frozen.process(blockPtrs.data(), desired.data() + m * B, pred.data(), res.data(), B);
  }
  frozen.getFilter(0, after.data());
  TASK_CHECK(std::memcmp(before.data(), after.data(), L * sizeof(float)) == 0);
  TASK_CHECK(frozen.setLearning(true));
  return 0;
}


static int learningHalfBlockRejection()
{
  const size_t B = 8;
  fftconvolver::AdaptiveConvolver convolver;
  TASK_CHECK(convolver.init(B, 12, 2, 0.2f, 0.5f, 1e-3f));

  const auto refs = makeReferences(2, 2 * B, 0x11112222);
  const auto desired = makeDesired(2 * B, 0x33334444);
  auto ptrs = planePointers(refs);
  const size_t L = 12;
  std::vector<float> pred(B, 0.0f), res(B, 0.0f), taps(L, 0.0f);

  // Mid-block toggles fail and leave learning switched on
  convolver.process(ptrs.data(), desired.data(), pred.data(), res.data(), 3);
  TASK_CHECK(!convolver.setLearning(false));
  TASK_CHECK(!convolver.setLearning(true));
  std::vector<const float*> restPtrs(refs.size(), nullptr);
  for (size_t c=0; c<refs.size(); ++c) restPtrs[c] = refs[c].data() + 3;
  convolver.process(restPtrs.data(), desired.data() + 3, pred.data(), res.data(), B - 3);

  // Learning was still active: the completed block changed the coefficients
  convolver.getFilter(0, taps.data());
  bool anyNonZero = false;
  for (size_t i=0; i<taps.size(); ++i) anyNonZero = anyNonZero || taps[i] != 0.0f;
  TASK_CHECK(anyNonZero);

  // At a block boundary toggles succeed
  TASK_CHECK(convolver.setLearning(false));
  TASK_CHECK(convolver.setLearning(true));
  return 0;
}


static int learningConvergenceAndFreeze()
{
  const size_t B = 32;
  const size_t L = 129;
  const size_t channels = 2;
  const size_t learnBlocks = 4096;
  const size_t tailFrames = 500;
  const size_t frames = learnBlocks * B + tailFrames;
  const float mu = 0.25f;
  const float beta = 0.2f;
  const float delta = 1e-4f;

  const auto refs = makeReferences(channels, frames, 0x77665544);
  std::vector<float> desired(frames, 0.0f);

  // Two known sparse L-tap responses
  std::vector<std::vector<float> > truth(channels, std::vector<float>(L, 0.0f));
  truth[0][0] = 0.5f;   truth[0][17] = -0.3f; truth[0][64] = 0.25f; truth[0][128] = 0.15f;
  truth[1][3] = 0.4f;   truth[1][40] = -0.2f; truth[1][96] = 0.3f;  truth[1][128] = -0.1f;

  for (size_t t=0; t<frames; ++t)
  {
    double sample = 0.0;
    for (size_t c=0; c<channels; ++c)
    {
      for (size_t l=0; l<L && l<=t; ++l)
      {
        sample += static_cast<double>(truth[c][l]) * refs[c][t - l];
      }
    }
    desired[t] = static_cast<float>(sample);
  }

  fftconvolver::AdaptiveConvolver convolver;
  TASK_CHECK(convolver.init(B, L, channels, mu, beta, delta));
  auto ptrs = planePointers(refs);

  // All test-side buffers are allocated before the allocation accounting starts
  std::vector<float> predicted(frames, 0.0f);
  std::vector<float> residual(frames, 0.0f);
  std::vector<std::vector<float> > taps(channels, std::vector<float>(L, 0.0f));
  std::vector<float> continuedPred(tailFrames, 0.0f), continuedRes(tailFrames, 0.0f);
  std::vector<const float*> chunkPtrs(channels, nullptr);

  // No heap traffic while processing the whole learning phase
  uint64_t newBefore = g_newCalls;
  uint64_t deleteBefore = g_deleteCalls;
  convolver.process(ptrs.data(), desired.data(), predicted.data(), residual.data(), learnBlocks * B);
  TASK_CHECK(g_newCalls == newBefore);
  TASK_CHECK(g_deleteCalls == deleteBefore);
  std::printf("PROCESS_ALLOCATIONS new=0 delete=0 frames=%zu\n", learnBlocks * B);

  // >=18 dB residual reduction over the last 256 output blocks
  const size_t window = 256 * B;
  const size_t firstEmitted = B;
  const size_t available = learnBlocks * B - firstEmitted;
  const size_t windowStart = firstEmitted + (available - window);
  double targetEnergy = 0.0;
  double residualEnergy = 0.0;
  for (size_t t=windowStart; t<windowStart + window; ++t)
  {
    targetEnergy += static_cast<double>(desired[t - B]) * desired[t - B];
    residualEnergy += static_cast<double>(residual[t]) * residual[t];
  }
  const double db = 10.0 * std::log10(targetEnergy / residualEnergy);
  std::printf("CONVERGENCE reduction=%.3f dB (target >= 18 dB)\n", db);
  TASK_CHECK(db >= 18.0);

  // setLearning() at a block boundary must not allocate either
  TASK_CHECK(convolver.setLearning(false));
  TASK_CHECK(g_newCalls == newBefore);
  TASK_CHECK(g_deleteCalls == deleteBefore);

  // Export the learned taps (getFilter is not part of the process contract)
  for (size_t c=0; c<channels; ++c)
  {
    convolver.getFilter(c, taps[c].data());
  }

  // Continue with fragmented calls; outputs stay continuous. One block is
  // already queued, so continued frame i is the prediction for input frame
  // (learnBlocks-1)*B + i; the first queued block still used the pre-freeze
  // filters, so comparisons start once frozen-filter blocks are reached.
  newBefore = g_newCalls;
  deleteBefore = g_deleteCalls;
  size_t pos = 0;
  size_t call = 0;
  const size_t sizes[] = {1, 7, 31};
  while (pos < tailFrames)
  {
    size_t n = std::min(tailFrames - pos, sizes[call++ % 3]);
    for (size_t c=0; c<channels; ++c)
    {
      chunkPtrs[c] = refs[c].data() + learnBlocks * B + pos;
    }
    convolver.process(chunkPtrs.data(), desired.data() + learnBlocks * B + pos,
                      continuedPred.data() + pos, continuedRes.data() + pos, n);
    pos += n;
  }
  TASK_CHECK(g_newCalls == newBefore);
  TASK_CHECK(g_deleteCalls == deleteBefore);

  // Independent time-domain convolution with the exported taps must match the
  // frozen convolver's continued prediction sample by sample.
  const double tol = 5e-4;
  for (size_t i=B; i<tailFrames; ++i)
  {
    const size_t t = (learnBlocks - 1) * B + i;
    double fir = 0.0;
    for (size_t c=0; c<channels; ++c)
    {
      for (size_t l=0; l<L && l<=t; ++l)
      {
        fir += static_cast<double>(taps[c][l]) * refs[c][t - l];
      }
    }
    TASK_CHECK(std::fabs(fir - static_cast<double>(continuedPred[i])) <= tol);
    // The residual belongs to the same input block as this prediction
    TASK_CHECK(std::fabs((static_cast<double>(desired[t]) - fir)
                         - static_cast<double>(continuedRes[i])) <= tol);
  }
  return 0;
}


// ============================================================================
// Registration
// ============================================================================

const struct task_case task_cases[] =
{
  {"Gradient", "DirectDftModel", gradientAgainstDirectDft},
  {"Gradient", "InvalidConfiguration", gradientInvalidConfigurations},

  {"Streaming", "WholeVsFragments", streamingWholeVsFragments},
  {"Streaming", "Silence", streamingSilence},
  {"Streaming", "InPlaceResidual", streamingInPlaceResidual},
  {"Streaming", "DelayIdentity", streamingDelayIdentity},
  {"Streaming", "ZeroLengthAndReset", streamingZeroLengthAndReset},

  {"Learning", "FreezeResume", learningFreezeResume},
  {"Learning", "HalfBlockRejection", learningHalfBlockRejection},
  {"Learning", "ConvergenceAndFreeze", learningConvergenceAndFreeze},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);

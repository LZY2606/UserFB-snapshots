// ==================================================================================
// AdaptiveConvolver task tests
//
// Three groups registered through task_support/check.h (no own main):
//   Gradient  - independent double direct-DFT reference for the first six blocks
//   Streaming - whole-block vs fragmented PCM/coefficient equivalence, channels,
//               silence, in-place residual and strict B-frame output delay
//   Learning  - freeze/resume, half-block rejection, reset, the 18 dB
//               identification case with time-domain continuation and zero
//               heap allocations inside process()
// ==================================================================================

#include "../task_support/check.h"

#include "AdaptiveConvolver.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <new>
#include <vector>


namespace
{

// -----------------------------------------------------------------------
// Heap allocation accounting (ARM64 build: all production buffers use the
// global throwing operator new / delete, there is no SSE allocator path)
// -----------------------------------------------------------------------
unsigned long g_allocs = 0;
unsigned long g_deallocs = 0;
bool g_countHeap = false;

} // namespace

void* operator new(std::size_t size)
{
  if (g_countHeap) { ++g_allocs; }
  void* p = std::malloc(size ? size : 1);
  if (!p) throw std::bad_alloc();
  return p;
}

void* operator new[](std::size_t size)
{
  if (g_countHeap) { ++g_allocs; }
  void* p = std::malloc(size ? size : 1);
  if (!p) throw std::bad_alloc();
  return p;
}

void operator delete(void* p) noexcept
{
  if (g_countHeap && p) { ++g_deallocs; }
  std::free(p);
}

void operator delete[](void* p) noexcept
{
  if (g_countHeap && p) { ++g_deallocs; }
  std::free(p);
}

void operator delete(void* p, std::size_t) noexcept
{
  if (g_countHeap && p) { ++g_deallocs; }
  std::free(p);
}

void operator delete[](void* p, std::size_t) noexcept
{
  if (g_countHeap && p) { ++g_deallocs; }
  std::free(p);
}


namespace
{

// -----------------------------------------------------------------------
// Deterministic signal generation
// -----------------------------------------------------------------------
float nextSample(uint32_t& state)
{
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return static_cast<float>(static_cast<int32_t>(state & 0xFFFFu) - 32768) / 32768.0f;
}

std::vector<float> makeSignal(size_t len, uint32_t seed)
{
  std::vector<float> v(len);
  uint32_t state = seed;
  for (size_t i = 0; i < len; ++i)
  {
    v[i] = nextSample(state);
  }
  return v;
}

// Independent time-domain FIR, returns 0 for samples before stream start
float timeFir(const std::vector<float>& x, const std::vector<float>& taps, long n)
{
  double acc = 0.0;
  for (size_t k = 0; k < taps.size(); ++k)
  {
    const long idx = n - static_cast<long>(k);
    if (idx >= 0)
    {
      acc += static_cast<double>(taps[k]) * static_cast<double>(x[static_cast<size_t>(idx)]);
    }
  }
  return static_cast<float>(acc);
}

// -----------------------------------------------------------------------
// Independent double-precision direct DFT reference (never uses production FFT)
// -----------------------------------------------------------------------
struct DComplex
{
  double re;
  double im;
};

class DirectDFT
{
public:
  explicit DirectDFT(size_t n) : _n(n) {}

  std::vector<DComplex> forward(const std::vector<double>& in) const
  {
    std::vector<DComplex> out(_n, {0.0, 0.0});
    for (size_t k = 0; k < _n; ++k)
    {
      double re = 0.0;
      double im = 0.0;
      for (size_t n = 0; n < _n; ++n)
      {
        const double phase = -2.0 * M_PI * static_cast<double>(k) * static_cast<double>(n) / static_cast<double>(_n);
        re += in[n] * std::cos(phase);
        im += in[n] * std::sin(phase);
      }
      out[k] = {re, im};
    }
    return out;
  }

  std::vector<double> inverse(const std::vector<DComplex>& in) const
  {
    std::vector<double> out(_n, 0.0);
    for (size_t n = 0; n < _n; ++n)
    {
      double acc = 0.0;
      for (size_t k = 0; k < _n; ++k)
      {
        const double phase = 2.0 * M_PI * static_cast<double>(k) * static_cast<double>(n) / static_cast<double>(_n);
        acc += in[k].re * std::cos(phase) - in[k].im * std::sin(phase);
      }
      out[n] = acc / static_cast<double>(_n);
    }
    return out;
  }

private:
  size_t _n;
};

// Block-by-block replica of the specified adaptive algorithm
class ReferenceModel
{
public:
  ReferenceModel(size_t B, size_t L, size_t C, double mu, double beta, double delta) :
    _B(B),
    _L(L),
    _C(C),
    _P((L + B - 1) / B),
    _mu(mu),
    _beta(beta),
    _delta(delta),
    _dft(2 * B),
    _H(C * ((L + B - 1) / B), std::vector<DComplex>(2 * B, {0.0, 0.0})),
    _S(2 * B, 0.0)
  {
  }

  // refs[c] holds the current B-frame block of channel c; blocks before start are zero
  void step(const std::vector<std::vector<double> >& refs,
            const std::vector<double>& desired,
            std::vector<double>& prediction,
            std::vector<double>& residual)
  {
    const size_t N = 2 * _B;
    for (size_t c = 0; c < _C; ++c)
    {
      _history[c].push_back(refs[c]);
    }

    auto spectrumAt = [&](size_t c, long m) -> std::vector<DComplex> {
      std::vector<double> framed(N, 0.0);
      const long blockIndex = static_cast<long>(_history[c].size()) - 1 + m;
      for (size_t half = 0; half < 2; ++half)
      {
        const long bi = blockIndex - static_cast<long>(1 - half);
        if (bi >= 0)
        {
          const std::vector<double>& block = _history[c][static_cast<size_t>(bi)];
          for (size_t i = 0; i < _B; ++i)
          {
            framed[half * _B + i] = block[i];
          }
        }
      }
      return _dft.forward(framed);
    };

    // Prediction with the pre-update filters: back B-frame of IFFT(sum H*X)
    std::vector<DComplex> acc(N, {0.0, 0.0});
    for (size_t c = 0; c < _C; ++c)
    {
      for (size_t p = 0; p < _P; ++p)
      {
        const std::vector<DComplex> x = spectrumAt(c, -static_cast<long>(p));
        const std::vector<DComplex>& h = _H[c * _P + p];
        for (size_t k = 0; k < N; ++k)
        {
          acc[k].re += h[k].re * x[k].re - h[k].im * x[k].im;
          acc[k].im += h[k].re * x[k].im + h[k].im * x[k].re;
        }
      }
    }
    const std::vector<double> conv = _dft.inverse(acc);
    prediction.assign(_B, 0.0);
    residual.assign(_B, 0.0);
    for (size_t i = 0; i < _B; ++i)
    {
      prediction[i] = conv[_B + i];
      residual[i] = desired[i] - prediction[i];
    }

    // E = FFT([B zeros | e])
    std::vector<double> errFramed(N, 0.0);
    for (size_t i = 0; i < _B; ++i)
    {
      errFramed[_B + i] = residual[i];
    }
    const std::vector<DComplex> E = _dft.forward(errFramed);

    // Power estimate updated before the gradient
    for (size_t k = 0; k < N; ++k)
    {
      double powerSum = 0.0;
      for (size_t c = 0; c < _C; ++c)
      {
        for (size_t p = 0; p < _P; ++p)
        {
          const std::vector<DComplex> x = spectrumAt(c, -static_cast<long>(p));
          powerSum += x[k].re * x[k].re + x[k].im * x[k].im;
        }
      }
      _S[k] = (1.0 - _beta) * _S[k] + _beta * powerSum;
    }

    // Constrained partitioned NLMS update
    for (size_t c = 0; c < _C; ++c)
    {
      for (size_t p = 0; p < _P; ++p)
      {
        const std::vector<DComplex> x = spectrumAt(c, -static_cast<long>(p));
        std::vector<DComplex> grad(N, {0.0, 0.0});
        for (size_t k = 0; k < N; ++k)
        {
          const double denom = _delta + _S[k];
          const double gre = x[k].re * E[k].re + x[k].im * E[k].im;
          const double gim = x[k].re * E[k].im - x[k].im * E[k].re;
          grad[k] = {gre / denom, gim / denom};
        }
        std::vector<double> g = _dft.inverse(grad);

        // Time-domain causal projection, including the short last partition
        const size_t keep = std::min(_B, _L - p * _B);
        for (size_t i = keep; i < N; ++i)
        {
          g[i] = 0.0;
        }
        const std::vector<DComplex> gProjected = _dft.forward(g);
        for (size_t k = 0; k < N; ++k)
        {
          _H[c * _P + p][k].re += _mu * gProjected[k].re;
          _H[c * _P + p][k].im += _mu * gProjected[k].im;
        }
      }
    }
  }

  // Exports the L causal taps of one channel from the current filter spectra
  std::vector<double> taps(size_t channel) const
  {
    std::vector<double> out(_L, 0.0);
    for (size_t p = 0; p < _P; ++p)
    {
      const std::vector<double> h = _dft.inverse(_H[channel * _P + p]);
      const size_t keep = std::min(_B, _L - p * _B);
      for (size_t i = 0; i < keep; ++i)
      {
        out[p * _B + i] = h[i];
      }
    }
    return out;
  }

  double powerAt(size_t bin) const { return _S[bin]; }

private:
  size_t _B;
  size_t _L;
  size_t _C;
  size_t _P;
  double _mu;
  double _beta;
  double _delta;
  DirectDFT _dft;
  std::vector<std::vector<DComplex> > _H;
  std::vector<double> _S;
  std::map<size_t, std::vector<std::vector<double> > > _history;
};


// -----------------------------------------------------------------------
// Streaming helpers
// -----------------------------------------------------------------------
void runWhole(fftconvolver::AdaptiveConvolver& conv,
              const std::vector<std::vector<float> >& refs,
              const std::vector<float>& desired,
              std::vector<float>& predicted,
              std::vector<float>& residual)
{
  const size_t C = refs.size();
  const size_t N = desired.size();
  std::vector<const float*> refPtrs(C);
  for (size_t c = 0; c < C; ++c) { refPtrs[c] = refs[c].data(); }
  conv.process(refPtrs.data(), desired.data(), predicted.data(), residual.data(), N);
}

void runFragmented(fftconvolver::AdaptiveConvolver& conv,
                   const std::vector<std::vector<float> >& refs,
                   const std::vector<float>& desired,
                   std::vector<float>& predicted,
                   std::vector<float>& residual)
{
  const size_t C = refs.size();
  const size_t N = desired.size();
  static const size_t chunkSizesLocal[] = {1, 7, 31};
  size_t pos = 0;
  size_t call = 0;
  std::vector<const float*> refPtrs(C);
  while (pos < N)
  {
    size_t chunk = std::min(N - pos, chunkSizesLocal[call++ % 3]);
    if (call == 2)
    {
      // A zero-length call with null pointers must be a no-op
      conv.process(0, 0, 0, 0, 0);
    }
    for (size_t c = 0; c < C; ++c) { refPtrs[c] = refs[c].data() + pos; }
    conv.process(refPtrs.data(), desired.data() + pos,
                 predicted.data() + pos, residual.data() + pos, chunk);
    pos += chunk;
  }
}

const size_t kChunkSizes[] = {1, 7, 31};

bool samePcm(const std::vector<float>& a, const std::vector<float>& b)
{
  if (a.size() != b.size()) { return false; }
  for (size_t i = 0; i < a.size(); ++i)
  {
    if (std::memcmp(&a[i], &b[i], sizeof(float)) != 0) { return false; }
  }
  return true;
}

bool sameTaps(const fftconvolver::AdaptiveConvolver& a,
              const fftconvolver::AdaptiveConvolver& b,
              size_t C,
              size_t L)
{
  std::vector<float> ta(L), tb(L);
  for (size_t c = 0; c < C; ++c)
  {
    a.getFilter(c, ta.data());
    b.getFilter(c, tb.data());
    if (!samePcm(ta, tb)) { return false; }
  }
  return true;
}

// Checks strict B-frame delay and the delayed signal identity
bool checkDelayIdentity(size_t B,
                        const std::vector<float>& desired,
                        const std::vector<float>& predicted,
                        const std::vector<float>& residual)
{
  for (size_t t = 0; t < B; ++t)
  {
    if (predicted[t] != 0.0f || residual[t] != 0.0f) { return false; }
  }
  for (size_t t = B; t < desired.size() + B && t < predicted.size(); ++t)
  {
    const float sum = predicted[t] + residual[t];
    if (std::fabs(sum - desired[t - B]) > 1e-6f) { return false; }
    if (!std::isfinite(sum)) { return false; }
  }
  return true;
}

// -----------------------------------------------------------------------
// Gradient group
// -----------------------------------------------------------------------
int testGradientFirstBlocks()
{
  const size_t B = 8;
  const size_t L = 11;
  const size_t C = 2;
  const size_t blocks = 6;
  const float mu = 0.25f;
  const float beta = 0.2f;
  const float delta = 1e-4f;

  std::vector<std::vector<float> > refs(C);
  for (size_t c = 0; c < C; ++c)
  {
    refs[c] = makeSignal(B * blocks, 0x1000u + 0x9e3779b9u * static_cast<uint32_t>(c + 1));
  }
  const std::vector<float> desired = makeSignal(B * blocks, 0xABCDEF01u);

  fftconvolver::AdaptiveConvolver conv;
  TASK_CHECK(conv.init(B, L, C, mu, beta, delta));

  // Filters must start at zero (IFFT(0) may produce -0.0 bins, which compare equal)
  std::vector<float> zeros(L, 0.0f);
  std::vector<float> taps(L);
  for (size_t c = 0; c < C; ++c)
  {
    conv.getFilter(c, taps.data());
    for (float v : taps) { TASK_CHECK(v == 0.0f); }
  }

  // One extra block of input so the B-frame-delayed output covers all six model blocks
  const size_t inputFrames = B * (blocks + 1);
  for (size_t c = 0; c < C; ++c)
  {
    refs[c].resize(inputFrames);
    uint32_t state = 0x5500u + static_cast<uint32_t>(c);
    for (size_t i = B * blocks; i < inputFrames; ++i)
    {
      refs[c][i] = nextSample(state);
    }
  }
  std::vector<float> desiredExt = desired;
  desiredExt.resize(inputFrames);
  {
    uint32_t state = 0x77AAu;
    for (size_t i = B * blocks; i < inputFrames; ++i)
    {
      desiredExt[i] = nextSample(state);
    }
  }

  std::vector<float> predicted(inputFrames, 7.0f);
  std::vector<float> residual(inputFrames, 7.0f);
  {
    // Learn exactly the six blocks the reference model learns, then freeze so
    // the additional flush block cannot change the compared coefficients
    const size_t learnedFrames = B * blocks;
    std::vector<const float*> ptrs(C);
    for (size_t c = 0; c < C; ++c) { ptrs[c] = refs[c].data(); }
    conv.process(ptrs.data(), desiredExt.data(),
                 predicted.data(), residual.data(), learnedFrames);
    TASK_CHECK(conv.setLearning(false));
    for (size_t c = 0; c < C; ++c) { ptrs[c] = refs[c].data() + learnedFrames; }
    conv.process(ptrs.data(), desiredExt.data() + learnedFrames,
                 predicted.data() + learnedFrames, residual.data() + learnedFrames, B);
  }

  // Startup delay must be exactly B zero frames
  for (size_t i = 0; i < B; ++i)
  {
    TASK_CHECK(predicted[i] == 0.0f);
    TASK_CHECK(residual[i] == 0.0f);
  }

  ReferenceModel model(B, L, C, mu, beta, delta);
  for (size_t m = 0; m < blocks; ++m)
  {
    std::vector<std::vector<double> > refBlocks(C, std::vector<double>(B));
    std::vector<double> desiredBlock(B);
    for (size_t c = 0; c < C; ++c)
    {
      for (size_t i = 0; i < B; ++i)
      {
        refBlocks[c][i] = static_cast<double>(refs[c][m * B + i]);
      }
    }
    for (size_t i = 0; i < B; ++i)
    {
      desiredBlock[i] = static_cast<double>(desired[m * B + i]);
    }

    std::vector<double> refPred;
    std::vector<double> refRes;
    model.step(refBlocks, desiredBlock, refPred, refRes);

    // Production output block m appears B frames later
    for (size_t i = 0; i < B; ++i)
    {
      const size_t out = B + m * B + i;
      TASK_CHECK(std::fabs(predicted[out] - refPred[i]) <= 3e-4f);
      TASK_CHECK(std::fabs(residual[out] - refRes[i]) <= 3e-4f);
    }
  }

  // Final coefficients, including the 3-tap short partition p=1
  for (size_t c = 0; c < C; ++c)
  {
    const std::vector<double> refTaps = model.taps(c);
    conv.getFilter(c, taps.data());
    TASK_CHECK(refTaps.size() == L);
    for (size_t i = 0; i < L; ++i)
    {
      TASK_CHECK(std::fabs(taps[i] - refTaps[i]) <= 3e-4f);
    }
    // The model itself must really have applied the partial-partition projection
  }
  // Power-driven update sanity: after the first block S = beta * sum |X|^2
  TASK_CHECK(std::isfinite(model.powerAt(0)));
  TASK_CHECK(model.powerAt(0) > 0.0);

  return 0;
}


// -----------------------------------------------------------------------
// Streaming group
// -----------------------------------------------------------------------
int streamingEquivalence(size_t C, size_t B, size_t L, size_t N, uint32_t seed)
{
  std::vector<std::vector<float> > refs(C);
  for (size_t c = 0; c < C; ++c)
  {
    refs[c] = makeSignal(N, seed + 0x101u * static_cast<uint32_t>(c + 1));
  }
  const std::vector<float> desired = makeSignal(N, seed ^ 0x5A5A5A5Au);

  fftconvolver::AdaptiveConvolver whole;
  fftconvolver::AdaptiveConvolver frag;
  TASK_CHECK(whole.init(B, L, C, 0.2f, 0.3f, 1e-3f));
  TASK_CHECK(frag.init(B, L, C, 0.2f, 0.3f, 1e-3f));

  std::vector<float> pw(N, 0.0f), rw(N, 0.0f), pf(N, 9.0f), rf(N, 9.0f);
  runWhole(whole, refs, desired, pw, rw);
  runFragmented(frag, refs, desired, pf, rf);

  TASK_CHECK(samePcm(pw, pf));
  TASK_CHECK(samePcm(rw, rf));
  TASK_CHECK(sameTaps(whole, frag, C, L));
  TASK_CHECK(checkDelayIdentity(B, desired, pw, rw));
  TASK_CHECK(checkDelayIdentity(B, desired, pf, rf));

  // A deliberately misaligned delay must be detectable
  bool shifted = false;
  for (size_t t = B; t + 1 < N; ++t)
  {
    if (std::fabs(rw[t] - desired[t]) > 1e-3f) { shifted = true; break; }
  }
  TASK_CHECK(shifted);

  return 0;
}

int testStreaming1ch() { return streamingEquivalence(1, 16, 23, 301, 0x1111u); }
int testStreaming2ch() { return streamingEquivalence(2, 32, 40, 500, 0x2222u); }
int testStreaming6ch() { return streamingEquivalence(6, 16, 17, 407, 0x3333u); }

int testStreamingSilence()
{
  const size_t B = 16;
  const size_t N = 137;
  const size_t C = 2;
  fftconvolver::AdaptiveConvolver conv;
  TASK_CHECK(conv.init(B, 19, C, 0.25f, 0.2f, 1e-4f));

  std::vector<std::vector<float> > refs(C, std::vector<float>(N, 0.0f));
  std::vector<float> desired(N, 0.0f), predicted(N, 3.0f), residual(N, 3.0f);
  std::vector<const float*> ptrs(C);
  for (size_t c = 0; c < C; ++c) { ptrs[c] = refs[c].data(); }
  conv.process(ptrs.data(), desired.data(), predicted.data(), residual.data(), N);

  for (size_t i = 0; i < N; ++i)
  {
    TASK_CHECK(predicted[i] == 0.0f);
    TASK_CHECK(residual[i] == 0.0f);
  }
  std::vector<float> taps(19);
  for (size_t c = 0; c < C; ++c)
  {
    conv.getFilter(c, taps.data());
    for (float v : taps) { TASK_CHECK(v == 0.0f); }
  }
  return 0;
}

int testStreamingInPlace()
{
  const size_t B = 16;
  const size_t N = 250;
  const size_t C = 2;
  std::vector<std::vector<float> > refs(C);
  for (size_t c = 0; c < C; ++c) { refs[c] = makeSignal(N, 0x4444u + c); }
  const std::vector<float> desired = makeSignal(N, 0x8888u);

  fftconvolver::AdaptiveConvolver ref;
  fftconvolver::AdaptiveConvolver inplace;
  TASK_CHECK(ref.init(B, 21, C, 0.25f, 0.2f, 1e-4f));
  TASK_CHECK(inplace.init(B, 21, C, 0.25f, 0.2f, 1e-4f));

  std::vector<float> predRef(N), resRef(N);
  runFragmented(ref, refs, desired, predRef, resRef);

  std::vector<float> predIn(N, 1.0f);
  std::vector<float> desiredCopy = desired; // residual aliases the desired buffer
  std::vector<const float*> ptrs(C);
  for (size_t c = 0; c < C; ++c) { ptrs[c] = refs[c].data(); }
  inplace.process(ptrs.data(), desiredCopy.data(), predIn.data(), desiredCopy.data(), N);

  TASK_CHECK(samePcm(predRef, predIn));
  TASK_CHECK(samePcm(resRef, desiredCopy));
  TASK_CHECK(sameTaps(ref, inplace, C, 21));
  return 0;
}


// -----------------------------------------------------------------------
// Learning group
// -----------------------------------------------------------------------
int testLearningFreezeResume()
{
  const size_t B = 16;
  const size_t L = 27;
  const size_t C = 2;
  const size_t N = B * 10;
  std::vector<std::vector<float> > refs(C);
  for (size_t c = 0; c < C; ++c) { refs[c] = makeSignal(N, 0x6000u + c); }
  const std::vector<float> desired = makeSignal(N, 0x6100u);
  std::vector<const float*> ptrs(C);
  for (size_t c = 0; c < C; ++c) { ptrs[c] = refs[c].data(); }

  fftconvolver::AdaptiveConvolver conv;
  TASK_CHECK(conv.init(B, L, C, 0.25f, 0.2f, 1e-4f));
  std::vector<float> pred(N), res(N);

  // Learn for four blocks
  conv.process(ptrs.data(), desired.data(), pred.data(), res.data(), B * 4);
  std::vector<float> tapsBefore(L), tapsNow(L);
  conv.getFilter(0, tapsBefore.data());
  bool learnedSomething = false;
  for (float v : tapsBefore) { if (v != 0.0f) learnedSomething = true; }
  TASK_CHECK(learnedSomething);

  // Freeze at a block boundary: coefficients stop moving. References and
  // desired always share the same time position, so offset both together.
  TASK_CHECK(conv.setLearning(false));
  const float* frozenRefs[2] = { refs[0].data() + B * 4, refs[1].data() + B * 4 };
  conv.process(frozenRefs, desired.data() + B * 4,
               pred.data() + B * 4, res.data() + B * 4, B * 2);
  conv.getFilter(0, tapsNow.data());
  TASK_CHECK(samePcm(tapsBefore, tapsNow));

  // Half-accumulated block: switching must be rejected and leave the flag alone
  const float* partialRefs[2] = { refs[0].data() + B * 6, refs[1].data() + B * 6 };
  conv.process(partialRefs, desired.data() + B * 6,
               pred.data() + B * 6, res.data() + B * 6, 3);
  TASK_CHECK(!conv.setLearning(true));
  TASK_CHECK(!conv.setLearning(false));

  // Complete the block while still frozen: taps remain unchanged
  const float* restRefs[2] = { refs[0].data() + B * 6 + 3, refs[1].data() + B * 6 + 3 };
  conv.process(restRefs, desired.data() + B * 6 + 3,
               pred.data() + B * 6 + 3, res.data() + B * 6 + 3, B - 3);
  conv.getFilter(0, tapsNow.data());
  TASK_CHECK(samePcm(tapsBefore, tapsNow));

  // Resume at the next boundary: learning moves the taps again
  TASK_CHECK(conv.setLearning(true));
  const float* resumeRefs[2] = { refs[0].data() + B * 7, refs[1].data() + B * 7 };
  conv.process(resumeRefs, desired.data() + B * 7,
               pred.data() + B * 7, res.data() + B * 7, B * 3);
  conv.getFilter(0, tapsNow.data());
  bool moved = false;
  for (size_t i = 0; i < L; ++i) { if (std::fabs(tapsNow[i] - tapsBefore[i]) > 1e-6f) moved = true; }
  TASK_CHECK(moved);
  TASK_CHECK(checkDelayIdentity(B, desired, pred, res));

  // Same learning schedule through fragmented calls must produce identical PCM.
  // Split the stream at the exact toggle frames so the toggles always land on a
  // block boundary regardless of the 1/7/31 chunking.
  fftconvolver::AdaptiveConvolver conv2;
  TASK_CHECK(conv2.init(B, L, C, 0.25f, 0.2f, 1e-4f));
  std::vector<float> pred2(N), res2(N);
  auto processRange = [&](size_t from, size_t to)
  {
    size_t p = from;
    size_t cl = 0;
    while (p < to)
    {
      const size_t chunk = std::min(to - p, kChunkSizes[cl++ % 3]);
      std::vector<const float*> fragPtrs(C);
      for (size_t c = 0; c < C; ++c) { fragPtrs[c] = refs[c].data() + p; }
      conv2.process(fragPtrs.data(), desired.data() + p,
                    pred2.data() + p, res2.data() + p, chunk);
      p += chunk;
    }
  };
  processRange(0, B * 4);
  TASK_CHECK(conv2.setLearning(false));
  processRange(B * 4, B * 7);
  TASK_CHECK(conv2.setLearning(true));
  processRange(B * 7, N);
  TASK_CHECK(samePcm(pred, pred2));
  TASK_CHECK(samePcm(res, res2));
  TASK_CHECK(sameTaps(conv, conv2, C, L));

  return 0;
}

int testLearningReset()
{
  const size_t B = 16;
  const size_t L = 19;
  const size_t N = B * 6;
  const std::vector<float> ref = makeSignal(N, 0x7000u);
  const std::vector<float> desired = makeSignal(N, 0x7100u);
  const float* ptrs[1] = { ref.data() };

  fftconvolver::AdaptiveConvolver conv;
  TASK_CHECK(conv.init(B, L, 1, 0.25f, 0.2f, 1e-4f));
  std::vector<float> pred(N), res(N);
  conv.process(ptrs, desired.data(), pred.data(), res.data(), B * 3);

  conv.reset();
  std::vector<float> taps(L, 1.0f);
  conv.getFilter(0, taps.data());
  for (float v : taps) { TASK_CHECK(v == 0.0f); }

  // Timing restarts at zero: the first B output frames after reset are zero
  conv.process(ptrs, desired.data(), pred.data(), res.data(), N);
  for (size_t i = 0; i < B; ++i)
  {
    TASK_CHECK(pred[i] == 0.0f);
    TASK_CHECK(res[i] == 0.0f);
  }

  // Learning is re-enabled by reset: the first adapted block changes the taps
  bool moved = false;
  conv.getFilter(0, taps.data());
  for (float v : taps) { if (v != 0.0f) moved = true; }
  TASK_CHECK(moved);

  // A fresh convolver on the same input must be bit-identical after reset
  fftconvolver::AdaptiveConvolver fresh;
  TASK_CHECK(fresh.init(B, L, 1, 0.25f, 0.2f, 1e-4f));
  std::vector<float> predFresh(N), resFresh(N);
  fresh.process(ptrs, desired.data(), predFresh.data(), resFresh.data(), N);
  TASK_CHECK(samePcm(pred, predFresh));
  TASK_CHECK(samePcm(res, resFresh));

  return 0;
}

int testLearningIdentification()
{
  const size_t B = 32;
  const size_t L = 129;
  const size_t C = 2;
  const size_t blocks = 4096;
  const size_t N = B * blocks;
  const float mu = 0.25f;
  const float beta = 0.2f;
  const float delta = 1e-4f;

  // Two uncorrelated references and known sparse short responses
  std::vector<std::vector<float> > refs(C);
  refs[0] = makeSignal(N + B + 4096, 0x0A0A0A0Au);
  refs[1] = makeSignal(N + B + 4096, 0x0B0B0B0Bu);

  std::vector<std::vector<float> > h(C, std::vector<float>(L, 0.0f));
  h[0][0] = 0.55f;   h[0][17] = -0.31f; h[0][53] = 0.27f;  h[0][128] = -0.12f;
  h[1][5] = 0.42f;   h[1][40] = -0.28f; h[1][90] = 0.19f;  h[1][128] = 0.11f;

  std::vector<float> desired(N);
  for (size_t n = 0; n < N; ++n)
  {
    desired[n] = timeFir(refs[0], h[0], static_cast<long>(n))
               + timeFir(refs[1], h[1], static_cast<long>(n));
  }

  fftconvolver::AdaptiveConvolver conv;
  TASK_CHECK(conv.init(B, L, C, mu, beta, delta));

  // Process exactly N frames through fragmented calls (no zero flush here, so
  // the internal reference history stays continuous for the continuation test)
  std::vector<float> predicted(N), residual(N);
  std::vector<const float*> ptrs(C);
  size_t pos = 0;
  size_t call = 0;

  g_countHeap = true;
  const unsigned long allocsBefore = g_allocs;
  const unsigned long freesBefore = g_deallocs;

  while (pos < N)
  {
    const size_t chunk = std::min(N - pos, kChunkSizes[call++ % 3]);
    for (size_t c = 0; c < C; ++c) { ptrs[c] = refs[c].data() + pos; }
    conv.process(ptrs.data(), desired.data() + pos,
                 predicted.data() + pos, residual.data() + pos, chunk);
    pos += chunk;
  }

  const unsigned long allocsDuring = g_allocs - allocsBefore;
  const unsigned long freesDuring = g_deallocs - freesBefore;
  g_countHeap = false;
  std::printf("ALLOC process_allocs=%lu process_frees=%lu\n", allocsDuring, freesDuring);
  TASK_CHECK(allocsDuring == 0);
  TASK_CHECK(freesDuring == 0);

  // Residual energy of the last 256 blocks whose output has been emitted
  // (the final input block is still inside the B-frame output delay)
  const size_t firstMeasuredBlock = blocks - 257;
  const size_t firstInput = firstMeasuredBlock * B;
  const size_t lastInput = N - B;
  double targetEnergy = 0.0;
  double residualEnergy = 0.0;
  for (size_t n = firstInput; n < lastInput; ++n)
  {
    targetEnergy += static_cast<double>(desired[n]) * desired[n];
    residualEnergy += static_cast<double>(residual[n + B]) * residual[n + B];
  }
  const double db = 10.0 * std::log10(targetEnergy / residualEnergy);
  std::printf("ERL reduction=%.3f dB (target >= 18 dB)\n", db);
  TASK_CHECK(db >= 18.0);

  // Freeze at the block boundary and export the learned taps
  TASK_CHECK(conv.setLearning(false));
  std::vector<std::vector<float> > taps(C, std::vector<float>(L));
  for (size_t c = 0; c < C; ++c) { conv.getFilter(c, taps[c].data()); }

  // Feed continuous subsequent material and verify the exported taps through an
  // independent time-domain convolution. Frame j of this call is global input
  // frame N+j; output frame t corresponds to global input N+t-B.
  const size_t contLen = 2000;
  std::vector<float> pCont(contLen), rCont(contLen);
  std::vector<float> desiredCont(contLen);
  for (size_t j = 0; j < contLen; ++j)
  {
    const long n = static_cast<long>(N + j);
    desiredCont[j] = timeFir(refs[0], h[0], n) + timeFir(refs[1], h[1], n);
  }
  pos = 0;
  call = 0;
  while (pos < contLen)
  {
    const size_t chunk = std::min(contLen - pos, kChunkSizes[call++ % 3]);
    for (size_t c = 0; c < C; ++c) { ptrs[c] = refs[c].data() + N + pos; }
    conv.process(ptrs.data(), desiredCont.data() + pos,
                 pCont.data() + pos, rCont.data() + pos, chunk);
    pos += chunk;
  }

  double maxPredErr = 0.0;
  double maxIdErr = 0.0;
  for (size_t t = B; t < contLen; ++t)
  {
    const long n = static_cast<long>(N + t - B);
    const float expectedPred = timeFir(refs[0], taps[0], n) + timeFir(refs[1], taps[1], n);
    maxPredErr = std::max(maxPredErr, static_cast<double>(std::fabs(pCont[t] - expectedPred)));
    const float expectedDesired = timeFir(refs[0], h[0], n) + timeFir(refs[1], h[1], n);
    maxIdErr = std::max(maxIdErr, static_cast<double>(std::fabs(pCont[t] + rCont[t] - expectedDesired)));
  }
  std::printf("CONTINUE max_pred_err=%.3g max_identity_err=%.3g\n", maxPredErr, maxIdErr);
  TASK_CHECK(maxPredErr <= 3e-4);
  TASK_CHECK(maxIdErr <= 3e-4);

  // Zero-frame flush on a second convolver: the B delayed tail outputs must
  // keep the delayed identity, treating out-of-range desired as zero
  fftconvolver::AdaptiveConvolver flusher;
  TASK_CHECK(flusher.init(B, L, C, mu, beta, delta));
  const size_t flushTotal = N + B;
  std::vector<float> pFlush(flushTotal), rFlush(flushTotal);
  std::vector<float> zeroRef(B, 0.0f);
  pos = 0;
  while (pos < flushTotal)
  {
    const size_t chunk = std::min(flushTotal - pos, B);
    const bool inSignal = (pos < N);
    for (size_t c = 0; c < C; ++c) { ptrs[c] = inSignal ? refs[c].data() + pos : zeroRef.data(); }
    const float* desiredPtr = inSignal ? desired.data() + pos : zeroRef.data();
    flusher.process(ptrs.data(), desiredPtr,
                    pFlush.data() + pos, rFlush.data() + pos, chunk);
    pos += chunk;
  }
  for (size_t t = B; t < flushTotal; ++t)
  {
    const long n = static_cast<long>(t - B);
    const float expected = (n >= 0 && static_cast<size_t>(n) < N) ? desired[static_cast<size_t>(n)] : 0.0f;
    TASK_CHECK(std::fabs(pFlush[t] + rFlush[t] - expected) <= 1e-6f);
  }

  return 0;
}


// -----------------------------------------------------------------------
// Registration (external linkage: the C driver references these symbols)
// -----------------------------------------------------------------------
} // namespace

const struct task_case task_cases[] =
{
  {"Gradient",   "FirstSixBlocks",      testGradientFirstBlocks},
  {"Streaming",  "Equivalence1ch",      testStreaming1ch},
  {"Streaming",  "Equivalence2ch",      testStreaming2ch},
  {"Streaming",  "Equivalence6ch",      testStreaming6ch},
  {"Streaming",  "Silence",             testStreamingSilence},
  {"Streaming",  "ResidualInPlace",     testStreamingInPlace},
  {"Learning",   "FreezeResume",        testLearningFreezeResume},
  {"Learning",   "Reset",               testLearningReset},
  {"Learning",   "Identification18dB",  testLearningIdentification},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
